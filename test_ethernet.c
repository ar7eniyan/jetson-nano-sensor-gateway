#include <stdio.h>
#include <string.h>
#include <unistd.h>  // close()
#include <sys/ioctl.h>  // ioctl()
#include <net/if.h>  // struct ifreq, various ioctls
#include <net/if_arp.h>  // ARPHRD_ETHER
#include <arpa/inet.h>  // htons()
#include <sys/socket.h>  // socket()
#include <linux/if_packet.h>  // struct sockaddr_ll and others
#include <net/ethernet.h>  // ETH_P_* constants


int mac_addr_from_ifname(int sock, char ifname[IFNAMSIZ], unsigned char (*mac_addr)[6])
{
    size_t len = strnlen(ifname, IFNAMSIZ);
    if (len == IFNAMSIZ) {
        fprintf(stderr, "Ifname is not NULL-terminated\n");
        return -1;
    }

    struct ifreq req = {0};
    strcpy(req.ifr_name, ifname);
    if (ioctl(sock, SIOCGIFHWADDR, &req) == -1) {
        perror("Unable to request the MAC address of the interface");
        return -1;
    }
    if (req.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        fprintf(
            stderr,
            "The interaface's ARP hardware type is 0x%x, 0x%x (ARPHRD_ETHER) expected\n",
            req.ifr_hwaddr.sa_family, ARPHRD_ETHER
        );
        return -1;
    }

    memcpy(mac_addr, req.ifr_hwaddr.sa_data, 6);
    return 0;
}

int ifindex_from_ifname(int sock, char ifname[IFNAMSIZ])
{
    size_t len = strnlen(ifname, IFNAMSIZ);
    if (len == IFNAMSIZ) {
        fprintf(stderr, "Ifname is not NULL-terminated\n");
        return -1;
    }

    struct ifreq req = {0};
    strcpy(req.ifr_name, ifname);
    if (ioctl(sock, SIOCGIFINDEX, &req) == -1) {
        perror("Unable to request the index of the interface");
        return -1;
    }

    return req.ifr_ifindex;
}

int main()
{
    int exit_code = 0;
    char ifname[IFNAMSIZ];
    strlcpy(ifname, "enp4s0", IFNAMSIZ);

    int raw_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (raw_sock == -1) {
        perror("Unable to create packet raw socket");
        exit_code = 1;
        goto close_sock;
    }

    unsigned char mac_addr[6];
    int ifindex = ifindex_from_ifname(raw_sock, ifname);
    if (ifindex == -1) {
        fprintf(stderr, "Unable to get the index of the interface \"%s\"\n", ifname);
        exit_code = 1;
        goto close_sock;
    }
    if (mac_addr_from_ifname(raw_sock, ifname, &mac_addr) == -1) {
        fprintf(stderr, "Unable to get MAC address of the interface \"%s\"\n", ifname);
        exit_code = 1;
        goto close_sock;
    }

    printf("interface %s\n", ifname);
    printf("    index %d\n", ifindex);
    printf("    mac %02x:%02x:%02x:%02x:%02x:%02x\n",
        mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

close_sock:
    if(raw_sock != -1 && close(raw_sock) == -1) {
        perror("Somehow close() failed");
    }

    return exit_code;
}
