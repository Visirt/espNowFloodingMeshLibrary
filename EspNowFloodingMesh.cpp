#ifdef ESP32
  #include <esp_now.h>
  #include <WiFi.h>
  #include <rom/crc.h>
  #include "mbedtls/aes.h"
#else
#include <ESP8266WiFi.h>
#include "AESLib.h" //From https://github.com/kakopappa/arduino-esp8266-aes-lib
#endif

#include "espnowBroadcast.h"
#include "EspNowFloodingMesh.h"

#define AES_BLOCK_SIZE  16
#define DISPOSABLE_KEY_LENGTH AES_BLOCK_SIZE
#define REJECTED_LIST_SIZE 50
#define REQUEST_REPLY_DATA_BASE_SIZE 20

#define USER_MSG 1
#define USER_REQUIRE_RESPONSE_MSG 4
#define USER_REQUIRE_REPLY_MSG 5


unsigned char ivKey[16] = {0xb2, 0x4b, 0xf2, 0xf7, 0x7a, 0xc5, 0xec, 0x0c, 0x5e, 0x1f, 0x4d, 0xc1, 0xae, 0x46, 0x5e, 0x75};

bool masterFlag = false;
bool syncronized = false;
bool batteryNode = false;
uint8_t syncTTL = 0;
bool isespNowFloodingMeshInitialized = false;
int myBsid = 0x112233;
uint64_t myNode = 0;

#pragma pack(push,1)
struct header{
uint8_t msgId;
uint8_t length;
uint32_t p1;
uint64_t node;
};

struct mesh_secred_part{
  struct header header;
  uint8_t data[240];
};

struct mesh_unencrypted_part{
  unsigned char bsid[3];
  uint8_t ttl;
  uint16_t crc16;
  void setBsid(uint32_t v) {
      bsid[0]=(v>>(16))&0xff;
      bsid[1]=(v>>(8))&0xff;
      bsid[2]=v&0xff;
  }
   void set(const uint8_t *v) {
      memcpy(this,v,sizeof(struct mesh_unencrypted_part));
  }
  uint32_t getBsid(){
      uint32_t ret=0;
      ret|=((uint32_t)bsid[0])<<16;
      ret|=((uint32_t)bsid[1])<<8;
      ret|=((uint32_t)bsid[2]);
      return ret;
  }
};
typedef struct mesh_unencrypted_part unencrypted_t;
#define SECRED_PART_OFFSET sizeof(unencrypted_t)


struct meshFrame{
  unencrypted_t unencrypted;
  struct mesh_secred_part encrypted;
};
#pragma pack(pop);
int espNowFloodingMesh_getTTL() {
    return syncTTL;
}
const unsigned char broadcast_mac[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
uint8_t aes_secredKey[] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE, 0xFF};
bool forwardMsg(const uint8_t *data, int len);
uint32_t sendMsg(uint8_t* msg, int size, int ttl, int msgId, void *ptr=NULL, uint64_t destNode = 0);
void hexDump(const uint8_t*b,int len);
static void (*espNowFloodingMesh_receive_cb)(const uint8_t *, int, uint64_t, uint32_t) = NULL;

uint16_t calculateCRC(int c, const unsigned char*b,int len);
uint16_t calculateCRC(struct meshFrame *m);
void decrypt(const uint8_t *_from, struct meshFrame *m, int size);

void (*errorPrintCB)(int,const char *) = NULL;

void espNowFloodingMesh_ErrorDebugCB(void (*callback)(int, const char *)){
    errorPrintCB = callback;
}

void print(int level, const char * format, ... )
{

 if(errorPrintCB){
      static char buffer[256];
      va_list args;
      va_start (args, format);
      vsprintf (buffer,format, args);

      errorPrintCB(level, buffer);

      va_end (args);
  }
}


void espNowFloodingMesh_setAesInitializationVector(const unsigned char iv[16]) {
  memcpy(ivKey, iv, sizeof(ivKey));
}

