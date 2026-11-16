#include "tcp.h"
#include "ip.h"
#include "net.h"
#include <string.h>

struct tcp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t  offset_reserved;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_ACK 0x10

static uint32_t local_ip;
static uint32_t seq = 1000;
static uint32_t ack = 0;
static uint16_t listen_port = 80;
static uint16_t remote_port = 0;
static uint32_t remote_ip = 0;
static int connection_open = 0;

static uint16_t htons(uint16_t x){return (x<<8)|(x>>8);}
static uint32_t htonl(uint32_t x){return (x<<24)|((x<<8)&0x00FF0000)|((x>>8)&0x0000FF00)|(x>>24);}

static void tcp_send(uint32_t dst_ip, uint16_t dst_port,
                     uint32_t seqn, uint32_t ackn,
                     uint8_t flags,
                     const void *payload, uint16_t plen)
{
    uint8_t pkt[1500];
    struct tcp_hdr *tcp = (struct tcp_hdr*)pkt;

    tcp->sport = htons(listen_port);
    tcp->dport = htons(dst_port);
    tcp->seq   = htonl(seqn);
    tcp->ack   = htonl(ackn);
    tcp->offset_reserved = (5 << 4);
    tcp->flags = flags;
    tcp->window = htons(1024);
    tcp->checksum = 0;
    tcp->urgent = 0;

    memcpy(pkt + sizeof(*tcp), payload, plen);

    ipv4_send_udp(dst_ip, listen_port, dst_port, pkt, sizeof(*tcp) + plen);
}

void tcp_init(uint32_t our_ip) {
    local_ip = our_ip;
}

void tcp_poll(void) {
    uint8_t buf[1500];
    uint32_t src_ip;
    uint16_t src_port;

    int len = ipv4_poll_udp(listen_port, &src_ip, &src_port, buf, sizeof(buf));
    if (!len) return;

    struct tcp_hdr *tcp = (struct tcp_hdr*)buf;
    uint8_t *data = buf + sizeof(*tcp);
    uint16_t dlen = len - sizeof(*tcp);

    uint32_t seqn = __builtin_bswap32(tcp->seq);
    uint32_t ackn = __builtin_bswap32(tcp->ack);

    uint8_t flags = tcp->flags;

    if (flags & TCP_FLAG_SYN) {
        remote_ip = src_ip;
        remote_port = src_port;
        ack = seqn + 1;
        seq = 2000;

        tcp_send(src_ip, src_port, seq, ack, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
        seq++;
        connection_open = 1;
        return;
    }

    if (!connection_open) return;

    if (flags & TCP_FLAG_ACK) {
        // normal ACK
    }

    if (dlen > 0) {
        ack = seqn + dlen;

        const char *resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello from your OS HTTP server!\n";

        tcp_send(src_ip, src_port, seq, ack, TCP_FLAG_ACK, 0, 0);
        tcp_send(src_ip, src_port, seq, ack, TCP_FLAG_ACK | TCP_FLAG_FIN,
                 resp, strlen(resp));
        seq += strlen(resp);
        return;
    }

    if (flags & TCP_FLAG_FIN) {
        ack = seqn + 1;
        tcp_send(src_ip, src_port, seq, ack, TCP_FLAG_ACK | TCP_FLAG_FIN, 0, 0);
        seq++;
        connection_open = 0;
        return;
    }
}
