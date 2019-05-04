#define _POSIX_C_SOURCE 199309
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/limits.h>
#include <sys/select.h>
#include <string.h>
#include <ctype.h>

#define BUF_SIZE 516
#define MODE_MAX 9
#define MAX_TIMEOUT 3

struct req {
  int sock_fd;
  struct sockaddr_in claddr;
  FILE *file_fs;
  int block_num;
  int n_read;
  int last;
  int n_timeout;
  timer_t timer_id;
};

struct req *requests[65536];

char buf[BUF_SIZE];
fd_set readfds;

void deleteTimer(struct req *r) {
  timer_delete(r -> timer_id);
}

void free_r(struct req *r) {
  deleteTimer(r);
  FD_CLR(r -> sock_fd, &readfds);
  close(r -> sock_fd);
  fclose(r -> file_fs);
  free(r);
}

void sendErr(int fd, struct sockaddr_in *claddr, char *errStr, int ec) {
  printf("%s\n", errStr);
  int op = htons(5);
  memcpy(buf, &op, 2);
  int ec_n = htons(ec);
  memcpy(buf + 2, &ec_n, 2);
  int numBytes = strlen(errStr) + 1;
  sprintf(buf + 4, "%s", errStr);
  socklen_t len = sizeof(struct sockaddr_in);
  if (sendto(fd, buf, 4 + numBytes, 0, (struct sockaddr *) claddr, len) != numBytes + 4) {
    perror("sendto");
    exit(EXIT_FAILURE);
  }
}

void logRecv(struct sockaddr_in *claddr, int numBytes, int opcode) {
  char claddrStr[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &(claddr -> sin_addr), claddrStr, INET_ADDRSTRLEN) == NULL) {
    printf("Couldn't convert client address to string\n");
  } else {
    printf("Server received opcode: %d, numBytes: %ld from (%s, %u)\n",
        opcode, (long) numBytes, claddrStr, ntohs(claddr -> sin_port));
  }
}

void logSend(struct req *r) {
  char claddrStr[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &(r -> claddr.sin_addr), claddrStr, INET_ADDRSTRLEN) == NULL) {
    printf("Couldn't convert client address to string\n");
  } else {
    printf("Server sent block: %d, numBytes: %ld to (%s, %u)\n",
        r -> block_num, (long) r -> n_read, claddrStr, ntohs(r -> claddr.sin_port));
  }
}

void resetTimer(struct req *r) {
  struct timespec value;
  value.tv_sec = 5;
  value.tv_nsec = 0;
  struct itimerspec its;
  its.it_interval = value;
  its.it_value = value;
  timer_settime(r -> timer_id, 0, &its , NULL);
}

void sendBlock(struct req *r) {
  int op = htons(3);
  memcpy(buf, &op, 2);
  int blockNum_n = htons(r -> block_num);
  memcpy(buf + 2, &blockNum_n, 2);
  size_t numBytes = fread(buf + 4, 1, 512, r -> file_fs);

  socklen_t len = sizeof(struct sockaddr_in);
  if (sendto(r -> sock_fd, buf, 4 + numBytes, 0, (struct sockaddr *) &(r ->claddr), len) != numBytes + 4) {
    perror("sendto");
    exit(EXIT_FAILURE);
  }

  r -> n_read = numBytes;
  logSend(r);
  if (numBytes < 512) {
    r -> last = 1;
  }
}

void timeout(union sigval sv) {
  struct req *r = requests[sv.sival_int];
  if (r -> n_timeout >= MAX_TIMEOUT) {
    sendErr(r -> sock_fd, &(r -> claddr), "max retries exceeded", 4);
    free_r(r);
    return;
  }

  printf("retry: %d, sfd: %d\n", r -> n_timeout + 1, r -> sock_fd);
  fseek(r -> file_fs, -1 * r -> n_read, SEEK_CUR);
  sendBlock(r);
  r -> n_timeout++;
}

void createTimer(struct req *r, int arg) {
  union sigval sigval;
  sigval.sival_int = arg;
  struct sigevent sev;
  sev.sigev_notify = SIGEV_THREAD;
  sev.sigev_value = sigval;
  sev.sigev_notify_function = timeout;
  sev.sigev_notify_attributes = NULL;
  timer_create(CLOCK_REALTIME, &sev, &(r -> timer_id));
}

char* getString(char *dst, char *src) {
  while (*src != '\0') {
    *dst = *src;
    src++;
    dst++;
  }
  *dst = '\0';
  return src + 1;
}

int parseReq(char *buf, char *filename, char *mode) {
  int opcode_net = 0;
  int opcode = 0;
  memcpy(&opcode_net, buf, 2);
  opcode = ntohs(opcode_net);
  buf = buf + 2;
  buf = getString(filename, buf);
  getString(mode, buf);
  return opcode;
}