void espNowFloodingMesh_setToBatteryNode(bool isBatteryNode) {
  batteryNode = isBatteryNode;
}

struct requestReplyDbItem{
    void (*cb)(const uint8_t *, int);
    uint32_t messageIdentifierCode;
    uint8_t ttl;
};
class RequestReplyDataBase{
public:
  RequestReplyDataBase(){
    index=0;
    memset(db, 0,sizeof(db));
    c=1;
  }
  ~RequestReplyDataBase(){}
  void add(uint32_t messageIdentifierCode, void (*f)(const uint8_t *, int)) {
    db[index].cb = f;
    db[index].messageIdentifierCode = messageIdentifierCode;
    index++;
    if(index>=REQUEST_REPLY_DATA_BASE_SIZE) {
      index = 0;
    }
  }
  uint32_t calculateMessageIdentifier() {
    String mac = WiFi.macAddress();
    uint32_t ret = calculateCRC(0, (const uint8_t*)mac.c_str(), 6);
    #ifdef ESP32
      ret = ret<<8 | (esp_random()&0xff);
    #else
      ret = ret<<8 | (random(0, 0xff)&0xff);
    #endif
    ret = ret<<8 | c++;
    if(c==0) { c=1; } //messageIdentifier is never zero
    return ret;
  }
  const struct requestReplyDbItem* getCallback(uint32_t messageIdentifierCode) {
    for(int i=0;i<REQUEST_REPLY_DATA_BASE_SIZE;i++) {
      if(db[i].messageIdentifierCode==messageIdentifierCode) {
        if(db[i].cb!=NULL) {
          return &db[i];
        }
      }
    }
    return NULL;
  }
  void removeItem() {//Cleaning db  --> Remove the oldest item
    memset(&db[index],0,sizeof(struct requestReplyDbItem));
    index++;
    if(index>=REQUEST_REPLY_DATA_BASE_SIZE) {
      index=0;
    }
  }
private:
    struct requestReplyDbItem db[REQUEST_REPLY_DATA_BASE_SIZE];
    int index;
    uint8_t c;
};
RequestReplyDataBase requestReplyDB;

class RejectedMessageDB{
public:
  ~RejectedMessageDB() {}
  RejectedMessageDB() {
    memset(rejectedMsgList,0, sizeof(rejectedMsgList));
    memset(ttlList,0, sizeof(ttlList));
    index=0;
  }
  void removeItem() { //Cleaning db  --> Remove the oldest item
    rejectedMsgList[index] = 0;
    ttlList[index] = 0;
    index++;
    if(index>=REJECTED_LIST_SIZE) {
      index=0;
    }
  }
  void addMessageToHandledList(struct meshFrame *m) {
    uint16_t crc = m->unencrypted.crc16;
    for(int i=0;i<REJECTED_LIST_SIZE;i++){
      if(rejectedMsgList[i]==crc) {
        if(ttlList[i]<m->unencrypted.ttl) {
          ttlList[i] = m->unencrypted.ttl;
        }
        return;
      }
    }
    rejectedMsgList[index] = crc;
    ttlList[index] = m->unencrypted.ttl;

    index++;
    if(index>=REJECTED_LIST_SIZE) {
      index=0;
    }
  }

  int isMessageInHandledList(struct meshFrame *m) {
    bool forwardNeeded=false;
    bool handled=false;
    uint16_t crc = m->unencrypted.crc16;
    for(int i=0;i<REJECTED_LIST_SIZE;i++){
      if(rejectedMsgList[i]==crc) {
        handled = true;
        if(ttlList[i]<m->unencrypted.ttl) {
          forwardNeeded = true;
        }
        break;
      }
    }
    if(forwardNeeded) return 2;
    if(handled) return 1;
    return 0;
  }
private:
    uint16_t rejectedMsgList[REJECTED_LIST_SIZE];
    uint8_t ttlList[REJECTED_LIST_SIZE];
    int index;
};
RejectedMessageDB rejectedMessageDB;


