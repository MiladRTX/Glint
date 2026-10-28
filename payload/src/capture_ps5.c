#include "glint.h"

#ifdef SL_PS5

/*
 * PS5 capture backend.
 *
 * Drives the console's H.264 encoder through libSceAvcap2. The capture calls
 * must run inside SceRemotePlay (the system process that owns the encoder),
 * so this file:
 *
 *   1. Resolves the libSceAvcap2 and libSceResourceArbitrator symbols inside
 *      the remote process (sl_resolve_in).
 *   2. Uses the ptrace trampoline in inject.c to invoke those functions on
 *      the remote process's stack.
 *   3. Shuttles the H.264 and audio buffers out through PT_IO and pushes
 *      them into the server's ring buffers.
 *
 * The SL_DIRECT_CAPTURE build (not used in release) resolves the module in
 * the payload process directly, bypassing the trampoline; it exists so the
 * call sequence can be exercised without the remote process in the picture.
 *
 * See docs/CAPTURE-INTERNALS.md for the parameter-block layout, the profile
 * table used for 1080p60, and the error-code semantics referenced below.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <ps5/kernel.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "inject.h"

/*
 * SCE error codes we interpret explicitly. Any other negative return is
 * logged verbatim and treated as a generic failure.
 */
enum sl_sce_error {
    SL_SCE_OPEN_VIDEO_UNSUPPORTED = 0x81950008u, /* OpenVideo: profile not supported */
    SL_SCE_START_LOCKED           = 0x80c78000u, /* Start:     encoder locked (needs reboot) */
    SL_SCE_SESSION_STALE          = 0x819b0007u, /* SetCallback: previous session not cleaned up */
};

extern int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                                    uint32_t flags, const void *unk1, int *result,
                                    uint32_t *handle);

static FILE *g_cap_log;

static void cap_log(const char *fmt, ...)
{
    if (!g_cap_log) g_cap_log = fopen("/data/glint/capture.log", "a");
    if (!g_cap_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_cap_log, fmt, args);
    va_end(args);
    fflush(g_cap_log);
}

typedef struct {
    uint64_t size;
    uint32_t bitrate;
    uint32_t budget;
} avcap2_bitrate_params;

/*
 * Direct-call bindings for libSceAvcap2 (used only by the SL_DIRECT_CAPTURE
 * path, which runs the capture sequence in-process rather than through the
 * remote trampoline). The names mirror the exported symbol names so the
 * RESOLVE() macro below can read as a plain table.
 */
typedef struct {
    uint32_t handle;
    int (*fn_initialize)(void);
    int (*fn_terminate)(void);
    int (*fn_start)(uint32_t);
    int (*fn_stop)(uint32_t);
    int (*fn_open_video)(void *, uint32_t *);
    int (*fn_read_video)(uint32_t, void *, size_t, size_t *, uint64_t *);
    int (*fn_set_bitrate)(uint32_t, avcap2_bitrate_params *);
    int (*fn_request_keyframe)(uint32_t);
    int (*fn_enable_scan)(void);
    int (*fn_get_video_mode)(uint32_t *);
    int (*fn_non_vcl)(uint32_t, void *);
    int (*fn_close)(uint32_t);
    uint32_t video;
} avcap2_state;

static avcap2_state av;
static pthread_t worker;
static int worker_active;

static int avcap2_open(void)
{
    int rc = 0;
    static const char *paths[] = {
        "/system/common/lib/libSceAvcap2.sprx",
        "libSceAvcap2.sprx",
    };
    int mod = -1;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        rc = 0;
        uint32_t ignored = 0;
        mod = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &rc, &ignored);
        cap_log("load %s rc=%d mod=0x%x\n", paths[i], rc, mod);
        if (mod >= 0 && rc == 0) break;
    }
    if (mod < 0 || rc != 0) return -1;

    uint32_t handle = 0;
    pid_t self = getpid();
    int kr = kernel_dynlib_handle(self, "libSceAvcap2.sprx", &handle);
    cap_log("kernel_dynlib_handle pid=%d rc=%d handle=0x%x\n", (int)self, kr, handle);
    if (kr != 0 || handle == 0) return -1;
    av.handle = handle;

#define RESOLVE(field, symbol) do { \
    intptr_t a = kernel_dynlib_dlsym(self, handle, symbol); \
    cap_log("dlsym %s addr=0x%llx\n", symbol, (unsigned long long)a); \
    if (a <= 0) return -1; \
    *(void **)&av.field = (void *)a; \
} while (0)

    RESOLVE(fn_initialize,       "sceAvcap2Initialize");
    RESOLVE(fn_terminate,         "sceAvcap2Terminate");
    RESOLVE(fn_start,             "sceAvcap2Start");
    RESOLVE(fn_stop,              "sceAvcap2Stop");
    RESOLVE(fn_open_video,        "sceAvcap2OpenVideo");
    RESOLVE(fn_read_video,        "sceAvcap2ReadVideo");
    RESOLVE(fn_set_bitrate,       "sceAvcap2SetVideoBitrate");
    RESOLVE(fn_request_keyframe,  "sceAvcap2RequestKeyFrame");
    RESOLVE(fn_enable_scan,       "sceAvcap2EnableScanInEncode");
    RESOLVE(fn_get_video_mode,    "sceAvcap2GetVideoOutMode");
    RESOLVE(fn_non_vcl,           "sceAvcap2GetNonVclNalUnitStream");
    RESOLVE(fn_close,             "sceAvcap2Close");
#undef RESOLVE
    return 0;
}

static void put32(uint8_t *p, size_t off, uint32_t value)
{
    memcpy(p + off, &value, 4);
}

static void put64(uint8_t *p, size_t off, uint64_t value)
{
    memcpy(p + off, &value, 8);
}

/*
 * sceAvcap2OpenVideo parameter block.
 *
 * The call takes a single 0x2e0-byte struct built from nested sub-blocks
 * (rate control, resolution, mask, output mode, audio companion). The
 * offsets below name each sub-block's start; `build_video_params` fills
 * them in a single pass. See docs/CAPTURE-INTERNALS.md for the full layout.
 */
