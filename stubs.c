#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>

// The function to register the process state to the controller.
// It was difficult to define this in the LD_PRELOAD-ed file
// so for now it's just defined here.
ssize_t
register_state (int sockfd, const char *state)
{
  printf ("[STUB] send %s\n", state);
  ssize_t bytes_sent = send (sockfd, state, sizeof (state), 0);
  printf ("[STUB] Sent state\n");

  return bytes_sent;

}
