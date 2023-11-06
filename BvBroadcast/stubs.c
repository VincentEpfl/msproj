#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <dlfcn.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <string.h>

#define CONTROLLER_FEEDBACK_PATH "./controller_feedback_socket"

static ssize_t (*real_send)(int sockfd, const void *buf, size_t len,
                            int flags) = NULL;

// The function to register the process state to the controller.
// It was difficult to define this in the LD_PRELOAD-ed file
// so for now it's just defined here.
ssize_t
register_state(const int *state, int len) // no need sockfd just put 0 send redirect
{
  typedef ssize_t (*original_send_t)(int, const void *, size_t, int);
  original_send_t original_send = (original_send_t)dlsym(RTLD_NEXT, "send");
  // worst case send via redirect but add special bit to msg to use the feedback socket 
  // if so then don't even need this just send in bv broadcast ...

  // Load the real send
  /*
  if (!real_send)
  {
    real_send = dlsym(RTLD_NEXT, "send"); // voir si ça casse pas tout... chatgpt says it works...
  } 
  */

  struct sockaddr_un address;
  int controller_feedback_socket;
  if ((controller_feedback_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Stub] socket");
    exit(EXIT_FAILURE);
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_FEEDBACK_PATH,
          sizeof(address.sun_path) - 1);

  if (connect(controller_feedback_socket, (struct sockaddr *)&address,
              sizeof(struct sockaddr_un)) == -1)
  {
    perror("[Stub] connect");
    exit(EXIT_FAILURE);
  }

  printf("[STUB] send\n");
  // write controller socket in redirect, read here, how ? use global variable ?
  // is there a variable shared between redirect bv broadcast and stubs ?
  // transmit but not clean
  // just open another co here and wait for a message in controller on this
  // specific co
  // should be fine because no other connection in parallel if I wait for state
  // one by one
  // and also exec 1 by 1 be we ll see
  ssize_t bytes_sent = original_send(controller_feedback_socket, state, len, 0);
  printf("[STUB] Sent state\n");

  close(controller_feedback_socket);

  return bytes_sent;
}