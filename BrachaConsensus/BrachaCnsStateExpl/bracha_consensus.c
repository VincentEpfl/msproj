#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>

#define N 4 // Total number of processes
#define T 1 // Maximum number of Byzantine processes
#define PORT_BASE 8080

// Functions prototypes to avoid compiling error due to circular dependency
void Bracha_broadcast(int originProcess, int value, int tag, int round, int dval);
void brachaProcessMessages(int originProcess, int value, int fromProcess, int tag, int round, int dval);

sem_t *sem;
sem_t *sem_init_brd;

// Message type
// format (round, tag, source, val, destination, dval)
typedef struct 
{
int originProcess;
int roundNum;
int tag;
int senderId;
int receivedValue;
int destinationId;
int dval;
} Message;

// State for 1 round of the consensus
typedef struct 
{
int received_values[N][3][N][2]; // For each process, values received by each process with each flag
int step[N][2]; // Step in the algorithm for each value 
int acceptedValue[N]; // For each process, value accepted by the broadcast algorithm
int numMsg;
Message msgbuffer[100]; // Buffer of messages for this round that have arrived at an earlier round
int numValid;
Message validMsg[100]; // Messages that have been validated
} RoundInfo;

// State of all the rounds
RoundInfo roundsInfo[100] = { // TODO init acceptedValues to -1 double loop ok...
    {
        {{{{0, 0}}}},
        {{0, 0}},
        {0},
        0,
        {
            {
                0,
                0,
                0,
                0,
                0,
                0,
                0
            }
        },
        0,
        {
            {
                0,
                0,
                0,
                0,
                0,
                0,
                0
            }
        }
    }
};

// This process id
int processId;

// Current phase number
int phase = 0; 

// Current round number
int rnd = 0;

int seed = 0;

// TODO (??)
bool decided = false;

// Current value, estimate of the decision
int value = -1;

// Tag to indicate that this process is ready to decide at that phase
int dval = 0;

// Value this process has decided
int decision = -1;

// "Random" number generator
// Replace with an external call to a better one
int randomBit() {
    int s = seed;
    seed++;
    if (s % 2 == 0) {
        return 1;
    } else {
        return 0;
    }
}

// How many different processes sent a certain value with a certain tag
// to this process at a certain round
int countDistinctProcessesForValue(int originProcess, int value, int tag, int roundNumber)
{
    int count = 0;
    for (int i = 0; i < N; i++)
    {
        if (roundsInfo[roundNumber].received_values[originProcess][tag][i][value] == 1)
        {
            count++;
        }
    }
    return count;
}

// Condition to get out of the waiting loop
bool condition(int round) {
    bool cond = false;
    if (roundsInfo[round].numValid >= N - T) { // BUG in consensus ?
        cond = true;
    }
    return cond;
}

// Condition to add a message to the set of valid messages
bool checkValidCond(int value, int round) {
    bool cond = false;
    int count = 0;
    for (int m = 0; m < roundsInfo[round - 1].numValid; m++) { 
        if (roundsInfo[round - 1].validMsg[m].receivedValue == value) { // TODO verify cond value ok 
            count++;
        }
    }
    if (count >= N - T) {
        cond = true;
    }
    return cond;
}

// Broadcast
void Bracha_broadcast(int originProcess, int value, int tag, int round, int dval)
{
    int sockfd;
    struct sockaddr_in serverAddr;
    
    printf("Process %d: Start Bracha broadcast\n", processId);
    
    for (int i = 0; i < N; i++)
    {
        if (i == processId)
        {
            // In this one broadcast don't send to itself (?)
            if (tag == 0) {
                continue;
            } else { // BUG in Broadcast ?
                brachaProcessMessages(originProcess, value, processId, tag, round, dval);
                continue;
            }
            
        }

        int message[7] = {round, originProcess, tag, processId, value, i, dval}; // format [round, origin, tag, from, value, to, dval] | tag 0:initial,1:echo,2:ready

        /*
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(PORT_BASE + i);
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
        
        if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1)
        {
            perror("[Process] Connect failure");
            exit(EXIT_FAILURE);
        }
        */
       sockfd = 1;
        

        send(sockfd, &message, sizeof(message), 0);

        printf("Process %d, round %d : Value %d sent to process %d with tag %d\n", processId, round, value, i, tag);

        //close(sockfd);
    }
}

