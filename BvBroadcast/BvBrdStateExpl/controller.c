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

// Controller that spawns processes, intercepts communications between
// the processes, and explore the execution state

#define CONTROLLER_FEEDBACK_PATH "./controller_feedback_socket"
#define CONTROLLER_PATH "./controller_socket"
#define MAXMSG 256

#define N 3 // Total number of processes
#define T 0 // Maximum number of Byzantine processes

// Message struct
typedef struct
{
  int type;   // send:0 or recv:1
  int from;   // -1 for recv msg, because can recv from any process
  int to;     // for recv msg this is the process that wants to recv
  int msg;    // for recv put -1
  int connfd; // -1 for send msg, because we don't keep the connection
  int forkId;
  int echo;           // 0 if not an echo msg, 1 if echo msg, -1 for recv msg
  int numDelivered;   // number of times it was delivered, always 0 or 1 for recv
  int delivered[500]; // forkIds where it was delivered
} Message;

// Message struct
typedef struct
{
  int from;   // -1 for recv msg, because can recv from any process
  int to;     // for recv msg this is the process that wants to recv
  int msg;    // for recv put -1
  int forkId;
} MessageTrace;

typedef struct
{
  int len;             // len of forkPath
  pid_t forkPath[500]; // what should be max length ?

  // received value format :
  // { process i :
  //     {#0s i recv from different processes, #1s i recv from different processes},
  // }
  int valuesCount[N][2];
  int killed; // 1 if state was killed because redundant, 0 if not
} State;

sem_t *sem;
sem_t *sem_init_brd;

// Array to store messages
Message msgbuffer[1000];
MessageTrace msghistory[1000];
int nummsg = 0;

// Array to store processes
pid_t processes[10000];
int numProcesses = N;
pid_t current_process;
int current_process_index;

// What should be max number of system state that we can track in parallel ?
State systemStates[1000] = {
    // good or need init all inside ?
    {
        0,
        {0},
        {{0, 0}},
        0},
};

int sockfd;
int feedback_sockfd;
int numStates = 1;
int numStatesKilled = 0;

int numOpenFd = 0;
int fds[1000];

int get_states_to_update(int *res, int *statesToUpdate, int recv_msg_index)
{
  int numStatesToUpdate = 0;
  int posInForkPath = 0;

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
      if (systemStates[s].forkPath[f] == msgbuffer[recv_msg_index].forkId)
      {
        statesToUpdate[numStatesToUpdate++] = s;
        posInForkPath = f;
        break;
      }
    }
  }

  if (numStatesToUpdate == 0)
  {
    // discard msg or something
    return -1;
  }

  res[0] = numStatesToUpdate;
  res[1] = posInForkPath;
  return 0;
}

void put_msg_in_buffer(int index, int *receivedMessage)
{
  msgbuffer[index].type = receivedMessage[0];
  msgbuffer[index].from = receivedMessage[1];
  msgbuffer[index].to = receivedMessage[2];
  msgbuffer[index].msg = receivedMessage[3];
  msgbuffer[index].connfd = -1;
  msgbuffer[index].forkId = receivedMessage[4];
  msgbuffer[index].echo = receivedMessage[5];
  msgbuffer[index].numDelivered = 0;
  for (int d = 0; d < 500; d++)
  {
    msgbuffer[index].delivered[d] = 0;
  }
}

int initSocket(bool feedback)
{
  bool timeout = !feedback;
  struct sockaddr_un address;
  int sockfd;

  if (feedback)
  {
    printf("[Controller] Create feedback socket\n");
  }
  else
  {
    printf("[Controller] Create controller socket\n");
  }

  if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Controller] receive socket");
    exit(EXIT_FAILURE);
  }

  if (timeout)
  {
    struct timeval tv; // timeval structure to set the timeout

    // Set the timeout value
    tv.tv_sec = 0;       // 1 seconds timeout
    tv.tv_usec = 10000; // 500000 microseconds

    // Set the timeout option
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv))
    {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;

  if (feedback)
  {
    strncpy(address.sun_path, CONTROLLER_FEEDBACK_PATH, sizeof(address.sun_path) - 1);
    unlink(CONTROLLER_FEEDBACK_PATH);
  }
  else
  {
    strncpy(address.sun_path, CONTROLLER_PATH, sizeof(address.sun_path) - 1);
    unlink(CONTROLLER_PATH);
  }

  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address)) == -1)
  {
    perror("[Controller]  bind");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, 1000) == -1) // max queue capacity might be an issue ?
  {
    perror("[Controller] listen");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  return sockfd;
}

