#ifndef GLINT_INJECT_H
#define GLINT_INJECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * Ptrace injector: stops a system process, borrows a writable page as scratch
 * and runs single function calls in it through a trap trampoline.
 */
typedef struct sl_inject {
    pid_t    target_pid;
    pid_t    thread_pid;     /* 0 means use target_pid */
    intptr_t scratch;        /* writable scratch area in target */
    size_t   scratch_len;
    int      attached;
    int      running;        /* target must be stopped before use */
    int      quarantined;    /* remote call got stuck, refuse further calls */
    int      stall_count;    /* times a remote call had to be kicked with SIGCONT */
    const char *stage;       /* last operation, for the capture log */
    int      last_errno;     /* errno from the last failed operation */
    uint64_t last_rip;       /* target RIP at attach, for diagnostics */
    int      last_prot;      /* protection read for the scratch page, for diagnostics */
} sl_inject;

/* Returned by the remote-call functions on failure (0x8000000000000000). */
#define SL_CALL_ERROR INT64_MIN

/*
 * Call `fn` inside the target with up to six integer arguments (SysV order:
 * rdi, rsi, rdx, rcx, r8, r9). Returns rax, or SL_CALL_ERROR.
 * Set save_fp to preserve and restore the target's x87/SSE state.
 */
int64_t inj_call_fast(sl_inject *ctx, intptr_t fn, const uint64_t args[6], int save_fp);
int64_t sl_inject_call_fast(sl_inject *ctx, intptr_t fn, const uint64_t args[6]);

/* Pid of the first process whose name matches, or -1. */
int sl_find_pid(const char *name);

/* dlsym of `sym` inside `basename` mapped in `pid`, or 0. */
intptr_t sl_resolve_in(pid_t pid, const char *basename, const char *sym);

/* Attach to the named process and stop it. 0 on success. */
int sl_inject_begin(const char *name, sl_inject *ctx);

/* Log the target's mapped pages around the stop point on the next failed attach, then clear. */
void sl_inject_set_map_dump(FILE *out);

/* Stop the target if it is running. 0 on success. */
int sl_inject_ensure_stopped(sl_inject *ctx);

/* Read/write target memory through PT_IO. 0 on success. */
int sl_inject_read_at(sl_inject *ctx, intptr_t addr, void *buf, size_t len);
int sl_inject_write_at(sl_inject *ctx, intptr_t addr, const void *buf, size_t len);

/* Detach and resume the target. */
void sl_inject_end(sl_inject *ctx);

#endif
