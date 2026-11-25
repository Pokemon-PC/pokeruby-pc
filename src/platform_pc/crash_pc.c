/*
 * crash_pc.c - report crashes with a raw backtrace.
 *
 * The executable is linked at a fixed base (0x10000000, no ASLR), so the
 * printed addresses are stable and can be turned into file:line with
 *     llvm-addr2line -f -e pokeemerald.exe 0x...
 * (tools/pc_port/symbolize_crash.py does that for a whole log).
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* REG_RIP... in <ucontext.h> */
#endif
#include "pc_hal.h"
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

static void print_backtrace(CONTEXT c, const char *tag);

/*
 * Debug aid for memory corruption: PRS_WATCH=0xADDR arms a hardware write
 * watchpoint (8 bytes) on the game thread. Every write that leaves a value
 * that is not 0 and not a GBA address (GBA map 0x02000000-0x07010000) - e.g. junk in a
 * heap link - is reported with a backtrace; execution then continues.
 */
static DWORD64 s_watch_addr;

static BOOL s_watch_all;   /* PRS_WATCH_ALL=1: report every write */

static BOOL watch_value_suspicious(DWORD64 v) {
    if (s_watch_all) {
        return TRUE;
    }
    return v != 0 && (v < 0x02000000u || v >= 0x07010000u);
}

/* Reports each emulated NULL-read site once (same stable addresses as the
 * crash reports: symbolize.py names them). */
static void note_null_read(DWORD64 rip, uintptr_t addr, bool is_write, CONTEXT c) {
    static DWORD64 seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) {
        if (seen[i] == rip) {
            return;
        }
    }
    if (nseen < 64) {
        seen[nseen++] = rip;
    }
    fprintf(stderr, "[prs:nullread] %s at 0x%llX through NULL emulated (GBA BIOS area: %s):\n",
            is_write ? "write" : "read", (unsigned long long)addr, is_write ? "ignored" : "reads 0");
    print_backtrace(c, "nullread");
}

