#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>

// TODO verify msg formats everything compatible trivial just focus
// TODO for the whole forkId thing, check if the base case (msg coming from original
// processes + when there is only one sys state) is handled correctly

#define CONTROLLER_FEEDBACK_PATH "./controller_feedback_socket" // test sur simpleSendRecv si ca passe

#define CONTROLLER_PATH "./controller_socket"
#define MAXMSG 256

#define N 4 // Total number of processes

sem_t *sem;

// Controller that spawns processes, intercepts communications between
// the processes, and explore the execution state

// Message struct
typedef struct
{
  int type;   // send:0 or recv:1
  int from;   // -1 for recv msg, because can recv from any process
  int to;     // for recv msg this is the process that wants to recv
  int msg;    // for recv put -1
  int connfd; // -1 for send msg, because we don't keep the connection
  int forkId;
  int numDelivered;  // number of times it was delivered, always 0 or 1 for recv
  int delivered[10]; // forkIds where it was delivered
} Message;

// Spawn processes
pid_t spawn_process(const char *process_name, int processId, int initialValue)
{
  printf("[Controller] Create process %s\n", process_name);
  pid_t pid = fork();

  if (pid == -1)
  {
    perror("[Controller] fork");
    exit(EXIT_FAILURE);
  }

  if (pid == 0)
  {
    char processIdStr[10], initialValueStr[10];
    sprintf(processIdStr, "%d", processId);
    sprintf(initialValueStr, "%d", initialValue);
    // Set LD_PRELOAD so that our file intercepts function calls
    setenv("LD_PRELOAD", "./redirect.so", 1);
    execlp(process_name, process_name, processIdStr, initialValueStr, (char *)NULL);
    perror("[Controller] execlp");
    exit(EXIT_FAILURE);
  }
  return pid;
}

// Compares the state of the system
// Returns false if state1 != state2
bool compareState(int state1[N][2], int state2[N][2])
{
  for (int i = 0; i < N; i++)
  {
    for (int j = 0; j < 2; j++)
    {
      if (state1[i][j] != state2[i][j])
      {
        return false;
      }
    }
  }
  return true;
}

// Compares the state of 2 processes
// Returns false if processState1 != processState2
bool compareProcessState(int processState1[2], int processState2[2])
{
  for (int i = 0; i < 2; i++)
  {
    if (processState1[i] != processState2[i])
    {
      return false;
    }
  }
  return true;
}

// TODO actually state should be
typedef struct
{
  int len;            // len of forkPath
  pid_t forkPath[10]; // what should be max length ?

  // received value format :
  // { process i :
  //     {#0s i recv from different processes, #1s i recv from different processes},
  // }
  int valuesCount[N][2];
  int killed; // 1 if state was killed because redundant, 0 if not
} StateTODO;

typedef struct
{
  int len;            // len of forkPath
  pid_t forkPath[10]; // what should be max length ?
  int committedValues[N][2];
  int killed; // 1 if state was killed because redundant, 0 if not
} State;

int setupControllerSocket()
{
  int sockfd;
  struct sockaddr_un address;
  printf("[Controller] Create controller socket\n");
  if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Controller] receive socket");
    exit(EXIT_FAILURE);
  }
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_PATH, sizeof(address.sun_path) - 1);
  unlink(CONTROLLER_PATH);

  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address)) == -1)
  {
    perror("[Controller]  bind");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, 5) == -1)
  {
    perror("[Controller] listen");
    close(sockfd);
    exit(EXIT_FAILURE);
  }
  return sockfd;
}

bool canDeliver(int numStates, int posInForkPath, StateTODO *systemStates, int *statesToUpdate, Message *msgbuffer, int sendIndex, int recvIndex)
{
  // Need forkPath[0] = 0 or something to make sure msg from base broadcast always ok
  // Check if the message comes from a parallel execution/state,
  // in this case we don't want it
  bool forkOk = true;
  if (numStates > 1)
  { // check init 1 ou 0, if only 1 state then ofc accept

    // if fork id of send msg is before (or same as) the forkid of recv msg, ok
    forkOk = false;
    for (int f = 0; f < posInForkPath + 1; f++)
    {
      if (systemStates[statesToUpdate[0]].forkPath[f] == msgbuffer[sendIndex].forkId)
      {
        forkOk = true;
        break;
      }
    }
  }

  // Check if the send message was already delivered to this state
  bool sendDeliverOk = true;
  if (msgbuffer[sendIndex].numDelivered > 0)
  {

    for (int f = 0; f < msgbuffer[sendIndex].numDelivered; f++)
    {
      for (int g = 0; g < posInForkPath; g++)
      {
        if (msgbuffer[sendIndex].delivered[f] == systemStates[statesToUpdate[0]].forkPath[g])
        {
          sendDeliverOk = false;
          break;
        }
      }
    }
  }

  // Check if the recv message was already delivered
  bool recvDeliverOk = true;
  if (msgbuffer[sendIndex].numDelivered > 0)
  {
    recvDeliverOk = false;
  }
  return recvDeliverOk && sendDeliverOk && msgbuffer[sendIndex].type == 0 && msgbuffer[sendIndex].to == msgbuffer[recvIndex].to && forkOk;
}

