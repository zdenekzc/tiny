#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void tcp_init(uint32_t our_ip);
void tcp_poll(void);

#ifdef __cplusplus
}
#endif
