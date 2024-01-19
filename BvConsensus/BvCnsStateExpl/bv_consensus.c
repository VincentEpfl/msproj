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

#define NUM_ROUNDS 1

sem_t *sem;
sem_t *sem_init_brd;

// Message type
// format (est/aux, round, source process , val, destination process)
typedef struct 
{
int type;
int roundNum;
int senderId;
int receivedValue;
int destinationId;
} Message;

// State for 1 round of the consensus
typedef struct 
{
int received_values[N][2]; // Values received by each process with the EST flag
int hasBroadcasted[2];  // Flags to check if the process has already broadcasted the values 0 and 1
int committedValues[2]; // Flags to check if the process has committed the values 0 and 1

int received_values_aux[N][2]; // Values received by each process with the AUX flag
int numMsg;
Message msgbuffer[10]; // Buffer of messages for this round that have arrived at an earlier round
} RoundInfo;

// State of all the rounds
RoundInfo roundsInfo[NUM_ROUNDS] = {
    {
        {{0, 0}},
        {0, 0},
        {0, 0},
        {{0, 0}},
        0,
        {
            {
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

int est; // Current estimate of the decision
int rnd = 0; // Current round number

// This process has reached a decision
bool decided = false;
int decided_value = -1;

int seed = 0;

// Common "random" number generator
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

// How many different processes sent a certain value with flag EST 
// to this process at a certain round
int countDistinctProcessesForValue(int value, int roundNumber)
{
    int count = 0;
    for (int i = 0; i < N; i++)
    {
        if (roundsInfo[roundNumber].received_values[i][value] == 1)
        {
            count++;
        }
    }
    return count;
}

// How many different processes sent a certain value with flag AUX 
// to this process at a certain round
int countDistinctProcessesForValueAux(int value, int roundNumber)
{
    int count = 0;
    for (int i = 0; i < N; i++)
    {
        if (roundsInfo[roundNumber].received_values_aux[i][value] == 1)
        {
            count++;
        }
    }
    return count;
}

// Condition to get out of the AUX waiting loop
bool waitcondition(int * values, int roundNumber) {
    //printf("#distinct 0 from AUX msg : %d\n", countDistinctProcessesForValueAux(0, roundNumber));
    //printf("#distinct 1 from AUX msg : %d\n", countDistinctProcessesForValueAux(1, roundNumber));
    //printf("committed value 0 ? %d (YES = 1)\n", roundsInfo[roundNumber].committedValues[0]);
    //printf("committed value 1 ? %d (YES = 1)\n", roundsInfo[roundNumber].committedValues[1]);
    bool cond = false;
    // BUG N - T -> N - T - 1 
    if (countDistinctProcessesForValueAux(0, roundNumber) >= N - T && roundsInfo[roundNumber].committedValues[0] == 1) {
        values[0] = 1;
        cond = true;
    }
    if (countDistinctProcessesForValueAux(1, roundNumber) >= N - T && roundsInfo[roundNumber].committedValues[1] == 1) {
        values[1] = 1;
        cond = true;
    }
    return cond;
}

// Simple broadcast 
void broadcast(int value, int roundNumber) {
    int sockfd;
    struct sockaddr_in serverAddr;

    printf("Process %d: Start broadcast\n", processId);
    
    for (int i = 0; i < N; i++)
    {
        if (i == processId)
        {
            // Send value to itself + receive
            //printf("Process %d, Round %d : Value %d sent to myself with tag AUX\n", processId, rnd, value);
            roundsInfo[roundNumber].received_values_aux[processId][value] = 1;
            continue;
        }

        // ATTENTION AuX tag 0 here but 1 in msg
        int message[5] = {0, rnd, processId, value, i}; // format (est/aux, round, source, val, destination)

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
        
        // TO HELP TRIGGER BUG
        //if (processId != 1) {
           //usleep(100000);
        //}
        send(sockfd, &message, sizeof(message), 0);
        //printf("Process %d, Round %d : Value %d sent to process %d with tag AUX\n", processId, rnd, value, i);
        //close(sockfd);
    }
}

// Process a received message with AUX tag
void processMessages(int value, int fromProcess, int roundNumber)
{
    roundsInfo[roundNumber].received_values_aux[fromProcess][value] = 1;

    roundsInfo[roundNumber].received_values_aux[processId][value] = 1; // Send value to itself, I think necessary (not clear in paper though)
       
}

// BV broadcast
void BV_broadcast(int value, int roundNumber) 
{
    int sockfd;
    struct sockaddr_in serverAddr;
    //int message[2] = {processId, value}; // First element is the process ID, second is the value

    printf("Process %d: Start BV broadcast\n", processId);
    roundsInfo[roundNumber].hasBroadcasted[value] = 1; // Mark the value as broadcasted. Mettre apres fin broadcast + clean (il y avait une raison de mettre avant mais now no)

    for (int i = 0; i < N; i++)
    {
        if (i == processId)
        {
            // Send value to itself + receive
            //printf("Process %d, Round %d : Value %d sent to myself with tag EST\n", processId, rnd, value);
            roundsInfo[roundNumber].received_values[processId][value] = 1;
            int distinctCount = countDistinctProcessesForValue(value, roundNumber);
            //printf("Process %d Value %d distinct count: %d\n", processId, value, distinctCount);
            // BUG Introduce bug 2T -> 2T - 1
            if (distinctCount > 2 * T && !roundsInfo[roundNumber].committedValues[value])
            {
                //printf("Process %d commits value %d\n", processId, value);
                roundsInfo[roundNumber].committedValues[value] = 1; // Mark the value as committed
            }
            
            continue;
        }

        int message[5] = {1, rnd, processId, value, i}; // format (est/aux, round, source, val, destination)

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
        
        
        // TO HELP TRIGGER BUG
        //usleep(100000);
        if (processId != 1) {
            usleep(5000);
        }
        send(sockfd, &message, sizeof(message), 0);

        //printf("Process %d, Round %d : Value %d sent to process %d with tag EST\n", processId, rnd, value, i);

        //close(sockfd);
    }
}

// Process a received message with EST tag
void BVprocessMessages(int value, int fromProcess, int roundNumber)
{
    roundsInfo[roundNumber].received_values[fromProcess][value] = 1;
    int distinctCount = countDistinctProcessesForValue(value, roundNumber);
    //printf("Process %d Value %d distinct count: %d\n", processId, value, distinctCount);
    // BUG Introduce bug 2T -> 2T - 1
    if (distinctCount > 2 * T && !roundsInfo[roundNumber].committedValues[value])
    {
        //printf("Process %d commits value %d\n", processId, value);
        roundsInfo[roundNumber].committedValues[value] = 1; // Mark the value as committed
        
    }
    if (distinctCount > T && !roundsInfo[roundNumber].hasBroadcasted[value])
    {
        BV_broadcast(value, roundNumber);
    }
    
}

// Process any received message
int processAllMessages(int type, int r, int value, int fromProcess, int toProcess, int roundNumber) {
    if (r > rnd) { 
        //printf("Store msg in buffer\n");
        roundsInfo[r].msgbuffer[roundsInfo[r].numMsg].type = type;
        roundsInfo[r].msgbuffer[roundsInfo[r].numMsg].roundNum = r;
        roundsInfo[r].msgbuffer[roundsInfo[r].numMsg].senderId = fromProcess;
        roundsInfo[r].msgbuffer[roundsInfo[r].numMsg].receivedValue = value;
        roundsInfo[r].msgbuffer[roundsInfo[r].numMsg].destinationId = toProcess;
        roundsInfo[r].numMsg = roundsInfo[r].numMsg + 1;
        return -1;
    }
    if (r < rnd) { // TODO make sure its not better to just process the msg and who cares 
        return -1;
    }
    if (type == 1) { // EST - BV broadcast
        BVprocessMessages(value, fromProcess, roundNumber);
    } else if (type == 0) { // AUX - broadcast
        processMessages(value, fromProcess, roundNumber);
    } else {
        perror("[Process] Wrong message type");
        exit(EXIT_FAILURE);
    }
    return 0;

}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <processId (0 to %d)> <initialValue (0 or 1)>\n", argv[0], N - 1);
        exit(1);
    }

    sem = sem_open("/sem_bv_consensus", O_CREAT, 0644, 0);
    if (sem == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }
    sem_init_brd = sem_open("/sem_bv_consensus_init_brd", O_CREAT, 0644, 0);
    if (sem_init_brd == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }


    processId = atoi(argv[1]);
    int initialValue = atoi(argv[2]);

    if (initialValue != 0 && initialValue != 1)
    {
        printf("[Process] Initial value must be 0 or 1.\n");
        exit(1);
    }

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

    if (listen(listenfd, 10) == -1)
    {
        perror("[Process] Listen failure");
        exit(EXIT_FAILURE);
    }

    printf("Process %d with initial value %d listening on port %d...\n", processId, initialValue, PORT_BASE + processId);
    
    sem_wait(sem);
    printf("Process %d done waiting for sockets init\n", processId);
    sem_close(sem);

    // Initialize estimate with the proposed value
    est = initialValue;

    while(1) {

        if (rnd > NUM_ROUNDS - 1) {
            printf("END : SHOULD BE ENOUGH ROUNDS\n");
            //break;
            while (1) {

                int receivedMessage[5];
                int nbytes = recv(listenfd, &receivedMessage, sizeof(receivedMessage), 0); // connfd-listenfd
                if (nbytes == -1)
                {
                    perror("[Process] Recv failure");
                    exit(EXIT_FAILURE);
                }

                // TODO maybe should still processs message dans tous les cas ca ferait pas plus de broadcast
                // mais le state renvoye juste apres serait + correct

                int valuesCount[NUM_ROUNDS][2][2]; 
                for (int r = 0; r < NUM_ROUNDS; r++) { 
                    valuesCount[r][0][0] = countDistinctProcessesForValue(0, r);
                    valuesCount[r][0][1] = countDistinctProcessesForValue(1, r);

                    valuesCount[r][1][0] = countDistinctProcessesForValueAux(0, r);
                    valuesCount[r][1][1] = countDistinctProcessesForValueAux(1, r);
                }
                
                char feedbackMessage[sizeof(decided_value) + sizeof(valuesCount)];
                memcpy(feedbackMessage, &decided_value, sizeof(decided_value));
                memcpy(feedbackMessage + sizeof(decided_value), valuesCount, sizeof(valuesCount));

                send(-1, &feedbackMessage, sizeof(feedbackMessage), 0);

            }
        }
        
        //printf("Process %d : Round %d, est = %d\n", processId, rnd, est);

        // BV broadcast

        // Broadcast the initial value
        BV_broadcast(est, rnd);

        if (rnd == 0) {
            sem_wait(sem_init_brd);
            printf("Process %d done waiting for broadcast init\n", processId);
            sem_close(sem_init_brd);
        }

        int receivedMessage[5]; // format (est/aux, round, source, val, destination)
        int n = 0;

        //printf("Enter EST loop\n");

        // EST waiting loop
        while (roundsInfo[rnd].committedValues[0] == 0 && roundsInfo[rnd].committedValues[1] == 0)
        {
            // First, process the messages of this round that were received in previous rounds 
            bool msgFound = false;
            for (int m = n; m < roundsInfo[rnd].numMsg; m++) {
                if (roundsInfo[rnd].msgbuffer[m].destinationId == processId
                && roundsInfo[rnd].msgbuffer[m].roundNum == rnd
                && roundsInfo[rnd].msgbuffer[m].type == 1
                ) {
                    processAllMessages(1, rnd, roundsInfo[rnd].msgbuffer[m].receivedValue, roundsInfo[rnd].msgbuffer[m].senderId, processId, rnd);
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
            
            int type = receivedMessage[0];
            int r = receivedMessage[1];
            int senderId = receivedMessage[2];
            int receivedValue = receivedMessage[3];
            int destinationId = receivedMessage[4];
            //printf("Process %d: Value %d received from process %d, round %d, type %d (1=EST)\n", processId, receivedValue, senderId, r, type);
            if (receivedValue >= 0)
            { // Ignore special signals
                int err = processAllMessages(type, r, receivedValue, senderId, destinationId, rnd);

                // This is where it registers its state to the controller 
                // TODO check if ok to do it here 
                int valuesCount[NUM_ROUNDS][2][2]; 
                for (int r = 0; r < NUM_ROUNDS; r++) { 
                    valuesCount[r][0][0] = countDistinctProcessesForValue(0, r);
                    valuesCount[r][0][1] = countDistinctProcessesForValue(1, r);

                    // ATTENTION AuX tag 1 here but 0 in msg
                    valuesCount[r][1][0] = countDistinctProcessesForValueAux(0, r);
                    valuesCount[r][1][1] = countDistinctProcessesForValueAux(1, r);
                }
                char feedbackMessage[sizeof(decided_value) + sizeof(valuesCount)];
                memcpy(feedbackMessage, &decided_value, sizeof(decided_value));
                memcpy(feedbackMessage + sizeof(decided_value), valuesCount, sizeof(valuesCount));

                send(-1, &feedbackMessage, sizeof(feedbackMessage), 0);

                if (err == -1) { // TODO make sure its not better to just process the msg and who cares 
                    //close(connfd);
                    continue;
                }
            }
            //close(connfd);
        }

        //printf("Out of EST loop\n");

        // Broadcast

        int w;
        if (roundsInfo[rnd].committedValues[0] == 1) {
            w = 0;
        } else {
            w = 1;
        }

        broadcast(w, rnd);

        //printf("Enter AUX loop\n");

        int values[2] = {0, 0};
        int n2 = 0;

        // AUX waiting loop
        while (!waitcondition(values, rnd))
        {
            // First, process the messages of this round that were received in previous rounds 
            bool msgFound = false;
            for (int m = n2; m < roundsInfo[rnd].numMsg; m++) {
                if (roundsInfo[rnd].msgbuffer[m].destinationId == processId
                && roundsInfo[rnd].msgbuffer[m].roundNum == rnd
                ) {
                    processAllMessages(roundsInfo[rnd].msgbuffer[m].type, rnd, roundsInfo[rnd].msgbuffer[m].receivedValue, roundsInfo[rnd].msgbuffer[m].senderId, processId, rnd);
                    // TODO UPDATE STATE CONTROLLER HERE ? (In the previous loop if its the last one (pre check condition) 
                    // then dont send feedback there but wait until it goes here, and went through ALL msgs in buffer
                    // to send feedback... 1 option I guess)
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
            
            int type = receivedMessage[0];
            int r = receivedMessage[1];
            int senderId = receivedMessage[2];
            int receivedValue = receivedMessage[3];
            int destinationId = receivedMessage[4];
            //printf("Process %d: Value %d received from process %d, round %d, type %d (1=EST)\n", processId, receivedValue, senderId, r, type);
            if (receivedValue >= 0)
            { // Ignore special signals
                int err = processAllMessages(type, r, receivedValue, senderId, destinationId, rnd);

                // This is where it registers its state to the controller 
                // TODO check if ok to do it here 
                int valuesCount[NUM_ROUNDS][2][2]; 
                for (int r = 0; r < NUM_ROUNDS; r++) { 
                    valuesCount[r][0][0] = countDistinctProcessesForValue(0, r);
                    valuesCount[r][0][1] = countDistinctProcessesForValue(1, r);

                    valuesCount[r][1][0] = countDistinctProcessesForValueAux(0, r);
                    valuesCount[r][1][1] = countDistinctProcessesForValueAux(1, r);
                }
                char feedbackMessage[sizeof(decided_value) + sizeof(valuesCount)];
                memcpy(feedbackMessage, &decided_value, sizeof(decided_value));
                memcpy(feedbackMessage + sizeof(decided_value), valuesCount, sizeof(valuesCount));

                send(-1, &feedbackMessage, sizeof(feedbackMessage), 0);

                if (err == -1) { // TODO make sure its not better to just process the msg and who cares 
                    //close(connfd);
                    continue;
                }
            }
            //close(connfd);
        }

        //printf("Out of AUX loop\n");
        //printf("values = [%d, %d]\n", values[0], values[1]);

        int s = randomBit(); // Get the common coin value for this round
        //printf("s = %d\n", s);

        // Decision logic based on received values and common coin
        if (values[0] && values[1]) {
            // Both values have been committed, adopt common coin value
            est = s;
        } else if (values[s]) {
            if (!decided) {
                // Decide on the value if it matches the common coin
                decided = true;
                decided_value = s;
                //printf("#############################################\n");
                printf("Process %d decides on value %d\n", processId, s);
                //printf("#############################################\n");
            }
            est = s;
        } else {
            // Adopt the value that has been committed
            est = 1 - s; //est = v != s
        }
        
        rnd++;
    }

    return 0;
}
