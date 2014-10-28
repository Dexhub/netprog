#include "server.h"

static int in_port_no, in_window_size;
static ClientRequest *Head = NULL;

static void sig_child(int signo) { 
    // Remove child entry from the linkedlist 
    ClientRequest *cur = Head;
    ClientRequest *prev = NULL;
    pid_t pid = wait(NULL);
    
    while (cur != NULL) {
        if (cur->childpid == pid) {
            // remove this entry
            if (Head == cur)
                Head = cur->next;
            else 
                prev = cur->next;

            printf(KRED "\nChild deleted" RESET "Pid: %d\n", pid);
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    printf("ERROR!! Unable to find a record for child with pid:%d \n",pid);
    return;
}

static int initializeParams() {
    char data[MAXLINE];
    FILE *inp_file = fopen(SERVER_IN, "r");

    if (inp_file == NULL) {
        err_quit("Unknown server argument file: '%s'\n", SERVER_IN);
    }
    in_port_no = getIntParamValue(inp_file);
    in_window_size = getIntParamValue(inp_file);
    Fclose(inp_file);
}

static int* bindAllInterfaces(struct ifi_info *ifihead, int totalIP) {
    struct ifi_info *ifi;
    struct sockaddr_in servaddr;
    int *sockfd;
    int i;

    sockfd = (int *) Malloc(totalIP * sizeof(int));

    for (i = 0, ifi = ifihead; ifi != NULL; ifi = ifi->ifi_next) {
        sockfd[i] = Socket(AF_INET, SOCK_DGRAM, 0);
        bzero(&servaddr, sizeof(servaddr));
        servaddr = *(struct sockaddr_in *)ifi->ifi_addr;
        servaddr.sin_port = htons(in_port_no);
        Bind(sockfd[i++], (SA *)&servaddr, sizeof(servaddr));
    }

    return sockfd;
}

static int compare_address(struct sockaddr_in *prev_req, struct sockaddr_in *new_req) {
    if ((prev_req->sin_addr.s_addr != new_req->sin_addr.s_addr) ||  // Check the IP Address
        (prev_req->sin_port != new_req->sin_port) ||                // Check the Port Number
        (prev_req->sin_family != new_req->sin_family))              // Check the Address family
    {
        return 1; // New Request
    } else {
        return 0; // Duplicate Request
    }
}

static ClientRequest* searchAndUpdateClientList(struct sockaddr_in cliaddr) {
    ClientRequest *cur = Head;
 
    // Check if it is a duplicate request
    while(cur != NULL) {
        if (compare_address(&(cur->cliaddr), &cliaddr) == 0)
            return NULL;
        cur = cur->next;
    }
    
    // Create a node if the entry is not present
    ClientRequest *new_node = (ClientRequest*) Malloc(sizeof(ClientRequest));
    new_node->cliaddr = cliaddr;
    new_node->next = Head;
    Head = new_node; // update head node

    return new_node;
}

static void Sleep(int sec, int msec) {
    struct timeval timeout;
    timeout.tv_sec = sec;
    timeout.tv_usec = msec;
    Select(1, NULL, NULL, NULL, &timeout);
}

static sigjmp_buf jmpbuf;

static void sig_alarm(int signo) {
    siglongjmp(jmpbuf, 1);
}


static int getNextPacket(TcpPckt *pckt, unsigned int seq, unsigned int ack, unsigned int winSize, int fd) {
    char buf[MAX_PAYLOAD+1];
    int ret = 0;
    int n = Read(fd, buf, MAX_PAYLOAD);

    return fillPckt(pckt, seq, ack, winSize, buf, n);
}

static pid_t serveNewClient(struct sockaddr_in cliaddr, int *sock_fd, int req_sock,
                            int total_IP, char* fileName, int isLocal)
{
    pid_t pid;

    // Child process
    if ((pid = Fork()) == 0) {
        /* Child would close all the socket descriptor except one
         * Next it would create a new socket and send a second handshake
         * to the client with the new socket in the payload.
         * It would also start a new timer for the second handshake 
         */

        // Close all the sockets except the one where request arrived
        int i;
        for (i = 0; i < total_IP; i++ ) {
            if (i != req_sock)
                Close(sock_fd[i]);
        }

        // create a new socked and connect with the client and bind to it
        
        struct sockaddr_in servAddr;
        int len, connFd, newChildPortNo, send2HSFromConnFd;
        struct rtt_info rttInfo;
        
        TcpPckt packet;
        unsigned int seqNum = 0;
        unsigned int ackNum;
        unsigned int winSize;
        char sendBuf[MAX_PAYLOAD+1], recvBuf[MAX_PAYLOAD+1];
        char errMsg[MAX_PAYLOAD+1] = "";

        // To get server IP address
        len = sizeof(struct sockaddr_in);
        Getsockname(sock_fd[req_sock], (SA *) &servAddr, &len);

        printf("Using Server IP:Port => %s\n", Sock_ntop((SA *) &servAddr, sizeof(struct sockaddr_in)));

        // Create a new connection socket
        connFd = Socket(AF_INET, SOCK_DGRAM, 0);

        // Set socket option -> SO_DONTROUTE if client is local
        if (isLocal) {
            int optVal = 1;
            Setsockopt(connFd, SOL_SOCKET, SO_DONTROUTE, &optVal, sizeof(optVal));
        }

        // Bind connection socket
        servAddr.sin_port = 0; // Choose a new port number
        Bind(connFd, (SA *)&servAddr, sizeof(servAddr));

        // Get new port number for connection socket
        Getsockname(connFd, (SA *) &servAddr, &len);
        newChildPortNo = ntohs(servAddr.sin_port);

        sprintf(sendBuf, "%d", newChildPortNo);
        send2HSFromConnFd = 0;
        Signal(SIGALRM, sig_alarm);

        rtt_init(&rttInfo);
        rtt_newpack(&rttInfo);

    send2HSAgain:
        // Send second handshake
        fillPckt(&packet, SYN_ACK_SEQ_NO, ACK_SEQ_NO, 0, sendBuf, MAX_PAYLOAD);
        printf(KYEL "\nSecond HS sent from Listening Socket:" RESET "New Conn Port No => %s\n" , packet.data);
        Sendto(sock_fd[req_sock], &packet, DATAGRAM_SIZE, 0, (SA *) &cliaddr, sizeof(cliaddr));

        if (send2HSFromConnFd) {
            printf(KYEL "Second HS sent from Conn Socket: New Conn Port No => %s" RESET "\n", packet.data);
            Sendto(connFd, &packet, DATAGRAM_SIZE, 0, (SA *) &cliaddr, sizeof(cliaddr));
        }
        
        // TODO: change alarm to setitimer
        alarm(rtt_start(&rttInfo)/1000);

        if (sigsetjmp(jmpbuf, 1) != 0) {
            printf(KYEL _3TABS "Timeout!\n" RESET);
            if (rtt_timeout(&rttInfo)) {
                char *str = "Server Child Terminated due to 12 Timeouts";
                err_msg(str);
                strcpy(errMsg, str);
                goto error;
            }
            send2HSFromConnFd = 1;
            goto send2HSAgain;
        } 

        // Receive third Handshake
        len = Recvfrom(connFd, &packet, DATAGRAM_SIZE, 0,  NULL, NULL);

        alarm(0);
        rtt_stop(&rttInfo);

        readPckt(&packet, len, &seqNum, &ackNum, &winSize, recvBuf);
        printf("\nThird HS received:" KGRN "Connection Establised Successfully\n" RESET);
        printf("Seq num: %d\t Ack num: %d\t Win Size: %d\n", seqNum, ackNum, winSize);

        // Connect to Client request
        Connect(connFd, (SA *) &cliaddr, sizeof(cliaddr));

        int fd;
        if ((fd = open(fileName, O_RDONLY)) == -1) {
            char *str = "Server Child Terminated due to Invalid FileName";
            err_msg(str);
            strcpy(errMsg, str);
            goto error;
        }

        // Ack becomes the seq no of the next packet
        seqNum = ackNum;

        while ((len = getNextPacket(&packet, seqNum, 0, 0, fd)) >= HEADER_LEN) {
            sleep(1); //TODO remove
            Writen(connFd, (void *) &packet, len);
            printf("\nPacket Sent =>\t Seq num: %d\t Total Bytes: %d\n", seqNum, len);
            seqNum++;

            if (len != DATAGRAM_SIZE)
                break;
        }

error:
        // Send a FIN to terminate connection
        len = fillPckt(&packet, FIN_SEQ_NO, 0, 0, errMsg, strlen(errMsg));
        Writen(connFd, (void *) &packet, len);

        Close(sock_fd[req_sock]);

        exit(0);
    } // End - Child Process
    
    return pid;
} 

static int listenAllConnections(struct ifi_info *ifihead, int *sockfd, int totalIP) {
    sigset_t sigset;
    fd_set fixedFdset, varFdset;
    int maxfd = sockfd[totalIP-1] + 1;
    int i, n;
    
    TcpPckt packet; 
    unsigned int seqNum = 0 ;
    unsigned int ackNum;
    unsigned int winSize;
    char recvBuf[MAX_PAYLOAD+1];

    FD_ZERO(&fixedFdset);
    for (i = 0 ; i < totalIP; i++)
        FD_SET(sockfd[i], &fixedFdset);

    Signal(SIGCHLD, sig_child); 
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGCHLD);

    while (1) {
        // Listen using select on all sockets
        varFdset = fixedFdset;
        if (select(maxfd, &varFdset, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                // Retry select() if interupted by signal handler of SIGCHLD
                continue;
            } else {
                err_sys("Server termination due to error on select()");
            }
        }
        
        // Check which Socket got packets
        for (i = 0 ; i < totalIP; i++) {
            if (FD_ISSET(sockfd[i], &varFdset)) {
                struct sockaddr_in cliaddr;
                socklen_t len = sizeof(cliaddr);
        
                n = Recvfrom(sockfd[i], &packet, DATAGRAM_SIZE, 0, (SA *)&cliaddr, &len);
                readPckt(&packet, n, &seqNum, &ackNum, &winSize, recvBuf);

                if (searchAndUpdateClientList(cliaddr) != NULL) {
                    int isLocal = verifyIfLocalAndGetHostIP(ifihead, &cliaddr.sin_addr, NULL);

                    printf("\nNew request from client %son Local Interface => %s\n",
                            isLocal == 0 ? "Not " : "",
                            Sock_ntop((SA *) &cliaddr, sizeof(struct sockaddr_in)));
                    printf(KGRN "First HS received: fileName => %s\n" RESET, recvBuf);
                    printf("Seq num: %d\t Ack num: %d\t Win Size: %d\n", seqNum, ackNum, winSize);

                    // Block SIGCHLD until parent sets child pid in ClientRequest list
                    sigprocmask(SIG_BLOCK, &sigset, NULL);
                    Head->childpid = serveNewClient(cliaddr, sockfd, i, totalIP, recvBuf, isLocal);
                    sigprocmask(SIG_UNBLOCK, &sigset, NULL);
                }
            }
        }
    }
}

int main() {
    struct ifi_info *ifihead;
    int *sockfd, totalIP;

    initializeParams();
    
    // Get all interfaces
    ifihead = Get_ifi_info_plus(AF_INET, 1/*doalias*/);
    totalIP = print_ifi_info_plus(ifihead);

    sockfd = bindAllInterfaces(ifihead, totalIP);
    listenAllConnections(ifihead, sockfd, totalIP);

    free_ifi_info_plus(ifihead);
    exit(0);
}

