#include "server.h"


file_info global_file_info;
FILE *filehandlers[MAX_FILES];

int myid;

clientnodes clinodes[CLIENT_NODE_COUNT];

pthread_t clientnode_threads[CLIENT_NODE_COUNT]; //threads to interact with client nodes

void printmessage(message_server msg) {
    
    printf("======================MESSAGE======================\n\n");
    printf("type %d ",msg.type);
    printf("id %d ",msg.id);
    if(msg.type == SERVER_MESSAGE_TYPE_REQUEST) {
        printf("request type %d \n\n",msg.subtype.request.type);
    } else if(msg.type == SERVER_MESSAGE_TYPE_RESPONSE) {
        printf("response type %d \n\n",msg.subtype.response.type);
    }
    printf("===================================================\n\n");

}

void sendResponse(int fd, message_server response){
//    printmessage(response);
    send(fd,&response,sizeof(message_server),0);
}


void * handleclient(void *val) {
    int fd = (uintptr_t)val;
    message_server rcv_msg = { 0 };
    message_server snd_msg = { 0 };
    
    while (1) {
        if( recv(fd , &rcv_msg, sizeof(message_server), MSG_WAITALL) <= 0){
            printf("recv error so exit\n");
            perror("recv() receiving request from server");
            close(fd);
            exit(1);
        }
        printmessage(rcv_msg);
        printf("client %d sent Request message\n",rcv_msg.id);
        switch(rcv_msg.type) {
             
            case SERVER_MESSAGE_TYPE_REQUEST:
                switch (rcv_msg.subtype.request.type) {
                    case SERVER_REQUEST_ENQUIRY:
                            printf("Received Enquiry\n");
                            snd_msg.type = SERVER_MESSAGE_TYPE_RESPONSE;
                            snd_msg.subtype.response.type = SERVER_RESPONSE_SUCCESS;
                            snd_msg.subtype.response.info.finfo = global_file_info;
                            sendResponse(fd,snd_msg);
                        break;
                    case SERVER_REQUEST_READ: {
                            char buffer[MAX_BUFF_SIZE + 1];
                            size_t len,minred = 0;
                            int fileid = rcv_msg.subtype.request.fileid;
                            char *last_newline;
                            snd_msg.type = SERVER_MESSAGE_TYPE_RESPONSE;
                            if(fileid < 0 || fileid > MAX_FILES || filehandlers[fileid] == NULL) {
                               
                                snd_msg.subtype.response.type = SERVER_RESPONSE_FAILED;
                                sendResponse(fd,snd_msg);
                                break;
                            }
                        
                            printf("Received message is of type read for file id %d\n",fileid);
                            memset(buffer, 0, MAX_BUFF_SIZE);
                            fseek(filehandlers[fileid], 0, SEEK_END);
                            if(filehandlers[fileid] == NULL) {
                                printf("file handler is NULL\n");
                                exit(1);
                            }
                            len = ftell(filehandlers[fileid]);
                            minred = len < MAX_BUFF_SIZE ? len : MAX_BUFF_SIZE;
                            fseek(filehandlers[fileid], -minred, SEEK_END);
                            len = ftell(filehandlers[fileid]);
                            len = 0;
                            len = fread( buffer, sizeof(char), minred, filehandlers[fileid]);
                            buffer[MAX_BUFF_SIZE] = '\0';
                            buffer[len-1] = 0; //Assuming last character is new line so replacing with 0
                            if(len < 0){
                                //Send error resp
                                snd_msg.subtype.response.type = SERVER_RESPONSE_FAILED;
                                sendResponse(fd,snd_msg);
                                break;
                            }
                            last_newline = strrchr(buffer,'\n'); // find last occurrence of newline
                            if(last_newline == NULL) {
                                last_newline = buffer;
                            } else {
                                last_newline++;
                            }
                            snd_msg.subtype.response.type = SERVER_RESPONSE_SUCCESS;
                            strcpy(snd_msg.subtype.response.info.buff,last_newline);
                            sendResponse(fd,snd_msg);
                    }
                        break;
                    case SERVER_REQUEST_WRITE: {
                            size_t len;
                            int fileid = rcv_msg.subtype.request.fileid;
                            printf("***** Received message is of type WRITE and writing text %s to file id %d\n",rcv_msg.subtype.request.buff,fileid);
                            snd_msg.type = SERVER_MESSAGE_TYPE_RESPONSE;
                            if(fileid < 0 || fileid > MAX_FILES || filehandlers[fileid] == NULL) {
                                
                                snd_msg.subtype.response.type = SERVER_RESPONSE_FAILED;
                                sendResponse(fd,snd_msg);
                                break;
                            }
                        
                            fseek(filehandlers[fileid], 0, SEEK_END);
                            len = fprintf(filehandlers[fileid],"%s\n", rcv_msg.subtype.request.buff);
                            if(len<0) {
                                snd_msg.subtype.response.type = SERVER_RESPONSE_FAILED;
                                sendResponse(fd,snd_msg);
                                break;
                            }else {
                                snd_msg.subtype.response.type = SERVER_RESPONSE_SUCCESS;
                                sendResponse(fd,snd_msg);
                            }
                    }
                        break;
                        
                    default:
                        printf("Server received invalid request, send invalid option reply");
                        snd_msg.subtype.response.type = SERVER_RESPONSE_INVALID;
                        sendResponse(fd,snd_msg);
                        break;
                }
                memset(&rcv_msg,0,sizeof(message_server));
                memset(&snd_msg,0,sizeof(message_server));
                break;
            default :
                printf("Server received response message, send invalid reply");
                break;
        }
    }
    
    
    
    return NULL;
    
}