void espNowFloodingMesh_RecvCB(void (*callback)(const uint8_t *, int, uint64_t, uint32_t)){
  espNowFloodingMesh_receive_cb = callback;
}

void espNowFloodingMesh_delay(unsigned long tm) {
  for(int i=0;i<(tm/10);i++){
    espNowFloodingMesh_loop();
    delay(10);
  }
}

void espNowFloodingMesh_loop(){
  if(isespNowFloodingMeshInitialized==false) return;
   //Clean data base
  static unsigned long dbtm = millis();
  unsigned long elapsed = millis()-dbtm;
  if(elapsed>=500) {
    dbtm = millis();
    requestReplyDB.removeItem();
    rejectedMessageDB.removeItem();
  }
  
  delay(1);
}
void espNowFloodingMesh_setToMasterRole(bool master, unsigned char ttl){
  masterFlag = master;
  syncTTL = ttl;
}
uint16_t calculateCRC(int c, const unsigned char*b,int len) {
  #ifdef ESP32JJJ
    return crc16_le(0, b, len);
  #else
    //Copied from https://www.lammertbies.nl/forum/viewtopic.php?t=1528
    uint16_t crc = 0xFFFF;
    int i;
    if (len) do {
    crc ^= *b++;
    for (i=0; i<8; i++) {
      if (crc & 1) crc = (crc >> 1) ^ 0x8408;
      else crc >>= 1;
    }
    } while (--len);
    return(~crc);
  #endif
}

uint16_t calculateCRC(struct meshFrame *m){
  //uint16_t crc = m->encrypted.header.crc16;
  //m->encrypted.header.crc16 = 0;
  int size = m->encrypted.header.length + sizeof(m->encrypted.header);
  uint16_t ret = calculateCRC(0, (const unsigned char*)m + SECRED_PART_OFFSET,size);
  //m->encrypted.header.crc16 = crc;
  return ret;
}

void hexDump(const uint8_t*b,int len){
  //#ifdef DEBUG_PRINTS
  Serial.println();
  for(int i=0;i<len;i=i+16) {
    Serial.print("           ");
    for(int x=0;x<16&&(x+i)<len;x++) {
      if(b[i+x]<=0xf) Serial.print("0");
      Serial.print(b[i+x],HEX);
      Serial.print(" ");
    }
    printf("   ");
    for(int x=0;x<16&&(x+i)<len;x++) {
      if(b[i+x]<=32||b[i+x]>=126) {
          Serial.print("_");
      } else Serial.print((char)b[i+x]);
    }
    Serial.print("\n");
  }
  Serial.print("                   Length: ");
  Serial.println(len);
//  #endif
}

