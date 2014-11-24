#include "common.h"

static char filePath[1024], hostNode, hostIP[100];
static sigjmp_buf jmpToRetransmit;

static void sig_int(int signo) {
    unlink(filePath);
    exit(0);
}

static void sig_alarm(int signo) {
    siglongjmp(jmpToRetransmit, 1);
}

int main() {
    struct sockaddr_un cliAddr;
    char buffer[1024];
    int sockfd;

    time_t rawtime;
    struct tm * timeinfo;




    getFullPath(filePath, CLI_FILE, sizeof(filePath), TRUE);
    sockfd = createAndBindUnixSocket(filePath);
    hostNode = getHostVmNodeNo();
    getIPByVmNode(hostIP, hostNode);
    printf("Client running on VM%d (%s)\n", hostNode, hostIP);

    Signal(SIGINT, sig_int);
    Signal(SIGALRM, sig_alarm);
    while (1) {
        char serverIP[100];
        int serverNode, serverPort;
        bool forceRediscovery = FALSE;

        printf("\nChoose Server VM Node Number from VM1-VM10: ");
        if ((scanf("%d", &serverNode) != 1) || serverNode < 1 || serverNode > TOTAL_VMS) {
            break;
        }
        if (getIPByVmNode(serverIP, serverNode) == NULL) {
            err_msg("Warning: Unable to get IP address, using hostname instead"); 
            sprintf(serverIP, "vm%d", serverNode);
        }
        serverPort = SER_PORT;

jmpToRetransmit:
        msg_send(sockfd, serverIP, serverPort, "", forceRediscovery);

        alarm(CLI_TIMEOUT);

        if (sigsetjmp(jmpToRetransmit, 1) != 0) {
            forceRediscovery = TRUE;
            printf("Client at node vm%d: timeout on response from vm%d\n", hostNode, serverNode);
            goto jmpToRetransmit;
        }

        msg_recv(sockfd, buffer, serverIP, &serverPort);
        alarm(0);
	time ( &rawtime );
	timeinfo = localtime ( &rawtime );
        printf("%15s :Client at node vm%d: received from vm%d => %s\n", asctime(timeinfo), hostNode, serverNode, buffer);
    }

    err_msg("\nExiting! Thank you!\n");
    unlink(filePath);
    Close(sockfd);
}

