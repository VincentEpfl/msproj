#define _GNU_SOURCE
#include <sys/wait.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

// Used with LD_PRELOAD to intercept the calls to send and recv
// redirect those calls through the controller

static ssize_t (*real_send)(int sockfd, const void *buf, size_t len,
                            int flags) = NULL;
static ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags) =
    NULL;

#define CONTROLLER_PATH "./controller_socket"

#define CONTROLLER_FEEDBACK_PATH "./controller_feedback_socket"

// int controller_socket = 0;

int forkId = 0; // Only 0 at first for each process

// send override
ssize_t
send(int sockfd, const void *buf, size_t len, int flags)
{
  printf("[Intercept] Enter send\n");

  // Load the real send
  if (!real_send)
  {
    real_send = dlsym(RTLD_NEXT, "send");
  }

  if (sockfd == -1) { // this is a send to the feedback socket

  struct sockaddr_un address;
  int feedback_socket;
  if ((feedback_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Intercept] socket");
    exit(EXIT_FAILURE);
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_FEEDBACK_PATH,
          sizeof(address.sun_path) - 1);

  if (connect(feedback_socket, (struct sockaddr *)&address,
              sizeof(struct sockaddr_un)) == -1)
  {
    perror("[Intercept] connect");
    exit(EXIT_FAILURE);
  }

  // Send feedback message to the controller
  printf("[Intercept] Send feedback\n");
  int *intBuf = (int *)buf; // possible direct int[] ?
  int feedbackMessage[3];
  feedbackMessage[0] = forkId;         
  feedbackMessage[1] = intBuf[0]; 
  feedbackMessage[2] = intBuf[1]; 

  ssize_t bytes_sent = real_send(feedback_socket, &feedbackMessage, sizeof(feedbackMessage), 0);

  close(feedback_socket);
  return bytes_sent;
  } else {

  // If controller socket already exist use it
  // The only time it will be the case is in a child right after a recv
  // This will be used to send the state to the controller, and closed
  // right after
  // right now I'm thinking of using a different feedback socket so wouldnt need that
  // if (controller_socket == 0) {
  struct sockaddr_un address;
  int controller_socket;
  if ((controller_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Intercept] socket");
    exit(EXIT_FAILURE);
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_PATH,
          sizeof(address.sun_path) - 1);

  if (connect(controller_socket, (struct sockaddr *)&address,
              sizeof(struct sockaddr_un)) == -1)
  {
    perror("[Intercept] connect");
    exit(EXIT_FAILURE);
  }
  //  }

  struct sockaddr_in peer_addr;
  socklen_t peer_addr_len = sizeof(peer_addr);
  int port;
  int processId;
  if (getpeername(sockfd, (struct sockaddr *)&peer_addr, &peer_addr_len) == 0)
  {
    port = ntohs(peer_addr.sin_port);
    processId = port - 8080;
    printf("Intercepted message to process %d\n", processId);
  }
  else
  {
    perror("getpeername");
    exit(EXIT_FAILURE);
  }

  // Send (redirect) the message to the controller
  printf("[Intercept] Send\n");
  int *intBuf = (int *)buf; // possible direct int[] ?
  int sendMessage[5];
  sendMessage[0] = 0;         // type = send
  sendMessage[1] = intBuf[0]; // from = first elem of msg
  sendMessage[2] = processId; // to = determine from sock fd dest port
  sendMessage[3] = intBuf[1]; // msg = second elem of msg
  sendMessage[4] = forkId;    // forkId

  ssize_t bytes_sent = real_send(controller_socket, &sendMessage, sizeof(sendMessage), 0);

  close(controller_socket);

  return bytes_sent;
}
}

