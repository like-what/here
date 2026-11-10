#include "io.h"
#include "syscall.h"
#include "trap.h"

#include <stdint.h>

#pragma GCC optimize("no-tree-vectorize")

#define CLINT_BASE 0x2000000ull
#define MTIME_OFFSET 0xbff8ull
#define MTIMECMP_OFFSET 0x4000ull
#define TIME_SLICE 1024ull

#define CAUSE_ILLEGAL 2ull
#define CAUSE_UECALL 8ull
#define CAUSE_LOAD_PAGE 13ull
#define CAUSE_TIMER ((1ull << 63) | 7ull)

#define SYS_PUTC 1
#define SYS_SLEEP 2
#define SYS_SPAWN 3
#define SYS_EXIT 4

#define MAX_TASKS 8
#define STACK_SIZE 4096

enum TaskState { EMPTY, READY, RUNNING, SLEEPING };

typedef struct {
    enum TaskState state;
    int sleep_left;
    char mark;
    TrapContext ctx;
} Task;

static void copy_ctx(TrapContext* dst, const TrapContext* src) {
    const uint64_t* from = (const uint64_t*)src;
    uint64_t* to = (uint64_t*)dst;
    for (int i = 0; i < (int)(sizeof(TrapContext) / sizeof(uint64_t)); i++) {
        to[i] = from[i];
    }
}

static Task tasks[MAX_TASKS];
static int current = -1;
static int timer_ticks;
static int saw_illegal;
static int saw_load;
static char logbuf[64];
static int logn;

static uint64_t root_pt[512] __attribute__((aligned(4096)));
static uint8_t stacks[MAX_TASKS][STACK_SIZE] __attribute__((aligned(16)));

static uint64_t read_mtime(void) {
    return *(volatile uint64_t*)(CLINT_BASE + MTIME_OFFSET);
}

static void write_mtimecmp(uint64_t value) {
    *(volatile uint64_t*)(CLINT_BASE + MTIMECMP_OFFSET) = value;
}

static void arm_timer(void) {
    write_mtimecmp(read_mtime() + TIME_SLICE);
}

static void enable_user_pages(void) {
    for (int i = 0; i < 512; i++) {
        root_pt[i] = 0;
    }
    /* 1 GiB identity map at 0x80000000, reachable from U-mode. */
    root_pt[2] = (0x80000ull << 10) | 0xdfull;
    uint64_t satp = (8ull << 60) | ((uint64_t)root_pt >> 12);
    asm volatile("csrw 0x180, %0" : : "r"(satp));
    asm volatile(".word 0x12000073");
}

static uint64_t user_mstatus(void) {
    uint64_t mstatus = read_csr_mstatus();
    mstatus &= ~(3ull << 11);
    mstatus |= 1ull << 7;
    return mstatus;
}

static void user_exit_stub(void) {
    __syscall(SYS_EXIT);
}

static int alloc_task(void (*entry)(void), char mark) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != EMPTY) {
            continue;
        }
        TrapContext ctx;
        for (int r = 0; r < 32; r++) {
            ctx.x[r] = 0;
        }
        ctx.x[1] = (uint64_t)user_exit_stub;
        ctx.x[2] = (uint64_t)&stacks[i][STACK_SIZE];
        ctx.mepc = (uint64_t)entry;
        ctx.mstatus = user_mstatus();
        copy_ctx(&tasks[i].ctx, &ctx);
        tasks[i].state = READY;
        tasks[i].sleep_left = 0;
        tasks[i].mark = mark;
        return i;
    }
    return -1;
}

static int any_sleeping(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == SLEEPING) {
            return 1;
        }
    }
    return 0;
}

static void account_sleep(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == SLEEPING && --tasks[i].sleep_left <= 0) {
            tasks[i].state = READY;
            tasks[i].sleep_left = 0;
        }
    }
}

static int pick_next(void) {
    int start = current < 0 ? 0 : current;
    for (int step = 1; step <= MAX_TASKS; step++) {
        int i = (start + step) % MAX_TASKS;
        if (tasks[i].state == READY) {
            return i;
        }
    }
    return -1;
}

static void finish(void) {
    int count_a = 0;
    int count_b = 0;
    int count_c = 0;
    int count_p = 0;
    int count_z = 0;
    int count_f = 0;
    int first_b = -1;
    int last_a = -1;
    printf("monitor log: %s\n", logbuf);
    printf("timer ticks: %d\n", timer_ticks);
    for (int i = 0; i < logn; i++) {
        if (logbuf[i] == 'A') {
            count_a++;
            last_a = i;
        } else if (logbuf[i] == 'B') {
            count_b++;
            if (first_b < 0) {
                first_b = i;
            }
        } else if (logbuf[i] == 'C') {
            count_c++;
        } else if (logbuf[i] == 'P') {
            count_p++;
        } else if (logbuf[i] == 'Z') {
            count_z++;
        } else if (logbuf[i] == 'F') {
            count_f++;
        }
    }
    if (saw_illegal && saw_load && timer_ticks >= 8 && count_a == 4 && count_b == 4 &&
        count_c == 1 && count_p == 1 && count_z == 1 && count_f == 1 && first_b >= 0 &&
        first_b < last_a) {
        printf("=== M-mode monitor checks passed ===\n");
        pass();
    }
    printf("=== M-mode monitor checks failed ===\n");
    fail();
}

