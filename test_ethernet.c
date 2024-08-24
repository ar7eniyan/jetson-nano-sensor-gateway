#include <stdio.h>
#include <errno.h>
#include <unistd.h>  // close()
#include <arpa/inet.h>  // htons()
#include <sys/socket.h>  // socket()
#include <linux/if_packet.h>  // struct sockaddr_ll and others
#include <net/ethernet.h>  // ETH_P_* constants

int main()
{
    int exit_code = 0;
    
    int raw_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (raw_sock == -1) {
        perror("Unable to create packet raw socket: ");
        exit_code = 1;
        goto close_sock;
    }

close_sock:
    if(raw_sock != -1 && close(raw_sock) == -1) {
        perror("Somehow close() failed: ");
    }

    return exit_code;
}