void spawnProcesses()
{
  for (int i = 0; i < N; i++)
  {
    if ((processes[i] = fork()) == 0)
    {
      char processIdStr[10], initialValueStr[10];
      sprintf(processIdStr, "%d", i);
      if (i < 2)
      {
        sprintf(initialValueStr, "%d", 0);
      }
      else
      {
        sprintf(initialValueStr, "%d", 0);
      }

      // Replace child process with BV-broadcast process
      setenv("LD_PRELOAD", "./redirect.so", 1);
      execl("./bv_broadcast", "bv_broadcast", processIdStr, initialValueStr, (char *)NULL);
      perror("execl failed");
      exit(EXIT_FAILURE); // Exit if execl fails
    }
  }

  // Wait until all processes have setup their sockets
  sleep(2);

  // Signal all children to proceed, but only allow 1
  for (int i = 0; i < N; i++)
  {
    if (i == 0)
    {
      sem_post(sem);
      // printf("[Controller] Schedule process 0\n");
      // current_process = processes[0];
      // current_process_index = 0;
    }
    else
    {
      sem_post(sem);
      // kill(processes[i], SIGSTOP);
    }
  }

  // Wait until all processes have done init broadcast TEMPORARY
  sleep(5);

  // Signal all children to proceed, but only allow 1
  for (int i = 0; i < N; i++)
  {
    if (i == 0)
    {
      sem_post(sem_init_brd);
      printf("[Controller] Schedule process 0\n");
      current_process = processes[0];
      current_process_index = 0;
    }
    else
    {
      sem_post(sem_init_brd);
      kill(processes[i], SIGSTOP);
    }
  }
}

void init()
{
  systemStates[0].len = 1;
  systemStates[0].forkPath[0] = 0;

  // Create the semaphore
  sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 0);
  if (sem == SEM_FAILED)
  {
    if (errno == EEXIST)
    {
      // Semaphore already exists, try to unlink and create again
      printf("Semaphore already exists, trying to recreate it.\n");
      if (sem_unlink("/sem_bv_broadcast") == -1)
      {
        perror("Error unlinking semaphore");
        exit(EXIT_FAILURE);
      }
      sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 1);
      if (sem == SEM_FAILED)
      {
        perror("Error creating semaphore after unlinking");
        exit(EXIT_FAILURE);
      }
    }
    else
    {
      // Some other error occurred
      perror("Error creating semaphore");
      exit(EXIT_FAILURE);
    }
  }

  // Create the semaphore init brd
  sem_init_brd = sem_open("/sem_bv_broadcast_init_brd", O_CREAT, 0644, 0);
  if (sem_init_brd == SEM_FAILED)
  {
    if (errno == EEXIST)
    {
      // Semaphore already exists, try to unlink and create again
      printf("Semaphore already exists, trying to recreate it.\n");
      if (sem_unlink("/sem_bv_broadcast_init_brd") == -1)
      {
        perror("Error unlinking semaphore");
        exit(EXIT_FAILURE);
      }
      sem_init_brd = sem_open("/sem_bv_broadcast_init_brd", O_CREAT, 0644, 1);
      if (sem == SEM_FAILED)
      {
        perror("Error creating semaphore after unlinking");
        exit(EXIT_FAILURE);
      }
    }
    else
    {
      // Some other error occurred
      perror("Error creating semaphore");
      exit(EXIT_FAILURE);
    }
  }

  // Create controller socket to intercept processes communication
  // setupControllerSocket()

  sockfd = initSocket(false);
  feedback_sockfd = initSocket(true);

  // Spawn child processes
  spawnProcesses();
}

