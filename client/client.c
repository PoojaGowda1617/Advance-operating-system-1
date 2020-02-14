#include "client.h"

clientnodes clinodes[CLIENT_NODE_COUNT]; //To hold client nodes for communication
clientnodes currentnodeinfo; //self node information

//To handle multi-thread interaction
sync_data server_sync[SERVER_COUNT],clilistenstarted;
sync_data cli_req_sync[CLIENT_NODE_COUNT],clilistenstarted,requestqueue_lock,deferqueue_lock;

pthread_cond_t server_intn_done,client_intn_done,defer_wait[CLIENT_NODE_COUNT],process_thread_wait;

pthread_t server_threads[SERVER_COUNT]; //threads to interact with server
pthread_t clientnode_respond_threads[CLIENT_NODE_COUNT]; //threads on client received connection to process request
pthread_t clientnode_request_threads[CLIENT_NODE_COUNT]; //threads to establish client connection and to send requests
pthread_t listen_thread;//Thread to listen to incomming connections and switch then to their own thread for processing
pthread_t request_generator,proccess_thread; //Threads for creating random requests and to process the requests.


static int serverresponsecount = 0;
static int serverrespawaitingcount = 0;
static int clientreplycount = 0;
static int CS_request_in_progress=0;
static int CS_acquired = 0;
static int isrequesterthreadstarted = 0;

SERVER_REQUEST_RESPONSE requests[2] = {SERVER_REQUEST_READ,SERVER_REQUEST_WRITE}; //0 read operation, 1 write operation

pthread_mutex_t local_msg_lock;
pthread_mutex_t serverresponsemutex;
pthread_mutex_t client_reply_mutex;
pthread_mutex_t cs_acquire_inprogress;
pthread_mutex_t cs_acquired;
pthread_mutex_t requestgenthreadinit_lock;
pthread_mutex_t connections_lock;


message_server serverindividualmessages[SERVER_COUNT];

//***********************************Queue handling**************************************

QUEUE request_queue,defer_queue;

void initqueue(QUEUE *queue,QUEUE_TYPE type);
QUEUE_CODE insert(QUEUE *queue,QUEUE_TYPE type,void *data);
QUEUE_CODE delete(QUEUE *source_queue,QUEUE_TYPE type,void *dest_buffer);
QUEUE_CODE fetchdata(QUEUE *source_queue,QUEUE_TYPE type,void *dest_buffer,int index);
void *processthread(void *param);

//***************************************************************************************

/**
 * Function to parse the server and client config files to get connection details
 */

void closeallconnections() {
    
    int i;
    pthread_mutex_lock(&connections_lock);
    printf("Closing all the connections\n\n");
    for(i = 0; i < SERVER_COUNT;i++) {
        if(servers[i].socketfd != -1) {
            printf("server %d connection closed\n\n",i);
            close(servers[i].socketfd);
            servers[i].socketfd = -1;
        }
    }
    
    for(i = 0; i < CLIENT_NODE_COUNT;i++) {
        if(clinodes[i].socketfd != -1) {
            printf("client %d connection closed\n\n",i);
            close(clinodes[i].socketfd);
            clinodes[i].socketfd = -1;
        }
    }
    pthread_mutex_unlock(&connections_lock);
    exit(1);
}

int parseConfigFiles(int myid) {
    
    
    FILE *file;
    int i = 0;
    static int repeat = 0;

   do {
       file = fopen(PATH_TO_CLIENT_CONFIG, "r");
       
       if(file==NULL) {
           printf("Error: can't open client config file\n\n");
           return -1;
       } else {
           printf("client config file opened successfully\n\n");
       }

        for(i = 0;i<CLIENT_NODE_COUNT;i++)
        {
            int nodeid;
            if(fscanf(file,"%d",&nodeid) == EOF) //Reading host info from config file
            {
                printf("All clients are not running will retry after 2 seccs\n");
                sleep(2);
                break;

            }
            if(nodeid >= CLIENT_NODE_COUNT) {
                printf("Error: Client Id not supported\n\n");
                exit(0);
            }
            if(nodeid == myid) {
                if(repeat == 1) {
                    printf("Error: Please clear the client config file\n\n");
                    exit(1);
                }
                repeat++;
                currentnodeinfo.clientid = nodeid;
                fscanf(file,"%s",currentnodeinfo.ip);
                fscanf(file,"%d",&currentnodeinfo.port);
                currentnodeinfo.socketfd = -1;
                clinodes[nodeid].socketfd = -1;
                continue;
            }
            clinodes[nodeid].clientid = nodeid;
            fscanf(file,"%s",clinodes[nodeid].ip);
            fscanf(file,"%d",&clinodes[nodeid].port);
            clinodes[nodeid].socketfd = -1;
        }
    repeat = 0;
    fclose(file);
   }while(i != CLIENT_NODE_COUNT);
    
    printf("My node info id %d ip %s port %d\n",currentnodeinfo.clientid,currentnodeinfo.ip,currentnodeinfo.port);
    
    printf("====================================CLIENT NODES================================================\n\n");
    for(i = 0 ; i < CLIENT_NODE_COUNT ; i++)
	if( i != myid ) {
	     printf("id %d ip %s and port %d\n",clinodes[i].clientid,clinodes[i].ip,clinodes[i].port);
	}
    printf("====================================================================================\n\n");

    file = NULL;
    file = fopen(PATH_TO_SERVER_CONFIG, "r");

    if(file==NULL) {
        printf("Error: can't open server config file\n");
        return -1;
    } else {
        printf("server config file opened successfully\n");
    }
    
    for(i=0;i<SERVER_COUNT;i++)
    {
        fscanf(file,"%d",&servers[i].serverid);//Reading server info from config file
        fscanf(file,"%s",servers[i].ip);
        fscanf(file,"%d",&servers[i].port);
        servers[i].socketfd = -1;
    }
    
    printf("====================================SERVERS================================================\n\n");
    
    for(i = 0 ; i < SERVER_COUNT ; i++)
        printf("id %d ip %s and port %d\n",servers[i].serverid,servers[i].ip,servers[i].port);
    
    printf("====================================================================================\n\n");
    return 0;
}