void msg_recv_cb(const uint8_t *data, int len)
{
  #ifdef DEBUG_PRINTS
  Serial.print("REC[RAW]:");
  hexDump((uint8_t*)data,len);
  #endif
  struct meshFrame m;
  m.unencrypted.set(data);

    if(myBsid!=m.unencrypted.getBsid()) {
      //Serial.println(myBsid, HEX);
      //Serial.println(m.unencrypted.getBsid(), HEX);
      return;
    }
    if(len>=sizeof(struct meshFrame)) return;

    int messageStatus = rejectedMessageDB.isMessageInHandledList(&m);
    if(messageStatus==1) {
      //Message is already handled... No need to forward
      return;
    }
    rejectedMessageDB.addMessageToHandledList(&m);


    //memset(&m,0,sizeof(m));
    decrypt((const uint8_t*)data, &m, len);
#ifdef DEBUG_PRINTS
    Serial.print("REC:");
    hexDump((uint8_t*)&m,m.encrypted.header.length + sizeof(m.encrypted.header)+3);
#endif
    if(!(m.encrypted.header.msgId==USER_MSG||m.encrypted.header.msgId==USER_REQUIRE_RESPONSE_MSG||m.encrypted.header.msgId==USER_REQUIRE_REPLY_MSG)) {
        //Quick wilter;
        return;
    }
    if(m.encrypted.header.length>=0 && m.encrypted.header.length < (sizeof(m.encrypted.data) ) ){
      uint16_t crc = m.unencrypted.crc16;
      int messageLengtWithHeader = m.encrypted.header.length + sizeof(struct header);
      uint16_t crc16 = calculateCRC(&m);

        #ifdef DEBUG_PRINTS
        Serial.print("REC:");
        hexDump((uint8_t*)&m,messageLengtWithHeader);
        #endif

        if(crc16==crc) {

          bool ok = false;
          if(messageStatus==0) { //if messageStatus==0 --> message is not handled yet.
            if( m.encrypted.header.msgId==USER_MSG) {
              if(masterFlag || m.encrypted.header.node == myNode) {
                if(espNowFloodingMesh_receive_cb)
                  espNowFloodingMesh_receive_cb(m.encrypted.data, m.encrypted.header.length, m.encrypted.header.node, 0);
                ok = true;
              } else {
                #ifdef DEBUG_PRINTS
                Serial.println("Reject message:");
                hexDump((uint8_t*)&m,  messageLengtWithHeader);
                #endif
              }
            }
            
            if( m.encrypted.header.msgId==USER_REQUIRE_REPLY_MSG) {
              const struct requestReplyDbItem* d = requestReplyDB.getCallback(m.encrypted.header.p1);
              if(d!=NULL){
                d->cb(m.encrypted.data, m.encrypted.header.length);
              } else if(masterFlag || m.encrypted.header.node == myNode){
                if(espNowFloodingMesh_receive_cb)
                  espNowFloodingMesh_receive_cb(m.encrypted.data, m.encrypted.header.length, m.encrypted.header.node, m.encrypted.header.p1);
              }
                ok = true;
            }
            if(m.encrypted.header.msgId==USER_REQUIRE_RESPONSE_MSG) {
              if(masterFlag || m.encrypted.header.node == myNode) {
                espNowFloodingMesh_sendReply((uint8_t*)"ACK", 3, syncTTL, m.encrypted.header.p1);
                if(espNowFloodingMesh_receive_cb)
                  espNowFloodingMesh_receive_cb(m.encrypted.data, m.encrypted.header.length, m.encrypted.header.node, m.encrypted.header.p1);
                ok = true;
              } else {
                #ifdef DEBUG_PRINTS
                Serial.print("Reject message:");
                hexDump((uint8_t*)&m,  messageLengtWithHeader);
                #endif
                print(1,"Message rejected.");
              }
            }
        }
        if(ok && m.unencrypted.ttl>0 && batteryNode==false) {
          //Serial.println("TTL");
          //delay(1);
          forwardMsg(data, len);
        }
      } else {
      #ifdef DEBUG_PRINTS
        Serial.print("#CRC: ");Serial.print(crc16);Serial.print(" "),Serial.println(crc);
        for(int i=0;i<m.encrypted.header.length;i++){
          Serial.print("0x");Serial.print(data[i],HEX);Serial.print(",");
        }
        Serial.println();
        hexDump((uint8_t*)&m,200);
        Serial.println();
        hexDump((uint8_t*)data,200);
       #endif
      }
    } else {
      #ifdef DEBUG_PRINTS
      Serial.print("Invalid message received:"); Serial.println(0,HEX);
      hexDump(data,len);
      #endif
    }
}

void espNowFloodingMesh_end() {
}

uint64_t StringToInt(String node)
{
  uint64_t res = 0;
  if(node.length() > 8)
    return 0;
  for(char c : node)
    res = (res << 8) + c;
  return res;
}


//   void setSendCb(function<void(void)> f)
void espNowFloodingMesh_begin(int channel, int bsid, String nodeid) {
  myNode = StringToInt(nodeid);
  #ifndef ESP32
    randomSeed(analogRead(0));
  #endif

  espnowBroadcast_cb(msg_recv_cb);
  espnowBroadcast_begin(channel);

  isespNowFloodingMeshInitialized=true;

  myBsid = bsid;
}