static void schedule(TrapContext* cx) {
    for (;;) {
        int next = pick_next();
        if (next >= 0) {
            current = next;
            tasks[next].state = RUNNING;
            copy_ctx(cx, &tasks[next].ctx);
            __traps_return(cx);
        }
        if (!any_sleeping()) {
            finish();
        }
        uint64_t target = read_mtime() + 1;
        while (read_mtime() < target) {
        }
        account_sleep();
    }
}

static void note_char(char ch) {
    if (logn + 1 < (int)sizeof(logbuf)) {
        logbuf[logn++] = ch;
        logbuf[logn] = 0;
    }
    uart_putc(ch);
}

__attribute__((naked)) static void illegal_insn(void) {
    asm volatile(".word 0x00000000\n ret");
}

__attribute__((naked)) static void bad_load(void) {
    asm volatile("li a0, 0x1000\n ld a0, 0(a0)\n ret");
}

static void fault_task(void) {
    illegal_insn();
    bad_load();
    __syscall(SYS_PUTC, 'F');
}

static void task_a(void) {
    for (int n = 0; n < 4; n++) {
        for (volatile int i = 0; i < 20000; i++) {
        }
        __syscall(SYS_PUTC, 'A');
    }
}

static void task_b(void) {
    for (int n = 0; n < 4; n++) {
        for (volatile int i = 0; i < 20000; i++) {
        }
        __syscall(SYS_PUTC, 'B');
    }
}

static void child_task(void) {
    __syscall(SYS_PUTC, 'C');
}

static void spawn_task(void) {
    __syscall(SYS_SPAWN, (long)child_task);
    __syscall(SYS_PUTC, 'P');
}

static void sleep_task(void) {
    __syscall(SYS_SLEEP, 6);
    __syscall(SYS_PUTC, 'Z');
}

static void resume_same(TrapContext* cx) {
    copy_ctx(&tasks[current].ctx, cx);
    __traps_return(cx);
}

void trap_handler(TrapContext* cx) {
    uint64_t cause = read_csr_mcause();
    uint64_t mtval = read_csr_mtval();

    if (cause == CAUSE_TIMER) {
        timer_ticks++;
        arm_timer();
        account_sleep();
        if (current >= 0 && tasks[current].state == RUNNING) {
            copy_ctx(&tasks[current].ctx, cx);
            tasks[current].state = READY;
        }
        schedule(cx);
    }

    if (cause == CAUSE_ILLEGAL) {
        printf("[trap] illegal mcause=%llx mepc=%llx mtval=%llx\n", cause, cx->mepc, mtval);
        if (cause == CAUSE_ILLEGAL && mtval == 0 && cx->mepc == (uint64_t)illegal_insn) {
            saw_illegal = 1;
        }
        cx->mepc += 4;
        resume_same(cx);
    }

    if (cause == CAUSE_LOAD_PAGE) {
        printf("[trap] load fault mcause=%llx mepc=%llx mtval=%llx\n", cause, cx->mepc, mtval);
        if (mtval == 0x1000 && cx->mepc == (uint64_t)bad_load + 4) {
            saw_load = 1;
        }
        cx->mepc += 4;
        resume_same(cx);
    }

    if (cause == CAUSE_UECALL) {
        long nr = (long)cx->x[17];
        long arg = (long)cx->x[10];
        cx->mepc += 4;
        cx->x[10] = 0;
        if (nr == SYS_PUTC) {
            note_char((char)arg);
            resume_same(cx);
        } else if (nr == SYS_SLEEP) {
            copy_ctx(&tasks[current].ctx, cx);
            tasks[current].state = SLEEPING;
            tasks[current].sleep_left = arg > 0 ? (int)arg : 1;
            schedule(cx);
        } else if (nr == SYS_SPAWN) {
            int id = alloc_task((void (*)(void))arg, 'C');
            cx->x[10] = (uint64_t)id;
            resume_same(cx);
        } else if (nr == SYS_EXIT) {
            tasks[current].state = EMPTY;
            schedule(cx);
        }
        printf("unknown ecall %ld\n", nr);
        fail();
    }

    printf("unexpected trap mcause=%llx mepc=%llx mtval=%llx\n", cause, cx->mepc, mtval);
    fail();
}

int main(void) {
    printf("=== M-mode monitor ===\n");
    enable_user_pages();
    arm_timer();
    trap_init();

    alloc_task(fault_task, 'F');
    alloc_task(task_a, 'A');
    alloc_task(task_b, 'B');
    alloc_task(sleep_task, 'Z');
    alloc_task(spawn_task, 'P');

    uint64_t kernel_sp;
    asm volatile("csrr %0, mscratch" : "=r"(kernel_sp));
    TrapContext* frame = (TrapContext*)(kernel_sp - sizeof(TrapContext));
    current = 0;
    tasks[0].state = RUNNING;
    copy_ctx(frame, &tasks[0].ctx);
    __traps_return(frame);
    return 0;
}
