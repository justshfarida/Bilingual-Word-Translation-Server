#ifndef PROTO_H
#define PROTO_H

#include <pthread.h>
#include <sys/types.h>

// ---- Keys (shared between server & client)
#define SHM_KEY        0x1234
#define MSG_WORD_KEY   0x2345  // file -> word-pair queue
#define MSG_REQ_KEY    0x3456  // client -> server requests
#define MSG_RESP_KEY   0x4567  // server -> client replies

// ---- Paths
#define PID_FILE       "/tmp/dict_server.pid"
#define DICTIONARY_DIR "./dictionary_files"

// ---- Limits
#define MAX_WORD_LENGTH 50
#define MAX_WORDS       2048

typedef struct {
    char english[MAX_WORD_LENGTH];
    char french[MAX_WORD_LENGTH];
} WordPair;

typedef struct {
    pthread_mutex_t mutex;    // process-shared mutex
    int size;
    int initialized;          // 1 once initialized
    WordPair words[MAX_WORDS];
} SharedDictionary;

// V2: producer/consumer messages (mtype: 1 EN->FR, 2 FR->EN)
typedef struct {
    long mtype;
    char english[MAX_WORD_LENGTH];
    char french[MAX_WORD_LENGTH];
} MsgWord;

// V3: request/response (mtype: 1 ask EN->FR, 2 ask FR->EN)
typedef struct {
    long   mtype;       // 1 or 2
    pid_t  reply_to;    // client PID
    unsigned req_id;    // optional correlation
    char   word[MAX_WORD_LENGTH];
} MsgReq;

typedef struct {
    long   mtype;       // = client PID (reply channel)
    unsigned req_id;
    int    found;       // 1 found, 0 not found
    char   from[MAX_WORD_LENGTH];
    char   to[MAX_WORD_LENGTH];
} MsgResp;

#endif // PROTO_H