void espNowFloodingMesh_secredkey(const unsigned char key[16]){
  memcpy(aes_secredKey, key, sizeof(aes_secredKey));
}

void decrypt(const uint8_t *_from, struct meshFrame *m, int size) {
  unsigned char iv[16];
  memcpy(iv,ivKey,sizeof(iv));

  uint8_t to[2*16];
  for(int i=0;i<size;i=i+16) {
      const uint8_t *from = _from + i + SECRED_PART_OFFSET;
      uint8_t *key = aes_secredKey;

      #ifdef DISABLE_CRYPTING
        memcpy(to,from,16);
      #else
        #ifdef ESP32

          esp_aes_context ctx;
          esp_aes_init( &ctx );
          esp_aes_setkey( &ctx, key, 128 );
          esp_aes_acquire_hardware ();
          esp_aes_crypt_cbc(&ctx, ESP_AES_DECRYPT, 16, iv, from, to);
          esp_aes_release_hardware ();
          esp_aes_free(&ctx);

        #else
          AES aesLib;
          aesLib.set_key( (byte *)key , sizeof(key));
          aesLib.do_aes_decrypt((byte *)from,16 , to, key, 128, iv);
        #endif
      #endif

      if((i+SECRED_PART_OFFSET+16)<=sizeof(m->encrypted)) {
        memcpy((uint8_t*)m+i+SECRED_PART_OFFSET, to, 16);
      }
  }
}

int encrypt(struct meshFrame *m) {
  int size = ((m->encrypted.header.length + sizeof(m->encrypted.header))/16)*16+16;

  unsigned char iv[16];
  memcpy(iv,ivKey,sizeof(iv));
  uint8_t to[2*16];

  for(int i=0;i<size;i=i+16) {
      uint8_t *from = (uint8_t *)m+i+SECRED_PART_OFFSET;
      uint8_t *key = aes_secredKey;
     #ifdef DISABLE_CRYPTING
       memcpy((void*)to,(void*)from,16);
     #else
        #ifdef ESP32
         esp_aes_context ctx;
         esp_aes_init( &ctx );
         esp_aes_setkey( &ctx, key, 128 );
         esp_aes_acquire_hardware();
         esp_aes_crypt_cbc(&ctx, ESP_AES_ENCRYPT, 16, iv, from, to);
         esp_aes_release_hardware();
         esp_aes_free(&ctx);
        #else
          AES aesLib;
          aesLib.set_key( (byte *)key , sizeof(key));
          aesLib.do_aes_encrypt((byte *)from, size , (uint8_t *)&m->encrypted, key, 128, iv);
          break;
        #endif
      #endif
      memcpy((uint8_t*)m+i+SECRED_PART_OFFSET, to, 16);
  }
/*
  for(int i=m->encrypted.header.length + sizeof(m->encrypted.header)+1;i<size;i++) {
    #ifdef ESP32
    ((unsigned char*)&m->encrypted.header)[i]=esp_random();
    #else
    ((unsigned char*)&m->encrypted.header)[i]=random(0, 255);
    #endif
  }*/

  return size + SECRED_PART_OFFSET;
}

bool forwardMsg(const uint8_t *data, int len) {
  struct meshFrame m;
  memcpy(&m, data,len);

  if(m.unencrypted.ttl==0) return false;

  m.unencrypted.ttl = m.unencrypted.ttl-1;

  #ifdef DEBUG_PRINTS
  Serial.print("FORWARD:");
  hexDump((const uint8_t*)data, len);
  #endif

  espnowBroadcast_send((uint8_t*)(&m), len);

  return true;
}


