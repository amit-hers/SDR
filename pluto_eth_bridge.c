/*
 * pluto_eth_bridge.c — Runs on PlutoSDR Zynq ARM
 *
 * Bridges the physical Ethernet (eth0 / RJ45) to a UDP tunnel
 * that connects to the host PC running sol8.
 *
 * Host PC (sol8 L2bridge TAP) <──UDP 5900/5901──> Zynq (this daemon) <──raw socket──> eth0 (RJ45)
 *
 * Build (on host with cross-compiler):
 *   arm-linux-gnueabihf-gcc -O2 -static -o pluto_eth_bridge pluto_eth_bridge.c
 *
 * Usage on PlutoSDR:
 *   ./pluto_eth_bridge <host_ip> [eth_iface] [udp_port]
 *   ./pluto_eth_bridge 192.168.2.10          # host is 192.168.2.10, eth0, port 5900
 *   ./pluto_eth_bridge 192.168.2.10 eth0 5900
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>   /* AF_PACKET, sockaddr_ll */
#include <linux/if_ether.h>    /* ETH_P_ALL */

#define MAX_FRAME   1514
#define DEFAULT_IF  "eth0"
#define DEFAULT_PORT 5900

static volatile int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* Open a raw L2 socket on the named interface, return ifindex too */
static int open_raw(const char *iface, int *ifindex_out)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket AF_PACKET"); return -1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ-1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX"); close(fd); return -1;
    }
    int ifindex = ifr.ifr_ifindex;
    if (ifindex_out) *ifindex_out = ifindex;

    /* Bind to this interface only */
    struct sockaddr_ll sll = {
        .sll_family   = AF_PACKET,
        .sll_protocol = htons(ETH_P_ALL),
        .sll_ifindex  = ifindex,
    };
    if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        perror("bind AF_PACKET"); close(fd); return -1;
    }

    /* Put interface in promiscuous mode */
    struct packet_mreq mr = {
        .mr_ifindex = ifindex,
        .mr_type    = PACKET_MR_PROMISC,
    };
    setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr));

    printf("[bridge] raw socket on %s (ifindex=%d)\n", iface, ifindex);
    return fd;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <host_ip> [iface] [port]\n", argv[0]);
        return 1;
    }
    const char *host_ip = argv[1];
    const char *iface   = argc >= 3 ? argv[2] : DEFAULT_IF;
    int         port    = argc >= 4 ? atoi(argv[3]) : DEFAULT_PORT;

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    /* Raw socket for eth0 */
    int ifindex = 0;
    int raw_fd  = open_raw(iface, &ifindex);
    if (raw_fd < 0) return 1;

    /* UDP socket for host tunnel
     *   RX on port   → frames FROM host → write to eth0
     *   TX to port+1 → frames FROM eth0 → send to host
     */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("socket UDP"); close(raw_fd); return 1; }

    /* Bind UDP to listen port (host sends to this port) */
    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port),
    };
    if (bind(udp_fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("bind UDP"); close(raw_fd); close(udp_fd); return 1;
    }

    /* Host destination address (for frames going TO host) */
    struct sockaddr_in host_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port + 1),   /* host listens on port+1 */
    };
    host_addr.sin_addr.s_addr = inet_addr(host_ip);

    /* Destination for sending raw frames back to eth0 */
    struct sockaddr_ll dst_ll = {
        .sll_family  = AF_PACKET,
        .sll_ifindex = ifindex,
        .sll_halen   = 6,
    };

    printf("[bridge] UDP tunnel: recv on :%d  send to %s:%d\n",
           port, host_ip, port+1);
    printf("[bridge] Running — Ctrl-C to stop\n");

    uint8_t  buf[MAX_FRAME + 4];
    uint64_t eth_rx = 0, eth_tx = 0, udp_rx = 0, udp_tx = 0;
    time_t   last_print = time(NULL);

    while (g_run) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(raw_fd, &fds);
        FD_SET(udp_fd, &fds);
        int nfds = (raw_fd > udp_fd ? raw_fd : udp_fd) + 1;
        struct timeval tv = {1, 0};

        int n = select(nfds, &fds, NULL, NULL, &tv);
        if (n < 0 && errno == EINTR) continue;

        /* ── Ethernet frame → UDP to host ─────────────────────── */
        if (n > 0 && FD_ISSET(raw_fd, &fds)) {
            struct sockaddr_ll from;
            socklen_t flen = sizeof(from);
            ssize_t len = recvfrom(raw_fd, buf, sizeof(buf), 0,
                                   (struct sockaddr*)&from, &flen);
            if (len > 14) {
                /* Skip frames we transmitted ourselves (loopback avoidance) */
                if (from.sll_pkttype != PACKET_OUTGOING) {
                    sendto(udp_fd, buf, len, 0,
                           (struct sockaddr*)&host_addr, sizeof(host_addr));
                    eth_rx++; udp_tx++;
                }
            }
        }

        /* ── UDP from host → write to eth0 ──────────────────────── */
        if (n > 0 && FD_ISSET(udp_fd, &fds)) {
            ssize_t len = recvfrom(udp_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (len >= 14) {
                /* Destination MAC is the first 6 bytes of the frame */
                memcpy(dst_ll.sll_addr, buf, 6);
                sendto(raw_fd, buf, len, 0,
                       (struct sockaddr*)&dst_ll, sizeof(dst_ll));
                udp_rx++; eth_tx++;
            }
        }

        /* Print stats every 10 seconds */
        time_t now = time(NULL);
        if (now - last_print >= 10) {
            printf("[bridge] eth_rx=%llu eth_tx=%llu udp_rx=%llu udp_tx=%llu\n",
                   (unsigned long long)eth_rx, (unsigned long long)eth_tx,
                   (unsigned long long)udp_rx, (unsigned long long)udp_tx);
            last_print = now;
        }
    }

    printf("[bridge] Stopped. eth_rx=%llu eth_tx=%llu udp_rx=%llu udp_tx=%llu\n",
           (unsigned long long)eth_rx, (unsigned long long)eth_tx,
           (unsigned long long)udp_rx, (unsigned long long)udp_tx);
    close(raw_fd);
    close(udp_fd);
    return 0;
}
