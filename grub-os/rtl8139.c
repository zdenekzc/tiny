#include "rtl8139.h"
#include <string.h>

static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline void outw(uint16_t p, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw(uint16_t p){ uint16_t r; __asm__ volatile("inw %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline void outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p){ uint32_t r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p)); return r; }

/* PCI config helpers (local, no externs) */
static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (1U<<31) | ((uint32_t)bus<<16) | ((uint32_t)slot<<11)
                  | ((uint32_t)func<<8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (1U<<31) | ((uint32_t)bus<<16) | ((uint32_t)slot<<11)
                  | ((uint32_t)func<<8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static int pci_find_device(uint16_t vendor, uint16_t device,
                           uint8_t *bus, uint8_t *slot, uint8_t *func) {
    uint32_t target = ((uint32_t)device << 16) | vendor;
    for (uint16_t b=0; b<256; b++) {
        for (uint8_t s=0; s<32; s++) {
            uint32_t id = pci_read32((uint8_t)b, s, 0, 0);
            if (id == target) {
                *bus = (uint8_t)b; *slot = s; *func = 0;
                return 1;
            }
        }
    }
    return 0;
}

static rtl8139_t dev;
static int active = 0;
static int found = 0;
static uint8_t saved_bus, saved_slot, saved_func;

#define RX_BUF_SIZE  8192
static uint8_t rx_buf[RX_BUF_SIZE + 16 + 1500] __attribute__((aligned(4)));
static uint32_t rx_ptr = 0;

static uint8_t tx_cur = 0;
static const uint8_t TSAD[4] = {0x20,0x24,0x28,0x2C};
static const uint8_t TSD[4]  = {0x10,0x14,0x18,0x1C};

static void rtl_read_mac(void) {
    uint32_t lo = inl(dev.io_base + 0x00);
    uint16_t hi = inw(dev.io_base + 0x04);
    dev.mac[0] = lo & 0xFF;
    dev.mac[1] = (lo >> 8) & 0xFF;
    dev.mac[2] = (lo >> 16) & 0xFF;
    dev.mac[3] = (lo >> 24) & 0xFF;
    dev.mac[4] = hi & 0xFF;
    dev.mac[5] = (hi >> 8) & 0xFF;
}

int rtl8139_probe(void) {
    if (found) return 1;
    found = pci_find_device(0x10EC, 0x8139, &saved_bus, &saved_slot, &saved_func);
    return found;
}

int rtl8139_is_active(void) {
    return active;
}

int rtl8139_init(uint8_t mac[6]) {
    if (!rtl8139_probe()) return 0;

    /* enable bus mastering */
    uint32_t cmd = pci_read32(saved_bus, saved_slot, saved_func, 0x04);
    cmd |= (1<<2);
    pci_write32(saved_bus, saved_slot, saved_func, 0x04, cmd);

    /* BAR0 I/O base */
    uint32_t bar0 = pci_read32(saved_bus, saved_slot, saved_func, 0x10);
    dev.io_base = (uint16_t)(bar0 & ~3);

    /* power on */
    outb(dev.io_base + 0x52, 0x00);

    /* reset */
    outb(dev.io_base + 0x37, 0x10);
    while (inb(dev.io_base + 0x37) & 0x10);

    /* init RX buffer */
    outl(dev.io_base + 0x30, (uint32_t)(uintptr_t)rx_buf);

    /* RX config: accept all */
    outl(dev.io_base + 0x44, 0x0000000F);

    /* enable RX + TX */
    outb(dev.io_base + 0x37, 0x0C);

    /* unmask RX/TX OK interrupts */
    outw(dev.io_base + 0x3C, 0x0005);

    rtl_read_mac();
    if (mac) memcpy(mac, dev.mac, 6);

    active = 1;
    return 1;
}

int rtl8139_send(const void *data, uint16_t len) {
    if (!active) return 0;
    uint32_t buf_phys = (uint32_t)(uintptr_t)data;

    outl(dev.io_base + TSAD[tx_cur], buf_phys);
    outl(dev.io_base + TSD[tx_cur], len);

    tx_cur = (tx_cur + 1) & 3;
    return len;
}

int rtl8139_recv(void *buf, uint16_t bufsize) {
    if (!active) return 0;

    /* CMD register bit 0 (BUFE) set means no packet */
    if (inb(dev.io_base + 0x37) & 0x01) return 0;

    uint16_t *hdr = (uint16_t*)(rx_buf + rx_ptr);
    uint16_t status = hdr[0];
    uint16_t len    = hdr[1];

    if (!(status & 0x01)) return 0;

    uint16_t copy = (len > bufsize) ? bufsize : len;
    uint8_t *pkt = (uint8_t*)(hdr + 2);
    memcpy(buf, pkt, copy);

    rx_ptr = (rx_ptr + len + 4 + 3) & ~3;
    if (rx_ptr >= RX_BUF_SIZE) rx_ptr -= RX_BUF_SIZE;
    outw(dev.io_base + 0x38, rx_ptr - 0x10); /* CAPR */

    return copy;
}

void rtl8139_poll(void) {
    if (!active) return;
    uint16_t isr = inw(dev.io_base + 0x3E);
    if (!isr) return;
    outw(dev.io_base + 0x3E, isr); /* ack */
}
