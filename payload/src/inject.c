#include "inject.h"

#ifdef SL_PS5

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <x86/reg.h>

#include <ps5/kernel.h>

#define SYS_PTRACE 26

/*
 * Authid temporarily installed on the payload process while it attaches to
 * a system process as a debugger. The payload keeps its original authid in
 * `saved_authid` across the ptrace call and restores it before returning.
 */
#define SL_PTRACE_AUTHID 0x4800000000010003ULL

/*
 * struct kinfo_proc offsets used by sl_find_pid.
 *
 * We walk the KERN_PROC_PROC sysctl output (mib = { CTL_KERN, KERN_PROC,
 * KERN_PROC_PROC }) rather than linking the FreeBSD libutil helpers. Each
 * entry starts with its total size as an int, so we read the pid at +0x48
 * and the comm string at +0x1bf relative to that entry.
 */
#define KINFO_PID_OFF   0x48
#define KINFO_COMM_OFF  0x1bf

/* Attach retry: SIGSTOP is delivered asynchronously, so the stopped state
 * takes a few ms to settle after PT_ATTACH. We poll waitpid() in 5 ms
 * increments for up to this many ms before giving up. */
#define SL_WAIT_STOP_MS 1995
#define SL_WAIT_STEP_MS 5

/*
 * ptrace with the payload temporarily given the debugger authid and full caps.
 * Restores authid and caps afterwards and preserves errno.
 */
static long inj_ptrace(int req, pid_t pid, void *addr, int data)
{
    pid_t self = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (saved_authid == 0) return -1;

    uint8_t saved_caps[16];
    if (kernel_get_ucred_caps(self, saved_caps) != 0) return -1;

    uint8_t full_caps[16];
    memset(full_caps, 0xff, sizeof(full_caps));

    if (kernel_set_ucred_authid(self, SL_PTRACE_AUTHID) != 0) return -1;
    if (kernel_set_ucred_caps(self, full_caps) != 0) {
        kernel_set_ucred_authid(self, saved_authid);
        return -1;
    }

    errno = 0;
    long rc = syscall(SYS_PTRACE, req, pid, addr, data);
    int err = errno;

    int caps_ok = kernel_set_ucred_caps(self, saved_caps);
    int authid_ok = kernel_set_ucred_authid(self, saved_authid);
    if (caps_ok == 0 && authid_ok == 0) errno = err;
    return rc;
}

int sl_find_pid(const char *name)
{
    int mib[4] = { 1, 14, 8, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0) return -1;

    char *buf = malloc(len);
    if (!buf) return -1;

    int pid = -1;
    if (sysctl(mib, 4, buf, &len, NULL, 0) == 0) {
        char *end = buf + len;
        for (char *p = buf; p < end;) {
            int entry_size = *(int *)p;
            if (entry_size <= 0) break;
            if (strcmp(p + KINFO_COMM_OFF, name) == 0) {
                pid = *(int *)(p + KINFO_PID_OFF);
                break;
            }
            p += entry_size;
        }
    }
    free(buf);
    return pid;
}

intptr_t sl_resolve_in(pid_t pid, const char *basename, const char *sym)
{
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, basename, &handle) != 0) return 0;
    return kernel_dynlib_dlsym(pid, handle, sym);
}

static pid_t tracee(const sl_inject *ctx)
{
    return ctx->thread_pid > 0 ? ctx->thread_pid : ctx->target_pid;
}

/* Poll waitpid until the target is stopped by a signal, or SL_WAIT_STOP_MS
 * elapses. Returns 0 if a stop was observed, -1 on timeout or wait error. */
static int wait_for_stop(pid_t pid)
{
    int status;
    for (int waited = 0; waited < SL_WAIT_STOP_MS; waited += SL_WAIT_STEP_MS) {
        pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED);
        /* FreeBSD WSTOPPED(status): low 8 bits are 0x7f. */
        if (r == pid) return (status & 0x7f) == 0x7f ? 0 : -1;
        if (r < 0 && errno != EINTR) return -1;
        usleep(SL_WAIT_STEP_MS * 1000);
    }
    return -1;
}

