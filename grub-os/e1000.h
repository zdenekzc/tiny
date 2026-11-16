#pragma once
#include <stdint.h>

int  e1000_probe(void);
int  e1000_init(uint8_t mac[6]);
int  e1000_is_active(void);
int  e1000_send(const void *data, uint16_t len);
int  e1000_recv(void *buf, uint16_t bufsize);