void schedule_new_process()
{
  kill(current_process, SIGSTOP);
  current_process_index = (current_process_index + 1) % numProcesses;
  current_process = processes[current_process_index];
  kill(current_process, SIGCONT);
  usleep(10000);
  // printf("[Controller] scheduling process %d on forkId %d\n", current_process_index, current_process);
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

bool checkStateValid(int state[N][2])
{
  // TODO do that except for the byzantines processes (technically that's the property)
  // TODO check the other properties
  int committed_values[N][2];
  for (int i = 0; i < N; i++)
  {
    for (int j = 0; j < 2; j++)
    {
      if (state[i][j] > 2 * T)
      { // Have to modify for bug
        committed_values[i][j] = 1;
      }
      else
      {
        committed_values[i][j] = 0;
      }
    }
  }
  bool valid = true;
  if (committed_values[0][0] == 0)
  {
    for (int i = 1; i < N; i++)
    {
      if (committed_values[i][0] != 0)
      {
        valid = false;
      }
    }
  }
  else
  {
    for (int i = 1; i < N; i++)
    {
      if (committed_values[i][0] != 1)
      {
        valid = false;
      }
    }
  }
  if (committed_values[0][1] == 0)
  {
    for (int i = 1; i < N; i++)
    {
      if (committed_values[i][1] != 0)
      {
        valid = false;
      }
    }
  }
  else
  {
    for (int i = 1; i < N; i++)
    {
      if (committed_values[i][1] != 1)
      {
        valid = false;
      }
    }
  }
  return valid;
}

bool checkAllStates()
{
  bool invalid = false;
  for (int s = 0; s < numStates; s++)
  {
    if (systemStates[s].killed == 1)
    {
      continue;
    }

    if (!checkStateValid(systemStates[s].valuesCount))
    {
      invalid = true;
      // maybe shut down everything, every process etc
      printf("[Controller] INVALID STATE FOUND\n");
      printf("State %d:\n", s);
      printf("forkPath: ");
      for (int f = 0; f < systemStates[s].len; f++)
      {
        printf("%d/", systemStates[s].forkPath[f]);
      }
      printf("\n");
      printf("values count: \n");
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
  if (!invalid)
  {
    printf("[Controller] NO INVALID STATE FOUND\n");
  }
}

void deliver_message(int delivered, int to)
{
  msgbuffer[delivered].delivered[msgbuffer[delivered].numDelivered] = msgbuffer[to].forkId;
  msgbuffer[delivered].numDelivered = msgbuffer[delivered].numDelivered + 1;
}

void printMessage(int index)
{
  printf("msg:[t:%d, from:%d, to:%d, value:%d, connfd:%d, forkId:%d, numDelivered:%d]\n",
         msgbuffer[index].type, msgbuffer[index].from, msgbuffer[index].to, msgbuffer[index].msg, msgbuffer[index].connfd,
         msgbuffer[index].forkId, msgbuffer[index].numDelivered);
  if (msgbuffer[index].numDelivered > 0)
  {
    printf("delivered:[");
    for (int d = 0; d < msgbuffer[index].numDelivered; d++)
    {
      printf("%d, ", msgbuffer[index].delivered[d]);
    }
    printf("]\n");
  }
}

bool canDeliverState(int posInForkPath, int stateToUpdate, int sendIndex, int recvIndex)
{
  //  Check if the message comes from a parallel execution/state,
  //  in this case we don't want it
  bool forkOk = true;
  if (numStates > 1) // Possible que ca soit le cas mais que ca se voit pas car ordre msg exec ok...
  {
    // if fork id of send msg is before (or same as) the forkid of recv msg, ok
    forkOk = false;
    for (int f = 0; f < systemStates[stateToUpdate].len; f++) // TODO f < posinforkpath + 1 -> all
    {
      // printf("[CONTROLLER TEST] state fork %d / send msg fork %d\n", systemStates[statesToUpdate[0]].forkPath[f], msgbuffer[sendIndex].forkId);
      if (systemStates[stateToUpdate].forkPath[f] == msgbuffer[sendIndex].forkId)
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
      for (int g = 0; g < posInForkPath + 1; g++) // TODO < pos in fork path OR just all ? (then maybe no need forkpath...)
      {
        // printf("[CONTROLLER TEST] send msg delivered %d\n", msgbuffer[sendIndex].delivered[f]);
        if (msgbuffer[sendIndex].delivered[f] == systemStates[stateToUpdate].forkPath[g])
        {
          sendDeliverOk = false;
          break;
        }
      }
    }
  }
  //}

  // Check if the recv message was already delivered
  // bool recvDeliverOk = true;
  // if (msgbuffer[recvIndex].numDelivered > 0)
  //{
  // recvDeliverOk = false;

  /*
  for (int f = 0; f < msgbuffer[recvIndex].numDelivered; f++)
  {
    for (int g = 0; g < posInForkPath + 1; g++) // TODO < pos in fork path OR just all ? (then maybe no need forkpath...)
    {
      if (msgbuffer[recvIndex].delivered[f] == systemStates[stateToUpdate].forkPath[g])
      {
        recvDeliverOk = false;
        break;
      }
    }
  }
  */
  //}
  // return recvDeliverOk && ...
  return sendDeliverOk && msgbuffer[sendIndex].type == 0 && msgbuffer[recvIndex].type == 1 && msgbuffer[sendIndex].to == msgbuffer[recvIndex].to && forkOk;
}

bool canDeliverRecvState(int stateToUpdate, int recvIndex)
{
  bool recvDeliver = true;
  /*
  if (msgbuffer[recvIndex].numDelivered > 0)
  {
    for (int f = 0; f < msgbuffer[recvIndex].numDelivered; f++)
    {
      for (int g = 0; g < systemStates[stateToUpdate].len; g++) // TODO < pos in fork path OR just all ? (then maybe no need forkpath...)
      {
        if (msgbuffer[recvIndex].delivered[f] == systemStates[stateToUpdate].forkPath[g])
        {
          recvDeliver = false;
          break;
        }
      }
    }
  }
  */

 // RECV1
  if (msgbuffer[recvIndex].numDelivered > 0) {
    recvDeliver = false;
  }

  return recvDeliver;
}

void sendMsgToProcess(int connfd, const void *message, int msglen, void *recmsg, int recmsglen)
{
  int *messageint = (int *)message;
  printf("[Controller] Send msg %d %d %d\n", messageint[0], messageint[1], messageint[2]);
  int nbs = send(connfd, message, msglen, 0);
  printf("[Controller] %d bytes sent\n", nbs);
  if (nbs < 0)
  {
    perror("[Controller] send failure");
    exit(EXIT_FAILURE);
  }

  // Recover the resulting state
  // format [forkid, processState]

  int feedback_connfd;
  if ((feedback_connfd = accept(feedback_sockfd, NULL, NULL)) < 0)
  {
    //close(feedback_connfd); TODO necessary ?
    perror("[Controller] accept failure");
    exit(EXIT_FAILURE);
  }
  else
  {
    int nb = recv(feedback_connfd, recmsg, recmsglen, 0);
    if (nb < 0)
    {
      perror("[Controller] recv state feedback socket failure");
      exit(EXIT_FAILURE);
    } else if (nb == 0) {
      printf("[Controller] ERROR FEEDBACK RECEIVE NOTHING\n");
    }
    close(feedback_connfd);
  }
}

void sendMsgAndRecvState(int connfd, const void *message, int msglen, int send_msg_index, void *newProcessState, void *forkInfo)
{
  // format fork: [1, from:processId, value:0/1]
  // format kill: [2, -1, -1] maybe put which child to kill
  int recmsg[3];
  sendMsgToProcess(connfd, message, msglen, &recmsg, sizeof(recmsg));

  //printf("[Controller] state recovered\n");
  int *newProcessStateInt = (int *)newProcessState;
  newProcessStateInt[0] = recmsg[1];
  newProcessStateInt[1] = recmsg[2];
  int *forkInfoInt = (int *)forkInfo;
  forkInfoInt[0] = recmsg[0];
  forkInfoInt[1] = numProcesses;
  processes[numProcesses++] = forkInfoInt[0];
  kill(forkInfoInt[0], SIGSTOP);
  printf("[Controller] process %d state is now {%d, %d} in forkid %d\n", msgbuffer[send_msg_index].to, newProcessStateInt[0], newProcessStateInt[1], forkInfoInt[0]);
}

void duplicateState(int originState, int destState)
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

void updateState(int stateToUpdate, int forkid, int *newProcessState, int updatedProcess)
{
  systemStates[stateToUpdate].forkPath[systemStates[stateToUpdate].len] = forkid;
  systemStates[stateToUpdate].len = systemStates[stateToUpdate].len + 1;
  for (int l = 0; l < 2; l++)
  {
    systemStates[stateToUpdate].valuesCount[updatedProcess][l] = newProcessState[l];
  }
}

bool killStateAlreadyThere(int state, int numStates, int killHandle, bool forkid_killed)
{
  bool forkid_killed_temp = forkid_killed;
  for (int z = 0; z < numStates; z++)
  {
    if (z == state || systemStates[z].killed == 1)
    {
      continue;
    }
    if (compareState(systemStates[z].valuesCount, systemStates[state].valuesCount))
    {

      if (!forkid_killed_temp)
      {
        kill(killHandle, SIGTERM); // TODO SIGTERM OR SIGKILL
        //waitpid(killHandle, NULL, 0); // necessary ?
        forkid_killed_temp = true;
      }
      //printf("[Controller] kill state %d on forkid %d\n", state, killHandle);

      // there I could send(connfd, kill msg with forkid0) instead of SIGKILL
      systemStates[state].killed = 1;
      numStatesKilled = numStatesKilled + 1;
    }
  }
  return forkid_killed_temp;
}

void printControllerState(State *systemStates, int numStates)
{
  printf("[Controller] Print Controller State :\n");
  for (int s = 0; s < numStates; s++)
  {
    if (systemStates[s].killed == 1)
    {
      continue;
    }
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
    printf("[Controller] messages exchanged: \n");
    for (int f = 0; f < systemStates[s].len; f++)
    {
      for (int g = 0; g < nummsg; g++) {
        if (msghistory[g].forkId == systemStates[s].forkPath[f]) {
          printf("Message %d ", msghistory[g].msg);
          printf("from %d ", msghistory[g].from);
          printf("to %d\n", msghistory[g].to);
        }
      }
      
    }
  }
}

void addMsgToHistory(int forkid, int from, int to, int val) {
        msghistory[nummsg].forkId = forkid;
        msghistory[nummsg].from = from;
        msghistory[nummsg].to = to;
        msghistory[nummsg].msg = val;
        nummsg = nummsg + 1;
}

int handleMessagePair(int recvIndex, int sendIndex, int fd, bool recv)
{
  int connfd = 0;
  if (recv) {
    connfd = fd;
  } else {
    connfd = msgbuffer[recvIndex].connfd;
  }

  // Look through the message array if the message it wants is
  // already there

  // Get the system states to update

  int statesToUpdateTemp[numStates];
  int res[2];
  if (get_states_to_update(res, statesToUpdateTemp, recvIndex) == -1)
  {
    return -1; // continue;
  }
  int numStatesToUpdateTemp = res[0];
  int posInForkPath = res[1];

  int statesToUpdateTemp2[numStatesToUpdateTemp];
  int numStatesToUpdateTemp2 = 0;
  for (int s = 0; s < numStatesToUpdateTemp; s++)
  {
    if (canDeliverRecvState(statesToUpdateTemp[s], recvIndex))
    {
      statesToUpdateTemp2[numStatesToUpdateTemp2++] = statesToUpdateTemp[s];
    }
  }

  int statesToUpdate[numStatesToUpdateTemp2];
  int numStatesToUpdate = 0;
  int statesNoAction[numStatesToUpdateTemp2];
  int numStatesNoAction = 0;
  for (int s = 0; s < numStatesToUpdateTemp2; s++)
  {
    if (canDeliverState(systemStates[statesToUpdateTemp2[s]].len - 1, statesToUpdateTemp2[s], sendIndex, recvIndex)) // 1 posinforkpath attention len - 1 to compensate pos+1 in fct
    {
      statesToUpdate[numStatesToUpdate++] = statesToUpdateTemp2[s];
    }
    else
    { // verify that this includes the right states (I checked once seems ok)
      statesNoAction[numStatesNoAction++] = statesToUpdateTemp2[s];
    }
  }

  if (numStatesToUpdate != 0)
  // if (canDeliver(statesToUpdate, numStatesToUpdate, j, i))
  {
    /*
    printf("[Controller] send msg to receiver\n");
    printMessage(sendIndex);
    printf("to recv : \n");
    printMessage(recvIndex);

    printf("[Controller] numStatesToUpdate: %d, posInForkPath: %d\n", numStatesToUpdate, posInForkPath);
    printf("[Controller] States to update :");
    printf("[");
    for (int s = 0; s < numStatesToUpdate; s++)
    {
      printf("%d,", statesToUpdate[s]);
    }
    printf("]\n");

    if (numStatesNoAction > 0)
    {
      printf("[Controller] There are states in recv msg range that cant be updated:\n");
      printf("[");
      for (int s = 0; s < numStatesNoAction; s++)
      {
        printf("%d,", statesNoAction[s]);
      }
      printf("]\n");
    } 
    */

    kill(current_process, SIGSTOP); // it's possible the current process didn't send this recv msg

    //msg_was_delivered = true; TODO handle with return value

    // recv msg always need to be delivered only once TODO not necessarily
    deliver_message(recvIndex, sendIndex);

    // send msg might need to be sent to different states
    deliver_message(sendIndex, recvIndex);

    // schedule the process that sent the recv message and is waiting for controller instructions

    if (msgbuffer[recvIndex].forkId == 0)
    {
      current_process_index = msgbuffer[recvIndex].to;
      current_process = processes[current_process_index];
    }
    else
    {
      current_process = msgbuffer[recvIndex].forkId;
      for (int p = 0; p < numProcesses; p++)
      {
        if (processes[p] == current_process)
        {
          current_process_index = p;
          break;
        }
      }
    }
    kill(current_process, SIGCONT);
    usleep(10000);
    // printf("[Controller] Schedule process %d on forkId %d to send instructions\n", current_process_index, current_process);

    // If send message is an echo message (first check forkid != 0 then check echo tag I guess)
    //  send(connfd, msg[instr:delivernothing])
    // recv(connfd) -> forkid (je peux le faire direct sur cette socket)
    // add forkid0 to states to update as normal, add this forkid to the other states
    // do that here, also in the case where exploration (echo msg from p1/p3), same just
    // add the option to not deliver
    // also check kill state etc
    int newProcessStateNoAction[2];
    int forkInfoNoAction[2];
    int forkidNoAction;
    int forkidNoAction_index;
    if (msgbuffer[sendIndex].echo == 1 && numStatesNoAction > 0)
    {
      /*
      printf("[Controller] Received an echo message, try this\n");
      printf("[Controller] States with no action :");
      printf("[");
      for (int s = 0; s < numStatesNoAction; s++)
      {
        printf("%d,", statesNoAction[s]);
      }
      printf("]\n");
      */
      int messageNoAction[4] = {3, msgbuffer[sendIndex].from, msgbuffer[sendIndex].msg, msgbuffer[sendIndex].to};
      sendMsgAndRecvState(connfd, &messageNoAction, sizeof(messageNoAction), sendIndex, &newProcessStateNoAction, &forkInfoNoAction);
      forkidNoAction = forkInfoNoAction[0];
      forkidNoAction_index = forkInfoNoAction[1];

      // Update the system states This doesnt act on the same state than the rest so should compose fine
      for (int s = 0; s < numStatesNoAction; s++)
      {
        // just update forkpath and len
        updateState(statesNoAction[s], forkidNoAction, newProcessStateNoAction, msgbuffer[recvIndex].to);
        // actual state should not change so no need to kill
      }

      // here no need to update msghistory because for this id no message was received
    }

    // Try to send the message

    int newProcessState[2];
    int forkInfo[2];
    int message[4] = {1, msgbuffer[sendIndex].from, msgbuffer[sendIndex].msg, msgbuffer[sendIndex].to};
    sendMsgAndRecvState(connfd, &message, sizeof(message), sendIndex, &newProcessState, &forkInfo);
    int forkid0 = forkInfo[0];
    int forkid0_index = forkInfo[1];

    // add msg to history
    addMsgToHistory(forkid0, msgbuffer[sendIndex].from, msgbuffer[sendIndex].to, msgbuffer[sendIndex].msg);

    if (true) // msgbuffer[sendIndex].from == 2  msgbuffer[sendIndex].from == 3
    {
      // Try to send the message with the opposite value
      //printf("[Controller] send opposite msg to receiver\n");
      int opValue = 1 - msgbuffer[sendIndex].msg;
      int messageOp[4] = {1, msgbuffer[sendIndex].from, opValue, msgbuffer[sendIndex].to};
      int newProcessStateOp[2];
      int forkInfoOp[2];
      sendMsgAndRecvState(connfd, &messageOp, sizeof(messageOp), sendIndex, &newProcessStateOp, &forkInfoOp);
      int forkid1 = forkInfoOp[0];
      int forkid1_index = forkInfoOp[1];

      if (compareProcessState(newProcessState, newProcessStateOp))
      {
        // Here I consider that I kill forkid1 by default

        //printf("[Controller] Same result: kill a child\n");
        int killMessage[4] = {2, -1, -1, -1};
        if (send(connfd, &killMessage, sizeof(killMessage), 0) == -1)
        {
          perror("[Controller] send fail");
          exit(EXIT_FAILURE);
        }

        bool forkid0_killed = false;
        // Update the system states
        for (int s = 0; s < numStatesToUpdate; s++)
        {
          updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[recvIndex].to);
          // probablement ajouter kill state... si kill forkid0 just schedule un autre...
          forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
        }

        // stop the current one ? it loops waiting for controller instructions anyway
        numProcesses = numProcesses - 1;
        processes[numProcesses] = -1; // "delete" forkid1

        if (forkid0_killed)
        {
          numProcesses = numProcesses - 1;
          processes[numProcesses] = -1; // "delete" forkid0
          schedule_new_process();
        }
        else
        { // to be fair maybe also use schedule
          kill(current_process, SIGSTOP);
          // waiting_processes[num_waiting_processes++] = current_process;
          current_process = forkid0;
          current_process_index = forkid0_index;
          kill(forkid0, SIGCONT);
          usleep(10000);
          // printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
        }
      }
      else
      {
        // add op msg to history
        addMsgToHistory(forkid1, msgbuffer[sendIndex].from, msgbuffer[sendIndex].to, opValue);

        // Copy sys state to update in 1 new state for each fork
        bool forkid0_killed = false;
        bool forkid1_killed = false;
        for (int s = 0; s < numStatesToUpdate; s++)
        {

          // Copies the state to update into a new state object in the array of states
          duplicateState(statesToUpdate[s], numStates);

          // Update the states
          updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[recvIndex].to);
          updateState(numStates, forkid1, newProcessStateOp, msgbuffer[recvIndex].to);

          numStates = numStates + 1;

          // If the new system states are the same as some that are already stored, kill the new ones
          forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
          forkid1_killed = killStateAlreadyThere(numStates - 1, numStates, forkid1, forkid1_killed);
        }
        if (forkid0_killed && forkid1_killed)
        {
          numProcesses = numProcesses - 1;
          processes[numProcesses] = -1;
          numProcesses = numProcesses - 1;
          processes[numProcesses] = -1;
          schedule_new_process();
        }
        else if (forkid0_killed)
        {
          numProcesses = numProcesses - 1;
          processes[numProcesses - 1] = processes[numProcesses]; // copy forkid1 in forkid0 place (overwrite forkid0)
          processes[numProcesses] = -1;                          // "delete" forkid1 : delete forkid0
          kill(current_process, SIGSTOP);
          // waiting_processes[num_waiting_processes++] = current_process;
          current_process = forkid1;
          current_process_index = forkid1_index - 1;
          kill(forkid1, SIGCONT);
          usleep(10000);
          // printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid1);
        }
        else if (forkid1_killed)
        {
          numProcesses = numProcesses - 1;
          processes[numProcesses] = -1; // "delete" forkid1
          kill(current_process, SIGSTOP);
          // waiting_processes[num_waiting_processes++] = current_process;
          current_process = forkid0;
          current_process_index = forkid0_index;
          kill(forkid0, SIGCONT);
          usleep(10000);
          // printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
        }
        else
        { // both are alive, just chose 1
          kill(current_process, SIGSTOP);
          // waiting_processes[num_waiting_processes++] = current_process;
          current_process = forkid0;
          current_process_index = forkid0_index;
          kill(forkid0, SIGCONT);
          usleep(10000);
          // printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
        }
      }
    }
    else
    { // end if p = 3 for expl
      // In this case we only transmit the message with its actual value
      // Update the system states
      bool forkid0_killed = false;
      for (int s = 0; s < numStatesToUpdate; s++)
      {
        updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[recvIndex].to);
        forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
      }

      if (forkid0_killed)
      {
        numProcesses = numProcesses - 1;
        processes[numProcesses] = -1; // "delete" forkid0
      }
      schedule_new_process();
    }
    //printControllerState(systemStates, numStates);
    // checkAllStates();
    // close(connfd); // We might need it later since several send can be sent to one deliver
    // break; // In fact can have several send delivered to one recv...
    return 1; // msg was delivered 
  }
return 0; // msg was not delivered
}