#define SL_VP_SIZE               0x2e0
#define SL_VP_OFF_SIZE           0x08  /* u32: total block size (= SL_VP_SIZE) */
#define SL_VP_OFF_HEADER         0x10  /* u32: fixed header value 0x28 */
#define SL_VP_OFF_USAGE          0x1c  /* u32: usage class, 2 = encode */
#define SL_VP_OFF_FPS_MODE       0x20  /* u32: profile FPS selector */
#define SL_VP_OFF_FPS_FLAG       0x24  /* u32: fixed 1 */
#define SL_VP_OFF_FEATURE        0x30  /* u32: capture feature id */
#define SL_VP_OFF_RATE           0xb8  /* rate-control sub-block */
#define SL_VP_OFF_FORMAT         0x1e8 /* format sub-block */
#define SL_VP_OFF_WIDTH_BLOCK    0x208 /* width sub-block (u32 at +8) */
#define SL_VP_OFF_RES_BLOCK      0x20c /* resolution sub-block (u32 at +8) */
#define SL_VP_OFF_MASK_BLOCK     0x218 /* mask sub-block */
#define SL_VP_OFF_VIDEO_OUT      0x2a0 /* VideoOutMode sub-block */
#define SL_VP_OFF_AUDIO          0x2a4 /* audio companion sub-block */
#define SL_VP_OFF_ENABLE_FLAG    0x2b4 /* u32: fixed 1 */
#define SL_VP_OFF_FLAG_BYTE      0x2bc /* u8: fixed 1 */

static void build_video_params(uint8_t *p, uint32_t feature, uint32_t width, uint32_t resolution,
                               uint32_t fps_mode, uint32_t video_out_mode,
                               uint32_t bitrate, uint32_t minqp)
{
    memset(p, 0, SL_VP_SIZE);

    put32(p, SL_VP_OFF_SIZE,     SL_VP_SIZE);
    put32(p, SL_VP_OFF_HEADER,   0x28);
    put32(p, SL_VP_OFF_USAGE,    2);
    put32(p, SL_VP_OFF_FPS_MODE, fps_mode);
    put32(p, SL_VP_OFF_FPS_FLAG, 1);
    put32(p, SL_VP_OFF_FEATURE,  feature);

    /* Rate-control sub-block. */
    const size_t rate = SL_VP_OFF_RATE;
    put64(p, rate + 0x08, 0xe0);
    put64(p, rate + 0x10, 0x20);
    put32(p, rate + 0x18, bitrate);
    put32(p, rate + 0x1c, bitrate);
    put32(p, rate + 0x20, (bitrate * 0x3e9 + 59999) / 60000);
    put32(p, rate + 0x24, minqp);
    put32(p, rate + 0x28, 0x33);

    /* Format sub-block. */
    put64(p, SL_VP_OFF_FORMAT + 0x08, 0xf8);
    put64(p, SL_VP_OFF_FORMAT + 0x10, 0x28);

    /* Width / resolution sub-blocks. */
    put32(p, SL_VP_OFF_WIDTH_BLOCK + 0x08, width);
    put32(p, SL_VP_OFF_RES_BLOCK   + 0x08, resolution);

    /* Mask sub-block: a 0x80-byte bitmap of all-ones. */
    put64(p, SL_VP_OFF_MASK_BLOCK + 0x08, 0x88);
    memset(p + SL_VP_OFF_MASK_BLOCK + 0x10, 0xff, 0x80);

    /* VideoOutMode + audio companion sub-blocks. */
    put32(p, SL_VP_OFF_VIDEO_OUT + 0x08, video_out_mode);
    put32(p, SL_VP_OFF_AUDIO     + 0x08, 2);
    put64(p, SL_VP_OFF_AUDIO     + 0x0c, 0x10);

    put32(p, SL_VP_OFF_ENABLE_FLAG, 1);
    p[SL_VP_OFF_FLAG_BYTE] = 1;
}

/*
 * Layout of the blob written by sceAvcap2GetNonVclNalUnitStream:
 *   +0x10  u32  SPS length
 *   +0x18  SPS bytes (NAL header type 7)
 *   +0x98  u32  PPS length
 *   +0xa0  PPS bytes (NAL header type 8)
 * See docs/CAPTURE-INTERNALS.md for the full non-VCL blob layout.
 */
#define SL_NONVCL_SPS_LEN   0x10
#define SL_NONVCL_SPS       0x18
#define SL_NONVCL_PPS_LEN   0x98
#define SL_NONVCL_PPS       0xa0
#define SL_NONVCL_MIN       0xa4

static int find_headers(const uint8_t *buffer, size_t len,
                        uint8_t *sps, size_t *sps_len,
                        uint8_t *pps, size_t *pps_len)
{
    if (len >= SL_NONVCL_MIN) {
        uint32_t sl = 0, pl = 0;
        memcpy(&sl, buffer + SL_NONVCL_SPS_LEN, 4);
        memcpy(&pl, buffer + SL_NONVCL_PPS_LEN, 4);
        if (sl > 4 && sl <= 128 && pl > 1 && pl <= 128 &&
            (buffer[SL_NONVCL_SPS] & 0x1f) == 7 &&
            (buffer[SL_NONVCL_PPS] & 0x1f) == 8) {
            memcpy(sps, buffer + SL_NONVCL_SPS, sl);
            memcpy(pps, buffer + SL_NONVCL_PPS, pl);
            *sps_len = sl;
            *pps_len = pl;
            return 0;
        }
    }
    return -1;
}

/*
 * Height -> capture profile. 1080p60 is the only profile shipped: OpenVideo
 * refuses 1440p on current firmware (returns SL_SCE_OPEN_VIDEO_UNSUPPORTED),
 * and a failed 1440p attempt can leave the encoder in a state where Start
 * returns SL_SCE_START_LOCKED on subsequent attempts, so we open 1080p
 * directly and keep the table narrow.
 */
struct profile_map {
    uint16_t height;
    uint32_t id, feature, width, resolution, fps_mode;
};
static const struct profile_map g_profiles[] = {
    { 1080, 12, 12, 0x780, 0x438, 4 },
};

static const struct profile_map *find_profile(uint16_t height)
{
    for (unsigned i = 0; i < sizeof(g_profiles) / sizeof(g_profiles[0]); i++) {
        if (g_profiles[i].height == height) return &g_profiles[i];
    }
    return NULL;
}

