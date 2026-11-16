#include "ip.h"
#include <string.h>

#define TFTP_PORT 69

int tftp_get(const char *filename, uint8_t *dest, uint32_t maxlen, uint32_t *out_size) {
    uint16_t client_port = 40000; // arbitrary
    uint8_t rrq[2 + 256];
    uint16_t idx = 0;

    rrq[idx++] = 0x00; rrq[idx++] = 0x01; // RRQ
    size_t flen = strlen(filename);
    memcpy(&rrq[idx], filename, flen); idx += flen;
    rrq[idx++] = 0;
    memcpy(&rrq[idx], "octet", 5); idx += 5;
    rrq[idx++] = 0;

    uint32_t server_ip = 0x0A000202; // 10.0.2.2

    ipv4_send_udp(server_ip, client_port, TFTP_PORT, rrq, idx);

    uint32_t offset = 0;
    uint16_t block_expected = 1;
    uint16_t server_port = 0;

    for (;;) {
        uint8_t buf[600];
        uint32_t src_ip;
        uint16_t src_port;
        int len = ipv4_poll_udp(client_port, &src_ip, &src_port, buf, sizeof(buf));
        if (!len) continue;

        if (!server_port) server_port = src_port;

        if (len < 4) continue;
        uint16_t op = (buf[0]<<8)|buf[1];
        uint16_t block = (buf[2]<<8)|buf[3];

        if (op == 3) { // DATA
            if (block != block_expected) {
                // resend last ACK?
            } else {
                uint16_t dlen = len - 4;
                if (offset + dlen > maxlen) dlen = maxlen - offset;
                memcpy(dest + offset, buf + 4, dlen);
                offset += dlen;

                uint8_t ack[4] = {0x00,0x04, buf[2], buf[3]};
                ipv4_send_udp(server_ip, client_port, server_port, ack, 4);

                block_expected++;
                if (dlen < 512) break; // last block
            }
        } else if (op == 5) {
            // ERROR
            break;
        }
    }

    if (out_size) *out_size = offset;
    return 1;
}
