#ifndef ESP_NOBRADCAST_H
#define ESP_NOBRADCAST_H


//#define DISABLE_CRYPTING //send messages as plain text
//#define DEBUG_PRINTS
    void espNowFloodingMesh_begin(int channel, int bsid, String nodeid);

    void espNowFloodingMesh_end();

    void espNowFloodingMesh_setChannel(int channel);

    void espNowFloodingMesh_setToMasterRole(bool master=true, unsigned char ttl=0 /*ttl for sync messages*/);
    void espNowFloodingMesh_setToBatteryNode(bool isBatteryNode=true);

    void espNowFloodingMesh_RecvCB(void (*callback)(const uint8_t *, int, uint64_t, uint32_t));
    void espNowFloodingMesh_send(uint8_t* msg, int size, int ttl=0); //Max message length is 236byte
    void espNowFloodingMesh_secredkey(const unsigned char key[16]);
    void espNowFloodingMesh_setAesInitializationVector(const unsigned char iv[16]);

    void espNowFloodingMesh_ErrorDebugCB(void (*callback)(int,const char *));


    uint32_t espNowFloodingMesh_sendAndHandleReply(uint8_t* msg, int size, int ttl, void (*f)(const uint8_t *, int)); //Max message length is 236byte

    //Run this only in Mainloop!!!
    bool espNowFloodingMesh_sendAndWaitReply(uint8_t* msg, int size, int ttl, int tryCount=1, void (*f)(const uint8_t *, int)=NULL, int timeoutMs=3000, int expectedCountOfReplies=1, uint64_t dest = 0); //Max message length is 236byte
    bool espNowFloodingMesh_sendWithACK(uint8_t* msg, int size, String dest);

    void espNowFloodingMesh_sendReply(uint8_t* msg, int size, int ttl, uint32_t replyIdentifier);

    void espNowFloodingMesh_loop();

    void espNowFloodingMesh_delay(unsigned long tm);
    int espNowFloodingMesh_getTTL();

#endif
