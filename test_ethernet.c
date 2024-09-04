#include <assert.h>  // assert()
#include <arpa/inet.h>  // htons()
#include <net/ethernet.h>  // ETH_P_* constants
#include <net/if.h>  // struct ifreq, various ioctls
#include <net/if_arp.h>  // ARPHRD_ETHER
#include <netinet/ether.h>  // ehter_aton()
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>  // exit(), EXIT_*
#include <string.h>
#include <sys/ioctl.h>  // ioctl()
#include <sys/socket.h>  // socket()
#include <unistd.h>  // close()

#include <linux/if_packet.h>  // struct sockaddr_ll and others

#define CUSTOM_ETHERTYPE 0xDEAD
#define PERIPH_CTRL_MAC "aa:bb:cc:dd:ee:ff"  // Placeholder for now


// Private utility functions
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


// Abstraction around a raw Ethernet socket associated with a single peer on a
// specific network interface.
// TODO: move into a separate header
typedef struct {
    int sockfd;
    // sll_family, sll_protocol, sll_ifindex, sll_addr and sll_halen are used
    struct sockaddr_ll peer_addr;
} eth_comms_t;

typedef struct {
    const char *ifname;
    uint16_t ethertype;  // In host byte order
    unsigned char *peer_mac_ptr;  // Must be 6 bytes long!
} eth_open_and_bind_params_t;

// Open the raw socket, bind it to the specified network interface,
// confiugure the EtherType and the peer MAC address.
//
// Returns: 0 on success, -1 on failure.
int eth_open_and_bind(eth_comms_t *comms, eth_open_and_bind_params_t params)
{
    char ifname_buf[IFNAMSIZ];
    size_t ifname_sz = strlcpy(ifname_buf, params.ifname, sizeof ifname_buf);
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

    // Fill in only the fields needed for bind()
    struct sockaddr_ll peer_addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(params.ethertype),
        .sll_ifindex = ifindex,
    };

    if (bind(sockfd, (struct sockaddr *)&peer_addr, sizeof peer_addr) == -1) {
        perror("Unable to bind to the interface and protocol: ");
        goto exit_error;
    }

    comms->sockfd = sockfd;
    memcpy(&comms->peer_addr, &peer_addr, sizeof peer_addr);
    // Fields needed for further sends
    memcpy(comms->peer_addr.sll_addr, params.peer_mac_ptr, 6);
    comms->peer_addr.sll_halen = 6;
    return 0;

exit_error:
    if (sockfd != -1) {
        assert(close(sockfd) == 0 && "Can't close the socket");
    }
    return -1;
}

// A close() wrapper for the underlying socket
void eth_close(eth_comms_t *comms)
{
    assert(close(comms->sockfd) == 0 && "Can't close the socket");
}

// Send the frame to the configured MAC address with the configured EtherType.
// Compatible with the POSIX send() except for the first argument.
int eth_send_frame(eth_comms_t *comms, const char *buf, size_t len)
{
    int ret = sendto(comms->sockfd, buf, sizeof buf, 0,
        (struct sockaddr *)&comms->peer_addr, sizeof comms->peer_addr);
    if (ret == -1) {
        perror("Error sending Ethernet frame");
    }
    return ret;
}

// Receive the first frame that matches the configured EtherType and peer MAC,
// discarding everything else. Compatible with the POSIX recv() except for the
// first argument
int eth_recv_frame(eth_comms_t *comms, char *buf, size_t len)
{
    int ret;
    struct sockaddr_ll recv_addr;
    socklen_t recv_addrlen = sizeof recv_addr;

    for (;;) {
        ret = recvfrom(
            comms->sockfd, buf, len, 0,
            (struct sockaddr *)&recv_addr, &recv_addrlen
        );
        if (ret == -1) {
            perror("Error receiving Ethernet frame");
            return -1;
        }
        assert(recv_addrlen == sizeof recv_addr &&
            "Address of invalid length returned from recvfrom()"
        );
        assert(recv_addr.sll_hatype == ARPHRD_ETHER &&
            "Got packet with unexpected ARP hardware type"
        );
        assert(recv_addr.sll_halen == 6 &&
            "Got packet with unexpected MAC address length"
        );
        assert(recv_addr.sll_protocol == comms->peer_addr.sll_protocol &&
            "Got packet with unexpected EtherType"
        );
        if (recv_addr.sll_pkttype == PACKET_HOST &&
            memcmp(recv_addr.sll_addr, comms->peer_addr.sll_addr, 6) == 0
        ) {
            return ret;
        }
    }
}

int main()
{
    int ret, exit_code = EXIT_SUCCESS;

    struct ether_addr *periph_ctrl_mac_addr = ether_aton(PERIPH_CTRL_MAC);
    if (periph_ctrl_mac_addr == NULL) {
        fprintf(stderr, "Invalid MAC address specified: %s\n", PERIPH_CTRL_MAC);
        exit_code = EXIT_FAILURE;
        goto finalize;
    }

    eth_comms_t comms;
    ret = eth_open_and_bind(&comms, (eth_open_and_bind_params_t){
        .ifname = "enp4s0", .ethertype = CUSTOM_ETHERTYPE,
        .peer_mac_ptr = periph_ctrl_mac_addr->ether_addr_octet});
    if (ret == -1) {
        fprintf(stderr, "Can't open the socket or bind to it, exiting...\n");
        exit_code = EXIT_FAILURE;
        goto finalize;
    }

    const char ping[4] = {'p', 'i', 'n', 'g'};
    ret = eth_send_frame(&comms, ping, sizeof ping);
    if (ret == -1) {
        fprintf(stderr, "Error sending the ping packet\n");
        exit_code = EXIT_FAILURE;
        goto finalize;
    }
    printf("Ping-\n");

    const char pong[4] = {'p', 'o', 'n', 'g'};
    char recv_buf[sizeof pong];
    ret = eth_recv_frame(&comms, recv_buf, sizeof recv_buf);
    if (ret == -1) {
        fprintf(stderr, "Error receiving the pong packet\n");
        exit_code = EXIT_FAILURE;
        goto finalize;
    }
    if (ret == sizeof pong && memcmp(recv_buf, pong, sizeof pong) == 0) {
        fprintf(stderr, "Got pong frame != ['p', 'o', 'n', 'g'] (length %d)\n",
            ret);
        exit_code = EXIT_FAILURE;
        goto finalize;
    }

    printf("-pong\n");

finalize:
    eth_close(&comms);
    exit(exit_code);
}