// Process a received message that is from the same round as this process 
// TODO verify round ok (round process vs round msg)
void brachaProcessMessages(int originProcess, int value, int fromProcess, int tag, int round, int dval)
{
    roundsInfo[round].received_values[originProcess][tag][fromProcess][value] = 1;
    int distinctCount = countDistinctProcessesForValue(originProcess, value, tag, round);
    printf("Process %d, round %d : Value %d from p%d Tag %d distinct count: %d\n", processId, round, 
    value, originProcess, tag, distinctCount);

    // BUG in broadcast ?
    
    if (roundsInfo[round].step[originProcess][value] == 0 && ( countDistinctProcessesForValue(originProcess, value, 0, round) >= 1 
    || countDistinctProcessesForValue(originProcess, value, 1, round) >= (N+T)/2 + 1
    || countDistinctProcessesForValue(originProcess, value, 2, round) >= T+1 )) {
        printf("Process %d, round %d : goes from step 0 to step 1 for value %d (origin p%d)\n", 
        processId, round, value, originProcess);
        roundsInfo[round].step[originProcess][value] = roundsInfo[round].step[originProcess][value] + 1;
        Bracha_broadcast(originProcess, value, roundsInfo[round].step[originProcess][value], round, dval);
    }
    if (roundsInfo[round].step[originProcess][value] == 1 && ( countDistinctProcessesForValue(originProcess, value, 1, round) >= (N+T)/2 + 1 
    || countDistinctProcessesForValue(originProcess, value, 2, round) >= T+1 )) {
        printf("Process %d, round %d : goes from step 1 to step 2 for value %d (origin p%d)\n", 
        processId, round, value, originProcess);
        roundsInfo[round].step[originProcess][value] = roundsInfo[round].step[originProcess][value] + 1;
        Bracha_broadcast(originProcess, value, roundsInfo[round].step[originProcess][value], round, dval);
    }
    if (roundsInfo[round].step[originProcess][value] == 2 && countDistinctProcessesForValue(originProcess, value, 2, round) >= (2*T+1)) {
        printf("Process %d, round %d : goes from step 2 to step 3 for value %d (origin p%d)\n", 
        processId, round, value, originProcess);
        roundsInfo[round].step[originProcess][value] = roundsInfo[round].step[originProcess][value] + 1;
        // Accept value
        roundsInfo[round].acceptedValue[originProcess] = value;
        printf("###############################\n");
        printf("Process %d, round %d: Value %d accepted from process %d.\n", processId, round, value, originProcess);
        printf("###############################\n");
        if (round == 1 && (value == 0 || value == 1)) { // TODO check init round is 0 or 1
            // add this msg to valid set
            printf("Add this message to the valid set\n");
            roundsInfo[round].validMsg[roundsInfo[round].numValid].originProcess = originProcess;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].destinationId = processId;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].receivedValue = value;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].roundNum = round;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].senderId = fromProcess;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].tag = tag;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].dval = dval;
            roundsInfo[round].numValid = roundsInfo[round].numValid + 1;
        }
        if (round > 1 && (value == 0 || value == 1)) { // checkValidCond(value, round)
            // add this msg to valid set
            printf("Add this message to the valid set\n");
            roundsInfo[round].validMsg[roundsInfo[round].numValid].originProcess = originProcess;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].destinationId = processId;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].receivedValue = value;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].roundNum = round;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].senderId = fromProcess;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].tag = tag;
            roundsInfo[round].validMsg[roundsInfo[round].numValid].dval = dval;
            roundsInfo[round].numValid = roundsInfo[round].numValid + 1;
        }
    }
    
    
}