int sendMsgToProcess(int connfd, int *message, int *newProcessState)
{
  send(connfd, &message, sizeof(message), 0);

  // Recover the resulting state
  // format [forkid, processState]
  int recmsg[3];
  ssize_t brec = recv(connfd, recmsg, sizeof(recmsg), 0);
  if (brec == -1)
  {
    perror("[Controller] recv state");
    exit(EXIT_FAILURE);
  }

  printf("[Controller] state recovered\n");
  // Useless but just to get the idea I will refactor after...
  newProcessState[0] = recmsg[1];
  newProcessState[1] = recmsg[2];
  int forkid = recmsg[0];
  return forkid;
}

void duplicateState(StateTODO *systemStates, int originState, int destState)
{
  // Copies the state to update into a new state object in the array of states
  for (int l = 0; l < N; l++)
  {
    for (int m = 0; m < 2; m++)
    {
      systemStates[destState].valuesCount[l][m] = systemStates[originState].valuesCount[l][m];
    }
  }

  for (int k = 0; k < systemStates[originState].len; k++)
  {
    systemStates[destState].forkPath[k] = systemStates[originState].forkPath[k];
  }

  systemStates[destState].len = systemStates[originState].len;
}

void updateState(StateTODO *systemStates, int stateToUpdate, int forkid, int *newProcessState, int updatedProcess)
{
  systemStates[stateToUpdate].forkPath[systemStates[stateToUpdate].len] = forkid;
  systemStates[stateToUpdate].len = systemStates[stateToUpdate].len + 1;
  for (int l = 0; l < 2; l++)
  {
    systemStates[stateToUpdate].valuesCount[updatedProcess][l] = newProcessState[l];
  }
}

void killStateAlreadyThere(int state, StateTODO *systemStates, int numStates, int killHandle)
{
  for (int z = 0; z < numStates; z++)
  {
    if (z == state || systemStates[z].killed == 1)
    {
      continue;
    }
    if (compareState(systemStates[z].valuesCount, systemStates[state].valuesCount))
    {

      kill(killHandle, SIGKILL);
      waitpid(killHandle, NULL, 0);
      // there I could send(connfd, kill msg with forkid0) instead of SIGKILL
      systemStates[state].killed = 1; // TODO need handle states that are killed
      // also I'm sure to kill the state just created, better (like no msg sent or whatev) ?
      // more or less efficient than doing it after ?
    }
  }
}

void printControllerState(StateTODO *systemStates, int numStates)
{
  printf("[Controller] Print Controller State :\n");
  for (int s = 0; s < numStates; s++)
  {
    printf("[Controller] State %d:\n", s);
    printf("[Controller] forkPath: ");
    for (int f = 0; f < systemStates[s].len; f++)
    {
      printf("%d/", systemStates[s].forkPath[f]);
    }
    printf("\n");
    printf("[Controller] values count: \n");
    for (int p = 0; p < N; p++)
    {
      printf("process %d : {", p);
      for (int v = 0; v < 2; v++)
      {
        printf("%d, ", systemStates[s].valuesCount[p][v]);
      }
      printf("}\n");
    }
  }
}