void *server_thread(void *serverid) {
    
    int id = (uintptr_t)serverid;
    servernodes *serverinfo = &servers[id];
    struct sockaddr_in server_addr;
    struct hostent *host;
    message_server server_msg = {0};
    message_server server_resp = {0};

    printf("Establishing connection to server id %d, ip %s and port %d\n",serverinfo->serverid,serverinfo->ip,serverinfo->port);
    
    if ((serverinfo->socketfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("FAULT: Server socket create failed");
        exit(1);
    }
    if ((host=gethostbyname(serverinfo->ip)) == NULL)
    {
        perror("gethostbyname");
        exit(1);
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(serverinfo->port);
    server_addr.sin_addr = *((struct in_addr *)host->h_addr);
    memset(&(server_addr.sin_zero), '\0', 8);
    
    if (connect(serverinfo->socketfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1)
    {
        perror("FAULT: Server socket connect failed");
        exit(1);
    }
    
    //get the meta data from server(Meta data is assumed to be not in critical section as all files are consistent initially and no client modifies meta data)
    if(id == 0) { //As initially files are consistent read from only one server.
        
        server_msg.id = currentnodeinfo.clientid;
        server_msg.type = SERVER_MESSAGE_TYPE_REQUEST;
        server_msg.subtype.request.type = SERVER_REQUEST_ENQUIRY;
        printf("=====> Sending Enquiry message to server message \n\n");
        send(serverinfo->socketfd,&server_msg,sizeof(message_server),0); //sending enquiry message
        int ret = recv(serverinfo->socketfd,(void *)&server_resp,sizeof(message_server),MSG_WAITALL);

        if(ret <= 0) {
            printf("server enquiry failed\n\n");
            closeallconnections();
        }
        
        printf("<======= Received Enquiry response from server\n\n");
        if(server_resp.type == 1 && server_resp.subtype.response.type == 4) {
            global_file_info =  server_resp.subtype.response.info.finfo;
            printf("Total Files %d in server\n",global_file_info.count);
        } else {
            printf("Enquiry response error\n\n");
            closeallconnections();
        }
        
        //start requestor thread
        pthread_mutex_lock(&requestgenthreadinit_lock);
        if(isrequesterthreadstarted == 0) {
            printf("Request generation thread started\n");
            pthread_create( &proccess_thread, NULL, &processthread, NULL);     //process thread
            pthread_create( &request_generator, NULL, &request_generator_function, NULL);     //request generating thread
            
        }
        isrequesterthreadstarted = 1;
        pthread_mutex_unlock(&requestgenthreadinit_lock);
    }
   
    while(1) {
        pthread_mutex_lock(&server_sync[id].lock);
        /* wait on allocated condition for signal to process the server request, Based on request type(read ,write) corresponding server threads be signaled*/
        pthread_cond_wait(&server_sync[id].wait,&server_sync[id].lock);
        pthread_mutex_unlock(&server_sync[id].lock);
        server_msg = serverindividualmessages[id]; //Read the request from the server specific slot and send req
        send(serverinfo->socketfd,&server_msg,sizeof(message_server),0); //sending message
        int ret = recv(serverinfo->socketfd,(void *)&server_resp,sizeof(message_server),MSG_WAITALL);
        if(ret <= 0) {
            printf("server request failed for option %d\n",server_msg.subtype.request.type );
            closeallconnections();
        } else {
            if(server_resp.subtype.response.type == SERVER_RESPONSE_FAILED) {
                printf("Request to server %d Operation failed\n",id);
            } else {
                if(server_msg.subtype.request.type == SERVER_REQUEST_WRITE) {
                    printf("Write Operation to server %d Success\n",id);
                } else if(server_msg.subtype.request.type == SERVER_REQUEST_READ) {
                    printf("Read operation is success on server %d with data %s\n",id,server_resp.subtype.response.info.buff);
                }
		sleep(2);
            }
        }
        pthread_mutex_lock(&serverresponsemutex);
        serverresponsecount++; //Keep track of how many servers have responded.
        if(serverresponsecount == serverrespawaitingcount) {
            //Once we receive expected number of responces, siganal to the waiting party.
            pthread_cond_signal(&server_intn_done);
        }
        pthread_mutex_unlock(&serverresponsemutex);

    }
    exit(0);
}

void sendreply(int fd,int client_id) {
    
    printf("====> Sending reply to client %d\n\n",client_id);
    client_message snd_msg;
    snd_msg.messagetype = CLIENT_MESSAGE_TYPE_REPLY;
    snd_msg.clientid = currentnodeinfo.clientid;
    send(fd,&snd_msg,sizeof(client_message),0);
}

void *handleclinodeincomingreq(void *filedes) {
    
    int fd = (uintptr_t)filedes;
    client_message rcv_msg;
    
    while(1) {
        memset(&rcv_msg,0,sizeof(client_message));
        int ret = recv(fd,(void *)&rcv_msg,sizeof(client_message),MSG_WAITALL); //wait for requests
        if(ret <= 0) {
            close(fd);
            return NULL;
        }
        pthread_mutex_lock(&requestqueue_lock.lock); //lock request queue to stop further request generation. //TOODO
        printf("<======== Received request from client node %d for file %d with origination time %jd\n",rcv_msg.clientid,rcv_msg.fileid,(uintmax_t)rcv_msg.timestamp);
        
        //Ricart-Agrawala Algorithm starts
        if(rcv_msg.messagetype == CLIENT_MESSAGE_TYPE_REQUEST) {
            int cs_requested = 0, in_cs = 0;
            pthread_mutex_lock(&cs_acquire_inprogress);
            pthread_mutex_lock(&cs_acquired);
            cs_requested = CS_request_in_progress;
            in_cs = CS_acquired ;
            pthread_mutex_lock(&local_msg_lock);

            //In CS but request  received is for another file, So give permission: TO SUPPORT PARALLEL ACCESS
            if(in_cs && g_local_msg.file_id == rcv_msg.fileid) {
                printf("***** Already in CS so defering the reply *****\n");
                pthread_mutex_lock(&deferqueue_lock.lock);
                insert(&defer_queue,QUEUE_TYPE_DEFER,&rcv_msg);
                pthread_mutex_unlock(&requestqueue_lock.lock);
                pthread_mutex_unlock(&cs_acquire_inprogress);
                pthread_mutex_unlock(&cs_acquired);
                pthread_mutex_unlock(&local_msg_lock);
                pthread_cond_wait(&defer_wait[rcv_msg.clientid],&deferqueue_lock.lock);
                pthread_mutex_unlock(&deferqueue_lock.lock);
                sendreply(fd,rcv_msg.clientid);
                continue;

            } else if(cs_requested && g_local_msg.file_id == rcv_msg.fileid) {
                //break the tie;
                printf("Requested to enter CS so check for priority\n\n");
                if(g_local_msg.timestamp > rcv_msg.timestamp) {
                    //They won
                    printf("======> Received time stamp which is earlier than my request, so sending reply\n\n");
                    sendreply(fd,rcv_msg.clientid);
                } else if(g_local_msg.timestamp == rcv_msg.timestamp && rcv_msg.clientid < currentnodeinfo.clientid) {
                    //They won
                    printf("=======> requests generated at same time but the requested client node id smaller ,so it has higher priority.. Sending reply\n\n");
                    sendreply(fd,rcv_msg.clientid);
                } else {
                    //We won defer the reply
                    printf("***** Delaying reply as my request has higher priority *****\n ");
                    pthread_mutex_lock(&deferqueue_lock.lock);
                    insert(&defer_queue,QUEUE_TYPE_DEFER,&rcv_msg);
                    pthread_mutex_unlock(&requestqueue_lock.lock);
                    pthread_mutex_unlock(&cs_acquire_inprogress);
                    pthread_mutex_unlock(&cs_acquired);
                    pthread_mutex_unlock(&local_msg_lock);
                    pthread_cond_wait(&defer_wait[rcv_msg.clientid],&deferqueue_lock.lock);
                    pthread_mutex_unlock(&deferqueue_lock.lock);
                    sendreply(fd,rcv_msg.clientid);
                    continue;
                }
            } else {
                printf("Sending reply, as I am not requesting the critical section\n\n ");
                sendreply(fd,rcv_msg.clientid);
            }
            pthread_mutex_unlock(&cs_acquire_inprogress);
            pthread_mutex_unlock(&cs_acquired);
            pthread_mutex_unlock(&requestqueue_lock.lock);
            pthread_mutex_unlock(&local_msg_lock);
        } else {
            printf("Error: Expected Request mismatched.. Terminating\n");
            closeallconnections();
            return NULL;
        }
        //Ricart-Agrawala Algorithm ends
    }
}

/**
 * Function to send the intended request to all the client nodes
 */
void *handleclinodeoriginatereq(void *clientid) {
    
    int id = (uintptr_t)clientid;
    struct sockaddr_in cli_addr;
    struct hostent *host;
    client_message snd_msg = {0};
    client_message rcv_msg = {0};
    clientnodes *node = &clinodes[id];
    
    printf("=====> Establishing connection to client node id %d, ip %s and port %d\n",node->clientid,node->ip,node->port);
    
    if ((node->socketfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("FAULT: Server socket create failed\n");
        exit(1);
    }
    if ((host=gethostbyname(node->ip)) == NULL)
    {
        perror("gethostbyname\n");
        exit(1);
    }
    
    cli_addr.sin_family = AF_INET;
    cli_addr.sin_port = htons(node->port);
    cli_addr.sin_addr = *((struct in_addr *)host->h_addr);
    memset(&(cli_addr.sin_zero), '\0', 8);
    
    if (connect(node->socketfd, (struct sockaddr *)&cli_addr, sizeof(struct sockaddr)) == -1) {
        perror("FAULT: client node socket connect failed");
        closeallconnections();
    }
    pthread_mutex_init(&cli_req_sync[id].lock,NULL);
    pthread_cond_init(&cli_req_sync[id].wait,NULL);
    while(1) {
        pthread_mutex_lock(&cli_req_sync[id].lock);
        //Wait untill request is generated and signaled
        pthread_cond_wait(&cli_req_sync[id].wait,&cli_req_sync[id].lock);
        pthread_mutex_unlock(&cli_req_sync[id].lock);
        pthread_mutex_lock(&local_msg_lock);
        snd_msg.clientid = currentnodeinfo.clientid;
        snd_msg.timestamp = g_local_msg.timestamp;
        snd_msg.fileid = g_local_msg.file_id;
        snd_msg.messagetype = CLIENT_MESSAGE_TYPE_REQUEST;
        pthread_mutex_unlock(&local_msg_lock);
        printf("====> sending Request message From client id %d  with timestamp: %jd to client node %d, access to file %d\n",snd_msg.clientid,(uintmax_t)snd_msg.timestamp,id,snd_msg.fileid);
        send(node->socketfd,&snd_msg,sizeof(client_message),0); 
        int ret = recv(node->socketfd,(void *)&rcv_msg,sizeof(client_message),MSG_WAITALL);
        printf("<====== received response from client node %d\n",rcv_msg.clientid);
        if(ret <= 0) {
            closeallconnections();
        }
        if(rcv_msg.messagetype == CLIENT_MESSAGE_TYPE_REPLY) {
            pthread_mutex_lock(&client_reply_mutex);
            clientreplycount++;
            
            if(clientreplycount == (CLIENT_NODE_COUNT - 1)) {
                //Received all requests.
                printf("As we have received all the client replies enter critical section\n");
                pthread_cond_signal(&client_intn_done);
            }
            pthread_mutex_unlock(&client_reply_mutex);
            
        } else {
            perror("ERROR: UNINENDED MESSAGE\n");
            closeallconnections();
        }
    }
    return NULL;
}



void *listenthread(void *id) {
    
    int listensockfd = -1,newfd = -1 , set = 1,port,currentconnections=0,i = 0;
    socklen_t len = sizeof (struct sockaddr_in);
    struct hostent *host;
    struct sockaddr_in addr,cliaddr;


    printf("listen thread started\n");
    if((listensockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("FAULT: socket create failed\n");
        exit(1);
    }
    
    if (setsockopt(listensockfd, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(int)) == -1) {
        perror("setsockopt() failed\n");
        exit(1);
    }
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(currentnodeinfo.port);
    if ((host=gethostbyname(currentnodeinfo.ip)) == NULL)
    {
        perror("gethostbyname failed in listen\n");
        exit(1);
    }
    addr.sin_addr =  *((struct in_addr *)host->h_addr);
    memset(addr.sin_zero, '\0', sizeof (addr.sin_zero));
    
    if( bind(listensockfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind() failed\n");
        close(listensockfd);
        exit(1);
    }
    
    if (listen(listensockfd,MAX_NODE_BACKLOG) == -1) {
        perror("listen failed\n");
        close(listensockfd);
        exit(1);
    }

    pthread_mutex_lock(&clilistenstarted.lock);
    pthread_cond_signal(&clilistenstarted.wait);
    pthread_mutex_unlock(&clilistenstarted.lock);
    
    while (currentconnections < (CLIENT_NODE_COUNT - 1)) {
        printf("****** ready to accept the connections *****\n");
        if(( newfd =  accept(listensockfd, (struct sockaddr *)&cliaddr, &len)) == -1) {
            perror("accept failed\n");
        }
        pthread_create( &clientnode_respond_threads[currentconnections++], NULL, &handleclinodeincomingreq, (void *)(uintptr_t)newfd);   //Thread creation for the incomming connection.
    }
    
    for(i=0;i<(CLIENT_NODE_COUNT - 1);i++) {
        pthread_join( clientnode_respond_threads[i], NULL);               //Join all client node threads
    }
    return NULL;
}

void *processthread(void *param) {
    
    int i =0,any_req_is_pending = 0;
    while(1) {
        pthread_mutex_lock(&requestqueue_lock.lock);
        if(any_req_is_pending == 0) {
            pthread_cond_wait(&process_thread_wait,&requestqueue_lock.lock);
        }
        pthread_mutex_lock(&cs_acquire_inprogress);
        CS_request_in_progress = 1;      //Intention to take CS i.e sending the request but still not acquired the CS
        pthread_mutex_unlock(&cs_acquire_inprogress);
        
        struct local_msg reqmsg;
        pthread_mutex_lock(&local_msg_lock);
        QUEUE_CODE res = fetchdata(&request_queue,QUEUE_TYPE_REQUEST,&reqmsg,0);
        memcpy(&g_local_msg,&reqmsg,sizeof(struct local_msg));
        pthread_mutex_unlock(&local_msg_lock);
        pthread_mutex_unlock(&requestqueue_lock.lock);
        
        if(res != QUEUE_CODE_SUCCESS) {
            pthread_mutex_lock(&cs_acquire_inprogress);
            CS_request_in_progress = 0;
            pthread_mutex_unlock(&cs_acquire_inprogress);
            continue;
        }
        
        for(i = 0 ; i < CLIENT_NODE_COUNT ; i ++) {
            if(i == currentnodeinfo.clientid) {
                continue;
            }
            pthread_mutex_lock(&cli_req_sync[i].lock);
            pthread_cond_signal(&cli_req_sync[i].wait);
            pthread_mutex_unlock(&cli_req_sync[i].lock);
        }
        if(CLIENT_NODE_COUNT > 1) {
            printf("Waiting for client replies to enter CS\n");
            pthread_mutex_lock(&client_reply_mutex);
            pthread_cond_wait(&client_intn_done,&client_reply_mutex);
            clientreplycount = 0;
            pthread_mutex_unlock(&client_reply_mutex);
        }
        
        pthread_mutex_lock(&cs_acquire_inprogress);
        pthread_mutex_lock(&cs_acquired);
        CS_acquired = 1; //Now acquired the CS. So block the replies and use server to perform operations
        CS_request_in_progress = 0;
        pthread_mutex_unlock(&cs_acquired);
        pthread_mutex_unlock(&cs_acquire_inprogress);
        printf("Client entered critical section and sending the requests to Server(s)\n");
        
        for( ; ;) {
            int cancontinuecsuse = 0,clear_defer = 0;
            
            pthread_mutex_lock(&serverresponsemutex);
            serverresponsecount = 0;
            serverrespawaitingcount = 0;
            if(reqmsg.operation == SERVER_REQUEST_READ) { //request only one server for read OP
                
                message_server *msg = &serverindividualmessages[reqmsg.server_id];
                msg->id = currentnodeinfo.clientid;
                msg->type = SERVER_MESSAGE_TYPE_REQUEST;
                msg->subtype.request.type = SERVER_REQUEST_READ;
                msg->subtype.request.fileid = reqmsg.file_id;
                
                //signal the server thread
                pthread_mutex_lock(&server_sync[reqmsg.server_id].lock);
                pthread_cond_signal(&server_sync[reqmsg.server_id].wait);
                pthread_mutex_unlock(&server_sync[reqmsg.server_id].lock);
                serverrespawaitingcount = 1;
            } else { //Request all servers for write operation  to keep files consistent
                serverrespawaitingcount = 0;
                for ( i = 0;i < SERVER_COUNT ; i++) {
                    message_server *msg = &serverindividualmessages[i];
                    msg->id = currentnodeinfo.clientid;
                    msg->type = SERVER_MESSAGE_TYPE_REQUEST;
                    msg->subtype.request.type = SERVER_REQUEST_WRITE;
                    msg->subtype.request.fileid = reqmsg.file_id;
                    memset(msg->subtype.request.buff,0,200);
                    snprintf(msg->subtype.request.buff,200,"%d %jd",msg->id,(uintmax_t)reqmsg.timestamp);
                    pthread_mutex_lock(&server_sync[i].lock);
                    pthread_cond_signal(&server_sync[i].wait);
                    pthread_mutex_unlock(&server_sync[i].lock);
                    ++serverrespawaitingcount; //Number of servers we are requesting. Response should match this count
                }
            }
            
            printf("Waiting for server Responces to be finished\n");
            pthread_cond_wait(&server_intn_done,&serverresponsemutex);
            printf("Server Interaction completed\n");
            pthread_mutex_unlock(&serverresponsemutex);
            pthread_mutex_lock(&requestqueue_lock.lock);
            
            QUEUE_CODE ret = delete(&request_queue,QUEUE_TYPE_REQUEST,NULL); //delete the currrent req from queue
            if(ret == QUEUE_CODE_EMPTH) {
                printf("We dont have any other request in queue, no need to hold CS\n");
                cancontinuecsuse = 0; //No other requests in the req queue, So diretly release the CS.
                clear_defer = 1;
                any_req_is_pending = 0;
            } else {
                printf("We have already another request in queue, lets check to reuse CS\n");
                ret = fetchdata(&request_queue,QUEUE_TYPE_REQUEST,&reqmsg,0);
                any_req_is_pending = 1;
                 printf("next request in queue is file id %d, time stamp %jd\n",reqmsg.file_id,(uintmax_t)reqmsg.timestamp);
                pthread_mutex_lock(&local_msg_lock);
                memcpy(&g_local_msg,&reqmsg,sizeof(struct local_msg));
                pthread_mutex_unlock(&local_msg_lock);
                if(ret != QUEUE_CODE_SUCCESS) {
                    printf("as req is failed successfully mark CSUSE 0\n");
                    cancontinuecsuse = 0;
                } else {
                    printf("as req is successfully mark CSUSE 1\n");
                    cancontinuecsuse = 1;
                }
            }
            
            pthread_cond_signal(&requestqueue_lock.wait);
            
            pthread_mutex_lock(&deferqueue_lock.lock);

                int i = 0;
                for (;;) { //As defer queue is a priority queue we will only chek untill 1st lower priority msg
                    client_message climsg;
                    QUEUE_CODE ret;
                    ret = fetchdata(&defer_queue,QUEUE_TYPE_DEFER,&climsg,i);
                    if(ret == QUEUE_CODE_UNDERFLOW) { //No defer messages, So we can continue using CS
                        printf("Defer queue underflow break it\n");
                        break;
                    }
                    printf("next defer msg in queue is file id %d, time stamp %jd\n",climsg.fileid,(uintmax_t)climsg.timestamp);
                    if(clear_defer == 1 || ((reqmsg.timestamp > climsg.timestamp) || (reqmsg.timestamp == climsg.timestamp && currentnodeinfo.clientid > climsg.clientid))) {
                        //Others won the CS, Lets release and send send defered messages
                        cancontinuecsuse = 0;
                        delete(&defer_queue,QUEUE_TYPE_DEFER,&climsg);
                        pthread_cond_signal(&defer_wait[climsg.clientid]);//signal only priority client node
                        printf("client %d defer req has high priority so sending reply msg\n",climsg.clientid);
                    } else {
                        printf("At index %d we have priority so ignore\n",i);
                        break;
                    }
                }
            
            if(cancontinuecsuse == 1) {
                printf("Reusing the CS\n");
                 pthread_mutex_unlock(&deferqueue_lock.lock);
                 pthread_mutex_unlock(&requestqueue_lock.lock);
                 continue;
            } else {
                printf("Relesing CS and sending defer messages if any\n");
                pthread_mutex_lock(&cs_acquired);
                CS_acquired = 0;
                pthread_mutex_unlock(&cs_acquired);
                printf("Cleared CS\n");
            }
            //DUMMY CHANGES ENDS
            
            pthread_mutex_unlock(&deferqueue_lock.lock);
            pthread_mutex_unlock(&requestqueue_lock.lock);
            break;
        }
    }
}

void *request_generator_function(void *param) {
    
    printf("Requestor thread Started\n");
    while(1) {
        unsigned int sleeptime = ((rand() %10000000) + 100); //sleep for 100ms to 10S
        usleep(sleeptime); //Random
        pthread_mutex_lock(&requestqueue_lock.lock); //Try to take lock here. If we already received request from other nodes don't process our request as we have race condition here.
        SERVER_REQUEST_RESPONSE operation = requests[(rand() % 2)];
        int server = rand() % SERVER_COUNT; //Select random server
        int fileid = global_file_info.details[rand() % global_file_info.count].id; //Random file id
        time_t local_time = time(NULL);
        printf("Request generated for operation %s on server %d for file id %d at time %jd\n",operation==SERVER_REQUEST_READ ? "READ": "WRITE",server,fileid,local_time);
    
        struct local_msg msg;
        msg.server_id = server;
        msg.file_id = fileid;
        msg.timestamp = local_time;
        msg.operation = operation;
        
        QUEUE_CODE ret = insert(&request_queue,QUEUE_TYPE_REQUEST,&msg); //insert into queue
       //Signal process thread
        printf("generated request is inserted into the queue\n");
        if(ret == QUEUE_CODE_FULL) {
            printf("Request queue is full wait for empty slot");
            pthread_cond_wait(&requestqueue_lock.wait,&requestqueue_lock.lock);
        }
        printf("Signaling process thread\n");
        pthread_cond_signal(&process_thread_wait);
        printf("Request gen thread releasing the req queue lock\n");
        pthread_mutex_unlock(&requestqueue_lock.lock);
    }
    return NULL;
}

int main(int argc, char **argv) {
    
    struct ifaddrs *ifaddr, *ifa;
    char ip[NI_MAXHOST];
    FILE *configfile;
    int myid=-1,port=-1,i = 0;
    char input = 'N';
    struct hostent *host;
    
    if(argc != 3) {
        printf("please provide arguments (clientID, PORT)\n");
        return 1;
    }
    myid = atoi(argv[1]);
    port = atoi(argv[2]);
                    
    if (myid < 0 || myid >= CLIENT_NODE_COUNT) {
        printf("client id not supported\n");
        return 1;
    }
    
    srand(time(0) + getpid()); //seed current time + pid
    
    pthread_mutex_init(&serverresponsemutex,NULL);
    pthread_mutex_init(&local_msg_lock,NULL);
    pthread_mutex_init(&client_reply_mutex,NULL);
    pthread_mutex_init(&cs_acquire_inprogress,NULL);
    pthread_mutex_init(&cs_acquired,NULL);
    pthread_mutex_init(&clilistenstarted.lock,NULL);
    pthread_mutex_init(&requestgenthreadinit_lock,NULL);
    pthread_mutex_init(&connections_lock,NULL);
    pthread_mutex_init(&requestqueue_lock.lock,NULL);
    pthread_mutex_init(&deferqueue_lock.lock,NULL);


    pthread_cond_init(&server_intn_done,NULL);
    pthread_cond_init(&clilistenstarted.wait,NULL);
    pthread_cond_init(&client_intn_done,NULL);
    pthread_cond_init(&requestqueue_lock.wait,NULL);
    pthread_cond_init(&deferqueue_lock.wait,NULL);
    pthread_cond_init(&process_thread_wait,NULL);
    
    for(i = 0 ; i < CLIENT_NODE_COUNT; i++)
    {
        pthread_cond_init(&defer_wait[i],NULL);
    }

    if (getifaddrs(&ifaddr) == -1) {
        perror("ERROR: getifaddrs\n");
        exit(1);
    }
    for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if((ifa->ifa_addr->sa_family == AF_INET) && strncmp(ifa->ifa_name, "lo", 2)) {
            int s = getnameinfo(ifa->ifa_addr,  sizeof(struct sockaddr_in) , ip, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            printf("IP address is :%s\n", ip);
            break;
        }
    }
    freeifaddrs(ifaddr);
    if ((host=gethostbyname(ip)) == NULL)
    {
        perror("gethostbyname failed\n");
        exit(1);
    }
    
    strncpy(currentnodeinfo.ip,ip,25);
    currentnodeinfo.port = port;
    currentnodeinfo.clientid = myid;
    
    configfile = fopen ( PATH_TO_CLIENT_CONFIG, "a+" );
    if (!configfile) {
        perror( PATH_TO_SERVER_CONFIG);
        exit(1);
    }
    
    fprintf(configfile, "%d ", myid);
    fprintf(configfile, "%s ", ip);
    fprintf(configfile, "%d\n", port);
    
    fclose(configfile);
    
    pthread_mutex_lock(&clilistenstarted.lock);
    pthread_create( &listen_thread, NULL, &listenthread,NULL);     //Creating listen thread
    
    pthread_cond_wait(&clilistenstarted.wait,&clilistenstarted.lock);
    pthread_mutex_unlock(&clilistenstarted.lock);
    
    initqueue(&request_queue,QUEUE_TYPE_REQUEST);
    initqueue(&defer_queue,QUEUE_TYPE_DEFER);
    
    if(parseConfigFiles(myid) < 0) {
        printf("Error in config parse.. abort\n");
        exit(1);
    }

    
    // Establish connections with all servers.
    for(i = 0 ; i < SERVER_COUNT ; i++) {
        printf("Server %d thread started\n",i);
        pthread_mutex_init(&(server_sync[i].lock),NULL);
        pthread_cond_init(&(server_sync[i].wait),NULL);
        pthread_create( &server_threads[i], NULL, &server_thread, (void *)(uintptr_t)i);     //Creating server threads
    }

    for(i = 0 ; i < CLIENT_NODE_COUNT ; i++) {
        if(i == myid) {
            continue;
        }
        printf("client %d thread started\n",i);
        pthread_create( &clientnode_request_threads[i], NULL, &handleclinodeoriginatereq, (void *)(uintptr_t)i);//Init connections with other clients
    }

    for(i=0;i<5;i++) {
        pthread_join( server_threads[i], NULL);               //Join all server threads
    }

    pthread_join(listen_thread, NULL);               //Join Listen thread

    for(i=0;i<CLIENT_NODE_COUNT;i++) {
        pthread_join(clientnode_request_threads[i], NULL);               //Join all client node threads
    }

    pthread_join(request_generator,NULL);  //Join request gen thread
    pthread_join(proccess_thread,NULL);  //Join request gen thread
}


//************************************ QUEUE HANDLING *********************************

void initqueue(QUEUE *queue,QUEUE_TYPE type)
{
    memset(queue,0,sizeof(QUEUE));
    queue->type = type;
    queue->front = 0;
    queue->rear = 0;
    queue->count = 0;
}

void destroyqueue(QUEUE *queue)
{
    
}

QUEUE_CODE insert(QUEUE *queue,QUEUE_TYPE type,void *data)
{
    int i = 0,j = 0;
    if(((queue->rear + 1)%MAX_MESSAGE_QUEUE_SIZE)  == queue->front) {
        printf("inserting Operation failed OVERFLOW\n");
        return QUEUE_CODE_OVERFLOW;
    }
    client_message actualdata = *(client_message *)data;
    if(type == QUEUE_TYPE_REQUEST) {
        queue->queue_data.local_msg_queue[queue->rear] = *( struct local_msg *)data;
        queue->rear = (queue->rear + 1)%MAX_MESSAGE_QUEUE_SIZE;
    } else if(type == QUEUE_TYPE_DEFER) {
        //Priority defer queue.
        if(queue->front != -1) {
            for( i = queue->front; i != queue->rear;) {
                client_message msg = (client_message)(queue->queue_data.client_msg_queue[i]);
                if((actualdata.timestamp < msg.timestamp) || (actualdata.timestamp == msg.timestamp && (actualdata.clientid < msg.clientid))) { //New msg have highest priority so place it before
                    queue->queue_data.client_msg_queue[i] = actualdata;
                    actualdata = msg;
                    for (j = (i+1)%MAX_MESSAGE_QUEUE_SIZE; j != queue->rear;) {
                        msg = (client_message)(queue->queue_data.client_msg_queue[j]);
                        queue->queue_data.client_msg_queue[j] = actualdata;
                        actualdata = msg;
                        j = (j+1)%MAX_MESSAGE_QUEUE_SIZE;

                    }
                    break;
                }
                i = (i+1)%MAX_MESSAGE_QUEUE_SIZE;
            }
        }
        queue->queue_data.client_msg_queue[queue->rear] = actualdata;
        queue->rear = (queue->rear + 1)%MAX_MESSAGE_QUEUE_SIZE;
    } else {
        return QUEUE_CODE_INVALID;
    }
    
    if(queue->front  == -1) {
        queue->front  = 0;
    }
    
    if(queue->front  == (queue->rear + 1) % MAX_MESSAGE_QUEUE_SIZE) {
        return QUEUE_CODE_FULL;
    }
    return QUEUE_CODE_SUCCESS;

}

QUEUE_CODE delete(QUEUE *source_queue,QUEUE_TYPE type,void *dest_buffer)
{

    if(source_queue->front == -1 || source_queue->front == source_queue->rear) {
        printf("Delete operation under flow front %d rear %d\n",source_queue->front,source_queue->rear);
        return QUEUE_CODE_UNDERFLOW;
    }
    
    if(dest_buffer == NULL) {
        source_queue->front = (source_queue->front + 1)%MAX_MESSAGE_QUEUE_SIZE;
    } else {
        if(type == QUEUE_TYPE_REQUEST) {
            memcpy(dest_buffer,&source_queue->queue_data.local_msg_queue[source_queue->front],sizeof(struct local_msg));
            source_queue->front = (source_queue->front + 1)%MAX_MESSAGE_QUEUE_SIZE;
        } else if(type == QUEUE_TYPE_DEFER) {
            memcpy(dest_buffer,&source_queue->queue_data.client_msg_queue[source_queue->front],sizeof(client_message));
            source_queue->front = (source_queue->front + 1)%MAX_MESSAGE_QUEUE_SIZE;
        } else {
            return QUEUE_CODE_INVALID;
        }
    }
    
    if(source_queue->front == source_queue->rear) {
        source_queue->front = -1;
        source_queue->rear = 0;
        return QUEUE_CODE_EMPTH;
    }
    return QUEUE_CODE_SUCCESS;
}

QUEUE_CODE fetchdata(QUEUE *source_queue,QUEUE_TYPE type,void *dest_buffer,int index) {
    
    if(source_queue->front == -1 || ((source_queue->front + index) == source_queue->rear)) {
        printf("Fetch data underflow\n");
        return QUEUE_CODE_UNDERFLOW;
    }
    
    if(type == QUEUE_TYPE_REQUEST) {
        memcpy(dest_buffer,&source_queue->queue_data.local_msg_queue[source_queue->front + index],sizeof(struct local_msg));
    } else if(type == QUEUE_TYPE_DEFER) {
        
        memcpy(dest_buffer,&source_queue->queue_data.client_msg_queue[source_queue->front + index],sizeof(client_message));
    } else {
        printf("Fetch data invalid\n");
        return QUEUE_CODE_INVALID;
    }
     return QUEUE_CODE_SUCCESS;}

//*************************************************************************************