int main(int argc, char* argv[]) {
  struct sockaddr_in svaddr, claddr;
  char svaddrStr[INET_ADDRSTRLEN];
  int sfd;
  int nfds;
  FD_ZERO(&readfds);

  if (argc != 2) {
    printf("provide a port number\n");
    exit(EXIT_FAILURE);
  }
  int port_sv = atoi(argv[1]);

  sfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  struct in_addr inaddr_any;
  inaddr_any.s_addr = htonl(INADDR_ANY);

  memset(&svaddr, 0, sizeof(struct sockaddr_in));
  svaddr.sin_family = AF_INET;
  svaddr.sin_addr = inaddr_any;
  svaddr.sin_port = htons(port_sv);

  if (bind(sfd, (struct sockaddr *) &svaddr, sizeof(struct sockaddr_in)) == -1) {
    perror("bind");
    exit(EXIT_FAILURE);
  }

  if (inet_ntop(AF_INET, &svaddr.sin_addr, svaddrStr, INET_ADDRSTRLEN) != NULL) {
    printf("listening to ip %s port %u\n", svaddrStr, ntohs(svaddr.sin_port));
  }

  nfds = sfd + 1;
  FD_SET(sfd, &readfds);

  for (;;) {
    fd_set readfdsDup = readfds;
    select(nfds, &readfdsDup, NULL, NULL, 0);
    for (int fd = 0; fd < nfds; fd++) {
      if (FD_ISSET(fd, &readfdsDup)) {
        if (fd == sfd) {
          // new request

          socklen_t len = sizeof(struct sockaddr_in);
          int numBytes = recvfrom(sfd, buf, BUF_SIZE, 0, (struct sockaddr *) &claddr, &len);
          if (numBytes == -1) {
            perror("recvfrom");
            exit(EXIT_FAILURE);
          }

          int opcode_n = 0;
          int opcode = 0;
          memcpy(&opcode_n, buf, 2);
          opcode = ntohs(opcode_n);

          logRecv(&claddr, numBytes, opcode);
          if (opcode == 4){
            // ack
            int bn_n = 0;
            memcpy(&bn_n, buf + 2, 2);
            int bn = ntohs(bn_n);
            printf("ack block: %d\n", bn);
            continue;
          } else if (opcode == 5) {
            // error
            int ec_n = 0;
            memcpy(&ec_n, buf + 2, 2);
            int ec = ntohs(ec_n);
            printf("error %d: %s\n", ec, buf + 4);
            continue;
          }

          char filename[PATH_MAX];
          char mode[MODE_MAX];
          parseReq(buf, filename, mode);
          printf("filename: %s\n", filename);
          printf("mode: %s\n", mode);
          int modeLen = strlen(mode);
          for (int i = 0; i < modeLen; i++) mode[i] = tolower(mode[i]);
          if (strncmp(mode, "octet", 5) != 0) {
            sendErr(sfd, &claddr, "only octet mode supported", 4);
            continue;
          }
          if (opcode == 1) {
            // rrq
            FILE *f = fopen(filename, "r");
            if (f == NULL) {
              sendErr(sfd, &claddr, "file not found", 1);
              continue;
            }

            int sfd1 = socket(AF_INET, SOCK_DGRAM, 0);
            if (sfd1 == -1) {
              perror("socket");
              exit(EXIT_FAILURE);
            }
            nfds = sfd1 + 1;
            FD_SET(sfd1, &readfds);

            struct req *r = malloc(sizeof(struct req));
            r -> sock_fd = sfd1;
            r -> claddr = claddr;
            r -> file_fs = f;
            r -> block_num = 1;
            r -> n_timeout = 0;
            r -> last = 0;

            sendBlock(r);

            getsockname(sfd1, (struct sockaddr *) &svaddr, &len);
            int port = ntohs(svaddr.sin_port);
            requests[port] = r;

            createTimer(r, port);
            resetTimer(r);
          }
        } else {
          // ack (existing connection)
          socklen_t len = sizeof(struct sockaddr_in);
          int numBytes = recvfrom(fd, buf, BUF_SIZE, 0, (struct sockaddr *) &claddr, &len);
          if (numBytes == -1) {
            perror("recvfrom");
            exit(EXIT_FAILURE);
          }

          getsockname(fd, (struct sockaddr *) &svaddr, &len);
          int port = ntohs(svaddr.sin_port);
          struct req *r = requests[port];

          int op_n = 0;
          int op = 0;
          memcpy(&op_n, buf, 2);
          op = ntohs(op_n);

          logRecv(&claddr, numBytes, op);
          if (op == 4) {
            // ack
            int blockNum_n = 0;
            int blockNum = 0;
            memcpy(&blockNum_n, buf + 2, 2);
            blockNum = ntohs(blockNum_n);

            if (blockNum == r -> block_num) {
              if (r -> last) {
                printf("complete, %d blocks transferred\n", blockNum);
                free_r(r);
              } else {
                r -> block_num++;
                r -> n_timeout = 0;
                sendBlock(r);
                resetTimer(r);
              }
            } else {
              printf("incorrect block num, rec: %d, req: %d\n", blockNum, r -> block_num);
            }

          } else if (op == 5) {
            // error
            int ec_n = 0;
            memcpy(&ec_n, buf + 2, 2);
            int ec = ntohs(ec_n);
            printf("error %d: %s\n", ec, buf + 4);
            free_r(r);
          }
        }
      }
    }
  }

  return 0;
}
