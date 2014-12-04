#include "utils.h"

int getVmNodeByIP(char *ip) {
    struct hostent *hostInfo = NULL;
    struct in_addr ipInfo;
    int node = 0;

    if (inet_pton(AF_INET, ip, &ipInfo) > 0) {
        hostInfo = gethostbyaddr(&ipInfo, sizeof(ipInfo), AF_INET);
        sscanf(hostInfo->h_name, "vm%d", &node);
    }
    return node;
}

char* getIPByVmNode(char *ip, int node) {
    struct hostent *hostInfo = NULL;
    char hostName[100];

    sprintf(hostName, "vm%d", node);
    hostInfo = gethostbyname(hostName);

    if (hostInfo && inet_ntop(AF_INET, hostInfo->h_addr, ip, INET_ADDRSTRLEN))
        return ip;
    else
        return NULL;
}

int getHostVmNodeNo() {
    char hostname[1024];
    int nodeNo;

    gethostname(hostname, 10);
    nodeNo = atoi(hostname+2);
    if (nodeNo < 1 || nodeNo > 10) {
        err_msg("Warning: Invalid hostname '%s'", hostname);
    }
    return nodeNo;
}
static char* getParam(FILE *fp, char *ptr, int n) {
    char line[MAXLINE];

    if (fgets(line, n, fp) == NULL || strlen(line) == 0) {
        return NULL;
    }

    if (sscanf(line, "%s", ptr) > 0)
        return ptr;
    return NULL;
}

char* getStringParamValue(FILE *inp_file, char *paramVal) {
    if (getParam(inp_file, paramVal, PARAM_SIZE) == NULL) {
        err_quit("Invalid parameter\n");
    }
    return paramVal;
}

int getIntParamValue(FILE *inp_file) {
    char paramStr[PARAM_SIZE];
    int paramIVal;

    if (getParam(inp_file, paramStr, PARAM_SIZE) == NULL ||
            ((paramIVal = atoi(paramStr)) == 0)
       ) {
        err_quit("Invalid parameter\n");
    }
    return paramIVal;
}
char* getFullPath(char *fullPath, char *fileName, int size, bool isTemp) {

    if (getcwd(fullPath, size) == NULL) {
        err_msg("Unable to get pwd via getcwd()");
    }

    strcat(fullPath, fileName);

    if (isTemp) {
        if (mkstemp(fullPath) == -1) {
            err_msg("Unable to get temp file via mkstemp()");
        }
    }

    return fullPath;
}

int createAndBindUnixSocket(char *filePath) {
    struct sockaddr_un sockAddr;
    int sockfd;

    sockfd = Socket(AF_LOCAL, SOCK_DGRAM, 0);

    bzero(&sockAddr, sizeof(sockAddr));
    sockAddr.sun_family = AF_LOCAL;
    strcpy(sockAddr.sun_path, filePath);

    unlink(filePath);
    Bind(sockfd, (SA*) &sockAddr, sizeof(sockAddr));

    return sockfd;
}

