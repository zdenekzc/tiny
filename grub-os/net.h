#pragma once
#include <stdint.h>

typedef struct {
    uint8_t mac[6];
    uint32_t ip;      // in network byte order
    uint32_t gw_ip;   // gateway IP (network byte order)
} net_config_t;

int  net_init(net_config_t *cfg);          // auto-detect NIC, init, set MAC
int  net_send(const void *data, uint16_t len);
int  net_recv(void *buf, uint16_t bufsize); // returns length or 0
