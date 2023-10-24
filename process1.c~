#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// The sender process : just sends a message to the receiver process.
int
main ()
{
  // Create socket 
  struct sockaddr_un address;
  int sockfd;
  size_t addr_length;
  char *socket_path = "./mysocket";
  printf ("[Sender] Create socket\n");

  if ((sockfd = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
      perror ("[Sender] socket");
      exit (EXIT_FAILURE);
    }

  memset (&address, 0, sizeof (struct sockaddr_un));
  address.sun_family = AF_UNIX;
  strncpy (address.sun_path, socket_path, sizeof (address.sun_path) - 1);

  printf ("[Sender] connect\n");
  if (connect
      (sockfd, (struct sockaddr *) &address,
       sizeof (struct sockaddr_un)) == -1)
    {
      perror ("[Sender] connect");
      exit (EXIT_FAILURE);
    }

  // Uncomment to give time to the receiver to send its message to the controller
  // first to get the 2nd message order

  //sleep(15);


  // Send message on the socket. 
  // Right now the content itself is used to identify
  // the message, but should be metadata eg 
  // msg = send from addr(this) to addr(dest) + data
  char *message = "sen";
  printf ("[Sender] Send message %s\n", message);
  send (sockfd, message, sizeof (message), 0);
  printf ("[Sender] This is the end for this process\n");

  close (sockfd);
  return 0;
}