int main()
{

  init();

  // Events that make the whole thing progress = messages between processes
  // = messages intercepted by controller
  // Wait for messages to advance execution
  // = msg recveived : stop process that sent, exec/continue a process in array

  // format : [send:0/recv:1, from:processId/-1, to:processId, value:0/1, forkid]
  int receivedMessage[6];
  int connfd;
  int i = 0;
  int noNewConnection = 0;
  int nothingDelivered = 0;
  printf("[Controller] Listen for incoming messages\n");
  while (1)
  {
    if (nothingDelivered > 150)
    {
      break;
    }
    if ((connfd = accept(sockfd, NULL, NULL)) < 0)
    {
      // Check if the error was due to a timeout
      if (errno == EWOULDBLOCK || errno == EAGAIN)
      {
        // printf("[Controller] No connections within the timeout period.\n");
        usleep(10000);
        schedule_new_process();
        noNewConnection = noNewConnection + 1;
        if (noNewConnection > 300)
        {
          break;
        }
      }
      else
      {
        // An error occurred that wasn't a timeout
        perror("[Controller] Accept failure");
        exit(EXIT_FAILURE);
      }
    }
    else
    {
      printf("[Controller] NUM PROCESSES : %d\n", numProcesses);
      fds[numOpenFd] = connfd;
      numOpenFd++;
      printf("[Controller] NUM FD : %d\n", numOpenFd);


      printf("[Controller] New connection\n");
      noNewConnection = 0;
      ssize_t len = recv(connfd, &receivedMessage, sizeof(receivedMessage), 0);
      if (len == 0)
      {
        //perror("[Controller] Recv failure len == 0");
        //exit(EXIT_FAILURE);
        printf("[Controller] NOTHING MORE TO RECV\n");
        break;
      }
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
        put_msg_in_buffer(i, receivedMessage);
        bool msg_was_delivered = false;

        if (receivedMessage[0] == 1)
        {
          // Recv message = a process wants to receive a message from another
          //printf("[Controller] This is a recv message\n");
          // kill(current_process, SIGSTOP); //*
          msgbuffer[i].connfd = connfd;
          int r = 0;

          for (int j = 0; j < i; j++)
          {
            int del = 0;
            del = handleMessagePair(i, j, connfd, true);
            if (del == -1) {
              continue;
            } 
            if (del == 1) {
              msg_was_delivered = true;
            }
          }

          // if the recv message was not delivered, schedule another process
          if (!msg_was_delivered)
          {
            nothingDelivered = nothingDelivered + 1;
            printf("[Controller] recv msg was not delivered\n");
            schedule_new_process();
            usleep(10000);
          }
          else
          {
            nothingDelivered = 0;
            printf("[Controller] Number of states we went through : %d\n", numStates);
            printf("[Controller] Number of states we killed : %d\n", numStatesKilled);
          }
          // RECV1
          //close(connfd); TODO TRY NO CLOSE OBVIOUSLY NOT OPTIMAL
          //numOpenFd = numOpenFd - 1;
        }

        if (receivedMessage[0] == 0)
        {
          // This is a send message : a process sends some data to another
          //printf("[Controller] This is a send message\n");
          // kill(current_process, SIGSTOP);
          //  Go through the message buffer to see if the process waiting for this
          //  data is already there

          for (int j = 0; j < i; j++)
          {
            int del = 0;
            del = handleMessagePair(j, i, connfd, false);
            if (del == -1) {
              continue;
            } 
            if (del == 1) {
              msg_was_delivered = true;
            }
          }

          // if the send message was not delivered, schedule another process
          if (!msg_was_delivered)
          {
            nothingDelivered = nothingDelivered + 1;
            printf("[Controller] send msg was not delivered\n");
            schedule_new_process();
            usleep(10000);
          }
          else
          {
            nothingDelivered = 0;
            printf("[Controller] Number of states we went through : %d\n", numStates);
            printf("[Controller] Number of states we killed : %d\n", numStatesKilled);
          }
          //close(connfd); TODO TRY NO CLOSE OBVIOUSLY NOT OPTIMAL
          //numOpenFd = numOpenFd - 1;
        }
        i++;
      }
    }
  }

  printControllerState(systemStates, numStates);
  checkAllStates();
  for (int m1 = 0; m1 < i; m1++)
  {
    for (int m2 = 0; m2 < i; m2++)
    {
      if (m1 == m2)
      {
        continue;
      }
      if (msgbuffer[m1].type == 1)
      { // m1 recv msg
        int statesToUpdateTemp[numStates];
        int res[2];
        if (get_states_to_update(res, statesToUpdateTemp, m1) == -1)
        {
          continue;
        }
        int numStatesToUpdateTemp = res[0];
        int posInForkPath = res[1];

        int statesToUpdateTemp2[numStatesToUpdateTemp];
        int numStatesToUpdateTemp2 = 0;
        for (int s = 0; s < numStatesToUpdateTemp; s++)
        {
          if (canDeliverRecvState(statesToUpdateTemp[s], m1))
          {
            statesToUpdateTemp2[numStatesToUpdateTemp2++] = statesToUpdateTemp[s];
          }
        }

        int statesToUpdate[numStatesToUpdateTemp2];
        int numStatesToUpdate = 0;
        int statesNoAction[numStatesToUpdateTemp2];
        int numStatesNoAction = 0;
        for (int s = 0; s < numStatesToUpdateTemp2; s++)
        {
          if (canDeliverState(systemStates[statesToUpdateTemp2[s]].len - 1, statesToUpdateTemp2[s], m2, m1)) // 1 posinforkpath attention len - 1 to compensate pos+1 in fct
          {
            statesToUpdate[numStatesToUpdate++] = statesToUpdateTemp2[s];
          }
          else
          { // verify that this includes the right states (I checked once seems ok)
            statesNoAction[numStatesNoAction++] = statesToUpdateTemp2[s];
          }
        }
        if (numStatesToUpdate != 0)
        {
          printf("[Controller] Still something to deliver :\n");
          printMessage(m2);
          printf("to recv : \n");
          printMessage(m1);
        }
      }
      else
      { // m1 send msg
        int statesToUpdateTemp[numStates];
        int res[2];
        if (get_states_to_update(res, statesToUpdateTemp, m2) == -1)
        {
          continue;
        }
        int numStatesToUpdateTemp = res[0];
        int posInForkPath = res[1];

        int statesToUpdateTemp2[numStatesToUpdateTemp];
        int numStatesToUpdateTemp2 = 0;
        for (int s = 0; s < numStatesToUpdateTemp; s++)
        {
          if (canDeliverRecvState(statesToUpdateTemp[s], m2))
          {
            statesToUpdateTemp2[numStatesToUpdateTemp2++] = statesToUpdateTemp[s];
          }
        }

        int statesToUpdate[numStatesToUpdateTemp2];
        int numStatesToUpdate = 0;
        int statesNoAction[numStatesToUpdateTemp2];
        int numStatesNoAction = 0;
        for (int s = 0; s < numStatesToUpdateTemp2; s++)
        {
          if (canDeliverState(systemStates[statesToUpdateTemp2[s]].len - 1, statesToUpdateTemp2[s], m1, m2)) // 1 posinforkpath attention len - 1 to compensate pos+1 in fct
          {
            statesToUpdate[numStatesToUpdate++] = statesToUpdateTemp2[s];
          }
          else
          { // verify that this includes the right states (I checked once seems ok)
            statesNoAction[numStatesNoAction++] = statesToUpdateTemp2[s];
          }
        }
        if (numStatesToUpdate != 0)
        {
          printf("[Controller] Still something to deliver :\n");
          printMessage(m1);
          printf("to recv : \n");
          printMessage(m2);
        }
      }
    }
  }
  printf("[Controller] End of simulation\n");
  printf("[Controller] Number of states we went through : %d\n", numStates);
  printf("[Controller] Number of states we killed : %d\n", numStatesKilled);

  for (int f = 0; f < numOpenFd; f++) {
    close(fds[f]);
  }
  for (int p = 0; p < numProcesses; p++) {
    kill(processes[p], SIGTERM);
  }

  close(sockfd);
  unlink(CONTROLLER_PATH);
  unlink(CONTROLLER_FEEDBACK_PATH);

  while (wait(NULL) != -1)
    ;

  sem_close(sem);
  sem_unlink("/sem_bv_broadcast"); // Cleanup the semaphore

  sem_close(sem_init_brd);
  sem_unlink("/sem_bv_broadcast_init_brd"); // Cleanup the semaphore

  return 0;
}