// Override recv
ssize_t
recv(int sockfd, void *buf, size_t len, int flags)
{
  printf("[Intercept] Enter recv\n");

  // Load the real recv
  if (!real_recv)
  {
    real_recv = dlsym(RTLD_NEXT, "recv");
  }

  // We also need the real send
  if (!real_send)
  {
    real_send = dlsym(RTLD_NEXT, "send");
  }

  struct sockaddr_un address;
  int controller_socket;

  // Create a socket to the controller

  if ((controller_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Intercept] socket");
    exit(EXIT_FAILURE);
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_PATH, sizeof(address.sun_path) - 1);

  if (connect(controller_socket, (struct sockaddr *)&address,
              sizeof(struct sockaddr_un)) == -1)
  {
    perror("[Intercept] connect");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in local_addr;
  socklen_t addr_len = sizeof(local_addr);
  if (getsockname(sockfd, (struct sockaddr *)&local_addr, &addr_len) < 0)
  {
    perror("ERROR getting socket name");
    exit(EXIT_FAILURE);
  }
  int port = ntohs(local_addr.sin_port);
  int processId = port - 8080;

  // For now no timeout, check in SimpleSendRecv if needed

  // Maybe send state here. That way controller can tell to kill if redondant ?
  // then need to consider the last recv controller receives, for which it
  // has no more msg to transmit, to get the final state (impacted by the
  // last actual recv just before)
  // Yeah just send back state directly I guess, for now
  // If i need to open a new socket / co in the controller just after sending
  // the message to fork, and wait for reply from child, well i will do that,
  // for now

  // Send a message to the controller that this process is ready to receive
  // Right now the instruction is read directly from the message value, but
  // the idea is to include this in format :
  // msg = this process is ready to receive from a certain sender process
  printf("[Intercept] send to controller\n");
  int sendMessage[5];
  sendMessage[0] = 1;         // type = recv
  sendMessage[1] = -1;        // from = first elem of msg
  sendMessage[2] = processId; // to = determine from sock fd port IF NO do trick osef put param or put serv address in global and access here whatev... no energy for this shit
  sendMessage[3] = -1;        // msg = -1 recv msg
  sendMessage[4] = forkId;    // forkId

  ssize_t bytes_sent = real_send(controller_socket, sendMessage, sizeof(sendMessage), 0);

  // Wait for instructions from the controller
  // Carefull with recv, blocking but stream ? Insure that I recv the
  // right thing.
  // flags MSG_PEEK or MSG_WAITALL might help (using len)
  // This is Unix socket, maybe just put it SOCK_DGRAM (UDP like but
  // reliable and no reorder) instead of SOCK_STREAM (TCP like)

  // Message format
  int receivedMessage[3];

  pid_t children[2]; // Only 2 here no ?
  int i = 0;

  printf("[Intercept] wait for controller instructions\n");

  while (1)
  {

    ssize_t bytes_received = real_recv(controller_socket, &receivedMessage, sizeof(receivedMessage), 0);
    if (bytes_received == -1)
    {
      perror("Recv failure");
      exit(EXIT_FAILURE);
    }

    int instruction = receivedMessage[0];
    int from = receivedMessage[1];
    int value = receivedMessage[2];
    printf("[Intercept] recv from controller, instr %d : {from:%d, value:%d}\n", instruction, from, value);

    // Say instruction 1 is fork
    if (instruction == 1)
    {
      printf("[Intercept] fork\n");
      children[i] = fork();
      if (children[i] < 0)
      {
        perror("[Intercept] fork");
        exit(EXIT_FAILURE);
      }
      if (children[i] == 0)
      {
        // The child process continues the execution with the msg value
        // received by the controller

        forkId = getpid();

        // Unwrap message from controller and transmit data to process
        int *intBuf = (int *)buf;
        intBuf[0] = from;
        intBuf[1] = value;

        // don't close controller socket so that child can send state
        // to controller directly (this will close)
        return bytes_received;
      }
      i = i + 1;
    }
    else if (instruction == 2)
    { // TODO instruction kill parent ?
      printf("[Intercept] kill\n");
      // Say instruction 2 is kill
      // Would it be better to have an instruction to kill a child process ?
      // But then which one, here again only 2 options and if kill its that
      // they are the same so any works, but more generally.. well who cares

      // how about children processes ?

      // close (controller_socket);
      // exit(0);

      // Here I kill the last child (same because 2 and kill if =)
      if (kill(children[i - 1], SIGTERM) < 0)
      {
        perror("[Intercept] kill");
        exit(EXIT_FAILURE);
      }
    }
  }

  close(controller_socket);
  return 0;
}
// TODO override other function to register state