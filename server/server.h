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
#include <semaphore.h>
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
#define CLIENT_NODE_COUNT 5
#define SERVER_COUNT 3
#define MAX_FILES 2
#define PATH_TO_SERVER_CONFIG "../config/serverconfig.txt" //PATH to global file
#define PATH_TO_SHARED_FILES "./Files/"
#define MAX_BUFF_SIZE 60

typedef struct file_info{
    int count;
    struct details {
        int id;
        char name[20];
    }details[MAX_FILES];
}file_info;

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

typedef struct clientnodes {
    int clientid;
    int socketfd;
}clientnodes; //struct to hold client nodes info
