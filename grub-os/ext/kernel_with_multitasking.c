#include <stdint.h>
#include <stddef.h>

/* I/O */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0,%1"::"a"(val),"Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1,%0":"=a"(r):"Nd"(port)); return r;
}

/* VGA */

#define VGA_MEM ((uint16_t*)0xB8000)
#define VGA_W   80
#define VGA_H   25
#define COLOR(fg,bg) ((fg)|((bg)<<4))

static uint8_t term_color = COLOR(0xF,0x0);

void vga_clear(void) {
    for (int y=0;y<VGA_H;y++)
        for (int x=0;x<VGA_W;x++)
            VGA_MEM[y*VGA_W+x] = ((uint16_t)term_color<<8) | ' ';
}

void vga_puts_at(const char *s, int x, int y, uint8_t color) {
    while (*s && x<VGA_W) {
        VGA_MEM[y*VGA_W+x] = ((uint16_t)color<<8) | (uint8_t)*s++;
        x++;
    }
}

/* IDT */

struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern void isr_timer_stub(void);   /* from timer.asm */
extern void idt_load(void *);       /* from idt.asm   */

static void idt_set_gate(int n, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[n].base_lo = base & 0xFFFF;
    idt[n].base_hi = (base >> 16) & 0xFFFF;
    idt[n].sel     = sel;
    idt[n].always0 = 0;
    idt[n].flags   = flags;
}

static void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    for (int i=0;i<256;i++) {
        idt[i].base_lo = 0;
        idt[i].base_hi = 0;
        idt[i].sel     = 0x08;
        idt[i].always0 = 0;
        idt[i].flags   = 0x8E;
    }

    idt_set_gate(32, (uint32_t)isr_timer_stub, 0x08, 0x8E); /* IRQ0 */

    idt_load(&idtp);
}

/* PIT timer */

static void pit_init(uint32_t hz) {
    uint32_t divisor = 1193180 / hz;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

/* Tasking */

typedef struct task {
    uint32_t esp;
    uint32_t ebp;
    uint32_t eip;
    uint32_t id;
    struct task *next;
} task_t;

extern void context_switch(uint32_t *old_esp, uint32_t *old_ebp,
                           uint32_t new_esp, uint32_t new_ebp, uint32_t new_eip);

#define MAX_TASKS  4
static task_t tasks[MAX_TASKS];
static int    task_count = 0;
static task_t *current = NULL;

static uint8_t *stack_top = (uint8_t*)0x00900000; /* simple static stacks */

static task_t *task_create(void (*entry)(void)) {
    if (task_count >= MAX_TASKS) return NULL;

    task_t *t = &tasks[task_count];
    t->id = task_count;

    uint8_t *stack = stack_top - 0x2000; /* 8 KB per task */
    stack_top = stack;

    uint32_t *stk = (uint32_t*)(stack + 0x2000);
    *(--stk) = (uint32_t)entry; /* fake return address */

    t->esp = (uint32_t)stk;
    t->ebp = (uint32_t)stk;
    t->eip = (uint32_t)entry;
    t->next = NULL;

    task_count++;
    return t;
}

static void scheduler_init(void) {
    for (int i=0;i<task_count;i++) {
        tasks[i].next = &tasks[(i+1)%task_count];
    }
    current = &tasks[0];
}

void schedule(void) {
    if (!current || !current->next) return;
    task_t *prev = current;
    task_t *next = current->next;
    current = next;

    context_switch(&prev->esp, &prev->ebp, next->esp, next->ebp, next->eip);
}

/* Timer ISR (C part) */

void timer_handler(void) {
    static uint32_t ticks = 0;
    ticks++;
    if (ticks % 10 == 0) { /* switch every 10 ticks */
        schedule();
    }
}

/* Demo tasks */

void task_a(void) {
    int y = 5;
    while (1) {
        vga_puts_at("Task A running   ", 0, y, COLOR(0xA,0x0));
        for (volatile int i=0;i<1000000;i++);
    }
}

void task_b(void) {
    int y = 6;
    while (1) {
        vga_puts_at("Task B running   ", 0, y, COLOR(0xE,0x0));
        for (volatile int i=0;i<1000000;i++);
    }
}

/* Kernel entry */

void kernel_main(void) {
    vga_clear();
    vga_puts_at("Multitasking demo kernel", 0, 0, COLOR(0xF,0x0));

    idt_init();
    pit_init(100); /* 100 Hz */

    task_create(task_a);
    task_create(task_b);
    scheduler_init();

    __asm__ volatile("sti"); /* enable interrupts */

    while (1) {
        vga_puts_at("Kernel idle loop ", 0, 2, COLOR(0x7,0x0));
        for (volatile int i=0;i<1000000;i++);
    }
}
