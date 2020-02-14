#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/times.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <signal.h>
#include <netdb.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ifaddrs.h>

#define MAX_NODE_BACKLOG 5
#define CLIENT_NODE_COUNT 5 //Total clients running 5
#define SERVER_COUNT 3 //3 servers
#define MAX_FILES 2
#define MAX_BUFF_SIZE 60
#define PATH_TO_SERVER_CONFIG "../config/serverconfig.txt" //PATH to global file
#define PATH_TO_CLIENT_CONFIG "../config/clientconfig.txt"
#define MAX_MESSAGE_QUEUE_SIZE 6

void *request_generator_function(void *param);

typedef struct file_info{
    int count;
    struct details {
        int id;
        char name[20];
    }details[MAX_FILES];
}file_info;

file_info global_file_info;


typedef struct servernodes {
    int serverid;
    char ip[25];
    unsigned int port;
    int socketfd;
}servernodes;

servernodes servers[SERVER_COUNT];

typedef struct clientnodes {
    int clientid;
    char ip[25];
    unsigned int port;
    int socketfd;
}clientnodes; //struct to hold client nodes info

typedef struct sync_data {
    pthread_mutex_t lock;
    pthread_cond_t wait;
}sync_data;

typedef enum SERVER_MESSAGE_TYPE {
    SERVER_MESSAGE_TYPE_REQUEST,
    SERVER_MESSAGE_TYPE_RESPONSE
}SERVER_MESSAGE_TYPE;

typedef enum SERVER_REQUEST_RESPONSE {
    SERVER_REQUEST_ENQUIRY,
    SERVER_REQUEST_READ,
    SERVER_REQUEST_WRITE,
    SERVER_RESPONSE_INVALID,
    SERVER_RESPONSE_SUCCESS,
    SERVER_RESPONSE_FAILED
}SERVER_REQUEST_RESPONSE;

typedef enum CLIENT_MESSAGE_TYPE {
    CLIENT_MESSAGE_TYPE_REQUEST,
    CLIENT_MESSAGE_TYPE_REPLY
}CLIENT_MESSAGE_TYPE;

struct local_msg {
    int server_id;
    int file_id;
    SERVER_REQUEST_RESPONSE req;
    time_t timestamp;
    SERVER_REQUEST_RESPONSE operation;
}g_local_msg;

typedef struct message_server {
    SERVER_MESSAGE_TYPE type;
    int id;
    union subtype{
        struct request{
            SERVER_REQUEST_RESPONSE type;
            int fileid;
            char buff[200];
        }request;
        struct response {
            SERVER_REQUEST_RESPONSE type;
            union info {
                file_info finfo;
                char buff[200];
            }info;
        }response;
    }subtype;
}message_server;

typedef struct client_message {
    CLIENT_MESSAGE_TYPE messagetype;
    int fileid;
    int clientid;
    time_t timestamp;
}client_message;

//***********************************Queue handling**************************************
typedef enum QUEUE_TYPE {
    QUEUE_TYPE_REQUEST,
    QUEUE_TYPE_DEFER
}QUEUE_TYPE;

typedef enum QUEUE_CODE {
    QUEUE_CODE_FULL,
    QUEUE_CODE_EMPTH,
    QUEUE_CODE_OVERFLOW,
    QUEUE_CODE_UNDERFLOW,
    QUEUE_CODE_SUCCESS,
    QUEUE_CODE_INVALID
    
}QUEUE_CODE;

typedef struct queue {
    int front,rear,count;
    QUEUE_TYPE type;
    union queue_data {
        struct local_msg local_msg_queue[MAX_MESSAGE_QUEUE_SIZE];
        client_message client_msg_queue[MAX_MESSAGE_QUEUE_SIZE];
    }queue_data;
}QUEUE;
//***************************************************************************************