/*
 * Attach to `name` and stop it. Retries EPERM/EBUSY for about 2 s, then
 * records a scratch area just below the stopped RIP.
 */
static FILE *g_map_dump;

void sl_inject_set_map_dump(FILE *out)
{
    g_map_dump = out;
}

#define SL_MAP_DUMP_SPAN 0x200000
#define SL_MAP_DUMP_STEP 0x4000

static void dump_map(sl_inject *ctx, uint64_t center, uint64_t rsp)
{
    if (!g_map_dump) return;
    FILE *out = g_map_dump;
    g_map_dump = NULL;

    fprintf(out, "map dump rip=0x%llx rsp=0x%llx pid=%d\n",
            (unsigned long long)center, (unsigned long long)rsp, ctx->target_pid);
    uint64_t lo = center > SL_MAP_DUMP_SPAN ? center - SL_MAP_DUMP_SPAN : 0x1000;
    uint64_t hi = center + SL_MAP_DUMP_SPAN;
    for (uint64_t a = lo; a < hi; a += SL_MAP_DUMP_STEP) {
        int p = kernel_get_vmem_protection(ctx->target_pid, (intptr_t)a, SL_MAP_DUMP_STEP);
        if (p >= 0) fprintf(out, "  0x%llx prot=0x%x\n", (unsigned long long)a, (unsigned)p);
    }
    fflush(out);
}

#define SL_SCRATCH_PROT_RW 0x80000002u
#define SL_SCRATCH_WINDOW  0x4000

static int prot_is_rw(int prot)
{
    return prot >= 0 && ((uint32_t)prot & SL_SCRATCH_PROT_RW) == 2;
}

/*
 * Pick a 16 KB writable window for the trap and the call: rip - 0x4000 first.
 * If that page isn't mapped writable, scan outward from RIP in 16 KB steps for
 * the nearest writable window.
 */
static int pick_scratch(sl_inject *ctx, uint64_t rip_page, intptr_t *out)
{
    int prot = -1;
    if (rip_page > SL_SCRATCH_WINDOW) {
        prot = kernel_get_vmem_protection(ctx->target_pid,
                                          (intptr_t)(rip_page - SL_SCRATCH_WINDOW), 0x1000);
        if (prot_is_rw(prot)) {
            *out = (intptr_t)((rip_page - SL_SCRATCH_WINDOW) & ~(uint64_t)(SL_SCRATCH_WINDOW - 1));
            ctx->last_prot = prot;
            return 0;
        }
    }

    for (uint64_t d = SL_SCRATCH_WINDOW; d <= SL_MAP_DUMP_SPAN; d += SL_SCRATCH_WINDOW) {
        for (int dir = -1; dir <= 1; dir += 2) {
            if (dir < 0 && rip_page < d) continue;
            uint64_t a = dir < 0 ? rip_page - d : rip_page + d;
            a &= ~(uint64_t)(SL_SCRATCH_WINDOW - 1);
            prot = kernel_get_vmem_protection(ctx->target_pid, (intptr_t)a, SL_SCRATCH_WINDOW);
            if (prot_is_rw(prot)) {
                *out = (intptr_t)a;
                ctx->last_prot = prot;
                return 0;
            }
        }
    }
    ctx->last_prot = prot;
    return -1;
}

/* Attach errors worth retrying. On FreeBSD/PS5, PT_ATTACH can fail with
 * EBUSY (another tracer is attaching) or EEXIST (the target is already
 * being traced) during normal rapid-restart windows; both clear in a few
 * tens of ms. */
#define SL_ATTACH_EEXIST 17
#define SL_ATTACH_EBUSY  16

