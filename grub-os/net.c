#include "net.h"
#include "e1000.h"
#include "rtl8139.h"
#include <string.h>

static net_config_t g_cfg;

int net_init(net_config_t *cfg) {
    memcpy(&g_cfg, cfg, sizeof(g_cfg));

    if (e1000_probe() && e1000_init(g_cfg.mac)) return 1;
    if (rtl8139_probe() && rtl8139_init(g_cfg.mac)) return 1;
    return 0;
}

int net_send(const void *data, uint16_t len) {
    if (e1000_is_active()) return e1000_send(data, len);
    if (rtl8139_is_active()) return rtl8139_send(data, len);
    return 0;
}

int net_recv(void *buf, uint16_t bufsize) {
    if (e1000_is_active()) return e1000_recv(buf, bufsize);
    if (rtl8139_is_active()) return rtl8139_recv(buf, bufsize);
    return 0;
}
