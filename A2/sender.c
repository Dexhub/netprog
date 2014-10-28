#include "server.h"

static int getNextNewPacket(TcpPckt *pckt, uint32_t seq, uint32_t ack, uint32_t winSize, int fileFd) {
    char buf[MAX_PAYLOAD+1];
    int n = Read(fileFd, buf, MAX_PAYLOAD);
    return fillPckt(pckt, seq, ack, winSize, buf, n);
}

static void printSendWindow(SendWinQueue *SendWinQ) {
    int i;
    printf(KBLU "Sending Window =>  ");
    printf("Oldest SeqNum: %d  Next SeqNum: %d  Cwin: %d  SSFresh: %d  Contents:",
            SendWinQ->oldestSeqNum, SendWinQ->nextNewSeqNum, SendWinQ->cwin, SendWinQ->ssfresh);
    for (i = 0; i < SendWinQ->winSize; i++) {
        if (IS_PRESENT(SendWinQ, i))
            printf(" %d", GET_SEQ_NUM(SendWinQ, i));
        else
            printf(" x");
    }
    printf( RESET "\n");
}

static SendWinNode* addPacketToSendWin(SendWinQueue *SendWinQ, TcpPckt *packet, int dataSize) {
    SendWinNode *wnode = GET_WNODE(SendWinQ, packet->seqNum);
    fillPckt(&wnode->packet, packet->seqNum, packet->ackNum, packet->winSize, packet->data, dataSize);
    wnode->dataSize = dataSize;
    wnode->isPresent = 1;
    wnode->numOfRetransmits = 0;

    // Update nextNewSeqNum
    if (SendWinQ->nextNewSeqNum == packet->seqNum) {
        SendWinQ->nextNewSeqNum++;
    }

    return wnode;
}

static void waitUntilCwinIsNonZero(SendWinQueue *SendWinQ, int connFd) {
    while (SendWinQ->cwin == 0) {
        // TODO: Use select on connFd and send a probe packet
        
    }
}

static void zeroOutRetransmitCnts(SendWinQueue *SendWinQ) {
    int i;
    for (i = 0; i < SendWinQ->winSize; i++) {
        SendWinQ->wnode[i].numOfRetransmits = 0;
    }
}

void initializeSendWinQ(SendWinQueue *SendWinQ, int sendWinSize, int recWinSize, int nextSeqNum) {
    SendWinQ->wnode         = (SendWinNode*) calloc(sendWinSize, sizeof(SendWinNode));
    SendWinQ->winSize       = sendWinSize;
    SendWinQ->cwin          = min(sendWinSize, recWinSize); // TODO: Change to 1
    SendWinQ->oldestSeqNum  = nextSeqNum;
    SendWinQ->nextNewSeqNum = nextSeqNum;
    SendWinQ->ssfresh       = 0;
}

static sigjmp_buf jmpbuf;

static void sig_alarm(int signo) {
    siglongjmp(jmpbuf, 1);
}

void sendFile(SendWinQueue *SendWinQ, int connFd, int fileFd, struct rtt_info rttInfo) {
    SendWinNode *wnode;
    TcpPckt packet;
    uint32_t seqNum, ackNum, winSize, expectedAckNum;
    int i, len, done, numPacketsSent, dupAcks;

    Signal(SIGALRM, sig_alarm);

    done = 0;
    while (!done) {
        waitUntilCwinIsNonZero(SendWinQ, connFd);
        zeroOutRetransmitCnts(SendWinQ); // TODO ??

sendAgain:
        // Send Packets
        seqNum = SendWinQ->oldestSeqNum;
        for (i = 0; i < SendWinQ->cwin; i++) {

            if (seqNum < SendWinQ->nextNewSeqNum) {
                // Packet already in sending window
                int wInd = seqNum % SendWinQ->winSize;
                assert(IS_PRESENT(SendWinQ, wInd) && "Packet should be present");
                assert((seqNum == GET_SEQ_NUM(SendWinQ, wInd)) && "Invalid Seq Num of Sending Packet");

                len = GET_DATA_SIZE(SendWinQ, wInd);
                wnode = &SendWinQ->wnode[wInd];
                wnode->numOfRetransmits++;
            } else {
                // Get new packet and add to sending window
                len = getNextNewPacket(&packet, seqNum, 0, 0, fileFd);
                wnode = addPacketToSendWin(SendWinQ, &packet, len);
            }

            // Send packet and update timestamp
            Writen(connFd, (void *) &wnode->packet, len);
            wnode->timestamp = rtt_ts(&rttInfo);

            printf("\nPacket Sent =>\t Seq num: %d\t Total Bytes: %d\n", seqNum, len);
            printSendWindow(SendWinQ);

            seqNum++;

            // No more file contents to send
            if (len != DATAGRAM_SIZE) {
                done = 1;
                break;
            }
        }

        // TODO: change alarm to setitimer
        alarm(rtt_start(&rttInfo)/1000);

        if (sigsetjmp(jmpbuf, 1) != 0) {
            printf(KYEL _3TABS "Timeout!\n" RESET);
            int retransmitCnt = GET_OLDEST_SEQ_WNODE(SendWinQ)->numOfRetransmits;
            if (rtt_timeout(&rttInfo, retransmitCnt)) {
                char *str = "Server Child Terminated due to 12 Timeouts";
                err_msg(str);
                break;
            }
            goto sendAgain;
        } 

        expectedAckNum = seqNum;
        dupAcks = 0;

        // Receive ACKs
        while (1) {
            len = Read(connFd, (void *) &packet, DATAGRAM_SIZE);
            readPckt(&packet, len, NULL, &ackNum, &winSize, NULL);
            printf("\nACK Received =>  ACK num: %d\t Advertised Win: %d\n", ackNum, winSize);

            // TODO: Need to Change
            SendWinQ->cwin = min(winSize, SendWinQ->winSize);

            if (SendWinQ->oldestSeqNum == ackNum) {
                dupAcks++;
                if (dupAcks == 3) {
                    printf("3 Duplicate ACKs received. Enabling Fast Retransmit\n");
                    done = 0;
                    break;
                }
            } else {
                while (SendWinQ->oldestSeqNum < ackNum) {
                    int wInd = GET_OLDEST_SEQ_IND(SendWinQ);
                    assert(IS_PRESENT(SendWinQ, wInd) && "Packet should be present");
                    assert((SendWinQ->oldestSeqNum == GET_SEQ_NUM(SendWinQ, wInd)) &&
                            "Invalid Seq Num of Sending Packet");

                    rtt_stop(&rttInfo, SendWinQ->wnode[wInd].timestamp);
                    SendWinQ->wnode[wInd].isPresent = 0;
                    SendWinQ->oldestSeqNum++;
                }
                printSendWindow(SendWinQ);
                dupAcks = 0;
            }

            if (expectedAckNum == ackNum) {
                // All packets successfully sent and acknowledged
                break;
            }
        }

        alarm(0);
    }
}

void terminateConnection(int connFd, char *errMsg) {
    TcpPckt packet;
    int len;

    // Send a FIN to terminate connection
    len = fillPckt(&packet, FIN_SEQ_NO, 0, 0, errMsg, strlen(errMsg));
    Writen(connFd, (void *) &packet, len);

    // TODO: Recv FIN-ACK from client
}

