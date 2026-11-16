typedef struct task {
    uint32_t esp;
    uint32_t ebp;
    uint32_t eip;
    uint32_t pid;
    uint32_t state;
    struct task *next;
} task_t;