static uint16_t g_height_current;   /* last height that opened, 0 until the first open */
static int g_run_count;             /* completed runs; a run after the first is a reopen */
static int g_restart_pending;       /* restart taken by the live loop, serviced by the next run */
static int g_need_terminate;        /* SetCallback reported a stale session (0x819b0007) */
static volatile int g_encoder_locked; /* set once; no retries after this, reboot required */

static void encoder_locked(const char *where)
{
    int first = !g_encoder_locked;
    g_encoder_locked = 1;
    /* The encoder-locked state does not reliably clear across a payload
     * restart, so we don't claim it will. */
    cap_log("encoder refused to start (%s)\n", where);
    if (first) sl_notify(1, "Glint: the PS5 refused to start video capture (0x%08x). Wait a minute, then load Glint again.",
                        (unsigned)SL_SCE_START_LOCKED);
}


static uint16_t g_prev_height;      /* height before the last successful open, to restore on a failed change */
static uint16_t g_profile_confirm;  /* requested height waiting for its first IDR to confirm the SPS */
static int g_profile_error_pending; /* a failed change to report again after the restore server start */
static uint8_t g_last_sps[128];     /* SPS from the previous session, to spot a stale blob after a resize */
static size_t g_last_sps_len;

/* Marker file /data/glint/no_avcap2_reset turns off the conditional Terminate. */
static int avcap2_reset_disabled(void)
{
    return access("/data/glint/no_avcap2_reset", F_OK) == 0;
}

/*
 * Clear resource-arbitrator callbacks 11 and 12, plus the feature this session used.
 * The arbitrator keeps them after a crash or kill, and SetCallback then fails with 0x819b0007.
 */
/* OpenVideo errors in the 0x8195000x range all mean "this profile cannot be
 * opened" rather than "the session is stale", so they are retried against the
 * next candidate rather than treated as encoder failure. */
static int profile_unsupported(uint32_t rc)
{
    return (rc & 0xfffffff0u) == 0x81950000u;
}

/* Log a call result as "Label -> 0x%08x (ok)" or "(error)". */
static void log_call(const char *label, int64_t rc)
{
    int failed = rc == SL_CALL_ERROR || (int32_t)rc < 0;
    cap_log("%s -> 0x%08x (%s)\n", label, (uint32_t)rc, failed ? "error" : "ok");
}

static void log_call_id(const char *name, uint32_t id, int64_t rc)
{
    char label[48];
    snprintf(label, sizeof(label), "%s(%u)", name, id);
    log_call(label, rc);
}

static void clear_arbitrator(sl_inject *ctx, intptr_t ra_clear, uint32_t extra)
{
    uint32_t ids[3] = { 11, 12, extra };
    for (int i = 0; i < 3; i++) {
        if (ids[i] == 0) continue;
        if (i == 2 && (extra == 11 || extra == 12)) continue;
        uint64_t args[6] = { ids[i] };
        int64_t rc = sl_inject_call_fast(ctx, ra_clear, args);
        log_call_id("ClearCallback", ids[i], rc);
    }
}

/*
 * One capture session: attach to SceRemotePlay, run the avcap2 sequence inside
 * it, serve frames until the session ends, and log every call.
 */
