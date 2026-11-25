#include "hw_pc.h"

#include "platform_pc.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#endif

#define RES(base, size) (base), (((base) + (size) + 0xFFFFu) & ~0xFFFFu)

typedef struct {
    uint32_t    base, end;
    const char *name;
    bool        mapped;
} Region;

static Region s_regions[] = {
    { RES(PC_EWRAM_BASE, PC_EWRAM_SIZE), "EWRAM",         false },
    { RES(PC_IWRAM_BASE, PC_IWRAM_SIZE), "IWRAM",         false },
    { RES(PC_IO_BASE,    PC_IO_SIZE),    "I/O registers", false },
    { RES(PC_PLTT_BASE,  PC_PLTT_SIZE),  "palettes",      false },
    { RES(PC_VRAM_BASE,  PC_VRAM_SIZE),  "VRAM",          false },
    { RES(PC_OAM_BASE,   PC_OAM_SIZE),   "OAM",           false },
};
#define REGION_COUNT (sizeof s_regions / sizeof s_regions[0])

#define PRS_HW_CHILD_ENV "PRS_HW_PRERESERVED"

static bool map_fixed(uint32_t base, uint32_t size) {
    void *want = (void *)(uintptr_t)base;
#if defined(_WIN32)
    /* Relaunched child: the parent already committed the region for us. */
    MEMORY_BASIC_INFORMATION mbi;
    if (GetEnvironmentVariableA(PRS_HW_CHILD_ENV, NULL, 0) != 0 &&
        VirtualQuery(want, &mbi, sizeof mbi) != 0 && mbi.State == MEM_COMMIT &&
        mbi.AllocationBase == want && mbi.Type == MEM_PRIVATE && mbi.RegionSize >= size) {
        return true;
    }
    void *got = VirtualAlloc(want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (got != want) {
        if (got != NULL) {
            VirtualFree(got, 0, MEM_RELEASE);
        }
        return false;
    }
    return true;
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    void *got = mmap(want, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (got == MAP_FAILED) {
        return false;
    }
    if (got != want) {
        munmap(got, size);
        return false;
    }
    return true;
#endif
}

static void unmap_fixed(uint32_t base, uint32_t size) {
#if defined(_WIN32)
    (void)size;
    VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE);
#else
    munmap((void *)(uintptr_t)base, size);
#endif
}

static void report_conflicts(uint32_t base, uint32_t end) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = base; a < end; a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (VirtualQuery((void *)a, &mbi, sizeof mbi) == 0) {
            break;
        }
        if (mbi.State != MEM_FREE) {
            char name[MAX_PATH] = "";
            if (mbi.Type == MEM_IMAGE) {
                GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof name);
            } else if (mbi.Type == MEM_MAPPED) {
                K32GetMappedFileNameA(GetCurrentProcess(), mbi.BaseAddress, name, sizeof name);
            }
            pc_warn("hw:   0x%08llX-0x%08llX in use%s%s", (unsigned long long)(uintptr_t)mbi.BaseAddress,
                    (unsigned long long)((uintptr_t)mbi.BaseAddress + mbi.RegionSize), name[0] ? ": " : "", name);
        }
    }
#else
    (void)base;
    (void)end;
#endif
}

static int s_reserve_state;   /* 0 = not tried, 1 = ok, -1 = failed */
static int s_failed_region = -1;

static void reserve_all(void) {
    if (s_reserve_state != 0) {
        return;
    }
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        Region *r = &s_regions[i];
        if (!map_fixed(r->base, r->end - r->base)) {
            s_failed_region = (int)i;
            for (unsigned j = 0; j < i; j++) {
                unmap_fixed(s_regions[j].base, s_regions[j].end - s_regions[j].base);
                s_regions[j].mapped = false;
            }
            s_reserve_state = -1;
            return;
        }
        r->mapped = true;
    }
    s_reserve_state = 1;
}

#if defined(_WIN32)
/* user32/gdi32 (pulled in by SDL) can map sections at low addresses before
 * any code of ours runs. If so, re-create this process suspended, commit the
 * GBA ranges in the child before it starts, and relay its exit code.
 * PRS_NO_RELAUNCH=1 disables this (debugging). */
static void relaunch_with_prereserved_map(void) {
    if (GetEnvironmentVariableA(PRS_HW_CHILD_ENV, NULL, 0) != 0 ||
        GetEnvironmentVariableA("PRS_NO_RELAUNCH", NULL, 0) != 0) {
        return;
    }
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
        return;
    }
    SetEnvironmentVariableA(PRS_HW_CHILD_ENV, "1");
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, GetCommandLineW(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        SetEnvironmentVariableA(PRS_HW_CHILD_ENV, NULL);
        return;
    }
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        void *want = (void *)(uintptr_t)s_regions[i].base;
        SIZE_T size = s_regions[i].end - s_regions[i].base;
        if (VirtualAllocEx(pi.hProcess, want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != want) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SetEnvironmentVariableA(PRS_HW_CHILD_ENV, NULL);
            return;
        }
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job != NULL) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
        ZeroMemory(&lim, sizeof lim);
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(code);
}
#endif