uint32_t sendMsg(uint8_t* msg, int size, int ttl, int msgId, void *ptr, uint64_t destNode) {
  uint32_t ret=0;
  if(size>=sizeof(struct mesh_secred_part)) {
    #ifdef DEBUG_PRINTS
    Serial.println("espNowFloodingMesh_send: Invalid size");
    #endif
    return false;
  }

  static struct meshFrame m;
  memset(&m,0x00,sizeof(struct meshFrame)); //fill
  m.encrypted.header.length = size;
  if(masterFlag)
    m.encrypted.header.node = destNode;
  else
    m.encrypted.header.node = myNode;
  m.unencrypted.crc16 = 0;
  m.encrypted.header.msgId = msgId;
  m.unencrypted.ttl= ttl;
  m.unencrypted.setBsid(myBsid);
  m.encrypted.header.p1 = requestReplyDB.calculateMessageIdentifier();

  if(msg!=NULL){
    memcpy(m.encrypted.data, msg, size);
  }

  if(msgId==USER_REQUIRE_RESPONSE_MSG) {

    ret = m.encrypted.header.p1;
    requestReplyDB.add(m.encrypted.header.p1, (void (*)(const uint8_t*, int))ptr);
    //Serial.print("Send request with "); Serial.println(m.encrypted.header.p1);
  } if(msgId==USER_REQUIRE_REPLY_MSG && ptr!=NULL) {
    m.encrypted.header.p1 = *((uint32_t*)ptr);
  }

  m.unencrypted.crc16 = calculateCRC(&m);
  #ifdef DEBUG_PRINTS
   Serial.print("Send0:");
   hexDump((const uint8_t*)&m, size+20);
  #endif
  rejectedMessageDB.addMessageToHandledList(&m);

  int sendSize = encrypt(&m);

/*
struct meshFrame mm;
Serial.print("--->:");
decrypt((const uint8_t*)&m, &mm, sendSize);
Serial.print("--->:");
hexDump((const uint8_t*)&mm, size+20);
Serial.print("--->:");
*/

   #ifdef DEBUG_PRINTS
    Serial.print("Send[RAW]:");
    hexDump((const uint8_t*)&m, sendSize);
  #endif
  
  espnowBroadcast_send((uint8_t*)&m, sendSize);
  return ret;
}

void espNowFloodingMesh_send(uint8_t* msg, int size, int ttl)  {
   sendMsg(msg, size, ttl, USER_MSG);
}

void espNowFloodingMesh_sendReply(uint8_t* msg, int size, int ttl, uint32_t replyIdentifier)  {
   sendMsg(msg, size, ttl, USER_REQUIRE_REPLY_MSG, (void*)&replyIdentifier);
}

uint32_t espNowFloodingMesh_sendAndHandleReply(uint8_t* msg, int size, int ttl, void (*f)(const uint8_t *, int), uint64_t dest) {
  return sendMsg(msg, size, ttl, USER_REQUIRE_RESPONSE_MSG, (void*)f, dest);
}

bool espNowFloodingMesh_sendWithACK(uint8_t* msg, int size, String dest)
{
  return espNowFloodingMesh_sendAndWaitReply(msg, size, syncTTL, 5, [](const uint8_t *data, int size){}, 100, 1, StringToInt(dest));
}

bool espNowFloodingMesh_sendAndWaitReply(uint8_t* msg, int size, int ttl, int tryCount, void (*f)(const uint8_t *, int), int timeoutMs, int expectedCountOfReplies, uint64_t dest){
  static int replyCnt=0;
  static void (*callback)(const uint8_t *, int);
  callback = f;

  for(int i=0;i<tryCount;i++) {
    espNowFloodingMesh_sendAndHandleReply(msg, size, ttl, [](const uint8_t *data, int len){
      if(callback!=NULL) {
        callback(data,len);
      }
      replyCnt++;
    }, dest);

    unsigned long dbtm = millis();

    while(1) {
      espNowFloodingMesh_loop();
      delay(10);
      if(expectedCountOfReplies<=replyCnt) {
        return true; //OK all received;
      }
      unsigned long elapsed = millis()-dbtm;
      if(elapsed>timeoutMs) {
        //timeout
        print(0, "Timeout: waiting replies");
        break;
      }
    }
  }
  return false;
}