int sl_inject_begin(const char *name, sl_inject *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->stage = "find-pid";
    ctx->target_pid = sl_find_pid(name);
    if (ctx->target_pid <= 0) return -1;

    ctx->stage = "attach";
    long rc = inj_ptrace(PT_ATTACH, ctx->target_pid, NULL, 0);
    ctx->last_errno = errno;
    if (rc != 0) {
        for (int tries = 199; ; tries--) {
            if (ctx->last_errno != SL_ATTACH_EEXIST && ctx->last_errno != SL_ATTACH_EBUSY) goto fail;
            if (tries == 0) goto fail;
            usleep(10000);
            rc = inj_ptrace(PT_ATTACH, ctx->target_pid, NULL, 0);
            ctx->last_errno = errno;
            if (rc == 0) break;
        }
    }

    ctx->stage = "wait-stop";
    ctx->running = 1;
    if (wait_for_stop(ctx->target_pid) != 0) goto fail;

    ctx->stage = "get-regs";
    struct reg regs;
    if (inj_ptrace(PT_GETREGS, ctx->target_pid, &regs, 0) != 0) goto fail;

    ctx->stage = "scratch-prot";
    uint64_t rip_page = regs.r_rip & ~0xfffULL;
    ctx->last_rip = regs.r_rip;
    ctx->thread_pid = ctx->target_pid;
    ctx->scratch_len = 0x4000;
    if (pick_scratch(ctx, rip_page, &ctx->scratch) != 0) {
        ctx->last_errno = errno;
        dump_map(ctx, rip_page, regs.r_rsp);
        goto fail;
    }

    ctx->attached = 1;
    ctx->running = 0;
    ctx->stage = "attached";
    return 0;

fail:
    ctx->last_errno = errno;
    if (ctx->running) sl_inject_ensure_stopped(ctx);
    if (inj_ptrace(PT_DETACH, ctx->target_pid, NULL, 0) != 0) {
        kill(ctx->target_pid, SIGCONT);
    }
    ctx->attached = 0;
    ctx->running = 0;
    return -1;
}

int sl_inject_ensure_stopped(sl_inject *ctx)
{
    if (!ctx || !ctx->attached) return -1;
    if (!ctx->running) return 0;

    /* PT_GETREGS only succeeds while the target is already stopped. */
    struct reg regs;
    if (inj_ptrace(PT_GETREGS, tracee(ctx), &regs, 0) == 0) {
        ctx->running = 0;
        return 0;
    }

    if (kill(ctx->target_pid, SIGSTOP) != 0) return -1;
    if (wait_for_stop(ctx->target_pid) != 0) return -1;

    ctx->thread_pid = ctx->target_pid;
    ctx->running = 0;
    return 0;
}

int sl_inject_read_at(sl_inject *ctx, intptr_t addr, void *buf, size_t len)
{
    if (!ctx || !ctx->attached) return -1;
    if (sl_inject_ensure_stopped(ctx) != 0) return -1;

    struct ptrace_io_desc io = {
        .piod_op = PIOD_READ_D,
        .piod_offs = (void *)addr,
        .piod_addr = buf,
        .piod_len = len,
    };
    if (inj_ptrace(PT_IO, tracee(ctx), &io, 0) != 0) return -1;
    return io.piod_len == len ? 0 : -1;
}

int sl_inject_write_at(sl_inject *ctx, intptr_t addr, const void *buf, size_t len)
{
    if (!ctx || !ctx->attached) return -1;
    if (sl_inject_ensure_stopped(ctx) != 0) return -1;

    struct ptrace_io_desc io = {
        .piod_op = PIOD_WRITE_D,
        .piod_offs = (void *)addr,
        .piod_addr = (void *)buf,
        .piod_len = len,
    };
    if (inj_ptrace(PT_IO, tracee(ctx), &io, 0) != 0) return -1;
    return io.piod_len == len ? 0 : -1;
}

void sl_inject_end(sl_inject *ctx)
{
    if (!ctx || !ctx->attached) return;
    if (ctx->running) sl_inject_ensure_stopped(ctx);
    if (inj_ptrace(PT_DETACH, tracee(ctx), NULL, 0) != 0) {
        kill(ctx->target_pid, SIGCONT);
    }
    ctx->attached = 0;
    ctx->running = 0;
    ctx->thread_pid = 0;
}

