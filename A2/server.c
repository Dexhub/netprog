#include "unp.h"
#include "unpifiplus.h"
#include "common.h"
#include "setjmp.h"

typedef struct client_request {
    struct sockaddr_in cliaddr;
    pid_t childpid;
    struct client_request *next;
} client_request;

static int in_port_no, in_window_size;
static client_request *Head = NULL;

static void sig_child(int signo) { 
    // Remove child entry from the linkedlist 
    client_request *cur = Head;
    client_request *prev = NULL;
    pid_t pid = wait(NULL);
    
    while (cur != NULL) {
        if (cur->childpid == pid) {
            // remove this entry
            if (Head == cur)
                Head = cur->next;
            else 
                prev = cur->next;

            printf("Child deleted with Pid : %d\n", pid);
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
        err_quit("Unknown server argument file : '%s'\n", SERVER_IN);
    }
    in_port_no = getIntParamValue(inp_file);
    in_window_size = getIntParamValue(inp_file);
    Fclose(inp_file);
}

static int* getAllInterfaces(int *totalIP) {
    struct ifi_info *ifi, *ifihead;
    struct sockaddr *sa;
    int i, counter = 0;
    int *sockfd;
    struct sockaddr_in servaddr, cliaddr;
    u_char *ptr;
    
    // Get all interfaces
    ifihead = Get_ifi_info_plus(AF_INET, 1/*doalias*/);
    printf("\nFollowing are different Interfaces:\n");
    *totalIP = print_ifi_info_plus(ifihead);

    sockfd = (int *) malloc(*totalIP * sizeof(int));

    for (ifi = ifihead; ifi != NULL; ifi = ifi->ifi_next) {
        sockfd[counter] = Socket(AF_INET, SOCK_DGRAM, 0);
        bzero(&servaddr, sizeof(servaddr));
        servaddr = *(struct sockaddr_in *)ifi->ifi_addr;
        servaddr.sin_port = htons(in_port_no);
        Bind(sockfd[counter++], (SA *)&servaddr, sizeof(servaddr));
    }
    free_ifi_info_plus(ifihead);

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

static client_request* searchAndUpdateClientList(struct sockaddr_in cliaddr) {
    client_request *cur = Head;
 
    // Check if it is a duplicate request
    while(cur != NULL) {
        if (compare_address(&(cur->cliaddr), &cliaddr) == 0)
            return NULL;
        cur = cur->next;
    }
    
    // Create a node if the entry is not present
    client_request *new_node = (client_request*) malloc(sizeof(client_request));
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

static pid_t serveNewClient(struct sockaddr_in cliaddr, int *sock_fd, int req_sock,
                            int total_IP, char* fileName)
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
        char sendBuf[MAXLINE], recvBuf[MAXLINE];
        int len, connFd, newChildPortNo, send2HSFromConnFd;

        // To get server IP address
        len = sizeof(struct sockaddr_in);
        Getsockname(sock_fd[req_sock], (SA *) &servAddr, &len);

        printf("Using Server IP:Port => %s\n", Sock_ntop((SA *) &servAddr, sizeof(struct sockaddr_in)));

        // Create a new connection socket
        connFd = Socket(AF_INET, SOCK_DGRAM, 0);

        // Bind connection socket
        servAddr.sin_port = 0; // Choose a new port number
        Bind(connFd, (SA *)&servAddr, sizeof(servAddr));

        // Get new port number for connection socket
        Getsockname(connFd, (SA *) &servAddr, &len);
        newChildPortNo = ntohs(servAddr.sin_port);

        sprintf(sendBuf, "%d", newChildPortNo);
        send2HSFromConnFd = 0;
        Signal(SIGALRM, sig_alarm);

    send2HSAgain:
        // Send second handshake
        printf("Second HS sent from Listening Socket : New Conn Port No => %s\n", sendBuf);
        Sendto(sock_fd[req_sock], sendBuf, strlen(sendBuf), 0, (SA *) &cliaddr, sizeof(cliaddr));
        if (send2HSFromConnFd) {
            printf("Second HS sent from Conn Socket: New Conn Port No => %s\n", sendBuf);
            Sendto(connFd, sendBuf, strlen(sendBuf), 0, (SA *) &cliaddr, sizeof(cliaddr));
        }
        
        // TODO: change alarm to setitimer
        alarm(3);

        if (sigsetjmp(jmpbuf, 1) != 0) {
            printf("Timeout!!\n");
            send2HSFromConnFd = 1;
            goto send2HSAgain;
        } 

        // Receive third Handshake
        len = Recvfrom(connFd, recvBuf, MAXLINE, 0, NULL, NULL);

        alarm(0);
        recvBuf[len] = '\0';
        printf("Third HS received : ACK => %s\n", recvBuf);
        
        Close(sock_fd[req_sock]);

        // Connect to Client request
        Connect(connFd, (SA *) &cliaddr, sizeof(cliaddr));

        // TODO: Begin file transfer
        sprintf(sendBuf, "F");
        Writen(connFd, sendBuf, strlen(sendBuf));

        exit(0);
    } // End - Child Process
    
    return pid;
} 

static int listenAllConnections(int *sockfd,int totalIP) {
    char message[MAXLINE];
    sigset_t sigset;
    fd_set fixedFdset, varFdset;
    int maxfd = sockfd[totalIP-1] + 1;
    int i, n;

    FD_ZERO(&fixedFdset);
    for (i = 0 ; i < totalIP; i++)
        FD_SET(sockfd[i], &fixedFdset);

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

                n = Recvfrom(sockfd[i], message, MAXLINE, 0, (SA *)&cliaddr, &len);
                message[n] = '\0';
                if (searchAndUpdateClientList(cliaddr) != NULL) {
                    printf("\nNew request from client => %s\n", Sock_ntop((SA *) &cliaddr, sizeof(struct sockaddr_in)));
                    printf("First HS received : fileName => %s\n", message);

                    // Block SIGCHLD until parent sets child pid in client_request list
                    sigprocmask(SIG_BLOCK, &sigset, NULL);
                    Head->childpid = serveNewClient(cliaddr, sockfd, i, totalIP, message);
                    sigprocmask(SIG_UNBLOCK, &sigset, NULL);
                }
            }
        }
    }
}

int main() {
    int *sockfd, totalIP;

    Signal(SIGCHLD, sig_child); 
    initializeParams();
    sockfd = getAllInterfaces(&totalIP);
    listenAllConnections(sockfd, totalIP);
    exit(0);
}