int main(int argc, char **argv) {
    
    struct ifaddrs *ifaddr, *ifa;
    char ip[NI_MAXHOST];
    FILE *configfile;
    int listensockfd = -1,newfd = -1 , set = 1,port,currentconnections=0,i = 0;
    struct sockaddr_in addr,cliaddr;
    struct hostent *host;
    socklen_t len = sizeof (struct sockaddr_in);


    if(argc != 3) {
        printf("please provide arguments (serverID , PORT) \n");
        return 1;
    }
    myid = atoi(argv[1]);
    port = atoi(argv[2]);
    if (myid < 0 || myid >= SERVER_COUNT) {
        printf("server id not supported\n");
        return 1;
    }
    
    //listen for connections
    if (getifaddrs(&ifaddr) == -1) {
        perror("ERROR: getifaddrs\n");
        exit(1);
    }
    
    for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if((ifa->ifa_addr->sa_family == AF_INET) && strncmp(ifa->ifa_name, "lo", 2)) {
           int s = getnameinfo(ifa->ifa_addr,  sizeof(struct sockaddr_in) , ip, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            printf("IP address of this system is :%s\n", ip);
            break;
        }
    }
    freeifaddrs(ifaddr);
    if ((host=gethostbyname(ip)) == NULL)
    {
        perror("gethostbyname failed");
        exit(1);
    }
    configfile = fopen ( PATH_TO_SERVER_CONFIG, "a+" );
    if (!configfile) {
        perror( PATH_TO_SERVER_CONFIG);
        exit(1);
    }
    fprintf(configfile, "%d ", myid);
    fprintf(configfile, "%s ", ip);
    fprintf(configfile, "%d\n", port);

    fclose(configfile);
    
     global_file_info.count = MAX_FILES;
    for(i=0 ; i<MAX_FILES;i++) {
        char path[100];
        global_file_info.details[i].id = i;
        snprintf(global_file_info.details[i].name, 20, "file%d.txt", i);
        strncpy(path,PATH_TO_SHARED_FILES,100);
        strcat(path,global_file_info.details[i].name);
        filehandlers[i] = fopen(path, "a+");
        if(filehandlers[i] == NULL) {
            printf("File %s is failed to open",path);
            exit(1);
        }

    }
    
    if((listensockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("FAULT: socket create failed");
        exit(1);
    }
    if (setsockopt(listensockfd, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(int)) == -1) {
        perror("setsockopt() failed");
        exit(1);
    }
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr =  *((struct in_addr *)host->h_addr);
    memset(addr.sin_zero, '\0', sizeof (addr.sin_zero));
    
    if( bind(listensockfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind() failed");
        close(listensockfd);
        exit(1);
    }
    
    if (listen(listensockfd,MAX_NODE_BACKLOG) == -1) {
        perror("listen failed");
        close(listensockfd);
        exit(1);
    }

    while (currentconnections < CLIENT_NODE_COUNT) {
        
        if(( newfd =  accept(listensockfd, (struct sockaddr *)&cliaddr, &len)) == -1) {
            perror("accept failed");
        }
        
        printf("A client connected\n");
        pthread_create( &clientnode_threads[currentconnections++], NULL, &handleclient, (void *)(uintptr_t)newfd);     //Creating server thread

    }
    
    for( i=0;i<currentconnections;i++)
    {
        pthread_join( clientnode_threads[i], NULL);               //Join all  threads
    }
   
}


