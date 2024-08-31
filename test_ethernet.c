#include <assert.h>  // assert()
#include <arpa/inet.h>  // htons()
#include <net/ethernet.h>  // ETH_P_* constants
#include <net/if.h>  // struct ifreq, various ioctls
#include <net/if_arp.h>  // ARPHRD_ETHER
#include <netinet/ether.h>  // ehter_aton()
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>  // ioctl()
#include <sys/socket.h>  // socket()
#include <unistd.h>  // close()

#include <linux/if_packet.h>  // struct sockaddr_ll and others

#define CUSTOM_ETHERTYPE 0xDEAD
#define PERIPH_CTRL_MAC "aa:bb:cc:dd:ee:ff"  // Placeholder for now

typedef struct {
    int sockfd;
    int ifindex;
    unsigned char peer_mac[6];
} eth_comms_t;

int mac_addr_from_ifname(
    int sock, char ifname[IFNAMSIZ], unsigned char (*mac_addr)[6]
) {
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
        fprintf(stderr, "The interaface's ARP hardware type is 0x%x, 0x%x"
            "(ARPHRD_ETHER) expected\n", req.ifr_hwaddr.sa_family,
            ARPHRD_ETHER);
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

typedef struct {
    const char *ifname;
    uint16_t ethertype;
    unsigned char *peer_mac_ptr;  // Must be 6 bytes long!
} eth_open_and_bind_params_t;

int eth_open_and_bind(eth_comms_t *comms, eth_open_and_bind_params_t settings)
{
    char ifname_buf[IFNAMSIZ];
    size_t ifname_sz = strlcpy(ifname_buf, settings.ifname, sizeof ifname_buf);
    if (ifname_sz >= sizeof ifname_buf) {
        fprintf(stderr, "The supplied ifname is too long (>15 chars)\n");
        goto exit_error;
    }

    int sockfd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
    if (sockfd == -1) {
        perror("Unable to create packet raw socket");
        goto exit_error;
    }

    unsigned char mac_bytes[6];
    int ifindex = ifindex_from_ifname(sockfd, ifname_buf);
    if (ifindex == -1) {
        fprintf(stderr, "Can't get the index of the interface \"%s\"\n",
            ifname_buf);
        goto exit_error;
    }
    if (mac_addr_from_ifname(sockfd, ifname_buf, &mac_bytes) == -1) {
        fprintf(stderr, "Can't get the MAC address of the interface \"%s\"\n",
            ifname_buf);
        goto exit_error;
    }

    printf("Binding to interface %s\n", ifname_buf);
    printf("    index %d\n", ifindex);
    printf("    mac %02x:%02x:%02x:%02x:%02x:%02x\n", mac_bytes[0],
        mac_bytes[1], mac_bytes[2], mac_bytes[3], mac_bytes[4], mac_bytes[5]);

    struct sockaddr_ll bind_addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = settings.ethertype,
        .sll_ifindex = ifindex
    };

    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof bind_addr) == -1) {
        perror("Unable to bind to the interface and protocol: ");
        goto exit_error;
    }

    comms->sockfd = sockfd;
    comms->ifindex = ifindex;
    memcpy(comms->peer_mac, settings.peer_mac_ptr, 6);
    return 0;

exit_error:
    if (sockfd != -1) {
        assert(close(sockfd) == 0 && "Error closing socket");
    }
    return -1;
}

int main()
{
    int ret, exit_code = 0;

    struct ether_addr *periph_ctrl_mac_addr = ether_aton(PERIPH_CTRL_MAC);
    if (periph_ctrl_mac_addr == NULL) {
        fprintf(stderr, "Invalid MAC address specified: %s\n", PERIPH_CTRL_MAC);
        exit_code = 1;
        goto close_sock;
    }

    eth_comms_t comms;
    ret = eth_open_and_bind(&comms, (eth_open_and_bind_params_t){
        .ifname = "enp4s0", .ethertype = CUSTOM_ETHERTYPE,
        .peer_mac_ptr = periph_ctrl_mac_addr->ether_addr_octet});
    if (ret == -1) {
        fprintf(stderr, "Can't open the socket or bind to it, exiting...\n");
        return 1;
    }

 
    struct sockaddr_ll periph_ctrl_addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(CUSTOM_ETHERTYPE),
        .sll_ifindex = comms.ifindex,
        .sll_halen = 6
    };
    memcpy(periph_ctrl_addr.sll_addr, periph_ctrl_mac_addr->ether_addr_octet, 6);

    const char ping[4] = {'p', 'i', 'n', 'g'};
    ret = sendto(
        comms.sockfd, ping, sizeof ping, 0,
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
            comms.sockfd, recv_buf, sizeof recv_buf, 0,
            (struct sockaddr *)&recv_addr, &recv_addrlen
        );
        if (ret == -1) {
            perror("Error receiving pong packet: ");
            exit_code = 1;
            goto close_sock;
        }
        assert(recv_addrlen == sizeof recv_addr &&
            "Address of invalid length returned from recvfrom()"
        );
        assert(recv_addr.sll_hatype == ARPHRD_ETHER &&
            "Got packet with unexpected ARP hardware type"
        );
        assert(recv_addr.sll_halen == periph_ctrl_addr.sll_halen &&
            "Got packet with unexpected MAC address length"
        );
        if (
            recv_addr.sll_pkttype == PACKET_HOST &&
            ntohs(recv_addr.sll_protocol == CUSTOM_ETHERTYPE) &&
            memcmp(recv_addr.sll_addr, periph_ctrl_addr.sll_addr, periph_ctrl_addr.sll_halen) == 0
        ) {
            if (ret == sizeof pong && memcmp(recv_buf, pong, sizeof pong) == 0) {
                break;
            }
            fprintf(stderr, "Got pong message != ['p', 'o', 'n', 'g'] (length %d)\n", ret);
            exit_code = 1;
            goto close_sock;
        }
    }

    printf("YEAAAAAH!\n");

close_sock:
    if(close(comms.sockfd) == -1) {
        perror("Somehow close() failed");
    }

    return exit_code;
}