static int io_xfer(int op, pid_t pid, intptr_t addr, void *buf, size_t len)
{
    struct ptrace_io_desc io = {
        .piod_op = op,
        .piod_offs = (void *)addr,
        .piod_addr = buf,
        .piod_len = len,
    };
    if (inj_ptrace(PT_IO, pid, &io, 0) != 0) return -1;
    return io.piod_len == len ? 0 : -1;
}

/*
 * Wait up to timeout_ms for a stop. Returns 0 and sets *sig on stop,
 * 1 if the target is still running at the deadline, -1 on wait error.
 */
static int wait_stop_sig(pid_t pid, int timeout_ms, int *sig)
{
    /* Poll finely at first: most calls (ReadAudio, or ReadVideo with a frame
     * ready) finish within microseconds, and a flat 5 ms sleep added up to
     * 5 ms to each of the several calls made per video frame. Calls that
     * block (ReadVideo waiting for the encoder) are still seen within 0.1 ms
     * for a frame time; after that, back off. */
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED);
        if (r == pid) {
            if ((status & 0x7f) != 0x7f) return -1;
            *sig = (status >> 8) & 0xff;
            return 0;
        }
        if (r < 0 && errno != EINTR) return -1;
        clock_gettime(CLOCK_MONOTONIC, &t);
        int64_t us = (int64_t)(t.tv_sec - t0.tv_sec) * 1000000 + (t.tv_nsec - t0.tv_nsec) / 1000;
        if (us >= (int64_t)timeout_ms * 1000) return 1;
        usleep(us < 20000 ? 100 : us < 100000 ? 1000 : 5000);
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define SL_CALL_TIMEOUT_MS 12000
#define SL_CALL_MAX_RESTARTS 4
#define SL_SIGTRAP 5
#define SL_SCRATCH_TRAP_OFF 0xf00
#define SL_SCRATCH_REGION 0x4000
#define SL_CALL_STACK_RESERVE 0x208

/*
 * Remote call. Writes an int3 at scratch+0xf00,
 * points the target's return address at it, runs fn, and waits for the trap.
 * Afterwards restores the trap byte, return slot, registers, optional FP state
 * and page protection.
 */
