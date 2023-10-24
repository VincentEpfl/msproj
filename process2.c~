#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include "stubs.h"


// The receiver process : either receives a message from the sender process or times out
int
main ()
{
  // Create socket
  struct sockaddr_un address;
  int sockfd, connfd;
  size_t addr_length;
  char *socket_path = "./mysocket";
  printf ("[Receiver] Create socket\n");

  if ((sockfd = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
      perror ("[Receiver] socket");
      exit (EXIT_FAILURE);
    }

  memset (&address, 0, sizeof (struct sockaddr_un));
  address.sun_family = AF_UNIX;
  strncpy (address.sun_path, socket_path, sizeof (address.sun_path) - 1);
  unlink (socket_path);

  if (bind (sockfd, (struct sockaddr *) &address, sizeof (struct sockaddr_un))
      == -1)
    {
      perror ("[Receiver] bind");
      exit (EXIT_FAILURE);
    }

  if (listen (sockfd, 5) == -1)
    {
      perror ("[Receiver] listen");
      exit (EXIT_FAILURE);
    }

  if ((connfd = accept (sockfd, NULL, NULL)) == -1)
    {
      perror ("[Receiver] accept");
      exit (EXIT_FAILURE);
    }

  // sleep a bit so that the sender sends its message first
  // to the controller = process scheduling = easy case
  sleep (10);

  // Set a timeout on the recv operation on this socket
  struct timeval to;
  to.tv_sec = 10;
  to.tv_usec = 0;
  setsockopt (connfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof (struct timeval));

  char buf[256];
  int nbytes = recv (connfd, buf, sizeof (buf), 0);
    if (nbytes == 0)
    {
      // Right now after the state exploration the execution
      // continues here.
      printf ("[Receiver] Done with state exploration\n");
    }
  else if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
      // This is when a timeout is triggered
      char *state = "ko";
      int nb = register_state (connfd, state);
      if (nb == -1)
	{
	  perror ("[Receiver] register state");
	  exit (EXIT_FAILURE);
	}
      else
	{
	  printf ("[Receiver] register state %s\n", state);
 	}
    }
  else if (nbytes == -1)
    {
      // This is a normal error, that should not happen
      perror ("[Receiver] recv");
      exit (EXIT_FAILURE);
    }
  else
    {
      // This is when a message is received, no timeout
      char *state = "ok";
      int nb = register_state (connfd, state);
      if (nb == -1)
	{
	  perror ("[Receiver] register state");
	  exit (EXIT_FAILURE);
	}
      else
	{
	  printf ("[Receiver] register state %s\n", state);
	}

    }

  close (connfd);
  close (sockfd);
  unlink (socket_path);
  printf ("[Receiver] End of this receiver\n");

  return 0;
}
