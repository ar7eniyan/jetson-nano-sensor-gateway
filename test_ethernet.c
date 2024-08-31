#include <arpa/inet.h>  // htons()
#include <net/ethernet.h>  // ETH_P_* constants
#include <net/if.h>  // struct ifreq, various ioctls
#include <net/if_arp.h>  // ARPHRD_ETHER
#include <netinet/ether.h>  // ehter_aton()
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>  // ioctl()
#include <sys/socket.h>  // socket()
#include <unistd.h>  // close()

#include <linux/if_packet.h>  // struct sockaddr_ll and others

#define CUSTOM_ETHERTYPE 0xDEAD
#define PERIPH_CTRL_MAC "aa:bb:cc:dd:ee:ff"  // Placeholder for now

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
    int ret, exit_code = 0;
    char ifname[IFNAMSIZ];
    strlcpy(ifname, "enp4s0", IFNAMSIZ);

    int raw_sock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
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

    struct sockaddr_ll bind_addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = CUSTOM_ETHERTYPE,
        .sll_ifindex = ifindex
    };

    if (bind(raw_sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) == -1) {
        perror("Unable to bind to the interface and protocol: ");
        exit_code = 1;
        goto close_sock;
    }

    struct ether_addr *periph_ctrl_mac_addr = ether_aton(PERIPH_CTRL_MAC);
    if (periph_ctrl_mac_addr == NULL) {
        fprintf(stderr, "Invalid MAC address specified: %s\n", PERIPH_CTRL_MAC);
        exit_code = 1;
        goto close_sock;
    }

    // Should I specify the ifindex for sending or is it set automatically after bind?
    // NOTE: I should.
    struct sockaddr_ll periph_ctrl_addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(CUSTOM_ETHERTYPE),
        .sll_ifindex = ifindex,
        .sll_halen = 6
    };
    memcpy(periph_ctrl_addr.sll_addr, periph_ctrl_mac_addr->ether_addr_octet, 6);

    const char ping[4] = {'p', 'i', 'n', 'g'};
    ret = sendto(
        raw_sock, ping, sizeof ping, 0,
        (struct sockaddr *)&periph_ctrl_addr, sizeof periph_ctrl_addr
    );
    if (ret == -1) {
        perror("Error sending ping packet: ");
        exit_code = 1;
        goto close_sock;
    }

    const char pong[4] = {'p', 'o', 'n', 'g'};
    char recv_buf[sizeof pong];
    struct sockaddr_ll recv_addr;
    socklen_t recv_addrlen = sizeof recv_addr;
    for (;;) {
        ret = recvfrom(
            raw_sock, recv_buf, sizeof recv_buf, 0,
            (struct sockaddr *)&recv_addr, &recv_addrlen
        );
        if (ret == -1) {
            perror("Error receiving pong packet: ");
            exit_code = 1;
            goto close_sock;
        }
        if (recv_addrlen != sizeof recv_addr) {
            fprintf(stderr, "Invalid address returned from recvfrom()\n");
            exit_code = 1;
            goto close_sock;
        }
        if (recv_addr.sll_hatype != ARPHRD_ETHER) {
            fprintf(
                stderr,
                "Got packet with unexpected ARP hardware type: %d, expected %d (ARPHDR_ETHER)\n",
                recv_addr.sll_hatype, ARPHRD_ETHER
            );
            exit_code = 1;
            goto close_sock;
        }
        if (recv_addr.sll_halen != periph_ctrl_addr.sll_halen) {
            fprintf(
                stderr, "Got packet with unexpected hw address length: %d, expected %d\n",
                recv_addr.sll_halen, periph_ctrl_addr.sll_halen
            );
            exit_code = 1;
            goto close_sock;
        }
        if (
            recv_addr.sll_pkttype != PACKET_HOST ||
            ntohs(recv_addr.sll_protocol != CUSTOM_ETHERTYPE) ||
            memcmp(recv_addr.sll_addr, periph_ctrl_addr.sll_addr, periph_ctrl_addr.sll_halen) != 0
        ) {
            continue;
        }
        if (ret != sizeof pong && memcmp(recv_buf, pong, sizeof pong)) {
            fprintf(stderr, "Got invalid pong message != ['p', 'o', 'n', 'g'] (length %d)\n", ret);
            exit_code = 1;
            goto close_sock;
        }
        break;
    }

    printf("YEAAAAAH!\n");

close_sock:
    if(raw_sock != -1 && close(raw_sock) == -1) {
        perror("Somehow close() failed");
    }

    return exit_code;
}