int64_t inj_call_fast(sl_inject *ctx, intptr_t fn, const uint64_t args[6], int save_fp)
{
    if (!ctx || ctx->quarantined || !fn) return SL_CALL_ERROR;

    ctx->stage = "ensure-stopped";
    if (sl_inject_ensure_stopped(ctx) != 0) return SL_CALL_ERROR;

    pid_t pid = ctx->target_pid;
    pid_t tid = tracee(ctx);
    intptr_t trap = ctx->scratch + SL_SCRATCH_TRAP_OFF;
    intptr_t region = trap & ~(intptr_t)(SL_SCRATCH_REGION - 1);

    int prot = kernel_get_vmem_protection(pid, region, SL_SCRATCH_REGION);
    if (prot < 0) return SL_CALL_ERROR;

    ctx->stage = "get-regs";
    struct reg saved;
    if (inj_ptrace(PT_GETREGS, tid, &saved, 0) != 0) return SL_CALL_ERROR;

    struct fpreg saved_fp;
    if (save_fp) {
        ctx->stage = "get-fpregs";
        if (inj_ptrace(PT_GETFPREGS, tid, &saved_fp, 0) != 0) return SL_CALL_ERROR;
    }

    intptr_t call_sp = (intptr_t)((saved.r_rsp & ~0xfULL) - SL_CALL_STACK_RESERVE);

    ctx->stage = "read-stack";
    uint64_t old_word;
    if (io_xfer(PIOD_READ_D, pid, call_sp, &old_word, sizeof(old_word)) != 0) return SL_CALL_ERROR;

    if (kernel_set_vmem_protection(pid, region, SL_SCRATCH_REGION, prot | PROT_EXEC) != 0)
        return SL_CALL_ERROR;

    ctx->stage = "write-trap";
    uint8_t old_trap_byte;
    uint8_t int3 = 0xcc;
    int64_t result = SL_CALL_ERROR;
    int ok = 0;

    if (io_xfer(PIOD_READ_D, pid, trap, &old_trap_byte, 1) != 0) goto restore_mem;
    if (io_xfer(PIOD_WRITE_D, pid, trap, &int3, 1) != 0) goto restore_mem;

    uint64_t ret_addr = (uint64_t)trap;
    if (io_xfer(PIOD_WRITE_D, pid, call_sp, &ret_addr, sizeof(ret_addr)) != 0) goto restore_trap;

    struct reg call = saved;
    call.r_rip = (int64_t)fn;
    call.r_rsp = (int64_t)call_sp;
    call.r_rdi = (int64_t)args[0];
    call.r_rsi = (int64_t)args[1];
    call.r_rdx = (int64_t)args[2];
    call.r_rcx = (int64_t)args[3];
    call.r_r8  = (int64_t)args[4];
    call.r_r9  = (int64_t)args[5];
    call.r_rax = 0;

    ctx->stage = "set-regs";
    if (inj_ptrace(PT_SETREGS, tid, &call, 0) != 0) goto restore_trap;

    ctx->stage = "continue";
    ctx->running = 1;
    if (inj_ptrace(PT_CONTINUE, pid, (void *)1, 0) != 0) goto restore_trap;

    ctx->stage = "wait-trap";
    int64_t deadline = now_ms() + SL_CALL_TIMEOUT_MS;
    int kicked = 0;
    int restarts = 0;
    struct reg cur;

    while (now_ms() < deadline) {
        int sig = 0;
        int remaining = (int)(deadline - now_ms());
        if (remaining > 250) remaining = 250;

        int w = wait_stop_sig(pid, remaining, &sig);
        if (w < 0) break;
        if (w > 0) {
            if (!kicked) {
                kill(pid, SIGCONT);
                kicked = 1;
                ctx->stall_count++;
            }
            continue;
        }

        ctx->running = 0;
        if (inj_ptrace(PT_GETREGS, tid, &cur, 0) != 0) break;

        if (sig == SL_SIGTRAP && cur.r_rip == (int64_t)trap + 1) {
            result = cur.r_rax;
            ok = 1;
            break;
        }
        if (restarts >= SL_CALL_MAX_RESTARTS) break;

        /* Forward every signal except SIGSTOP (17) and SIGCONT (19), which
         * the kernel delivered to interrupt the trampoline rather than to
         * the target. */
        int pass = (sig == SIGSTOP || sig == SIGCONT) ? 0 : sig;
        restarts++;
        ctx->running = 1;
        if (inj_ptrace(PT_CONTINUE, pid, (void *)1, pass) != 0) break;
    }

    if (!ok) {
        ctx->stage = "timeout-stop";
        kill(pid, SIGSTOP);
        int sig = 0;
        if (wait_stop_sig(pid, 2000, &sig) == 0) ctx->running = 0;
    }

restore_trap:
    ctx->stage = "restore-trap";
    io_xfer(PIOD_WRITE_D, pid, trap, &old_trap_byte, 1);

restore_mem:
    ctx->stage = "restore-stack";
    io_xfer(PIOD_WRITE_D, pid, call_sp, &old_word, sizeof(old_word));

    ctx->stage = "restore-regs";
    int regs_ok = inj_ptrace(PT_SETREGS, tid, &saved, 0) == 0;
    int fp_ok = 1;
    if (save_fp) fp_ok = inj_ptrace(PT_SETFPREGS, tid, &saved_fp, 0) == 0;

    ctx->stage = "restore-protection";
    int prot_ok = kernel_set_vmem_protection(pid, region, SL_SCRATCH_REGION, prot) == 0;

    ctx->stage = "complete";
    if (!regs_ok || !fp_ok || !prot_ok) return SL_CALL_ERROR;
    return ok ? result : SL_CALL_ERROR;
}

int64_t sl_inject_call_fast(sl_inject *ctx, intptr_t fn, const uint64_t args[6])
{
    if (!ctx || ctx->quarantined) return SL_CALL_ERROR;
    return inj_call_fast(ctx, fn, args, 0);
}

#endif /* SL_PS5 */