// Process a received message
int processMessage(int originProcess, int msgRound, int tag, int value, int fromProcess, int toProcess, int round, int dval) {
    if (msgRound > round) { 
        printf("Store msg in buffer\n");
        roundsInfo[msgRound].msgbuffer[roundsInfo[msgRound].numMsg].originProcess = originProcess;
        roundsInfo[msgRound].msgbuffer[roundsInfo[msgRound].numMsg].tag = tag;
        roundsInfo[msgRound].msgbuffer[roundsInfo[msgRound].numMsg].roundNum = msgRound;
        roundsInfo[msgRound].msgbuffer[roundsInfo[msgRound].numMsg].senderId = fromProcess;
        roundsInfo[msgRound].msgbuffer[roundsInfo[msgRound].numMsg].receivedValue = value;
        roundsInfo[msgRound].msgbuffer[roundsInfo[msgRound].numMsg].destinationId = toProcess;
        roundsInfo[round].validMsg[roundsInfo[round].numValid].dval = dval;
        roundsInfo[msgRound].numMsg = roundsInfo[msgRound].numMsg + 1;
        return -1;
    }
    if (msgRound < round) { // TODO can probably process no pb, and might be necessary
        brachaProcessMessages(originProcess, value, fromProcess, tag, msgRound, dval);
        return 0;
    }
    brachaProcessMessages(originProcess, value, fromProcess, tag, round, dval);
    return 0;

}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <processId (0 to %d)> <initialValue (0 or 1)>\n", argv[0], N - 1);
        exit(1);
    }

    sem = sem_open("/sem_bracha_consensus", O_CREAT, 0644, 0);
    if (sem == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }
    sem_init_brd = sem_open("/sem_bracha_consensus_init_brd", O_CREAT, 0644, 0);
    if (sem_init_brd == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }


    processId = atoi(argv[1]);
    int initialValue = atoi(argv[2]);

    int listenfd, connfd;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    int receivedValue;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    int yes = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("[Process] ERROR on setsockopt");
        exit(EXIT_FAILURE);
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT_BASE + processId);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1)
    {
        perror("[Process] Bind failure");
        exit(EXIT_FAILURE);
    }

    if (listen(listenfd, 100) == -1)
    {
        perror("[Process] Listen failure");
        exit(EXIT_FAILURE);
    }

    printf("Process %d with initial value %d listening on port %d...\n", processId, initialValue, PORT_BASE + processId);
    
    sem_wait(sem);
    printf("Process %d done waiting for sockets init\n", processId);
    sem_close(sem);


    while(1) {

        if (phase > 5) {
            printf("\n");
            printf("END : SHOULD BE ENOUGH ROUNDS\n");
            printf("\n");
            printf("###############################\n");
            printf("###############################\n");
            
            while(1) {
                int receivedMessage[7]; 

                /*
                connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
            
                if (connfd == -1)
                {
                    perror("[Process] Accept failure");
                    exit(EXIT_FAILURE);
                } 
                */
                
                int nbytes = recv(listenfd, &receivedMessage, sizeof(receivedMessage), 0); // connfd-listenfd
                if (nbytes == -1)
                {
                    perror("[Process] Recv failure");
                    exit(EXIT_FAILURE);
                }
                
                int r = receivedMessage[0]; 
                int originProcess = receivedMessage[1];
                int tag = receivedMessage[2];
                int senderId = receivedMessage[3];
                int receivedValue = receivedMessage[4];
                int destinationId = receivedMessage[5];
                int dval = receivedMessage[6];
                printf("Process %d Round %d : Value %d (dval %d) received from process %d (origin %d) with tag %d in round %d\n", processId, 
                rnd, receivedValue, dval, senderId, originProcess, tag, r);
                if (receivedValue >= 0)
                { // Ignore special signals
                    processMessage(originProcess, r, tag, receivedValue, senderId, destinationId, rnd, dval);
                }
                //close(connfd);
            }
        }
        
        printf("Process %d : Phase %d, \n", processId, phase);

        // #####################
        // 1st broadcast

        rnd = 3*phase + 1;
        printf("Process %d : Round %d, \n", processId, rnd);

        // Broadcast the initial value
        
        value = initialValue;
        Bracha_broadcast(processId, value, 0, rnd, dval);

        if (rnd == 1) {
            sem_wait(sem_init_brd);
            printf("Process %d done waiting for broadcast init\n", processId);
            sem_close(sem_init_brd);
        }

        int receivedMessage[7]; 
        int n = 0;

        // 1st waiting loop
        while (!condition(rnd)) 
        {
            // First, process the messages of this round that were received in previous rounds 
            bool msgFound = false;
            for (int m = n; m < roundsInfo[rnd].numMsg; m++) {
                if (roundsInfo[rnd].msgbuffer[m].destinationId == processId
                && roundsInfo[rnd].msgbuffer[m].roundNum == rnd
                ) {
                    processMessage(roundsInfo[rnd].msgbuffer[m].originProcess, rnd, roundsInfo[rnd].msgbuffer[m].tag, roundsInfo[rnd].msgbuffer[m].receivedValue, roundsInfo[rnd].msgbuffer[m].senderId, processId, rnd, roundsInfo[rnd].msgbuffer[m].dval);
                    // TODO UPDATE STATE CONTROLLER HERE ?
                    msgFound = true;
                    n = m + 1;
                    break;
                }
            }
            if (msgFound) {
                continue;
            }
            
            // Then accept new messages

            /*
            connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
            
            if (connfd == -1)
            {
                perror("[Process] Accept failure");
                exit(EXIT_FAILURE);
            } 
            */
            
            int nbytes = recv(listenfd, &receivedMessage, sizeof(receivedMessage), 0); // connfd-listenfd
            if (nbytes == -1)
            {
                perror("[Process] Recv failure");
                exit(EXIT_FAILURE);
            }
            
            int r = receivedMessage[0]; 
            int originProcess = receivedMessage[1];
            int tag = receivedMessage[2];
            int senderId = receivedMessage[3];
            int receivedValue = receivedMessage[4];
            int destinationId = receivedMessage[5];
            int dval = receivedMessage[6];
            printf("Process %d Round %d : Value %d (dval %d) received from process %d (origin %d) with tag %d in round %d\n", processId, 
            rnd, receivedValue, dval, senderId, originProcess, tag, r);
            if (receivedValue >= 0)
            { // Ignore special signals
                processMessage(originProcess, r, tag, receivedValue, senderId, destinationId, rnd, dval);

                // This is where it registers its state to the controller 
                // TODO check if ok to do it here 
                int valuesCount[10][3][N][2]; // TODO maybe flatten array, but then again maybe it does that by default
                for (int r = 1; r <= 10; r++) { // TODO ATTENTION I THINK ROUNDS START AT 1 !!
                    for (int t = 0; t < 3; t++) {
                        for (int op = 0; op < N; op++) {
                            valuesCount[r][t][op][0] = countDistinctProcessesForValue(op, 0, t, r);
                            valuesCount[r][t][op][1] = countDistinctProcessesForValue(op, 1, t, r);
                        }
                    }
                }
                send(-1, &valuesCount, sizeof(valuesCount), 0);
            }
            //close(connfd);
        }
        // #####################

        // Update value
        // TODO il me semble qu'a ce stade on a que 0 ou 1 ?? Sinon just prendre maj value mais code un peu plus long...
        int valueCount[2] = {0, 0};
        for (int m = 0; m < roundsInfo[rnd].numValid; m++) {
            valueCount[roundsInfo[rnd].validMsg[m].receivedValue]++;
        }
        if (valueCount[0] >= valueCount[1]) { // BUG ?
            value = 0;
        } else {
            value = 1;
        }

        // #####################
        // 2nd broadcast
        rnd = 3*phase + 2;
        printf("Process %d : Round %d, \n", processId, rnd);

        // Broadcast value
        Bracha_broadcast(processId, value, 0, rnd, dval);

        int n2 = 0;

        // 2nd waiting loop
        while (!condition(rnd)) 
        {
            // First, process the messages of this round that were received in previous rounds 
            bool msgFound = false;
            for (int m = n2; m < roundsInfo[rnd].numMsg; m++) {
                if (roundsInfo[rnd].msgbuffer[m].destinationId == processId
                && roundsInfo[rnd].msgbuffer[m].roundNum == rnd
                ) {
                    processMessage(roundsInfo[rnd].msgbuffer[m].originProcess, rnd, roundsInfo[rnd].msgbuffer[m].tag, roundsInfo[rnd].msgbuffer[m].receivedValue, roundsInfo[rnd].msgbuffer[m].senderId, processId, rnd, roundsInfo[rnd].msgbuffer[m].dval);
                    // TODO UPDATE STATE CONTROLLER HERE ?
                    msgFound = true;
                    n2 = m + 1;
                    break;
                }
            }
            if (msgFound) {
                continue;
            }
            
            // Then accept new messages

            /*
            connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
            
            if (connfd == -1)
            {
                perror("[Process] Accept failure");
                exit(EXIT_FAILURE);
            } 
            */
            
            int nbytes = recv(listenfd, &receivedMessage, sizeof(receivedMessage), 0); // connfd-listenfd
            if (nbytes == -1)
            {
                perror("[Process] Recv failure");
                exit(EXIT_FAILURE);
            }
            
            int r = receivedMessage[0]; 
            int originProcess = receivedMessage[1];
            int tag = receivedMessage[2];
            int senderId = receivedMessage[3];
            int receivedValue = receivedMessage[4];
            int destinationId = receivedMessage[5];
            int dval = receivedMessage[6];
            printf("Process %d Round %d : Value %d (dval %d) received from process %d (origin %d) with tag %d in round %d\n", processId, 
            rnd, receivedValue, dval, senderId, originProcess, tag, r);
            if (receivedValue >= 0)
            { // Ignore special signals
                processMessage(originProcess, r, tag, receivedValue, senderId, destinationId, rnd, dval);

                // This is where it registers its state to the controller 
                // TODO check if ok to do it here 
                int valuesCount[10][3][N][2]; // TODO maybe flatten array, but then again maybe it does that by default
                for (int r = 1; r <= 10; r++) { // TODO ATTENTION I THINK ROUNDS START AT 1 !!
                    for (int t = 0; t < 3; t++) {
                        for (int op = 0; op < N; op++) {
                            valuesCount[r][t][op][0] = countDistinctProcessesForValue(op, 0, t, r);
                            valuesCount[r][t][op][1] = countDistinctProcessesForValue(op, 1, t, r);
                        }
                    }
                }
                send(-1, &valuesCount, sizeof(valuesCount), 0);
            }
            //close(connfd);
        }
        // #####################

        // Update the flag to indicate if this process is ready to decide at that phase
        int valueCount2[2] = {0, 0};
        for (int m = 0; m < roundsInfo[rnd].numValid; m++) {
            valueCount2[roundsInfo[rnd].validMsg[m].receivedValue]++;
        }
        if (valueCount2[0] >= N/2 || valueCount2[1] >= N/2) { // BUG ?
            dval = 1;
        }

        // #####################
        // 3rd broadcast
        rnd = 3*phase + 3;
        printf("Process %d : Round %d, \n", processId, rnd);

        // Broadcast value
        Bracha_broadcast(processId, value, 0, rnd, dval);

        int n3 = 0;

        // 3rd waiting loop
        while (!condition(rnd)) 
        {
            // First, process the messages of this round that were received in previous rounds 
            bool msgFound = false;
            for (int m = n3; m < roundsInfo[rnd].numMsg; m++) {
                if (roundsInfo[rnd].msgbuffer[m].destinationId == processId
                && roundsInfo[rnd].msgbuffer[m].roundNum == rnd
                ) {
                    processMessage(roundsInfo[rnd].msgbuffer[m].originProcess, rnd, roundsInfo[rnd].msgbuffer[m].tag, roundsInfo[rnd].msgbuffer[m].receivedValue, roundsInfo[rnd].msgbuffer[m].senderId, processId, rnd, roundsInfo[rnd].msgbuffer[m].dval);
                    // TODO UPDATE STATE CONTROLLER HERE ?
                    msgFound = true;
                    n3 = m + 1;
                    break;
                }
            }
            if (msgFound) {
                continue;
            }
            
            // Then accept new messages

            /*
            connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
            
            if (connfd == -1)
            {
                perror("[Process] Accept failure");
                exit(EXIT_FAILURE);
            } 
            */
            
            int nbytes = recv(listenfd, &receivedMessage, sizeof(receivedMessage), 0); // connfd-listenfd
            if (nbytes == -1)
            {
                perror("[Process] Recv failure");
                exit(EXIT_FAILURE);
            }
            
            int r = receivedMessage[0]; 
            int originProcess = receivedMessage[1];
            int tag = receivedMessage[2];
            int senderId = receivedMessage[3];
            int receivedValue = receivedMessage[4];
            int destinationId = receivedMessage[5];
            int dval = receivedMessage[6];
            printf("Process %d Round %d : Value %d (dval %d) received from process %d (origin %d) with tag %d in round %d\n", processId, 
            rnd, receivedValue, dval, senderId, originProcess, tag, r);
            if (receivedValue >= 0)
            { // Ignore special signals
                processMessage(originProcess, r, tag, receivedValue, senderId, destinationId, rnd, dval);

                // This is where it registers its state to the controller 
                // TODO check if ok to do it here 
                int valuesCount[10][3][N][2]; // TODO maybe flatten array, but then again maybe it does that by default
                for (int r = 1; r <= 10; r++) { // TODO ATTENTION I THINK ROUNDS START AT 1 !!
                    for (int t = 0; t < 3; t++) {
                        for (int op = 0; op < N; op++) {
                            valuesCount[r][t][op][0] = countDistinctProcessesForValue(op, 0, t, r);
                            valuesCount[r][t][op][1] = countDistinctProcessesForValue(op, 1, t, r);
                        }
                    }
                }
                send(-1, &valuesCount, sizeof(valuesCount), 0);
            }
            //close(connfd);
        }
        // #####################

        // Decision logic, update value

        int valueCount3[4] = {0, 0, 0, 0};
        for (int m = 0; m < roundsInfo[rnd].numValid; m++) {
            valueCount3[2*roundsInfo[rnd].validMsg[m].receivedValue + roundsInfo[rnd].validMsg[m].dval]++;
        }

        if (valueCount3[1] > 2*T) { // BUG ?
            value = 0;
            decision = value;
        } else if (valueCount3[3] > 2*T) {
            value = 1;
            decision = value;
        } else if (valueCount3[1] > T) {
            value = 0;
        } else if (valueCount3[3] > T) {
            value = 1;
        } else {
            value = randomBit();
        }

        // Process decided
        if (decision != -1) {
            printf("\n");
            printf("###############################\n");
            printf("###############################\n");
            printf("\n");
            printf("Process %d: Value %d decided.\n", processId, decision);
            printf("\n");
            printf("###############################\n");
            printf("###############################\n");
        }

        phase++;
    }

    return 0;
}
