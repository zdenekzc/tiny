#include "e1000.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* I/O helpers */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0,%1"::"a"(val),"Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(port));
    return r;
}

/* MMIO access */
static volatile uint32_t *mmio = 0;
static int active = 0;

static inline uint32_t mmio_read(uint32_t reg) {
    return mmio[reg/4];
}
static inline void mmio_write(uint32_t reg, uint32_t val) {
    mmio[reg/4] = val;
}

/* PCI config access */
static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (1U<<31) | (bus<<16) | (slot<<11) | (func<<8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (1U<<31) | (bus<<16) | (slot<<11) | (func<<8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

/* TX/RX ring sizes */
#define E1000_NUM_RX 32
#define E1000_NUM_TX 8

/* RX descriptor */
struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* TX descriptor */
struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static struct rx_desc rx_ring[E1000_NUM_RX] __attribute__((aligned(16)));
static struct tx_desc tx_ring[E1000_NUM_TX] __attribute__((aligned(16)));

static uint8_t rx_bufs[E1000_NUM_RX][2048];
static uint8_t tx_bufs[E1000_NUM_TX][2048];

static uint32_t rx_cur = 0;
static uint32_t tx_cur = 0;

/* E1000 registers */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_RAL0     0x5400
#define REG_RAH0     0x5404
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400

#define RCTL_EN      (1<<1)
#define RCTL_SBP     (1<<2)
#define RCTL_UPE     (1<<3)
#define RCTL_MPE     (1<<4)
#define RCTL_LPE     (1<<5)
#define RCTL_BAM     (1<<15)
#define RCTL_SECRC   (1<<26)

#define TCTL_EN      (1<<1)
#define TCTL_PSP     (1<<3)

#define TX_CMD_EOP   (1<<0)
#define TX_CMD_IFCS  (1<<1)
#define TX_CMD_RS    (1<<3)
#define TX_STATUS_DD (1<<0)
#define RX_STATUS_DD (1<<0)

static uint8_t saved_bus, saved_slot, saved_func;
static int found = 0;

static int find_e1000(void) {
    for (uint16_t b=0; b<256; b++) {
        for (uint8_t s=0; s<32; s++) {
            uint32_t id = pci_read32((uint8_t)b, s, 0, 0);
            if (id == 0x100E8086) {
                saved_bus = (uint8_t)b; saved_slot = s; saved_func = 0;
                return 1;
            }
        }
    }
    return 0;
}

int e1000_probe(void) {
    if (found) return 1;
    found = find_e1000();
    return found;
}

int e1000_is_active(void) {
    return active;
}

int e1000_init(uint8_t mac[6]) {
    if (!e1000_probe()) return 0;

    /* Enable bus mastering */
    uint32_t cmd = pci_read32(saved_bus, saved_slot, saved_func, 0x04);
    pci_write32(saved_bus, saved_slot, saved_func, 0x04, cmd | (1<<2));

    /* BAR0 */
    uint32_t bar0 = pci_read32(saved_bus, saved_slot, saved_func, 0x10);
    mmio = (volatile uint32_t*)(uintptr_t)(bar0 & ~0xF);

    /* Reset */
    mmio_write(REG_CTRL, mmio_read(REG_CTRL) | (1<<26));
    for (volatile int i=0;i<100000;i++);

    /* Program MAC into RAL0/RAH0 */
    uint32_t ral = (uint32_t)mac[0] | ((uint32_t)mac[1]<<8)
                 | ((uint32_t)mac[2]<<16) | ((uint32_t)mac[3]<<24);
    uint32_t rah = (uint32_t)mac[4] | ((uint32_t)mac[5]<<8) | (1U<<31);
    mmio_write(REG_RAL0, ral);
    mmio_write(REG_RAH0, rah);

    /* Init RX ring */
    memset(rx_ring, 0, sizeof(rx_ring));
    for (int i=0;i<E1000_NUM_RX;i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    mmio_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    mmio_write(REG_RDBAH, 0);
    mmio_write(REG_RDLEN, sizeof(rx_ring));
    mmio_write(REG_RDH, 0);
    mmio_write(REG_RDT, E1000_NUM_RX - 1);
    mmio_write(REG_RCTL, RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    /* Init TX ring */
    memset(tx_ring, 0, sizeof(tx_ring));
    for (int i=0;i<E1000_NUM_TX;i++) {
        tx_ring[i].addr = (uint64_t)(uintptr_t)tx_bufs[i];
        tx_ring[i].status = TX_STATUS_DD;
    }
    mmio_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    mmio_write(REG_TDBAH, 0);
    mmio_write(REG_TDLEN, sizeof(tx_ring));
    mmio_write(REG_TDH, 0);
    mmio_write(REG_TDT, 0);
    mmio_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));

    rx_cur = 0;
    tx_cur = 0;
    active = 1;
    return 1;
}

int e1000_send(const void *data, uint16_t len) {
    if (!active) return 0;
    if (len > 2048) len = 2048;

    uint32_t idx = tx_cur;
    memcpy(tx_bufs[idx], data, len);
    tx_ring[idx].addr = (uint64_t)(uintptr_t)tx_bufs[idx];
    tx_ring[idx].length = len;
    tx_ring[idx].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    tx_ring[idx].status = 0;

    tx_cur = (tx_cur + 1) % E1000_NUM_TX;
    mmio_write(REG_TDT, tx_cur);

    /* wait for descriptor done */
    while (!(tx_ring[idx].status & TX_STATUS_DD)) { }
    return len;
}

int e1000_recv(void *buf, uint16_t bufsize) {
    if (!active) return 0;

    struct rx_desc *d = &rx_ring[rx_cur];
    if (!(d->status & RX_STATUS_DD)) return 0;

    uint16_t len = d->length;
    if (len > bufsize) len = bufsize;
    memcpy(buf, rx_bufs[rx_cur], len);

    d->status = 0;
    mmio_write(REG_RDT, rx_cur);
    rx_cur = (rx_cur + 1) % E1000_NUM_RX;
    return len;
}
