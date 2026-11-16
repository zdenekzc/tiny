#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void arp_init(uint32_t our_ip);
int  arp_resolve(uint32_t ip, uint8_t mac_out[6]);

int  ipv4_send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                   const void *payload, uint16_t len);
int  ipv4_poll_udp(uint16_t listen_port,
                   uint32_t *src_ip, uint16_t *src_port,
                   void *buf, uint16_t bufsize);

#ifdef __cplusplus
}
#endif
