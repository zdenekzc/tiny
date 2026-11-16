#pragma once
#include <stdint.h>

typedef struct {
    uint32_t io_base;
    uint8_t  mac[6];
} rtl8139_t;

int  rtl8139_probe(void);
int  rtl8139_init(uint8_t mac[6]);
int  rtl8139_is_active(void);
int  rtl8139_send(const void *data, uint16_t len);
int  rtl8139_recv(void *buf, uint16_t bufsize);
void rtl8139_poll(void);