int main()
{
  // Array to store messages
  Message msgbuffer[100];

  // Array to store processes
  pid_t processes[100];
  int numProcesses = N;
  pid_t current_process;
  int current_process_index;

  // for now this is the state of the system
  int committedValues[N][2] = {{0, 0}};

  // What should be max number of system state that we can track in parallel ?
  StateTODO systemStates[50] = {
      // good or need init all inside ?
      {
          0,
          {0},
          {{0, 0}},
          0},
  };

  // Create the semaphore
  sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 0);
  if (sem == SEM_FAILED)
  {
    perror("Semaphore creation failed");
    exit(EXIT_FAILURE);
  }

  // Create controller socket to intercept processes communication
  // setupControllerSocket()
  int sockfd;
  struct sockaddr_un address;
  printf("[Controller] Create controller socket\n");
  if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Controller] receive socket");
    exit(EXIT_FAILURE);
  }
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_PATH, sizeof(address.sun_path) - 1);
  unlink(CONTROLLER_PATH);

  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address)) == -1)
  {
    perror("[Controller]  bind");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, 100) == -1) // Probablement bien plus que 5...
  {
    perror("[Controller] listen");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  // TEST
  int feedback_sockfd;
  struct sockaddr_un feedback_address;
  printf("[Controller] Create controller socket\n");
  if ((feedback_sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Controller] receive socket");
    exit(EXIT_FAILURE);
  }
  memset(&feedback_address, 0, sizeof(feedback_address));
  feedback_address.sun_family = AF_UNIX;
  strncpy(feedback_address.sun_path, CONTROLLER_FEEDBACK_PATH, sizeof(feedback_address.sun_path) - 1);
  unlink(CONTROLLER_FEEDBACK_PATH);

  if (bind(feedback_sockfd, (struct sockaddr *)&feedback_address, sizeof(feedback_address)) == -1)
  {
    perror("[Controller]  bind");
    close(feedback_sockfd);
    exit(EXIT_FAILURE);
  }

  if (listen(feedback_sockfd, 100) == -1)
  {
    perror("[Controller] listen");
    close(feedback_sockfd);
    exit(EXIT_FAILURE);
  }

  // Spawn processes
  /*
  for (int i = 0; i < N; i++)
  {
    processes[i] = spawn_process("bv_broadcast", i, 0);
  }
  */

  // Spawn child processes
  for (int i = 0; i < N; i++)
  {
    if ((processes[i] = fork()) == 0)
    {
      char processIdStr[10], initialValueStr[10];
      sprintf(processIdStr, "%d", i);
      sprintf(initialValueStr, "%d", 0);
      // Replace child process with BV-broadcast process
      setenv("LD_PRELOAD", "./redirect.so", 1);
      execl("./bv_broadcast", "bv_broadcast", processIdStr, initialValueStr, (char *)NULL);
      perror("execl failed");
      exit(EXIT_FAILURE); // Exit if execl fails
    }
  }

  // Wait until all processes have setup their sockets
  sleep(15);

  // Signal all children to proceed, but only allow 1
  for (int i = 0; i < N; i++)
  {
    if (i == 0) {
      sem_post(sem);
      printf("[Controller] Schedule process 0\n");
      current_process = processes[0];
      current_process_index = 0;
    } else {
      sem_post(sem);
      kill(processes[i], SIGSTOP);
    }
    
  }

  // Could start the execution of only 1 process to do 1 by 1 execution
  // Probably deal with that with SIGSTOP / SIGCONT

  // Events that make the whole thing progress = messages between processes
  // = messages intercepted by controller
  // Wait for messages to advance execution
  // = msg recveived : stop process that sent, exec/continue a process in array

  // format : [send:0/recv:1, from:processId/-1, to:processId, value:0/1, forkid]
  int receivedMessage[5];
  int connfd;
  int i = 0;
  int numStates = 1; // TODO maybe start at 1 and setup initial state...
  printf("[Controller] Listen for incoming messages\n");
  while (1)
  {
    connfd = accept(sockfd, NULL, NULL);
    if (connfd == -1) {
      perror("[Controller] Accept failure");
      exit(EXIT_FAILURE);
    }
    printf("[Controller] New connection\n");

    ssize_t len = recv(connfd, &receivedMessage, sizeof(receivedMessage), 0);

    if (len == -1)
        {
            perror("[Controller] Recv failure");
            exit(EXIT_FAILURE);
        }

    if (len > 0)
    {
      printf("[Controller] Something received : [t:%d, from:%d, to:%d, val:%d, forkid:%d]\n",
             receivedMessage[0], receivedMessage[1], receivedMessage[2], receivedMessage[3],
             receivedMessage[4]);

      // Store received message in the message array
      msgbuffer[i].type = receivedMessage[0];
      msgbuffer[i].from = receivedMessage[1];
      msgbuffer[i].to = receivedMessage[2];
      msgbuffer[i].msg = receivedMessage[3];
      msgbuffer[i].connfd = -1; // maybe put a default value
      msgbuffer[i].forkId = receivedMessage[4];
      msgbuffer[i].numDelivered = 0;
      for (int d = 0; d < 10; d++)
      {
        msgbuffer[i].delivered[d] = 0;
      }

      bool msg_was_delivered = false;

      if (receivedMessage[0] == 1)
      {
        // Recv message = a process wants to receive a message from another
        printf("[Controller] This is a recv message\n");
        msgbuffer[i].connfd = connfd;
        int r = 0;

        for (int j = 0; j < i; j++)
        {
          // Look through the message array if the message it wants is
          // already there

          // TODO handle init case no fork len =0 default forkid = 0 ? just make sure ok, need focus but ok

          // Get the system states to update

          int statesToUpdate[numStates];
          int numStatesToUpdate = 0;
          int posInForkPath = 0;
          if (numStates == 1) {
            numStatesToUpdate = 1;
            statesToUpdate[0] = 0;
          } else {
          for (int s = 0; s < numStates; s++)
          {
            // Don't want to update a state that was killed
            if (systemStates[s].killed == 1)
            {
              continue;
            }

            if (systemStates[s].len == 0)
            { // init le 1 elem de forkpath devrait etre 0
              systemStates[s].len = 1;
              systemStates[s].forkPath[0] = 0;
            }
            for (int f = 0; f < systemStates[s].len; f++)
            {
              if (systemStates[s].forkPath[f] == msgbuffer[i].forkId)
              {
                statesToUpdate[numStatesToUpdate++] = s;
                posInForkPath = f;
                break;
              }
            }
          }
        }

          if (numStatesToUpdate == 0)
          {
            // discard msg or something
            break;
          }

          printf("[Controller] numStatesToUpdate: %d, posInForkPath: %d\n", numStatesToUpdate, posInForkPath);
          printf("[Controller] States to update :");
          for (int s = 0; s < numStatesToUpdate; s++) {
            printf("%d,", statesToUpdate[s]);
          }
          printf("\n");

          // canDeliver(...)

          // Need forkPath[0] = 0 or something to make sure msg from base broadcast always ok
          // Check if the message comes from a parallel execution/state,
          // in this case we don't want it
          bool forkOk = true;
          if (numStates > 1)
          { // check init 1 ou 0, if only 1 state then ofc accept

            // if fork id of send msg is before (or same as) the forkid of recv msg, ok
            forkOk = false;
            for (int f = 0; f < posInForkPath + 1; f++)
            {
              if (systemStates[statesToUpdate[0]].forkPath[f] == msgbuffer[j].forkId)
              {
                forkOk = true;
                break;
              }
            }
          }

          // Check if the send message was already delivered to this state
          bool sendDeliverOk = true;
          if (numStates > 1) {
          if (msgbuffer[j].numDelivered > 0)
          {

            for (int f = 0; f < msgbuffer[j].numDelivered; f++)
            {
              for (int g = 0; g < posInForkPath; g++)
              {
                if (msgbuffer[j].delivered[f] == systemStates[statesToUpdate[0]].forkPath[g])
                {
                  sendDeliverOk = false;
                  break;
                }
              }
            }
          }
        }

          // Check if the recv message was already delivered
          bool recvDeliverOk = true;
          if (msgbuffer[j].numDelivered > 0)
          {
            recvDeliverOk = false;
          }

          if (recvDeliverOk && sendDeliverOk && msgbuffer[j].type == 0 && msgbuffer[j].to == msgbuffer[i].to && forkOk)
          {
            printf("[Controller] send msg to receiver\n");
            printf("msg:[t:%d, from:%d, to:%d, value:%d, connfd:%d, forkId:%d, numDelivered:%d]\n",
            msgbuffer[j].type, msgbuffer[j].from, msgbuffer[j].to, msgbuffer[j].msg, msgbuffer[j].connfd,
            msgbuffer[j].forkId, msgbuffer[j].numDelivered);
            if (msgbuffer[j].numDelivered > 0) {
              printf("delivered:[");
              for (int d = 0; d < msgbuffer[j].numDelivered; d++) {
                printf("%d, ", msgbuffer[j].delivered[d]);
              }
              printf("]\n");
            }

            msg_was_delivered = true;

            // recv msg always need to be delivered only once
            msgbuffer[i].delivered[msgbuffer[i].numDelivered] = msgbuffer[j].forkId;
            msgbuffer[i].numDelivered = msgbuffer[i].numDelivered + 1;

            // send msg might need to be sent to different states
            msgbuffer[j].delivered[msgbuffer[j].numDelivered] = msgbuffer[i].forkId;
            msgbuffer[j].numDelivered = msgbuffer[j].numDelivered + 1;

            // Try to send the message
            // format fork: [1, from:processId, value:0/1]
            // format kill: [2, -1, -1] maybe put which child to kill
            int message[3] = {1, msgbuffer[j].from, msgbuffer[j].msg};

            // sendMsgToProcess()
            send(connfd, &message, sizeof(message), 0);

            // Recover the resulting state
            // format [forkid, processState]
            int recmsg[3];
            int newProcessState[2];
            /*
            ssize_t brec = recv (connfd, recmsg, sizeof (recmsg), 0);
            if (brec == -1)
              {
                perror ("[Controller] recv state");
                exit (EXIT_FAILURE);
              }
              */

            // un truc comme ca pourrait etre une alternative
            // chatgpt says it works
            int feedback_connfd;
            if ((feedback_connfd = accept(feedback_sockfd, NULL, NULL)) != -1)
            {
              ssize_t brec = recv(feedback_connfd, recmsg, sizeof(recmsg), 0);
              if (brec == -1)
              {
                perror("[Controller] recv state");
                exit(EXIT_FAILURE);
              }
              close(feedback_connfd);
            }

            printf("[Controller] state recovered\n");
            // Useless but just to get the idea I will refactor after...
            newProcessState[0] = recmsg[1];
            newProcessState[1] = recmsg[2];
            int forkid0 = recmsg[0];
            processes[numProcesses] = forkid0;
            int forkid0_index = numProcesses;
            numProcesses = numProcesses + 1;
            kill(forkid0, SIGSTOP);
            printf("[Controller] process %d state is now {%d, %d} in forkid %d\n", msgbuffer[j].to, newProcessState[0], newProcessState[1], forkid0);

            // Everything after is if I change value and try to explore
            // For transparent stop here
            // Test transparent, test change value only 1 process...
            // (if msgbuffer[j].from == fixed process id)

            // Try to send the message with the opposite value
            printf("[Controller] send opposite msg to receiver\n");
            int opValue = 1 - msgbuffer[j].msg;
            int messageOp[3] = {1, msgbuffer[j].from, opValue};

            // sendMsgToProcess()
            ssize_t s3 = send(connfd, &messageOp, sizeof(messageOp), 0);
            if (s3 == -1)
            {
              perror("[Controller] send to fail");
              exit(EXIT_FAILURE);
            }

            // Recover the resulting state
            int recmsg2[3];
            int newProcessStateOp[2];
            // sleep (2);
            /*
            ssize_t brec4 = recv (connfd, recmsg2, sizeof (recmsg2), 0);
            if (brec4 == -1)
              {
                perror ("[Controller] recv to fail");
                exit (EXIT_FAILURE);
              }
              */

            int feedback_connfd2;
            if ((feedback_connfd2 = accept(feedback_sockfd, NULL, NULL)) != -1)
            {
              ssize_t brec4 = recv(feedback_connfd2, recmsg2, sizeof(recmsg2), 0);
              if (brec4 == -1)
              {
                perror("[Controller] recv state");
                exit(EXIT_FAILURE);
              }
              close(feedback_connfd2);
            }

            printf("[Controller] state recovered\n");
            // Useless but just to get the idea I will refactor after...
            newProcessStateOp[0] = recmsg2[1];
            newProcessStateOp[1] = recmsg2[2];
            int forkid1 = recmsg2[0];
            processes[numProcesses] = forkid1;
            int forkid1_index = numProcesses;
            numProcesses = numProcesses + 1;
            kill(forkid1, SIGSTOP);
            printf("[Controller] process %d state is now {%d, %d} in forkid %d\n", msgbuffer[j].to, newProcessStateOp[0], newProcessStateOp[1], forkid1);

            // I could just do it like normal and kill (everytime) when
            // I check if there are same sys states in controller ...
            if (compareProcessState(newProcessState, newProcessStateOp))
            {
              // Here I consider that I kill forkid1 by default
              

              printf("[Controller] Same result: kill a child\n");
              int killMessage[3] = {2, -1, -1};
              if (send(connfd, &killMessage, sizeof(killMessage), 0) == -1)
              {
                perror("[Controller] send fail");
                exit(EXIT_FAILURE);
              }

              // Update the system states
              for (int s = 0; s < numStatesToUpdate; s++)
              {

                // updateState()
                for (int l = 0; l < 2; l++)
                {
                  systemStates[statesToUpdate[s]].valuesCount[msgbuffer[i].to][l] = newProcessState[l];
                }
                // check that forkid0 is the right one
                // if too complicated just remove this whole thing, do both and delete with whole sys states
                // or recv which one i killed
                systemStates[statesToUpdate[s]].forkPath[systemStates[statesToUpdate[s]].len] = forkid0;
                systemStates[statesToUpdate[s]].len = systemStates[statesToUpdate[s]].len + 1;
                
              }
              // stop the current one ? it loops waiting for controller instructions anyway
              numProcesses = numProcesses - 1;
              processes[numProcesses] = -1; // "delete" forkid1
              kill(current_process, SIGSTOP);
              current_process = forkid0;
              current_process_index = forkid0_index;
              kill(forkid0, SIGCONT);
              printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);

            }
            else
            {

              // Copy sys state to update in 1 new state for each fork (en vrai besoin d'en add
              // 1 seul car le fork 0 peut etre le state original, just update forkpath du 1 aussi
              bool forkid0_killed = false;
              bool forkid1_killed = false;
              for (int s = 0; s < numStatesToUpdate; s++)
              {

                // Copies the state to update into a new state object in the array of states
                // duplicateState()
                for (int l = 0; l < N; l++)
                {
                  for (int m = 0; m < 2; m++)
                  {
                    systemStates[numStates].valuesCount[l][m] = systemStates[statesToUpdate[s]].valuesCount[l][m];
                  }
                }

                for (int k = 0; k < systemStates[statesToUpdate[s]].len; k++)
                {
                  systemStates[numStates].forkPath[k] = systemStates[statesToUpdate[s]].forkPath[k];
                }

                systemStates[numStates].len = systemStates[statesToUpdate[s]].len;

                // Update the states
                // updateState()
                systemStates[statesToUpdate[s]].forkPath[systemStates[statesToUpdate[s]].len] = forkid0;
                systemStates[statesToUpdate[s]].len = systemStates[statesToUpdate[s]].len + 1;
                for (int l = 0; l < 2; l++)
                {
                  systemStates[statesToUpdate[s]].valuesCount[msgbuffer[i].to][l] = newProcessState[l];
                }

                // updateState()
                systemStates[numStates].forkPath[systemStates[numStates].len] = forkid1;
                systemStates[numStates].len = systemStates[numStates].len + 1;
                for (int l = 0; l < 2; l++)
                {
                  systemStates[numStates].valuesCount[msgbuffer[i].to][l] = newProcessStateOp[l];
                }

                numStates = numStates + 1;

                // If the new system states are the same as some that are already stored, kill the new ones
                // killStateAlreadyThere()
                for (int z = 0; z < numStates; z++)
                {
                  if (z == statesToUpdate[s] || systemStates[z].killed == 1)
                  {
                    continue;
                  }
                  if (compareState(systemStates[z].valuesCount, systemStates[statesToUpdate[s]].valuesCount))
                  {
                    if (!forkid0_killed) {
                      kill(forkid0, SIGKILL);
                      waitpid(forkid0, NULL, 0);
                      forkid0_killed = true;
                    }

                    printf("[Controller] kill state %d on forkid %d\n", statesToUpdate[s], forkid0);
                    
                    // there I could send(connfd, kill msg with forkid0) instead of SIGKILL
                    systemStates[statesToUpdate[s]].killed = 1; // TODO need handle states that are killed
                    // also I'm sure to kill the state just created, better (like no msg sent or whatev) ?
                    // more or less efficient than doing it after ?
                  }
                }
                // killStateAlreadyThere
                for (int z = 0; z < numStates; z++)
                {
                  if (z == numStates - 1 || systemStates[z].killed == 1)
                  {
                    continue;
                  }
                  if (compareState(systemStates[z].valuesCount, systemStates[numStates - 1].valuesCount))
                  {
                    if (!forkid1_killed) {
                      kill(forkid1, SIGKILL);
                      waitpid(forkid1, NULL, 0);
                      forkid1_killed = true;
                    }

                    printf("[Controller] kill state %d on forkid %d\n", statesToUpdate[numStates - 1], forkid1);
                    
                    // there I could send(connfd, kill msg with forkid0) instead of SIGKILL
                    systemStates[numStates - 1].killed = 1; // TODO need handle states that are killed
                    // also I'm sure to kill the state just created, better (like no msg sent or whatev) ?
                    // more or less efficient than doing it after ?
                  }
                }
              }
              if (forkid0_killed) {
                numProcesses = numProcesses - 1;
                processes[numProcesses - 1] = processes[numProcesses]; // copy forkid1 in forkid0 place (overwrite forkid0)
                processes[numProcesses] = -1; // "delete" forkid1 : delete forkid0
                kill(current_process, SIGSTOP);
                current_process = forkid1;
                current_process_index = forkid1_index - 1;
                kill(forkid1, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid1);
              } else if (forkid1_killed) { // forkid0 and forkid1 should not both be killed
                numProcesses = numProcesses - 1;
                processes[numProcesses] = -1; // "delete" forkid1
                kill(current_process, SIGSTOP);
                current_process = forkid0;
                current_process_index = forkid0_index;
                kill(forkid0, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
              } else { // both are alive, just chose 1
                kill(current_process, SIGSTOP);
                current_process = forkid0;
                current_process_index = forkid0_index;
                kill(forkid0, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
              }

            }
            printControllerState(systemStates, numStates);
            close(connfd);
            break;
          }
          
        }

        // if the recv message was not delivered, schedule another process
        if (!msg_was_delivered) {
          printf("[Controller] recv msg was not delivered\n");
          kill(current_process, SIGSTOP);
          current_process_index = (current_process_index + 1) % numProcesses;
          current_process = processes[current_process_index];
          kill(current_process, SIGCONT);
          printf("[Controller] scheduling process %d on forkId %d\n", current_process_index, current_process);
        }
          
      }

      if (receivedMessage[0] == 0)
      {
        // This is a send message : a process sends some data to another
        printf("[Controller] This is a send message\n");
        kill(current_process, SIGSTOP);
        // Go through the message buffer to see if the process waiting for this
        // data is already there

        for (int j = 0; j < i; j++)
        {

          // Get the system state to update

          int statesToUpdate[numStates];
          int numStatesToUpdate = 0;
          int posInForkPath = 0;
          if (numStates == 1) {
            numStatesToUpdate = 1;
            statesToUpdate[0] = 0;
          } else {
          for (int s = 0; s < numStates; s++)
          {
            // Don't want to update a state that was killed
            if (systemStates[s].killed == 1)
            {
              continue;
            }
            if (systemStates[s].len == 0)
            { // init le 1 elem de forkpath devrait etre 0
              systemStates[s].len = 1;
            }
            for (int f = 0; f < systemStates[s].len; f++)
            {
              if (systemStates[s].forkPath[f] == msgbuffer[j].forkId)
              {
                statesToUpdate[numStatesToUpdate++] = s;
                posInForkPath = f;
                break;
              }
            }
          }
        }

          if (numStatesToUpdate == 0)
          {
            // discard msg or something
            break;
          }

          printf("[Controller] numStatesToUpdate: %d, posInForkPath: %d\n", numStatesToUpdate, posInForkPath);
          printf("[Controller] States to update :");
          for (int s = 0; s < numStatesToUpdate; s++) {
            printf("%d,", statesToUpdate[s]);
          }
          printf("\n");

          // canDeliver()

          // Check that the receiver is not in a parallel execution
          bool forkOk = true;
          if (numStates > 1)
          { // check init 1 ou 0, if only 1 state then ofc accept

            // if fork id of send msg is before (or same as) the forkid of recv msg, ok
            forkOk = false;
            for (int f = 0; f < posInForkPath + 1; f++)
            {
              if (systemStates[statesToUpdate[0]].forkPath[f] == msgbuffer[i].forkId)
              {
                forkOk = true;
                break;
              }
            }
          }

          // Check if the send message was already delivered to this state
          bool sendDeliverOk = true;
          if (numStates > 1) {
          if (msgbuffer[i].numDelivered > 0)
          {

            for (int f = 0; f < msgbuffer[i].numDelivered; f++)
            {
              for (int g = 0; g < posInForkPath; g++)
              {
                if (msgbuffer[i].delivered[f] == systemStates[statesToUpdate[0]].forkPath[g])
                {
                  sendDeliverOk = false;
                  break;
                }
              }
            }
          }
        }

          // Check if the recv message was already delivered
          bool recvDeliverOk = true;
          if (msgbuffer[j].numDelivered > 0)
          {
            recvDeliverOk = false;
          }

          // Found a recv message from the process that the send msg is addressed to
          if (recvDeliverOk && sendDeliverOk && forkOk && msgbuffer[j].type == 1 && msgbuffer[j].to == msgbuffer[i].to)
          {
            printf("[Controller] send msg to receiver\n");
            printf("send msg:[t:%d, from:%d, to:%d, value:%d, connfd:%d, forkId:%d, numDelivered:%d]\n",
            msgbuffer[i].type, msgbuffer[i].from, msgbuffer[i].to, msgbuffer[i].msg, msgbuffer[i].connfd,
            msgbuffer[i].forkId, msgbuffer[i].numDelivered);
            if (msgbuffer[i].numDelivered > 0) {
              printf("delivered:[");
              for (int d = 0; d < msgbuffer[i].numDelivered; d++) {
                printf("%d, ", msgbuffer[i].delivered[d]);
              }
              printf("]\n");
            }
            printf("to recv msg:[t:%d, from:%d, to:%d, value:%d, connfd:%d, forkId:%d, numDelivered:%d]\n",
            msgbuffer[j].type, msgbuffer[j].from, msgbuffer[j].to, msgbuffer[j].msg, msgbuffer[j].connfd,
            msgbuffer[j].forkId, msgbuffer[j].numDelivered);


            kill(current_process, SIGSTOP); // In case the msg is delivered several times
            msg_was_delivered = true;

            // TODO update same as above

            // recv msg always need to be delivered only once
            msgbuffer[j].delivered[msgbuffer[j].numDelivered] = msgbuffer[i].forkId;
            msgbuffer[j].numDelivered = msgbuffer[j].numDelivered + 1;

            // send msg might need to be sent to different states
            msgbuffer[i].delivered[msgbuffer[i].numDelivered] = msgbuffer[j].forkId;
            msgbuffer[i].numDelivered = msgbuffer[i].numDelivered + 1;

            if (msgbuffer[j].forkId == 0) {
              current_process_index = msgbuffer[j].to;
              current_process = processes[current_process_index];
            } else {
              current_process = msgbuffer[j].forkId;
              for (int p = 0; p < numProcesses; p++) {
                if (processes[p] == current_process) {
                  current_process_index = p;
                  break;
                }
              }
            }
            kill(current_process, SIGCONT);
            printf("[Controller] Schedule process %d on forkId %d to send instructions\n", current_process_index, current_process);

            // Try to send the message
            int message[3] = {1, msgbuffer[i].from, msgbuffer[i].msg};
            // sendMsgToProcess()
            int s1 = send(msgbuffer[j].connfd, &message, sizeof(message), 0);
            if (s1 == -1)
            {
              perror("[Controller] send fail");
              exit(EXIT_FAILURE);
            }

            // Recover the resulting state
            int recmsg[3];
            int newProcessState[2];
            /*
            ssize_t brec = recv (msgbuffer[j].connfd, recmsg, sizeof (recmsg), 0);
            if (brec == -1)
              {
                perror ("[Controller] recv state");
                exit (EXIT_FAILURE);
              }
              */

            int feedback_connfd;
            if ((feedback_connfd = accept(feedback_sockfd, NULL, NULL)) != -1)
            {
              ssize_t brec = recv(feedback_connfd, recmsg, sizeof(recmsg), 0);
              if (brec == -1)
              {
                perror("[Controller] recv state");
                exit(EXIT_FAILURE);
              }
              close(feedback_connfd);
            }

            printf("[Controller] state recovered\n");
            newProcessState[0] = recmsg[1];
            newProcessState[1] = recmsg[2];
            int forkid0 = recmsg[0];
            processes[numProcesses] = forkid0;
            int forkid0_index = numProcesses;
            numProcesses = numProcesses + 1;
            kill(forkid0, SIGSTOP);
            printf("[Controller] process %d state is now {%d, %d} in forkid %d\n", msgbuffer[i].to, newProcessState[0], newProcessState[1], forkid0);

            // Everything after is if I change value and try to explore
            // For transparent stop here
            // Test transparent, test change value only 1 process...
            // (if msgbuffer[j].from == fixed process id)

            // Try to send the message with opposite value
            printf("[Controller] send opposite msg to receiver\n");
            int opValue = 1 - msgbuffer[i].msg;
            int messageOp[3] = {1, msgbuffer[i].from, opValue};
            // sendMsgToProcess()
            ssize_t s3 = send(msgbuffer[j].connfd, &messageOp, sizeof(messageOp), 0);
            if (s3 == -1)
            {
              perror("[Controller] send to fail");
              exit(EXIT_FAILURE);
            }
            // Recover the resulting state
            int recmsg2[3];
            int newProcessStateOp[2];
            // sleep (2);
            /*
            ssize_t brec4 = recv (msgbuffer[j].connfd, recmsg2, sizeof (recmsg2), 0);
            if (brec4 == -1)
              {
                perror ("[Controller] recv to fail");
                exit (EXIT_FAILURE);
              }
              */

            int feedback_connfd2;
            if ((feedback_connfd2 = accept(feedback_sockfd, NULL, NULL)) != -1)
            {
              ssize_t brec4 = recv(feedback_connfd2, recmsg2, sizeof(recmsg2), 0);
              if (brec4 == -1)
              {
                perror("[Controller] recv state");
                exit(EXIT_FAILURE);
              }
              close(feedback_connfd2);
            }

            printf("[Controller] state recovered\n");
            // Useless but just to get the idea I will refactor after...
            newProcessStateOp[0] = recmsg2[1];
            newProcessStateOp[1] = recmsg2[2];
            int forkid1 = recmsg2[0];
            processes[numProcesses] = forkid1;
            int forkid1_index = numProcesses;
            numProcesses = numProcesses + 1;
            kill(forkid1, SIGSTOP);
            printf("[Controller] process %d state is now {%d, %d} in forkid %d\n", msgbuffer[i].to, newProcessStateOp[0], newProcessStateOp[1], forkid1);

            if (compareProcessState(newProcessState, newProcessStateOp))
            {
              // Here I consider that I kill forkid1 by default

              printf("[Controller] Same result: kill a child\n");
              int killMessage[3] = {2, -1, -1};
              if (send(msgbuffer[j].connfd, &killMessage, sizeof(killMessage), 0) == -1)
              {
                perror("[Controller] send fail");
                exit(EXIT_FAILURE);
              }

              // Update the system states
              for (int s = 0; s < numStatesToUpdate; s++)
              {
                // updateState()
                for (int l = 0; l < 2; l++)
                {
                  systemStates[statesToUpdate[s]].valuesCount[msgbuffer[j].to][l] = newProcessState[l];
                }
                // check that forkid0 is the right one
                // if too complicated just remove this whole thing, do both and delete with whole sys states
                // or recv which one i killed
                systemStates[statesToUpdate[s]].forkPath[systemStates[statesToUpdate[s]].len] = forkid0;
                systemStates[statesToUpdate[s]].len = systemStates[statesToUpdate[s]].len + 1;

              }
              numProcesses = numProcesses - 1;
              processes[numProcesses] = -1; // "delete" forkid1
              kill(current_process, SIGSTOP);
              current_process = forkid0;
              current_process_index = forkid0_index;
              kill(forkid0, SIGCONT);
              printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid0);

            }
            else
            {

              // Copy sys state to update in 1 new state for each fork (en vrai besoin d'en add
              // 1 seul car le fork 0 peut etre le state original, just update forkpath du 1 aussi
              bool forkid0_killed = false;
              bool forkid1_killed = false;
              for (int s = 0; s < numStatesToUpdate; s++)
              {

                // Copies the state to update into a new state object in the array of states
                // duplicateState()
                for (int l = 0; l < N; l++)
                {
                  for (int m = 0; m < 2; m++)
                  {
                    systemStates[numStates].valuesCount[l][m] = systemStates[statesToUpdate[s]].valuesCount[l][m];
                  }
                }

                for (int k = 0; k < systemStates[statesToUpdate[s]].len; k++)
                {
                  systemStates[numStates].forkPath[k] = systemStates[statesToUpdate[s]].forkPath[k];
                }

                systemStates[numStates].len = systemStates[statesToUpdate[s]].len;

                // Update the states
                // updateState()
                systemStates[statesToUpdate[s]].forkPath[systemStates[statesToUpdate[s]].len] = forkid0;
                systemStates[statesToUpdate[s]].len = systemStates[statesToUpdate[s]].len + 1;
                for (int l = 0; l < 2; l++)
                {
                  systemStates[statesToUpdate[s]].valuesCount[msgbuffer[j].to][l] = newProcessState[l];
                }

                // updateState()
                systemStates[numStates].forkPath[systemStates[numStates].len] = forkid1;
                systemStates[numStates].len = systemStates[numStates].len + 1;
                for (int l = 0; l < 2; l++)
                {
                  systemStates[numStates].valuesCount[msgbuffer[j].to][l] = newProcessStateOp[l];
                }

                numStates = numStates + 1;

                // If the new system states are the same as some that are already stored, kill the new ones
                // killStateAlreadyThere()
                for (int z = 0; z < numStates; z++)
                {
                  if (z == statesToUpdate[s] || systemStates[z].killed == 1)
                  {
                    continue;
                  }
                  if (compareState(systemStates[z].valuesCount, systemStates[statesToUpdate[s]].valuesCount))
                  {

                    if (!forkid0_killed) {
                      kill(forkid0, SIGKILL);
                      waitpid(forkid0, NULL, 0);
                      forkid0_killed = true;
                    }
                    printf("[Controller] kill state %d on forkid %d\n", statesToUpdate[s], forkid0);
                    
                    // there I could send(connfd, kill msg with forkid0) instead of SIGKILL
                    systemStates[statesToUpdate[s]].killed = 1; // TODO need handle states that are killed
                    // also I'm sure to kill the state just created, better (like no msg sent or whatev) ?
                    // more or less efficient than doing it after ?
                  }
                }

                // killStateAlreadyThere()
                for (int z = 0; z < numStates; z++)
                {
                  if (z == numStates - 1 || systemStates[z].killed == 1)
                  {
                    continue;
                  }
                  if (compareState(systemStates[z].valuesCount, systemStates[numStates - 1].valuesCount))
                  {

                    if (!forkid1_killed) {
                      kill(forkid1, SIGKILL);
                      waitpid(forkid1, NULL, 0);
                      forkid1_killed = true;
                    }
                    printf("[Controller] kill state %d on forkid %d\n", statesToUpdate[numStates - 1], forkid1);
                    
                    // there I could send(connfd, kill msg with forkid0) instead of SIGKILL
                    systemStates[numStates - 1].killed = 1; // TODO need handle states that are killed
                    // also I'm sure to kill the state just created, better (like no msg sent or whatev) ?
                    // more or less efficient than doing it after ?
                  }
                }
              }
              if (forkid0_killed) {
                numProcesses = numProcesses - 1;
                processes[numProcesses - 1] = processes[numProcesses]; // copy forkid1 in forkid0 place (overwrite forkid0)
                processes[numProcesses] = -1; // "delete" forkid1 : delete forkid0
                kill(current_process, SIGSTOP);
                current_process = forkid1;
                current_process_index = forkid1_index - 1;
                kill(forkid1, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid1);
              } else if (forkid1_killed) { // forkid0 and forkid1 should not both be killed
                numProcesses = numProcesses - 1;
                processes[numProcesses] = -1; // "delete" forkid1
                kill(current_process, SIGSTOP);
                current_process = forkid0;
                current_process_index = forkid0_index;
                kill(forkid0, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid0);
              } else { // both are alive, just chose 1
                kill(current_process, SIGSTOP);
                current_process = forkid0;
                current_process_index = forkid0_index;
                kill(forkid0, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid0);
              }


            }
            printControllerState(systemStates, numStates);
            // attention
            close(msgbuffer[j].connfd);
            // Since this is a send message, there could be other recv messages waiting to be delivered this msg
          }
        }

        // if the send message was not delivered, schedule another process
        if (!msg_was_delivered) {
          printf("[Controller] send msg was not delivered\n");
          current_process_index = (current_process_index + 1) % numProcesses;
          current_process = processes[current_process_index];
          kill(current_process, SIGCONT);
          printf("[Controller] scheduling process %d on forkId %d\n", current_process_index, current_process);
        }
        close(connfd);
      }
      i++;

    }
  }

  // accept is blocking so this is never reached

  close(sockfd);
  unlink(CONTROLLER_PATH);

  while (wait(NULL) != -1)
    ;

  sem_close(sem);
  sem_unlink("/sem_bv_broadcast"); // Cleanup the semaphore

  return 0;
}
