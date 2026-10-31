#include "glint.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

sl_options sl_opts = {
    .port = 7231,
    .height = 1080,
    .fps = 60,
    /* The encoder also caps each frame at bitrate / 60. At 50 Mbit/s that cap
     * (~104 KB) held fast motion at QP 22-28. At 100 Mbit/s a heavy 30 fps
     * game filled the ~208 KB cap on every frame, ~95 Mbit/s, and frames came
     * out up to 250 ms late with drops. Over a weak 5 GHz Wi-Fi link even a
     * steady 50 stalled for seconds (GTA at 25-35 was smooth), so the default
     * is 35. On a wired PS5, 75-100 gives sharper heavy scenes (bitrate=N).
     * Set once at start: the encoder can't change it on a running session. */
    .bitrate = 35000000u,
    .minqp = 18,
    .audio = 1,
};

static volatile int g_quit = 0;

#ifdef SL_PS5
/* The name in the console's process list (Payload Manager shows "payload.elf"
 * otherwise). Weak: a firmware without it still loads Glint. */
int sceKernelSetProcessName(const char *name) __attribute__((weak));

/* "120 Hz Output" (Settings > Screen and Video > Video Output) is the
 * registry entry VIDEOOUT_hfr: 0 = Auto/On, 1 = Off. A game that switches
 * the console to 120 Hz stops the capture for good, so Glint turns it Off
 * while it runs and puts it back when it stops. The original value is kept
 * in a file too, so a Glint that was killed is undone by the next clean stop. */
int sceRegMgrGetInt(long key, int *val) __attribute__((weak));
int sceRegMgrSetInt(long key, int val) __attribute__((weak));
#define SL_REG_VIDEOOUT_HFR 0x0a240000L
#define SL_HFR_OFF          1
#define SL_HFR_RESTORE_FILE "/data/glint/hfr_restore"

static int hfr_saved_read(int *value)
{
    FILE *f = fopen(SL_HFR_RESTORE_FILE, "r");
    if (!f) return -1;
    int ok = fscanf(f, "%d", value) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static void hfr_off(void)
{
    if (!sceRegMgrGetInt || !sceRegMgrSetInt) return;
    int value = -1;
    if (sceRegMgrGetInt(SL_REG_VIDEOOUT_HFR, &value) != 0 || value == SL_HFR_OFF) return;
    int saved;
    if (hfr_saved_read(&saved) != 0) {
        FILE *f = fopen(SL_HFR_RESTORE_FILE, "w");
        if (f) { fprintf(f, "%d\n", value); fclose(f); }
    }
    if (sceRegMgrSetInt(SL_REG_VIDEOOUT_HFR, SL_HFR_OFF) == 0)
        sl_notify(0, "Glint turned 120 Hz Output off while it runs. It comes back when Glint stops.");
}

static void hfr_restore(void)
{
    int saved;
    if (!sceRegMgrSetInt || hfr_saved_read(&saved) != 0) return;
    if (sceRegMgrSetInt(SL_REG_VIDEOOUT_HFR, saved) == 0) remove(SL_HFR_RESTORE_FILE);
}
#else
static void hfr_off(void) {}
static void hfr_restore(void) {}
#endif

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static int parse_u32(const char *value, uint32_t *out)
{
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end != 0 || parsed > 0xffffffffu) return -1;
    *out = (uint32_t)parsed;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "Glint: glint-payload "
        "[1080p] [bitrate=N] [minqp=N] [norate] [noaudio] [port]\n");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strncmp(arg, "bitrate=", 8) == 0) {
            uint32_t value;
            if (parse_u32(arg + 8, &value) != 0 || value == 0 || value > 100000000u) {
                usage();
                return 1;
            }
            sl_opts.bitrate = value;
        } else if (strncmp(arg, "minqp=", 6) == 0) {
            uint32_t value;
            if (parse_u32(arg + 6, &value) != 0 || value < 18 || value > 51) {
                usage();
                return 1;
            }
            sl_opts.minqp = (int)value;
        } else if (strcmp(arg, "1080p") == 0) {
            sl_opts.height = 1080;
        } else if (strcmp(arg, "norate") == 0) {
            sl_opts.bitrate = 0;
        } else if (strcmp(arg, "noaudio") == 0) {
            sl_opts.audio = 0;
        } else if (arg[0] >= '0' && arg[0] <= '9') {
            uint32_t port;
            if (parse_u32(arg, &port) != 0 || port < 1024 || port > 65535) {
                usage();
                return 1;
            }
            sl_opts.port = (uint16_t)port;
        } else {
            fprintf(stderr, "Glint: ignoring argument %s\n", arg);
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pthread_mutex_init(&g.lock, NULL);
    pthread_cond_init(&g.cond, NULL);
    snprintf(g.platform, sizeof(g.platform), "PS5");

#ifdef SL_PS5
    /* Log, recordings and marker files from before the rename (Screenlink). */
    if (access("/data/glint", F_OK) != 0 && access("/data/screenlink", F_OK) == 0)
        rename("/data/screenlink", "/data/glint");
    /* ".elf": Payload Manager lists only names ending in .elf as payloads;
     * anything else goes under "Show All System Processes". */
    if (sceKernelSetProcessName) sceKernelSetProcessName("Glint.elf");
#endif

    sl_notify(0, "Glint " SL_VERSION " loaded. Starting capture...");
    hfr_off();
    if (sl_capture_start() != 0) {
        fprintf(stderr, "Glint: capture unavailable\n");
        sl_notify(1, "Glint: capture is not available on this console.");
        hfr_restore();
        return 1;
    }

    /* Wait for the capture backend to publish its first configuration, or
     * for a stop request from the dashboard. */
    uint64_t wait_started = sl_now_ms();
    while (!g_quit) {
        if (sl_server_running()) {
            sl_sleep_ms(200);
            continue;
        }
        if (g.started_ms != 0) break;             /* server ran, then stopped */
        if (sl_capture_failed()) break;           /* encoder refused: no point waiting */
        if (sl_now_ms() - wait_started > 30000) break;
        sl_sleep_ms(100);
    }

    int served = g.started_ms != 0;
    sl_capture_stop();
    sl_server_stop();
    sl_server_wait();
    hfr_restore();
    if (served) sl_notify(0, "Glint stopped.");
    else if (!sl_error_notified())
        sl_notify(1, "Glint could not start capture. See /data/glint/capture.log");
    return 0;
}
