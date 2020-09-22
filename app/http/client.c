// HTTP client.

#include "lib.h"

void send_request(char *request) {
  int socket_fd = 0;
  struct sockaddr_in address;
  char response[1024];

  if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    write(1, "error: fail to create socket\n", 29);
    close(socket_fd);
    exit(1);
    return;
  }

  /*
  struct timeval read_timeout;
  read_timeout.tv_sec = 1;
  read_timeout.tv_usec = 1;
  //setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &read_timeout, sizeof(read_timeout));
  */

  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);

  if (connect(socket_fd, (struct sockaddr *) &address, sizeof(address)) == -1) {
    write(1, "error: fail to connect socket\n", 30);
    close(socket_fd);
    exit(1);
    return;
  }
  sendto(socket_fd, request, my_strlen(request), 0, (struct sockaddr *) &address, sizeof(address));

  int size = read(socket_fd, response, 1024);
  write(1, response, size);
  write(1, "\n", 1);

  close(socket_fd);
}

void start_line(char *request) {
  my_strcpy(request, "GET /index.html HTTP/1.1\n");
}

int main(int argc, char *argv[]) {
  char *request = (char *) my_malloc(1000);
  start_line(request);
  send_request(request);
  exit(0);
  return 0;
}