static void capture_session(void)
{
    uint16_t want_height = 0;
    int profile_run = sl_take_profile_request(&want_height);
    int restart_run = sl_take_restart_request() || g_restart_pending;
    g_restart_pending = 0;
    int profile_resolved = 0;
    uint16_t chosen_height = 0;
    sl_inject ctx;
    uint64_t self_authid = kernel_get_ucred_authid(getpid());
    cap_log("find_pid SceRemotePlay=%d self_authid=0x%llx\n",
            sl_find_pid("SceRemotePlay"), (unsigned long long)self_authid);

    /* The stop point is wherever SceRemotePlay happens to be, so retry until the scratch page is usable. */
    int attached = 0;
    sl_inject_set_map_dump(g_cap_log);
    for (int attempt = 1; attempt <= 8 && !attached; attempt++) {
        if (sl_inject_begin("SceRemotePlay", &ctx) == 0) {
            attached = 1;
            break;
        }
        cap_log("inject begin attempt %d failed stage=%s errno=%d rip=0x%llx prot=0x%x\n",
                attempt, ctx.stage, ctx.last_errno,
                (unsigned long long)ctx.last_rip, (unsigned)ctx.last_prot);
        usleep(200000);
    }
    if (!attached) return;
    cap_log("inject begin pid=%d scratch=0x%llx\n", ctx.target_pid,
            (unsigned long long)ctx.scratch);

    intptr_t init = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2Initialize");
    intptr_t mode = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2GetVideoOutMode");
    intptr_t open_video = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2OpenVideo");
    intptr_t enable_scan = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2EnableScanInEncode");
    intptr_t ra_set = sl_resolve_in(ctx.target_pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorSetCallback");
    intptr_t ra_change = sl_resolve_in(ctx.target_pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorChangeState");
    intptr_t ra_clear = sl_resolve_in(ctx.target_pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorClearCallback");
    intptr_t getpid_fn = sl_resolve_in(ctx.target_pid, "libkernel_sys.sprx", "getpid");
    cap_log("remote resolve init=0x%llx mode=0x%llx open=0x%llx scan=0x%llx ra=0x%llx/0x%llx/0x%llx getpid=0x%llx\n",
            (unsigned long long)init, (unsigned long long)mode, (unsigned long long)open_video,
            (unsigned long long)enable_scan, (unsigned long long)ra_set,
            (unsigned long long)ra_change, (unsigned long long)ra_clear,
            (unsigned long long)getpid_fn);
    if (!init || !mode || !open_video || !enable_scan || !ra_set || !ra_change || !ra_clear || !getpid_fn)
        goto done;

    uint64_t no_args[6] = { 0 };
    /* Startup reset: clear the arbitrator callbacks before any Initialize. */
    clear_arbitrator(&ctx, ra_clear, 0);

    int64_t rc = sl_inject_call_fast(&ctx, init, no_args);
    cap_log("remote Initialize rc=0x%llx stage=%s stall=%d\n",
            (unsigned long long)rc, ctx.stage, ctx.stall_count);
    if (rc == SL_CALL_ERROR) goto done;

    intptr_t out = ctx.scratch + 0x700;
    uint32_t zero = 0;
    if (sl_inject_write_at(&ctx, out, &zero, sizeof(zero)) != 0) goto done;

    uint64_t mode_args[6] = { (uint64_t)out };
    rc = sl_inject_call_fast(&ctx, mode, mode_args);
    uint32_t video_mode = 0;
    sl_inject_read_at(&ctx, out, &video_mode, sizeof(video_mode));
    cap_log("remote GetVideoOutMode rc=0x%llx mode=0x%x stage=%s\n",
            (unsigned long long)rc, video_mode, ctx.stage);

    /* 1080p.
     * Each attempt: Initialize, SetCallback(id), ChangeState(id, RUNNING),
     * EnableScanInEncode, OpenVideo; ClearCallback(id) when OpenVideo fails. */
    /*
     * Attempts: a requested profile tries that height, then the current one (restore).
     * A restart reopens the current height. Startup opens 1080.
     */
    const struct profile_map *attempts[2];
    unsigned attempt_count = 0;
    if (profile_run) {
        if (find_profile(want_height)) attempts[attempt_count++] = find_profile(want_height);
        uint16_t fallback = g_height_current ? g_height_current : 1080;
        if (fallback != want_height) attempts[attempt_count++] = find_profile(fallback);
        cap_log("profile change requested: %u (current %u)\n", want_height, fallback);
    } else if (restart_run) {
        uint16_t current = g_height_current ? g_height_current : 1080;
        attempts[attempt_count++] = find_profile(current);
        cap_log("capture restart requested (height %u)\n", current);
    } else {
        attempts[attempt_count++] = find_profile(1080);
    }
    if (attempt_count == 0) {
        cap_log("no usable profile for height %u\n", want_height);
        goto done;
    }

    /* Lay the OpenVideo request out in the scratch region: parameter block
     * at the base, out-handle at +8, "RUNNING" state string at +0x600. */
    /* Terminate only when an earlier Start or SetCallback reported a stale session (see g_need_terminate). */
    if (g_need_terminate && !avcap2_reset_disabled()) {
        intptr_t terminate_fn = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2Terminate");
        if (terminate_fn) {
            uint64_t term_args[6] = { 0 };
            int64_t term_rc = sl_inject_call_fast(&ctx, terminate_fn, term_args);
            cap_log("remote Terminate rc=0x%x\n", (uint32_t)term_rc);
        }
        g_need_terminate = 0;
    }

    /* Start clean: callbacks left set by earlier runs make SetCallback fail with 0x819b0007. */
    for (uint64_t id = 11; id <= 12; id++) {
        uint64_t clean_args[6] = { id };
        sl_inject_call_fast(&ctx, ra_clear, clean_args);
    }

    intptr_t params_at = ctx.scratch;
    intptr_t handle_at = ctx.scratch + 8;
    intptr_t running_at = ctx.scratch + 0x600;
    uint32_t handle = 0;
    uint64_t handle_qword = 0;
    int opened = 0;
    uint32_t chosen_feature = 0;
    for (unsigned i = 0; i < attempt_count; i++) {
        uint32_t id = attempts[i]->id;
        uint64_t none[6] = { 0 };

        rc = sl_inject_call_fast(&ctx, init, none);
        log_call("Initialize()", rc);

        uint64_t set_args[6] = { id, (uint64_t)getpid_fn };
        rc = sl_inject_call_fast(&ctx, ra_set, set_args);
        log_call_id("SetCallback", id, rc);
        if ((uint32_t)rc == SL_SCE_SESSION_STALE) g_need_terminate = 1;
        if (rc == SL_CALL_ERROR || (int32_t)rc < 0) goto attempt_failed;

        const char running[8] = "RUNNING";
        if (sl_inject_write_at(&ctx, running_at, running, sizeof(running)) != 0) goto attempt_failed;
        uint64_t change_args[6] = { id, (uint64_t)running_at };
        rc = sl_inject_call_fast(&ctx, ra_change, change_args);
        log_call_id("ChangeState", id, rc);
        if (rc == SL_CALL_ERROR || (int32_t)rc < 0) goto attempt_failed;

        rc = sl_inject_call_fast(&ctx, enable_scan, none);
        log_call("EnableScanInEncode()", rc);

        uint8_t params[SL_VP_SIZE];
        build_video_params(params, attempts[i]->feature, attempts[i]->width, attempts[i]->resolution,
                           attempts[i]->fps_mode, video_mode, sl_opts.bitrate, sl_opts.minqp);
        cap_log("OpenVideo params feature=%u %ux%u fps-mode=%u video-out-mode=0x%x\n",
                attempts[i]->feature, attempts[i]->width, attempts[i]->resolution,
                attempts[i]->fps_mode, video_mode);
        if (sl_inject_write_at(&ctx, params_at, params, sizeof(params)) != 0) goto attempt_failed;

        uint64_t open_args[6] = { (uint64_t)params_at, (uint64_t)handle_at };
        rc = sl_inject_call_fast(&ctx, open_video, open_args);
        log_call("OpenVideo()", rc);
        if (rc != SL_CALL_ERROR && (uint32_t)rc == 0) {
            /* On success OpenVideo also writes the full session handle (u64)
             * back into the first slot of the parameter block. */
            sl_inject_read_at(&ctx, ctx.scratch, &handle_qword, sizeof(handle_qword));
            cap_log("session handle=0x%llx\n", (unsigned long long)handle_qword);
        }
        sl_inject_read_at(&ctx, handle_at, &handle, sizeof(handle));
        if (rc != SL_CALL_ERROR && (uint32_t)rc == 0 && handle_qword) {
            opened = 1;
            chosen_feature = attempts[i]->feature;
            chosen_height = attempts[i]->height;
            break;
        }

attempt_failed:
        if (i + 1 < attempt_count) {
            cap_log("OpenVideo %s %up (0x%08x, video mode 0x%x); retrying %up with feature %u\n",
                    profile_unsupported((uint32_t)rc) ? "rejected" : "failed",
                    attempts[i]->height, (uint32_t)rc, video_mode,
                    attempts[i + 1]->height, attempts[i + 1]->feature);
            uint64_t clear_args[6] = { id };
            rc = sl_inject_call_fast(&ctx, ra_clear, clear_args);
            log_call_id("ClearCallback", id, rc);
        } else if (profile_unsupported((uint32_t)rc)) {
            /* Every candidate rejected: this isn't a bad profile, the encoder
             * won't open any profile right now. */
            encoder_locked("OpenVideo");
        }
    }
    if (!opened) goto done;

    /* Fetch the full 64-bit session handle written back into the first slot
     * of the parameter block (ctx.scratch) by OpenVideo. */
    uint64_t session = 0;
    sl_inject_read_at(&ctx, ctx.scratch, &session, sizeof(session));
    cap_log("session=0x%llx\n", (unsigned long long)session);

    /* SetVideoBitrate: {u64 size=0x10, u32 bitrate, u32 budget} at scratch+0x780. */
    intptr_t rate_at = ctx.scratch + 0x780;
    uint8_t rate_block[0x10] = { 0 };
    uint64_t rate_size = 0x10;
    uint32_t bitrate = sl_opts.bitrate;
    memcpy(rate_block, &rate_size, 8);
    memcpy(rate_block + 8, &bitrate, 4);
    memcpy(rate_block + 12, &bitrate, 4);
    if (sl_inject_write_at(&ctx, rate_at, rate_block, sizeof(rate_block)) != 0) goto done;
    intptr_t set_bitrate = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2SetVideoBitrate");
    intptr_t start = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2Start");
    intptr_t nonvcl = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2GetNonVclNalUnitStream");
    if (!set_bitrate || !start || !nonvcl) goto done;

    uint64_t rate_args[6] = { session, (uint64_t)rate_at };
    rc = sl_inject_call_fast(&ctx, set_bitrate, rate_args);
    cap_log("SetVideoBitrate companion=%u\n", bitrate);
    {
        char label[48];
        snprintf(label, sizeof(label), "SetVideoBitrate(%u)", bitrate);
        log_call(label, rc);
    }

    /* Audio before video Start: OpenAudio, read handle, malloc(0x10000), Audio Start. */
    intptr_t open_audio = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2OpenAudio");
    intptr_t audio_start = start;
    intptr_t malloc_fn = sl_resolve_in(ctx.target_pid, "libSceLibcInternal.sprx", "malloc");
    if (!open_audio || !malloc_fn) goto done;

    /* Audio request layout: the sceAvcap2OpenAudio struct starts at
     * scratch+0x400; the handle is written back at scratch+0x408. */
    uint8_t audio_block[0x440] = { 0 };
    uint64_t a_size = 0x38, a_hdr = 0x18;
    uint32_t a_format = 2, a_feature = chosen_feature, a_chan = 0x10, a_rate = 0x1f400;
    memcpy(audio_block + 0x408, &a_size, 8);
    memcpy(audio_block + 0x410, &a_hdr, 8);
    memcpy(audio_block + 0x41c, &a_format, 4);
    memcpy(audio_block + 0x424, &a_feature, 4);
    memcpy(audio_block + 0x428, &a_chan, 4);
    memcpy(audio_block + 0x430, &a_rate, 4);
    if (sl_inject_write_at(&ctx, ctx.scratch, audio_block, sizeof(audio_block)) != 0) goto done;

    uint64_t open_audio_args[6] = { (uint64_t)(ctx.scratch + 0x400), (uint64_t)(ctx.scratch + 0x408) };
    rc = sl_inject_call_fast(&ctx, open_audio, open_audio_args);
    log_call("Audio OpenAudio(format=0)", rc);
    if (rc == SL_CALL_ERROR || (int32_t)rc < 0) goto done;

    uint64_t audio_handle = 0;
    sl_inject_read_at(&ctx, ctx.scratch + 0x400, &audio_handle, sizeof(audio_handle));
    cap_log("audio handle=0x%llx\n", (unsigned long long)audio_handle);
    if (!audio_handle) goto done;

    uint64_t malloc_args[6] = { 0x10000 };
    int64_t buf = sl_inject_call_fast(&ctx, malloc_fn, malloc_args);
    cap_log("remote malloc(0x10000) -> 0x%llx\n", (unsigned long long)buf);
    if (buf == SL_CALL_ERROR || buf <= 0) goto done;

    uint64_t audio_start_args[6] = { audio_handle };
    rc = sl_inject_call_fast(&ctx, audio_start, audio_start_args);
    log_call("Audio Start", rc);
    if (rc == SL_CALL_ERROR || (int32_t)rc < 0) goto done;

    uint64_t start_args[6] = { session };
    rc = sl_inject_call_fast(&ctx, start, start_args);
    log_call("Start()", rc);
    if ((uint32_t)rc == SL_SCE_START_LOCKED) {
        /* The encoder is locked in a state only a reboot clears: don't retry,
         * don't try a remote Close (which may hang), just log and stop. */
        encoder_locked("Start");
        goto done;
    }
    if (rc == SL_CALL_ERROR || (int32_t)rc < 0) goto done;

    /* Header blob lands at scratch+0x2000; a 0x1a8-byte read covers it. */
    intptr_t headers_at = ctx.scratch + 0x2000;
    uint8_t headers[0x1a8];
    /* The encoder fills the blob asynchronously: retry until the SPS is complete (28 bytes, type 7). */
    int stale_sps = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        uint64_t nonvcl_args[6] = { session, (uint64_t)headers_at };
        rc = sl_inject_call_fast(&ctx, nonvcl, nonvcl_args);
        log_call("GetNonVclNalUnitStream()", rc);
        if (rc == SL_CALL_ERROR || (int32_t)rc < 0) continue;
        if (sl_inject_read_at(&ctx, headers_at, headers, sizeof(headers)) != 0) continue;
        uint32_t l = 0;
        memcpy(&l, headers + 0x10, 4);
        /* After a resolution change the blob can still hold the previous height's SPS. */
        int height_changed = g_height_current != 0 && g_height_current != chosen_height;
        stale_sps = height_changed && l == g_last_sps_len && l > 0 &&
                    memcmp(headers + 0x18, g_last_sps, l) == 0;
        if (l == 28 && (headers[0x18] & 0x1f) == 7 && !stale_sps) break;
        cap_log("header blob not ready (sps_len=%u stale=%d); retrying\n", l, stale_sps);
        sl_sleep_ms(100);
    }
    if (stale_sps) cap_log("warning: SPS still from the previous height after retries\n");
    if (sl_inject_read_at(&ctx, headers_at, headers, sizeof(headers)) == 0) {
        uint32_t sps_len = 0, pps_len = 0;
        memcpy(&sps_len, headers + 0x10, 4);
        memcpy(&pps_len, headers + 0x98, 4);
        cap_log("Annex B headers: SPS=%u PPS=%u\n", sps_len, pps_len);
        FILE *hf = fopen("/data/glint/h264.headers", "wb");
        if (hf) {
            fwrite(headers, 1, sizeof(headers), hf);
            fclose(hf);
        }
    }
    /*
     * ReadVideo(session, frame_buf, 0x100000, scratch+0x300, 0).
     * frame_buf is malloc(0x100000) in the target. The return value is the frame size.
     */
    uint64_t frame_malloc_args[6] = { 0x100000 };
    intptr_t frame_buf = (intptr_t)sl_inject_call_fast(&ctx, malloc_fn, frame_malloc_args);
    intptr_t read_video = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2ReadVideo");
    cap_log("frame_buf=0x%llx read_video=0x%llx\n", (unsigned long long)frame_buf,
            (unsigned long long)read_video);
    if (!frame_buf || !read_video) goto done;

    /* A decoder needs an IDR first, so ask for one before reading. */
    intptr_t request_key = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2RequestKeyFrame");
    if (request_key) {
        uint64_t key_args[6] = { session };
        int64_t key_rc = sl_inject_call_fast(&ctx, request_key, key_args);
        log_call("RequestKeyFrame()", key_rc);
    }

    intptr_t meta_at = ctx.scratch + 0x300;
    uint8_t *frame = malloc(0x200000);
    if (!frame) goto done;

    uint8_t sps[128], pps[128];
    size_t sps_len = 0, pps_len = 0;
    if (find_headers(headers, sizeof(headers), sps, &sps_len, pps, &pps_len) != 0) {
        cap_log("header parse failed; server not started\n");
        free(frame);
        goto done;
    }

    /* sl_server_start clears any pending profile request, so adopt one that arrived during setup.
     * The session is already at chosen_height, so this run reports it as failed and keeps that height. */
    uint16_t late_height = 0;
    if (!profile_run && sl_take_profile_request(&late_height)) {
        profile_run = 1;
        want_height = late_height;
        cap_log("profile change requested during setup: %u\n", late_height);
    }
    sl_opts.height = chosen_height;
    if (sl_server_start(sl_opts.port, sl_opts.bitrate, sl_opts.height, sl_opts.fps,
                         sps, sps_len, pps, pps_len) != 0) {
        cap_log("sl_server_start failed\n");
        free(frame);
        goto done;
    }
    cap_log("H.264 HTTP :%u -> 0\n", sl_opts.port);

    /* Server start clears the pending profile, so resolve it here. */
    g_prev_height = g_height_current ? g_height_current : chosen_height;
    g_height_current = chosen_height;
    memcpy(g_last_sps, sps, sps_len);
    g_last_sps_len = sps_len;
    if (profile_run) {
        if (chosen_height != want_height) {
            sl_profile_failed();
            g_profile_error_pending = 1;
            cap_log("profile failed; restored %u\n", chosen_height);
        } else if (!stale_sps) {
            sl_profile_applied(chosen_height);
            cap_log("profile applied: %u\n", chosen_height);
        } else {
            /* Applied once the first IDR confirms the new SPS (see the live loop). */
            g_profile_confirm = chosen_height;
        }
        profile_resolved = 1;
    }
    if (g_profile_error_pending) {
        sl_profile_failed();
        g_profile_error_pending = 0;
    }
    if (g_run_count > 0) {
        sl_h264_discontinuity();
        cap_log("discontinuity after reopen\n");
    }
    g_run_count++;

    /* Live loop: ReadVideo -> publish until the server asks us to stop. */
    intptr_t stop_fn = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2Stop");
    intptr_t close_fn = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2Close");
    unsigned frames = 0;
    unsigned failures = 0;
    intptr_t read_audio = sl_resolve_in(ctx.target_pid, "libSceAvcap2.sprx", "sceAvcap2ReadAudio");
    uint8_t *pcm = malloc(0x10000);
    sl_audio_configure("pcm_f32le", 2, 48000);
    while (g.running) {
        /* A profile or restart request ends this session; the reopen loop services it. */
        uint16_t requested = 0;
        if (sl_take_profile_request(&requested)) {
            cap_log("profile change requested: %u; ending session\n", requested);
            break;
        }
        if (sl_take_restart_request()) {
            cap_log("capture restart requested; ending session\n");
            g_restart_pending = 1;
            break;
        }

        if (request_key && sl_take_keyframe_request()) {
            uint64_t k[6] = { session };
            sl_inject_call_fast(&ctx, request_key, k);
        }

        uint64_t rv_args[6] = { session, (uint64_t)frame_buf, 0x100000, (uint64_t)meta_at, 0 };
        int64_t size = sl_inject_call_fast(&ctx, read_video, rv_args);
#ifdef SL_FORCE_STALL
        size = SL_CALL_ERROR;  /* test build only: force the stall path */
#endif
        if (size == SL_CALL_ERROR || size < 5 || size >= 0x200000 ||
            sl_inject_read_at(&ctx, frame_buf, frame, (size_t)size) != 0) {
            if (++failures >= 20) {
                cap_log("capture stalled: last ReadVideo=%s; reopening session\n",
                        size == SL_CALL_ERROR ? "timeout" : "bad");
                break;
            }
            sl_sleep_ms(1);
            continue;
        }
        failures = 0;

        /* The header blob can lag a resize until the encoder has produced an IDR: refresh SPS/PPS then. */
        if (stale_sps && (frame[4] & 0x1f) == 5) {
            stale_sps = 0;
            uint64_t na[6] = { session, (uint64_t)(ctx.scratch + 0x2000) };
            sl_inject_call_fast(&ctx, nonvcl, na);
            uint8_t hb[0x1a8];
            if (sl_inject_read_at(&ctx, ctx.scratch + 0x2000, hb, sizeof(hb)) == 0) {
                uint32_t l2 = 0, p2 = 0;
                memcpy(&l2, hb + 0x10, 4);
                memcpy(&p2, hb + 0x98, 4);
                if (l2 > 0 && l2 <= 128 && p2 > 0 && p2 <= 128 && (hb[0x18] & 0x1f) == 7) {
                    if (l2 == g_last_sps_len && memcmp(hb + 0x18, g_last_sps, l2) == 0) {
                        /* The SPS encodes the resolution, so an unchanged SPS means the encoder did not switch. */
                        cap_log("profile not applied: encoder still at the previous resolution (SPS unchanged)\n");
                        g_profile_confirm = 0;
                        sl_profile_failed();
                        g_profile_error_pending = 1;
                        g_height_current = g_prev_height;
                        g_restart_pending = 1;
                        break;
                    }
                    sl_server_start(sl_opts.port, sl_opts.bitrate, sl_opts.height, sl_opts.fps,
                                     hb + 0x18, l2, hb + 0xa0, p2);
                    memcpy(g_last_sps, hb + 0x18, l2);
                    g_last_sps_len = l2;
                    cap_log("SPS refreshed after first IDR: %u bytes\n", l2);
                    if (g_profile_confirm) {
                        sl_profile_applied(g_profile_confirm);
                        cap_log("profile applied: %u\n", g_profile_confirm);
                        g_profile_confirm = 0;
                    }
                }
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t pts = (uint32_t)((uint64_t)now.tv_sec * 90000 + (uint64_t)now.tv_nsec / 11111);
        sl_h264_publish(frame, (size_t)size, pts);

        /* Drain audio after each video frame: ReadAudio(handle, buf, 0x10000, meta, 1, 0). */
        for (int k = 0; read_audio && pcm && k < 8; k++) {
            uint64_t ra_args[6] = { audio_handle, (uint64_t)buf, 0x10000, (uint64_t)(ctx.scratch + 0x500), 1, 0 };
            int64_t n = sl_inject_call_fast(&ctx, read_audio, ra_args);
            if (n == SL_CALL_ERROR || n <= 0 || n > 0x10000) break;
            if (sl_inject_read_at(&ctx, (intptr_t)buf, pcm, (size_t)n) != 0) break;
            sl_audio_publish(pcm, (size_t)n, pts);
        }
        if (frames++ < 3) cap_log("live frame %u size=%lld nal=0x%02x\n", frames, (long long)size, frame[4] & 0x1f);
    }

    free(pcm);
    /* The server stays up across reopens: sl_server_start reconfigures it in place.
     * Stopping it here would make main() exit the process. */
    if (stop_fn) {
        uint64_t stop_args[6] = { session };
        int64_t rc_stop = sl_inject_call_fast(&ctx, stop_fn, stop_args);
        log_call("Stop()", rc_stop);
    }
    /* Audio shutdown: Audio Stop, then Audio Close. */
    if (stop_fn && audio_handle) {
        uint64_t astop_args[6] = { audio_handle };
        int64_t rc_astop = sl_inject_call_fast(&ctx, stop_fn, astop_args);
        log_call("Audio Stop", rc_astop);
    }
    if (close_fn && audio_handle) {
        uint64_t aclose_args[6] = { audio_handle };
        int64_t rc_aclose = sl_inject_call_fast(&ctx, close_fn, aclose_args);
        log_call("Audio Close", rc_aclose);
    }
    if (close_fn) {
        uint64_t close_args[6] = { session };
        int64_t rc_close = sl_inject_call_fast(&ctx, close_fn, close_args);
        log_call("Close()", rc_close);
    }
    /* Every session end clears the callbacks it used. */
    clear_arbitrator(&ctx, ra_clear, chosen_feature);
    cap_log("live capture stopped after %u frames\n", frames);
    free(frame);

    /* Give the target its heap back: the 1 MB frame buffer and the audio buffer. */
    intptr_t free_fn = sl_resolve_in(ctx.target_pid, "libSceLibcInternal.sprx", "free");
    if (free_fn) {
        uint64_t free_frame[6] = { (uint64_t)frame_buf };
        sl_inject_call_fast(&ctx, free_fn, free_frame);
        uint64_t free_audio[6] = { (uint64_t)buf };
        sl_inject_call_fast(&ctx, free_fn, free_audio);
    }

done:
    /* Never leave a profile request pending: a run that ends early reports it as failed. */
    if (profile_run && !profile_resolved) sl_profile_failed();
    /* Failed opens and early exits still clear the arbitrator (only if we are attached). */
    if (ctx.attached && ra_clear) clear_arbitrator(&ctx, ra_clear, chosen_feature);
    sl_inject_end(&ctx);
    cap_log("inject end\n");
}

#ifdef SL_RESET_RA
/* Clear resource-arbitrator callbacks 11 and 12 left behind by earlier runs. */
static void reset_arbitrator(void)
{
    sl_inject ctx;
    if (sl_inject_begin("SceRemotePlay", &ctx) != 0) {
        cap_log("reset attach failed stage=%s errno=%d\n", ctx.stage, ctx.last_errno);
        return;
    }
    intptr_t clear = sl_resolve_in(ctx.target_pid, "libSceResourceArbitrator.sprx",
                                    "sceResourceArbitratorClearCallback");
    if (clear) {
        for (uint64_t id = 11; id <= 12; id++) {
            uint64_t args[6] = { id };
            int64_t rc = sl_inject_call_fast(&ctx, clear, args);
            cap_log("reset ClearCallback(%llu) rc=0x%x\n", (unsigned long long)id, (uint32_t)rc);
        }
    }
    sl_inject_end(&ctx);
    cap_log("reset done\n");
}
#endif

static volatile int g_capture_quit;

static void *capture_main(void *arg)
{
    (void)arg;
    mkdir("/data/glint", 0777);
    g_cap_log = fopen("/data/glint/capture.log", "w");
    cap_log("Glint " SL_VERSION " capture start\n");

#ifdef SL_RESET_RA
    reset_arbitrator();
    return NULL;
#endif

#ifndef SL_DIRECT_CAPTURE
    /* Reopen the session after a stall, until asked to quit. */
    while (!g_capture_quit && !g_encoder_locked) {
        cap_log("capture run %d start\n", g_run_count + 1);
        capture_session();
        cap_log("capture run %d ended\n", g_run_count);
        if (!g_capture_quit && !g_encoder_locked) sl_sleep_ms(1000);
    }
    return NULL;
#endif

    if (avcap2_open() != 0) {
        cap_log("avcap2_open failed\n");
        return NULL;
    }

    int rc = av.fn_initialize ? av.fn_initialize() : 0;
    cap_log("Initialize rc=0x%x\n", rc);

    uint32_t video_mode = 0;
    if (av.fn_get_video_mode) {
        rc = av.fn_get_video_mode(&video_mode);
        cap_log("GetVideoOutMode rc=0x%x mode=0x%x\n", rc, video_mode);
    }

    /* 1080p, tried twice. */
    typedef struct { uint32_t feature, resolution, fps_mode, width, height; } Attempt;
    Attempt attempts[] = {
        { 12, 0x438, 4, 1920, 1080 },
        { 12, 0x438, 4, 1920, 1080 },
    };

    uint8_t params[0x2e0];
    uint32_t handle = 0;
    for (unsigned i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        build_video_params(params, attempts[i].feature, attempts[i].width, attempts[i].resolution,
                           attempts[i].fps_mode, 0x10, sl_opts.bitrate, sl_opts.minqp);
        cap_log("OpenVideo try feature=%u res=0x%x %ux%u\n",
                attempts[i].feature, attempts[i].resolution,
                attempts[i].width, attempts[i].height);
        rc = av.fn_open_video(params, &handle);
        cap_log("OpenVideo rc=0x%x handle=0x%x\n", rc, handle);
        if (rc == 0 && handle) {
            sl_opts.height = (uint16_t)attempts[i].height;
            break;
        }
    }
    if (rc != 0 || !handle) {
        if (av.fn_terminate) av.fn_terminate();
        return NULL;
    }
    av.video = handle;
    sl_audio_configure("none", 0, 0);

    if (av.fn_enable_scan) {
        rc = av.fn_enable_scan();
        cap_log("EnableScanInEncode rc=0x%x\n", rc);
    }

    if (sl_opts.bitrate && av.fn_set_bitrate) {
        avcap2_bitrate_params rate;
        rate.size = sizeof(rate);
        rate.bitrate = sl_opts.bitrate;
        rate.budget = sl_opts.bitrate;
        rc = av.fn_set_bitrate(handle, &rate);
        cap_log("SetVideoBitrate rc=0x%x\n", rc);
    }

    rc = av.fn_start(handle);
    cap_log("Start rc=0x%x\n", rc);
    if (rc != 0) return NULL;

    uint8_t headers[0x200];
    memset(headers, 0, sizeof(headers));
    rc = av.fn_non_vcl ? av.fn_non_vcl(handle, headers) : -1;
    cap_log("GetNonVclNalUnitStream rc=0x%x\n", rc);

    uint8_t sps[128], pps[128];
    size_t sps_len = 0, pps_len = 0;
    if (find_headers(headers, sizeof(headers), sps, &sps_len, pps, &pps_len) != 0) {
        /* Baseline H.264 1080p60 High profile headers, used only on the
         * SL_DIRECT_CAPTURE path when the non-VCL blob has not been produced
         * yet. The main trampoline path waits for a real IDR and never
         * reaches this fallback. See docs/CAPTURE-INTERNALS.md. */
        static const uint8_t baseline_1080p60_sps[] = {
            0x67, 0x4d, 0x40, 0x32, 0x91, 0xa0, 0x1e, 0x00, 0x89, 0xf9,
            0x70, 0x16, 0xa0, 0x20, 0x20, 0x28, 0x00, 0x00, 0x1f, 0x48,
            0x00, 0x0e, 0xa6, 0x04, 0x4d, 0x0f, 0x1f, 0x50
        };
        static const uint8_t baseline_1080p60_pps[] = { 0x68, 0xee, 0x3c, 0x80 };
        memcpy(sps, baseline_1080p60_sps, sizeof(baseline_1080p60_sps));
        memcpy(pps, baseline_1080p60_pps, sizeof(baseline_1080p60_pps));
        sps_len = sizeof(baseline_1080p60_sps);
        pps_len = sizeof(baseline_1080p60_pps);
        cap_log("using baseline 1080p60 SPS/PPS\n");
    }
    cap_log("headers sps=%llu pps=%llu\n", (unsigned long long)sps_len,
            (unsigned long long)pps_len);

    if (sl_server_start(sl_opts.port, sl_opts.bitrate, sl_opts.height,
                         sl_opts.fps, sps, sps_len, pps, pps_len) != 0) {
        cap_log("sl_server_start failed\n");
        return NULL;
    }
    cap_log("server started on %u\n", sl_opts.port);

    uint8_t *buffer = malloc(0x200000);
    if (!buffer) return NULL;

    int reads = 0;
    while (g.running) {
        if (sl_take_keyframe_request() && av.fn_request_keyframe)
            av.fn_request_keyframe(handle);

        size_t got = 0;
        uint64_t ts = 0;
        rc = av.fn_read_video(handle, buffer, 0x200000, &got, &ts);
        if (rc != 0 || got == 0) {
            if (reads < 8)
                cap_log("ReadVideo rc=0x%x got=%llu\n", rc, (unsigned long long)got);
            reads++;
            sl_sleep_ms(1);
            continue;
        }
        if (reads < 8) {
            cap_log("frame got=%llu ts=%llu key=%d\n", (unsigned long long)got,
                    (unsigned long long)ts, buffer[4] == 0x65 ? 1 : 0);
            reads++;
        }
        sl_h264_publish(buffer, got, (uint32_t)ts);
    }

    free(buffer);
    av.fn_stop(handle);
    if (av.fn_close) av.fn_close(handle);
    if (av.fn_terminate) av.fn_terminate();
    return NULL;
}

int sl_capture_start(void)
{
    if (worker_active) return 0;
    if (pthread_create(&worker, NULL, capture_main, NULL) != 0) return -1;
    worker_active = 1;
    return 0;
}

void sl_capture_stop(void)
{
    g_capture_quit = 1;
    g.running = 0;
    if (worker_active) {
        pthread_join(worker, NULL);
        worker_active = 0;
    }
}

#endif /* SL_PS5 */