static void note_div0(CONTEXT c) {
    static DWORD64 seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) {
        if (seen[i] == c.Rip) {
            return;
        }
    }
    if (nseen < 64) {
        seen[nseen++] = c.Rip;
    }
    fprintf(stderr, "[prs:div0] integer division by zero emulated (GBA libgcc: result 0):\n");
    print_backtrace(c, "div0");
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && s_watch_addr != 0 && (ep->ContextRecord->Dr6 & 1)) {
        DWORD64 v = *(volatile DWORD64 *)(uintptr_t)s_watch_addr;
        ep->ContextRecord->Dr6 = 0;
        if (watch_value_suspicious(v)) {
            fprintf(stderr, "\n[prs:watch] 0x%llX <- 0x%016llX\n", (unsigned long long)s_watch_addr,
                    (unsigned long long)v);
            print_backtrace(*ep->ContextRecord, "watch");
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionInformation[0] <= 1) {
        /* A read through NULL: the GBA reads its BIOS there (nullread_pc.c). */
        CONTEXT *c = ep->ContextRecord;
        uint64_t *regs[16] = { &c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                               &c->R8, &c->R9, &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15 };
        DWORD64 rip0 = c->Rip;
        uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        CONTEXT before = *c;
        uint32_t fl = (uint32_t)c->EFlags;
        bool is_write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        if (pc_emulate_null_access(regs, (uint64_t *)&c->Rip, &fl, addr, is_write)) {
            c->EFlags = fl;
            note_null_read(rip0, addr, is_write, before);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        /* Division by zero: 0, as libgcc's __divsi3 on the GBA (nullread_pc.c). */
        CONTEXT *c = ep->ContextRecord;
        uint64_t *regs[16] = { &c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                               &c->R8, &c->R9, &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15 };
        CONTEXT before = *c;
        if (pc_emulate_divide_by_zero(regs, (uint64_t *)&c->Rip)) {
            note_div0(before);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED && code != EXCEPTION_PRIV_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *ctx = ep->ContextRecord;
    fprintf(stderr, "\n[prs:crash] exception 0x%08lX at pc=0x%016llX", (unsigned long)code,
            (unsigned long long)ctx->Rip);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, " (%s 0x%016llX)",
                ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write to" :
                ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" : "read from",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    fprintf(stderr, "\n[prs:crash] rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX rbp=%llX rsp=%llX\n"
                    "[prs:crash] r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX\n",
            (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx,
            (unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
            (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->R8,
            (unsigned long long)ctx->R9, (unsigned long long)ctx->R10, (unsigned long long)ctx->R11,
            (unsigned long long)ctx->R12, (unsigned long long)ctx->R13, (unsigned long long)ctx->R14,
            (unsigned long long)ctx->R15);
    fprintf(stderr, "[prs:crash] backtrace:\n");

    /* Walk the faulting thread's stack with the x64 unwind tables. */
    print_backtrace(*ctx, "crash");
    pc_flash_flush();
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void print_backtrace(CONTEXT c, const char *tag) {
    if (c.Rip == 0) {
        /* Call through a NULL pointer: the caller's return address is on top
         * of the stack. */
        fprintf(stderr, "[prs:%s]   #-- 0x0000000000000000 (call through NULL)\n", tag);
        c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
        c.Rsp += 8;
    }
    for (int depth = 0; depth < 48 && c.Rip != 0; depth++) {
        fprintf(stderr, "[prs:%s]   #%02d 0x%016llX\n", tag, depth, (unsigned long long)c.Rip);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, NULL);
        if (fn == NULL) {
            c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        PVOID handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, NULL);
    }
    fflush(stderr);
}

void pc_log_backtrace(const char *tag) {
    CONTEXT c;
    RtlCaptureContext(&c);
    fprintf(stderr, "[prs:%s] game check failed; backtrace:\n", tag);
    print_backtrace(c, tag);
}

/* --- hang watchdog: if no frame is produced for a while, print where the
 * game thread is (same stable addresses as crash reports). */
static HANDLE s_game_thread;
static volatile LONG s_frames_seen;

void pc_watchdog_kick(void) {
    InterlockedIncrement(&s_frames_seen);
}

static DWORD WINAPI watchdog_main(LPVOID arg) {
    (void)arg;
    LONG last = -1;
    int stalled = 0, reported = 0;
    for (;;) {
        Sleep(1000);
        LONG now = s_frames_seen;
        stalled = (now == last) ? stalled + 1 : 0;
        last = now;
        if (stalled == 5 && reported < 3) {
            reported++;
            if (SuspendThread(s_game_thread) != (DWORD)-1) {
                CONTEXT c;
                memset(&c, 0, sizeof c);
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(s_game_thread, &c)) {
                    fprintf(stderr, "\n[prs:hang] no frame for 5 s (after %ld frames); game thread at:\n", (long)now);
                    print_backtrace(c, "hang");
                }
                /* Automated runs: a hang is a failure, do not wait forever. */
                const char *e = getenv("PRS_HANG_EXIT");
                if (e && *e && *e != '0') {
                    fflush(stderr);
                    ExitProcess(3);
                }
                ResumeThread(s_game_thread);
            }
        }
    }
    return 0;
}

/* Debug registers of a running thread can only be set while it is
 * suspended: done from a helper thread. */
static DWORD WINAPI arm_watch_main(LPVOID arg) {
    (void)arg;
    if (SuspendThread(s_game_thread) == (DWORD)-1) {
        return 1;
    }
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(s_game_thread, &c)) {
        /* PRS_WATCH_LEN = 1, 2, 4 or 8 bytes (default 8); x86 LEN encoding. */
        const char *len_env = getenv("PRS_WATCH_LEN");
        int len = len_env ? atoi(len_env) : 8;
        DWORD64 enc = len == 1 ? 0 : len == 2 ? 1 : len == 4 ? 3 : 2;
        DWORD64 mask = len == 1 ? 0 : len == 2 ? 1 : len == 4 ? 3 : 7;
        c.Dr0 = s_watch_addr & ~mask;
        /* L0 enabled, break on write */
        c.Dr7 = (c.Dr7 & ~(DWORD64)0xF0003) | 1 | (1u << 16) | (enc << 18);
        SetThreadContext(s_game_thread, &c);
    }
    ResumeThread(s_game_thread);
    return 0;
}

void pc_crash_install(void) {
    AddVectoredExceptionHandler(1, on_exception);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_game_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, 0);
    CreateThread(NULL, 64 * 1024, watchdog_main, NULL, 0, NULL);
    const char *watch = getenv("PRS_WATCH");
    if (watch != NULL) {
        s_watch_addr = (DWORD64)strtoull(watch, NULL, 0);
        const char *all = getenv("PRS_WATCH_ALL");
        s_watch_all = all && *all && *all != '0';
        HANDLE t = CreateThread(NULL, 64 * 1024, arm_watch_main, NULL, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        fprintf(stderr, "[prs:watch] hardware write watchpoint on 0x%llX\n", (unsigned long long)s_watch_addr);
    }
}
#else
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#define PRS_HAVE_EXECINFO 1
#endif

/* Stack addresses are stable (non-PIE executable at 0x10000000): they can
 * be symbolized offline (scripts/symbolize.py, llvm-symbolizer). The first
 * line is the interrupted instruction, then the unwound callers. */
static void print_frames(const char *tag, uintptr_t pc) {
    if (pc != 0) {
        fprintf(stderr, "[prs:%s]   #-- 0x%016llX\n", tag, (unsigned long long)pc);
    }
#ifdef PRS_HAVE_EXECINFO
    void *frames[48];
    int n = backtrace(frames, 48);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "[prs:%s]   #%02d %p\n", tag, i, frames[i]);
    }
#endif
    fflush(stderr);
}

/* Registers of the interrupted thread, in x86 encoding order (rax, rcx, rdx,
 * rbx, rsp, rbp, rsi, rdi, r8..r15), for the NULL-access emulator. */
#if defined(__x86_64__) && defined(__linux__)
#define PRS_CAN_EMULATE_NULL 1
static uintptr_t ctx_pc(ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]; }
static bool ctx_fault_is_write(ucontext_t *uc) { return (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0; }
static void ctx_regs(ucontext_t *uc, uint64_t *regs[16], uint64_t **rip, uint64_t **flags) {
    static const int order[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                   REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
    for (int i = 0; i < 16; i++) {
        regs[i] = (uint64_t *)&uc->uc_mcontext.gregs[order[i]];
    }
    *rip = (uint64_t *)&uc->uc_mcontext.gregs[REG_RIP];
    *flags = (uint64_t *)&uc->uc_mcontext.gregs[REG_EFL];
}
#elif defined(__x86_64__) && defined(__APPLE__)
#define PRS_CAN_EMULATE_NULL 1
static uintptr_t ctx_pc(ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext->__ss.__rip; }
static bool ctx_fault_is_write(ucontext_t *uc) { return (uc->uc_mcontext->__es.__err & 2) != 0; }
static void ctx_regs(ucontext_t *uc, uint64_t *regs[16], uint64_t **rip, uint64_t **flags) {
    _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t *r[16] = { &ss->__rax, &ss->__rcx, &ss->__rdx, &ss->__rbx, &ss->__rsp, &ss->__rbp, &ss->__rsi,
                        &ss->__rdi, &ss->__r8, &ss->__r9, &ss->__r10, &ss->__r11, &ss->__r12, &ss->__r13,
                        &ss->__r14, &ss->__r15 };
    memcpy(regs, r, sizeof r);
    *rip = &ss->__rip;
    *flags = &ss->__rflags;
}
#elif defined(__linux__) && defined(__aarch64__)
static uintptr_t ctx_pc(ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.pc; }
#elif defined(__APPLE__) && defined(__aarch64__)
static uintptr_t ctx_pc(ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext->__ss.__pc; }
#else
static uintptr_t ctx_pc(ucontext_t *uc) { (void)uc; return 0; }
#endif

#ifdef PRS_CAN_EMULATE_NULL
static void note_null_read(uintptr_t pc, uintptr_t addr, bool is_write) {
    static uintptr_t seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) {
        if (seen[i] == pc) {
            return;
        }
    }
    if (nseen < 64) {
        seen[nseen++] = pc;
    }
    fprintf(stderr, "[prs:nullread] %s at 0x%llX through NULL emulated (GBA BIOS area: %s):\n",
            is_write ? "write" : "read", (unsigned long long)addr, is_write ? "ignored" : "reads 0");
    print_frames("nullread", pc);
}
#endif

static void on_signal(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
#ifdef PRS_CAN_EMULATE_NULL
    if ((sig == SIGSEGV || sig == SIGBUS) && info != NULL) {
        /* A read/write through NULL: the GBA reads its BIOS there (nullread_pc.c). */
        uint64_t *regs[16], *rip, *flags;
        ctx_regs(uc, regs, &rip, &flags);
        uintptr_t pc0 = (uintptr_t)*rip;
        uint32_t fl = (uint32_t)*flags;
        bool is_write = ctx_fault_is_write(uc);
        if (pc_emulate_null_access(regs, rip, &fl, (uintptr_t)info->si_addr, is_write)) {
            *flags = (*flags & ~0xFFFFFFFFull) | fl;
            note_null_read(pc0, (uintptr_t)info->si_addr, is_write);
            return;
        }
    }
    if (sig == SIGFPE && info != NULL && info->si_code == FPE_INTDIV) {
        /* Division by zero: 0, as libgcc's __divsi3 on the GBA (nullread_pc.c). */
        uint64_t *regs[16], *rip, *flags;
        ctx_regs(uc, regs, &rip, &flags);
        uintptr_t pc0 = (uintptr_t)*rip;
        if (pc_emulate_divide_by_zero(regs, rip)) {
            static uintptr_t seen[64];
            static int nseen;
            bool known = false;
            for (int i = 0; i < nseen; i++) known |= seen[i] == pc0;
            if (!known) {
                if (nseen < 64) seen[nseen++] = pc0;
                fprintf(stderr, "[prs:div0] integer division by zero emulated (GBA libgcc: result 0):\n");
                print_frames("div0", pc0);
            }
            return;
        }
    }
#endif
    fprintf(stderr, "\n[prs:crash] signal %d at pc=0x%016llX (address %p)\n[prs:crash] backtrace:\n", sig,
            (unsigned long long)ctx_pc(uc), info != NULL ? info->si_addr : NULL);
    print_frames("crash", ctx_pc(uc));
    pc_flash_flush();
    _Exit(3);
}

/* --- hang watchdog: if no frame is produced for a while, the game thread is
 * interrupted (SIGUSR2) and prints where it is. */
static pthread_t s_game_thread;
static volatile int s_frames_seen;
static volatile int s_hang_frames;

void pc_watchdog_kick(void) {
    __atomic_add_fetch(&s_frames_seen, 1, __ATOMIC_RELAXED);
}

static void on_hang_probe(int sig, siginfo_t *info, void *uctx) {
    (void)sig;
    (void)info;
    fprintf(stderr, "\n[prs:hang] no frame for 5 s (after %d frames); game thread at:\n", s_hang_frames);
    print_frames("hang", ctx_pc(uctx));
    const char *e = getenv("PRS_HANG_EXIT");
    if (e && *e && *e != '0') {
        _Exit(3);
    }
}

static void *watchdog_main(void *arg) {
    (void)arg;
    int last = -1, stalled = 0, reported = 0;
    for (;;) {
        sleep(1);
        int now = __atomic_load_n(&s_frames_seen, __ATOMIC_RELAXED);
        stalled = (now == last) ? stalled + 1 : 0;
        last = now;
        if (stalled == 5 && reported < 3) {
            reported++;
            s_hang_frames = now;
            pthread_kill(s_game_thread, SIGUSR2);
        }
    }
    return NULL;
}

void pc_log_backtrace(const char *tag) {
    fprintf(stderr, "[prs:%s] game check failed; backtrace:\n", tag);
    print_frames(tag, 0);
}

void pc_crash_install(void) {
    /* Own signal stack: a stack overflow must still be reported. */
    static char alt[64 * 1024];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0 };
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    sa.sa_sigaction = on_hang_probe;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGUSR2, &sa, NULL);
    s_game_thread = pthread_self();
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_main, NULL) == 0) {
        pthread_detach(t);
    }
    if (getenv("PRS_WATCH") != NULL) {
        fprintf(stderr, "[prs:watch] hardware watchpoints are Windows-only for now\n");
    }
}
#endif
