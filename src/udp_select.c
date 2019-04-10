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

struct req {
  int sock_fd;
  FILE *file_fs;
  int block_num;
  int last;
};

struct req *requests[65536];

char buf[BUF_SIZE];

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

void rrq(char *filename, char *mode, struct sockaddr_in claddr) {
}

int main(int argc, char* argv[]) {
  struct sockaddr_in svaddr, claddr;
  char claddrStr[INET_ADDRSTRLEN];
  char svaddrStr[INET_ADDRSTRLEN];
  int sfd;
  int nfds;
  fd_set readfds;
  FD_ZERO(&readfds);

  if (argc != 2) {
    printf("provide a port number\n");
    exit(EXIT_FAILURE);
  }
  int port = atoi(argv[1]);

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
  svaddr.sin_port = htons(port);

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
    select(nfds, &readfds, NULL, NULL, 0);
    fflush(stdout);
    for (int fd = 0; fd < nfds; fd++) {
      printf("--->%d\n", fd);
      if (FD_ISSET(fd, &readfds)) {
        printf("===>%d\n", fd);
        if (fd == sfd) {
          socklen_t len = sizeof(struct sockaddr_in);
          int numBytes = recvfrom(sfd, buf, BUF_SIZE, 0, (struct sockaddr *) &claddr, &len);
          if (numBytes == -1) {
            perror("recvfrom");
            exit(EXIT_FAILURE);
          }

          // LOG: new connection
          if (inet_ntop(AF_INET, &claddr.sin_addr, claddrStr, INET_ADDRSTRLEN) == NULL) {
            printf("Couldn't convert client address to string\n");
          } else {
            printf("Server received %ld bytes from (%s, %u)\n",
                (long) numBytes, claddrStr, ntohs(claddr.sin_port));
          }

          int opcode_n = 0;
          int opcode = 0;
          memcpy(&opcode_n, buf, 2);
          opcode = ntohs(opcode_n);
          printf("opcode: %d\n", opcode);
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
          printf("opcode: %d\n", opcode);
          printf("filename: %s\n", filename);
          printf("mode: %s\n", mode);
          int modeLen = strlen(mode);
          for (int i = 0; i < modeLen; i++) mode[i] = tolower(mode[i]);
          if (strncmp(mode, "octet", 5) != 0) {
            char *errStr = "only octet mode supported";
            printf("%s\n", errStr);
            int op = htons(5);
            memcpy(buf, &op, 2);
            int ec = htons(4);
            memcpy(buf + 2, &ec, 2);
            int numBytes = strlen(errStr) + 1;
            sprintf(buf + 4, "%s", errStr);
            if (sendto(sfd, buf, 4 + numBytes, 0, (struct sockaddr *) &claddr, len) != numBytes + 4) {
              perror("sendto");
              exit(EXIT_FAILURE);
            }
            continue;
          }
          if (opcode == 1) {
            // rrq
            int sfd1 = socket(AF_INET, SOCK_DGRAM, 0);
            if (sfd1 == -1) {
              perror("socket");
              exit(EXIT_FAILURE);
            }

            FILE *f = fopen(filename, "r");
            if (f == NULL) {
              char *errStr = "file not found";
              printf("%s\n", errStr);
              int op = htons(5);
              memcpy(buf, &op, 2);
              int ec = htons(1);
              memcpy(buf + 2, &ec, 2);
              int numBytes = strlen(errStr) + 1;
              sprintf(buf + 4, "%s", errStr);
              if (sendto(sfd, buf, 4 + numBytes, 0, (struct sockaddr *) &claddr, len) != numBytes + 4) {
                perror("sendto");
                exit(EXIT_FAILURE);
              }
              close(sfd1);
              continue;
            }
            int op = htons(3);
            memcpy(buf, &op, 2);
            int blockNum = htons(1);
            memcpy(buf + 2, &blockNum, 2);
            size_t numBytes = fread(buf + 4, 1, 512, f);
            printf("block: %d, numBytes: %d\n", blockNum, (int) numBytes);

            socklen_t len = sizeof(struct sockaddr_in);
            if (sendto(sfd1, buf, 4 + numBytes, 0, (struct sockaddr *) &claddr, len) != numBytes + 4) {
              perror("sendto");
              exit(EXIT_FAILURE);
            }
            getsockname(sfd1, (struct sockaddr *) &svaddr, &len);
            int port = ntohs(svaddr.sin_port);
            printf("port: %d\n", port);
            struct req *r = malloc(sizeof(struct req));
            r -> sock_fd = sfd1;
            r -> file_fs = f;
            r -> block_num = 1;
            if (numBytes < 512) r -> last = 1;
            requests[port] = r;

            nfds = sfd1 + 1;
            FD_SET(sfd1, &readfds);
          }
        } else {
          socklen_t len = sizeof(struct sockaddr_in);
          int numBytes = recvfrom(fd, buf, BUF_SIZE, 0, (struct sockaddr *) &claddr, &len);
          if (numBytes == -1) {
            perror("recvfrom");
            exit(EXIT_FAILURE);
          }

          // LOG:
          if (inet_ntop(AF_INET, &claddr.sin_addr, claddrStr, INET_ADDRSTRLEN) == NULL) {
            printf("Couldn't convert client address to string\n");
          } else {
            printf("Server received %ld bytes from (%s, %u)\n",
                (long) numBytes, claddrStr, ntohs(claddr.sin_port));
          }

          getsockname(fd, (struct sockaddr *) &svaddr, &len);
          int port = ntohs(svaddr.sin_port);

          struct req *r = requests[port];

          int op_n = 0;
          int op = 0;
          memcpy(&op_n, buf, 2);
          op = ntohs(op_n);
          /* printf("opcode: %d\n", op); */
          if (op == 4) {
            // ack
            int blockNum_n = 0;
            int blockNum = 0;
            memcpy(&blockNum_n, buf + 2, 2);
            blockNum = ntohs(blockNum_n);

            if (blockNum == r -> block_num) {
              if (r -> last) {
                printf("complete, %d blocks transferred\n", blockNum);
                FD_CLR(r -> sock_fd, &readfds);
                close(r -> sock_fd);
                fclose(r -> file_fs);
                free(r);

              } else {
                blockNum++;
                int op = htons(3);
                memcpy(buf, &op, 2);
                blockNum_n = htons(blockNum);
                memcpy(buf + 2, &blockNum_n, 2);
                size_t numBytes = fread(buf + 4, 1, 512, r -> file_fs);
                /* printf("block: %d, numBytes: %d\n", blockNum, (int) numBytes); */

                socklen_t len = sizeof(struct sockaddr_in);
                if (sendto(r -> sock_fd, buf, 4 + numBytes, 0, (struct sockaddr *) &claddr, len) != numBytes + 4) {
                  perror("sendto");
                  exit(EXIT_FAILURE);
                }
                r -> block_num = blockNum;
                if (numBytes < 512) {
                  r -> last = 1;
                }
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
            FD_CLR(r -> sock_fd, &readfds);
            close(r -> sock_fd);
            fclose(r -> file_fs);
            free(r);
          }

        }
      }
    }
  }

  return 0;
}