#if defined(__GNUC__) || defined(__clang__)
/* Grab the GBA ranges before SDL threads, fonts, mappings... can land there. */
__attribute__((constructor(101))) static void reserve_before_main(void) {
    reserve_all();
#if defined(_WIN32)
    if (s_reserve_state < 0) {
        relaunch_with_prereserved_map();
    }
#endif
}
#endif

bool pc_hw_init(void) {
    reserve_all();
    if (s_reserve_state < 0) {
        const Region *r = &s_regions[s_failed_region];
        pc_warn("hw: cannot map %s at 0x%08X-0x%08X:", r->name, r->base, r->end);
        report_conflicts(r->base, r->end);
        return false;
    }
    pc_hw_reset();
    pc_log("hw: GBA memory map reserved at its native addresses");
    return true;
}

void pc_hw_reset(void) {
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        memset((void *)(uintptr_t)s_regions[i].base, 0, s_regions[i].end - s_regions[i].base);
    }
    /* Power-on values the game can observe. */
    *pc_hw_reg16(0x04000000u) = 0x0080;   /* DISPCNT: forced blank */
    *pc_hw_reg16(0x04000020u) = 0x0100;   /* BG2PA */
    *pc_hw_reg16(0x04000026u) = 0x0100;   /* BG2PD */
    *pc_hw_reg16(0x04000030u) = 0x0100;   /* BG3PA */
    *pc_hw_reg16(0x04000036u) = 0x0100;   /* BG3PD */
    *pc_hw_reg16(0x04000088u) = 0x0200;   /* SOUNDBIAS */
    pc_hw_set_keys(0);
    pc_hw_set_vcount(0, false);
}

void pc_hw_shutdown(void) {
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        if (s_regions[i].mapped) {
            unmap_fixed(s_regions[i].base, s_regions[i].end - s_regions[i].base);
            s_regions[i].mapped = false;
        }
    }
    s_reserve_state = 0;
}

bool pc_hw_is_mapped(uint32_t address, uint32_t size) {
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        const Region *r = &s_regions[i];
        if (r->mapped && address >= r->base && address < r->end && size <= r->end - address) {
            return true;
        }
    }
    return false;
}

#define REG_DISPSTAT 0x04000004u
#define REG_VCOUNT   0x04000006u
#define REG_KEYINPUT 0x04000130u

void pc_hw_set_keys(uint16_t keys) {
    *pc_hw_reg16(REG_KEYINPUT) = (uint16_t)(~keys & 0x03FF);
}

void pc_hw_set_vcount(uint16_t line, bool hblank) {
    *pc_hw_reg16(REG_VCOUNT) = line;
    uint16_t stat = *pc_hw_reg16(REG_DISPSTAT) & (uint16_t)~0x0007u;
    if (line >= 160 && line < 227) {
        stat |= 0x0001;                       /* VBlank */
    }
    if (hblank) {
        stat |= 0x0002;                       /* HBlank */
    }
    if ((stat >> 8) == line) {
        stat |= 0x0004;                       /* VCount match */
    }
    *pc_hw_reg16(REG_DISPSTAT) = stat;
}

/* The game's stack below 4 GiB: GBA code keeps addresses in 32-bit words.
 * Windows already places the main stack low when linked at a low base. */
#if defined(_WIN32)
void pc_run_on_low_stack(void (*fn)(void *), void *arg) {
    fn(arg);
}
#else
#if defined(__APPLE__)
#define PRS_ASM_SYM(name) "_" name
#else
#define PRS_ASM_SYM(name) name
#endif
void prs_call_on_stack(void *top, void (*fn)(void *), void *arg);
__asm__(".text\n"
        ".globl " PRS_ASM_SYM("prs_call_on_stack") "\n"
        PRS_ASM_SYM("prs_call_on_stack") ":\n"
        "    push %rbp\n"
        "    mov  %rsp, %rbp\n"
        "    mov  %rdi, %rsp\n"
        "    mov  %rdx, %rdi\n"
        "    call *%rsi\n"
        "    mov  %rbp, %rsp\n"
        "    pop  %rbp\n"
        "    ret\n");

#define PRS_LOW_STACK_SIZE (16u * 1024 * 1024)

void pc_run_on_low_stack(void (*fn)(void *), void *arg) {
    void *stack = MAP_FAILED;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_32BIT)
    stack = mmap(NULL, PRS_LOW_STACK_SIZE, PROT_READ | PROT_WRITE, flags | MAP_32BIT, -1, 0);
#endif
    for (uintptr_t hint = 0x40000000u; stack == MAP_FAILED && hint < 0xF0000000u; hint += 0x10000000u) {
        void *got = mmap((void *)hint, PRS_LOW_STACK_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (got == MAP_FAILED) {
            continue;
        }
        if ((uintptr_t)got + PRS_LOW_STACK_SIZE <= 0x100000000ull) {
            stack = got;
        } else {
            munmap(got, PRS_LOW_STACK_SIZE);
        }
    }
    if (stack == MAP_FAILED) {
        pc_warn("hw: no stack below 4 GiB available, running on the system stack");
        fn(arg);
        return;
    }
    void *top = (char *)stack + PRS_LOW_STACK_SIZE - 64;
    prs_call_on_stack(top, fn, arg);
}
#endif
