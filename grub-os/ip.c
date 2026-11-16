#include "ip.h"
#include "net.h"
#include <string.h>

#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP  0x0800

struct eth_hdr {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
} __attribute__((packed));

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t check;
} __attribute__((packed));

static net_config_t *cfg; // we’ll grab pointer from net_init if you like
static uint8_t broadcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

static uint16_t htons(uint16_t x){return (x<<8)|(x>>8);}
static uint32_t htonl(uint32_t x){return (x<<24)|((x<<8)&0x00FF0000)|((x>>8)&0x0000FF00)|(x>>24);}

static uint16_t ip_checksum(const void *vdata, size_t length) {
    const uint8_t *data = vdata;
    uint32_t acc = 0;
    for (size_t i=0;i+1<length;i+=2) {
        uint16_t word = (data[i]<<8) + data[i+1];
        acc += word;
        if (acc > 0xffff) acc -= 0xffff;
    }
    if (length & 1) {
        uint16_t word = data[length-1]<<8;
        acc += word;
        if (acc > 0xffff) acc -= 0xffff;
    }
    return ~acc;
}

/* ARP cache: single entry for gateway or server */
static uint32_t cached_ip = 0;
static uint8_t cached_mac[6];

void arp_init(uint32_t our_ip) {
    (void)our_ip;
    cached_ip = 0;
}

int arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    if (cached_ip == ip) {
        memcpy(mac_out, cached_mac, 6);
        return 1;
    }

    uint8_t pkt[64] = {0};
    struct eth_hdr *eth = (struct eth_hdr*)pkt;
    struct arp_pkt *arp = (struct arp_pkt*)(pkt + sizeof(*eth));

    memcpy(eth->dst, broadcast_mac, 6);
    memcpy(eth->src, cfg->mac, 6);
    eth->type = htons(ETH_TYPE_ARP);

    arp->htype = htons(1);
    arp->ptype = htons(ETH_TYPE_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(1); // request
    memcpy(arp->sha, cfg->mac, 6);
    arp->spa = cfg->ip;
    memset(arp->tha, 0, 6);
    arp->tpa = ip;

    net_send(pkt, 42);

    uint8_t buf[1514];
    for (;;) {
        int len = net_recv(buf, sizeof(buf));
        if (!len) continue;
        struct eth_hdr *reth = (struct eth_hdr*)buf;
        if (reth->type != htons(ETH_TYPE_ARP)) continue;
        struct arp_pkt *rarp = (struct arp_pkt*)(buf + sizeof(*reth));
        if (rarp->oper != htons(2)) continue;
        if (rarp->spa != ip) continue;
        memcpy(mac_out, rarp->sha, 6);
        cached_ip = ip;
        memcpy(cached_mac, mac_out, 6);
        return 1;
    }
}

/* IPv4 + UDP */

int ipv4_send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void *payload, uint16_t len) {
    uint8_t mac[6];
    if (!arp_resolve(dst_ip, mac)) return 0;

    uint8_t pkt[1514];
    struct eth_hdr *eth = (struct eth_hdr*)pkt;
    struct ip_hdr  *ip  = (struct ip_hdr*)(pkt + sizeof(*eth));
    struct udp_hdr *udp = (struct udp_hdr*)((uint8_t*)ip + sizeof(*ip));
    uint8_t *data = (uint8_t*)udp + sizeof(*udp);

    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, cfg->mac, 6);
    eth->type = htons(ETH_TYPE_IP);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = htons(sizeof(*ip) + sizeof(*udp) + len);
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = 17; // UDP
    ip->check = 0;
    ip->saddr = cfg->ip;
    ip->daddr = dst_ip;
    ip->check = ip_checksum(ip, sizeof(*ip));

    udp->sport = htons(src_port);
    udp->dport = htons(dst_port);
    udp->len   = htons(sizeof(*udp) + len);
    udp->check = 0; // skip UDP checksum

    memcpy(data, payload, len);

    uint16_t total = sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + len;
    return net_send(pkt, total);
}

int ipv4_poll_udp(uint16_t listen_port,
                  uint32_t *src_ip, uint16_t *src_port,
                  void *buf, uint16_t bufsize) {
    uint8_t pkt[1514];
    int len = net_recv(pkt, sizeof(pkt));
    if (!len) return 0;

    struct eth_hdr *eth = (struct eth_hdr*)pkt;
    if (eth->type != htons(ETH_TYPE_IP)) return 0;

    struct ip_hdr *ip = (struct ip_hdr*)(pkt + sizeof(*eth));
    if (ip->proto != 17) return 0;

    struct udp_hdr *udp = (struct udp_hdr*)((uint8_t*)ip + (ip->ver_ihl & 0x0F)*4);
    uint16_t dport = (udp->dport<<8)|(udp->dport>>8);
    if (dport != listen_port) return 0;

    uint16_t ulen = (udp->len<<8)|(udp->len>>8);
    uint16_t plen = ulen - sizeof(*udp);
    if (plen > bufsize) plen = bufsize;

    uint8_t *data = (uint8_t*)udp + sizeof(*udp);
    memcpy(buf, data, plen);

    if (src_ip) *src_ip = ip->saddr;
    if (src_port) *src_port = (udp->sport<<8)|(udp->sport>>8);

    return plen;
}
