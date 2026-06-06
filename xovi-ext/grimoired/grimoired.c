/* grimoired.c — On-device grimoire daemon (Phase 1)
 *
 * Runs entirely on the reMarkable. Watches /tmp/grimoire_armed for the
 * toggle state, and when armed, accepts stroke injection commands via
 * a simple file-trigger protocol.
 *
 * Phase 1: daemon skeleton + arming + in-process evdev injection.
 * Absorbs uinjectd's draw/erase/swipe code directly — no socket, no
 * separate daemon, no connection management.
 *
 * Injection trigger: write a JSON command to /tmp/grimoire_cmd:
 *   {"cmd":"draw","file":"/tmp/grimoire_strokes.json","speed":3}
 *   {"cmd":"erase","file":"/tmp/grimoire_thinking_live.json","speed":20}
 *   {"cmd":"swipe"}
 *
 * The daemon picks up the command, executes it, writes the result to
 * /tmp/grimoire_result, then deletes /tmp/grimoire_cmd.
 *
 * Usage: grimoired [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <png.h>

#define DEV_PATH     "/dev/input/event1"
#define TOUCH_PATH   "/dev/input/event2"
#define ARMED_PATH   "/tmp/grimoire_armed"
#define IDLE_PATH    "/tmp/grimoire_idle"
#define CMD_PATH     "/tmp/grimoire_cmd"
#define RESULT_PATH  "/tmp/grimoire_result"
#define LOG_PATH     "/tmp/grimoired.log"
#define FB_RAW_PATH  "/tmp/grimoire_fb.raw"
#define SCREENSHOT_TRIGGER "/tmp/grimoire_screenshot"
#define CAPTURE_OUT_PATH "/tmp/grimoire_capture.png"
#define MAX_CMD      8192

/* Framebuffer dimensions */
#define FB_WIDTH     1404
#define FB_HEIGHT    1872
#define FB_BPP       4
#define FB_SIZE      (FB_WIDTH * FB_HEIGHT * FB_BPP)

/* Crop parameters (match Python loop) */
#define CROP_TOP     80
#define CROP_BOTTOM  200
#define CROP_HEIGHT  (FB_HEIGHT - CROP_TOP - CROP_BOTTOM)

/* Grid artifact filter constants */
#define DARK_THRESHOLD   128
#define MIN_DARK_PIXELS  5
#define GRID_SIGNATURE   68
#define CLUSTER_WINDOW   2
#define CLUSTER_MIN      3

static int g_verbose = 0;
static int g_dev_fd = -1;
static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_inject_lock = PTHREAD_MUTEX_INITIALIZER;

/* ─── logging ────────────────────────────────────────────────────── */

static FILE *g_log_fp = NULL;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    /* Always write to log file */
    if (!g_log_fp) {
        g_log_fp = fopen(LOG_PATH, "a");
    }
    if (g_log_fp) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        fprintf(g_log_fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                t->tm_year+1900, t->tm_mon+1, t->tm_mday,
                t->tm_hour, t->tm_min, t->tm_sec);
        vfprintf(g_log_fp, fmt, ap);
        fflush(g_log_fp);
    }

    if (g_verbose) {
        va_list ap2;
        va_copy(ap2, ap);
        fprintf(stderr, "[grimoired] ");
        vfprintf(stderr, fmt, ap2);
        va_end(ap2);
    }

    va_end(ap);
}

/* ─── signal handling ────────────────────────────────────────────── */

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ─── evdev helpers (from uinjectd) ──────────────────────────────── */

static void emit_event(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    write(fd, &ev, sizeof(ev));
}

static void emit_syn(int fd) {
    emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static void rm_to_wacom(float rm_x, float rm_y, int *wx, int *wy) {
    float screen_x = rm_x + 702.0f;
    float screen_y = rm_y;
    *wx = (int)((1872.0f - screen_y) * 20966.0f / 1872.0f);
    *wy = (int)(screen_x * 15725.0f / 1404.0f);
}

static int rm_pressure_to_wacom(int rm_pressure) {
    return rm_pressure * 4095 / 255;
}

/* ─── minimal JSON parsing ───────────────────────────────────────── */

static const char *json_str(const char *json, const char *key, int *len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return NULL;
    p++;
    const char *start = p;
    while (*p && *p != '"') p++;
    *len = (int)(p - start);
    return start;
}

static int json_int(const char *json, const char *key, int def) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

/* ─── file loading ───────────────────────────────────────────────── */

static int load_file(const char *path, char **out, long *out_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);
    *out = buf;
    *out_size = sz;
    return 0;
}

static void write_result(const char *msg) {
    FILE *fp = fopen(RESULT_PATH, "w");
    if (fp) {
        fputs(msg, fp);
        fputc('\n', fp);
        fclose(fp);
    }
}

/* ─── image pipeline (Phase 2) ───────────────────────────────────── */

/* Read raw framebuffer into allocated buffer. Returns NULL on failure. */
static unsigned char *read_framebuffer(void) {
    FILE *fp = fopen(FB_RAW_PATH, "rb");
    if (!fp) return NULL;
    unsigned char *buf = malloc(FB_SIZE);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, FB_SIZE, fp);
    fclose(fp);
    if ((long)n < FB_SIZE) { free(buf); return NULL; }
    return buf;
}

/* Convert RGBA pixel to grayscale luminance */
static inline unsigned char rgba_to_gray(const unsigned char *px) {
    /* Standard luminance: 0.299R + 0.587G + 0.114B */
    return (unsigned char)((px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8);
}

/* Count dark pixels in a row of the cropped region.
 * Row index is relative to the crop (0 = CROP_TOP in raw FB). */
static int count_dark_in_row(const unsigned char *fb, int row) {
    int raw_y = row + CROP_TOP;
    if (raw_y < 0 || raw_y >= FB_HEIGHT) return 0;
    const unsigned char *row_ptr = fb + raw_y * FB_WIDTH * FB_BPP;
    int count = 0;
    for (int x = 0; x < FB_WIDTH; x++) {
        if (rgba_to_gray(row_ptr + x * FB_BPP) < DARK_THRESHOLD)
            count++;
    }
    return count;
}

/* Check if page has no real handwriting (only grid dots or blank).
 * Ported from Python _page_is_empty with grid artifact filter. */
static int page_is_empty(const unsigned char *fb) {
    int num_rows = CROP_HEIGHT;
    unsigned char *candidate = calloc(num_rows, 1);
    if (!candidate) return 1;

    for (int y = 0; y < num_rows; y++) {
        int dark = count_dark_in_row(fb, y);
        candidate[y] = (dark > MIN_DARK_PIXELS && dark != GRID_SIGNATURE) ? 1 : 0;
    }

    /* Check for clusters: any candidate row with >= CLUSTER_MIN neighbors
     * within ±CLUSTER_WINDOW means real content exists */
    for (int y = 0; y < num_rows; y++) {
        if (!candidate[y]) continue;
        int cluster = 0;
        for (int dy = -CLUSTER_WINDOW; dy <= CLUSTER_WINDOW; dy++) {
            int ny = y + dy;
            if (ny >= 0 && ny < num_rows && candidate[ny])
                cluster++;
        }
        if (cluster >= CLUSTER_MIN) {
            free(candidate);
            return 0;  /* Not empty */
        }
    }

    free(candidate);
    return 1;  /* Empty */
}

/* Find the bottom of real content in the cropped region.
 * Returns the Y coordinate (in raw FB space) of the content bottom,
 * or 0 if no content found. Uses grid-artifact filter. */
static int find_content_bottom(const unsigned char *fb) {
    int num_rows = CROP_HEIGHT;
    unsigned char *candidate = calloc(num_rows, 1);
    if (!candidate) return 0;

    for (int y = 0; y < num_rows; y++) {
        int dark = count_dark_in_row(fb, y);
        candidate[y] = (dark > MIN_DARK_PIXELS && dark != GRID_SIGNATURE) ? 1 : 0;
    }

    /* Find the lowest row that's part of a real cluster */
    int bottom_crop_y = -1;
    for (int y = num_rows - 1; y >= 0; y--) {
        if (!candidate[y]) continue;
        int cluster = 0;
        for (int dy = -CLUSTER_WINDOW; dy <= CLUSTER_WINDOW; dy++) {
            int ny = y + dy;
            if (ny >= 0 && ny < num_rows && candidate[ny])
                cluster++;
        }
        if (cluster >= CLUSTER_MIN) {
            bottom_crop_y = y;
            break;
        }
    }

    free(candidate);
    if (bottom_crop_y < 0) return 0;

    /* Add padding and convert back to raw FB coordinates */
    int bottom_raw = bottom_crop_y + CROP_TOP + 30;
    if (bottom_raw >= FB_HEIGHT - CROP_BOTTOM)
        bottom_raw = FB_HEIGHT - CROP_BOTTOM - 1;
    return bottom_raw;
}

/* Save cropped region as grayscale PNG via libpng.
 * Crops top CROP_TOP and bottom CROP_BOTTOM rows from raw FB. */
static int save_cropped_png(const unsigned char *fb, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                               NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, FB_WIDTH, CROP_HEIGHT, 8,
                 PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    /* Write row by row, converting RGBA to grayscale */
    unsigned char *row_buf = malloc(FB_WIDTH);
    if (!row_buf) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    for (int y = 0; y < CROP_HEIGHT; y++) {
        int raw_y = y + CROP_TOP;
        const unsigned char *src = fb + raw_y * FB_WIDTH * FB_BPP;
        for (int x = 0; x < FB_WIDTH; x++) {
            row_buf[x] = rgba_to_gray(src + x * FB_BPP);
        }
        png_write_row(png, row_buf);
    }

    png_write_end(png, NULL);
    free(row_buf);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

/* Trigger framebuffer capture via xovi extension and wait for dump.
 * Returns allocated raw FB buffer, or NULL on timeout. */
static unsigned char *trigger_and_read_fb(void) {
    /* Remove old dump and trigger new one */
    unlink(FB_RAW_PATH);
    int tf = open(SCREENSHOT_TRIGGER, O_CREAT|O_WRONLY, 0644);
    if (tf >= 0) close(tf);

    /* Wait for dump (xovi watch thread checks every ~1s worst case) */
    for (int i = 0; i < 30; i++) {
        usleep(100000);  /* 100ms polls */
        struct stat st;
        if (stat(FB_RAW_PATH, &st) == 0 && st.st_size >= FB_SIZE) {
            return read_framebuffer();
        }
    }
    return NULL;
}

/* ─── injection: draw ────────────────────────────────────────────── */

static int do_draw(int fd, const char *json, int delay_us) {
    char *p = (char *)json;
    int stroke_count = 0;

    while ((p = strstr(p, "\"points\"")) != NULL) {
        p = strchr(p, '[');
        if (!p) break;
        p++;

        /* Peek first point for hover frame */
        float hx, hy;
        int hs, hw, hd, hp;
        if (sscanf(p, "[%f,%f,%d,%d,%d,%d]", &hx, &hy, &hs, &hw, &hd, &hp) == 6) {
            int hwx, hwy;
            rm_to_wacom(hx, hy, &hwx, &hwy);
            emit_event(fd, EV_KEY, BTN_TOOL_PEN, 1);
            emit_event(fd, EV_ABS, ABS_X, hwx);
            emit_event(fd, EV_ABS, ABS_Y, hwy);
            emit_event(fd, EV_ABS, ABS_DISTANCE, 60);
            emit_syn(fd);
            usleep(2000);
        } else {
            emit_event(fd, EV_KEY, BTN_TOOL_PEN, 1);
            emit_event(fd, EV_ABS, ABS_DISTANCE, 60);
            emit_syn(fd);
            usleep(2000);
        }
        emit_event(fd, EV_KEY, BTN_TOUCH, 1);

        int point_count = 0;
        while (*p && *p != ']') {
            float x, y;
            int speed, width, direction, pressure;
            if (sscanf(p, "[%f,%f,%d,%d,%d,%d]", &x, &y, &speed, &width, &direction, &pressure) == 6) {
                int wx, wy;
                rm_to_wacom(x, y, &wx, &wy);
                int wp = rm_pressure_to_wacom(pressure);

                emit_event(fd, EV_ABS, ABS_X, wx);
                emit_event(fd, EV_ABS, ABS_Y, wy);
                emit_event(fd, EV_ABS, ABS_PRESSURE, wp);
                emit_event(fd, EV_ABS, ABS_DISTANCE, 0);
                emit_syn(fd);
                point_count++;
                usleep(delay_us);
            }
            p = strchr(p, ']');
            if (p) p++;
            while (*p && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r')) p++;
        }

        /* Clean pen lift */
        emit_event(fd, EV_ABS, ABS_PRESSURE, 0);
        emit_event(fd, EV_ABS, ABS_DISTANCE, 86);
        emit_syn(fd);
        emit_event(fd, EV_KEY, BTN_TOUCH, 0);
        emit_syn(fd);
        usleep(8000);
        /* Fully leave proximity so the device commits this stroke */
        emit_event(fd, EV_ABS, ABS_DISTANCE, 255);
        emit_event(fd, EV_KEY, BTN_TOOL_PEN, 0);
        emit_syn(fd);

        stroke_count++;
        log_msg("Stroke %d: %d points\n", stroke_count, point_count);
        usleep(60000);
    }
    return stroke_count;
}

/* ─── injection: erase ───────────────────────────────────────────── */

static int do_erase(int fd, const char *json, int delay_us) {
    static const float offsets[][2] = {
        {0, 0}, {-6, 0}, {6, 0},
    };
    int num_passes = sizeof(offsets) / sizeof(offsets[0]);
    int total_emitted = 0;

    for (int pass = 0; pass < num_passes; pass++) {
        float ox = offsets[pass][0];
        float oy = offsets[pass][1];
        char *p = (char *)json;
        int pass_emitted = 0;

        while ((p = strstr(p, "\"points\"")) != NULL) {
            p = strchr(p, '[');
            if (!p) break;
            p++;

            /* Peek first point for hover frame */
            float hx, hy;
            int hs, hw, hd, hp;
            char *saved_p = p;
            if (sscanf(p, "[%f,%f,%d,%d,%d,%d]", &hx, &hy, &hs, &hw, &hd, &hp) == 6) {
                int hwx, hwy;
                rm_to_wacom(hx + ox, hy + oy, &hwx, &hwy);
                emit_event(fd, EV_KEY, BTN_TOOL_RUBBER, 1);
                emit_event(fd, EV_ABS, ABS_X, hwx);
                emit_event(fd, EV_ABS, ABS_Y, hwy);
                emit_event(fd, EV_ABS, ABS_DISTANCE, 60);
                emit_syn(fd);
                usleep(2000);
            } else {
                emit_event(fd, EV_KEY, BTN_TOOL_RUBBER, 1);
                emit_event(fd, EV_ABS, ABS_DISTANCE, 60);
                emit_syn(fd);
                usleep(2000);
            }
            p = saved_p;
            emit_event(fd, EV_KEY, BTN_TOUCH, 1);

            int bracket_depth = 1;
            float last_x = 1e9f, last_y = 1e9f;
            const float min_dist_sq = 15.0f * 15.0f;

            while (*p && bracket_depth > 0) {
                if (*p == '[') {
                    float x, y;
                    int speed, width, direction, pressure;
                    if (sscanf(p, "[%f,%f,%d,%d,%d,%d]", &x, &y, &speed, &width, &direction, &pressure) == 6) {
                        float dx = (x + ox) - last_x;
                        float dy = (y + oy) - last_y;
                        if (dx*dx + dy*dy >= min_dist_sq || pass_emitted == 0) {
                            int wx, wy;
                            rm_to_wacom(x + ox, y + oy, &wx, &wy);

                            emit_event(fd, EV_ABS, ABS_X, wx);
                            emit_event(fd, EV_ABS, ABS_Y, wy);
                            emit_event(fd, EV_ABS, ABS_PRESSURE, 4095);
                            emit_event(fd, EV_ABS, ABS_DISTANCE, 0);
                            emit_syn(fd);

                            last_x = x + ox;
                            last_y = y + oy;
                            pass_emitted++;
                            total_emitted++;
                            usleep(delay_us);
                        }
                    }
                    char *close = strchr(p + 1, ']');
                    if (close) p = close + 1;
                    else break;
                } else if (*p == ']') {
                    bracket_depth--;
                    if (bracket_depth <= 0) break;
                    p++;
                } else {
                    p++;
                }
            }

            emit_event(fd, EV_ABS, ABS_PRESSURE, 0);
            emit_event(fd, EV_ABS, ABS_DISTANCE, 86);
            emit_syn(fd);
            emit_event(fd, EV_KEY, BTN_TOUCH, 0);
            emit_event(fd, EV_KEY, BTN_TOOL_RUBBER, 0);
            emit_syn(fd);
            usleep(10000);
        }
        log_msg("Erase pass %d/%d: %d points\n", pass+1, num_passes, pass_emitted);
    }
    return total_emitted;
}

/* ─── injection: page swipe ──────────────────────────────────────── */

static int do_swipe_page(void) {
    int fd = open(TOUCH_PATH, O_WRONLY);
    if (fd < 0) { log_msg("swipe: open %s failed\n", TOUCH_PATH); return -1; }

    const int x0 = 1200, x1 = 200, y = 1400;
    const int steps = 20;
    const int step_delay = 8000;

    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 9999);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x0);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit_event(fd, EV_ABS, ABS_MT_PRESSURE, 100);
    emit_syn(fd);
    usleep(step_delay);

    for (int i = 1; i <= steps; i++) {
        int cx = x0 + (x1 - x0) * i / steps;
        emit_event(fd, EV_ABS, ABS_MT_POSITION_X, cx);
        emit_event(fd, EV_ABS, ABS_MT_PRESSURE, 100);
        if (i == 3) {
            emit_event(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 17);
        }
        emit_syn(fd);
        usleep(step_delay);
    }

    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit_syn(fd);

    close(fd);
    log_msg("Swipe page done\n");
    return 0;
}

/* ─── command handling ───────────────────────────────────────────── */

static void handle_command(const char *line) {
    int cmd_len;
    const char *cmd = json_str(line, "cmd", &cmd_len);
    if (!cmd) {
        write_result("{\"resp\":\"?\",\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }

    /* Capture command: trigger screenshot, process, save PNG */
    if (strncmp(cmd, "capture", 7) == 0) {
        log_msg("Capture: triggering framebuffer dump\n");
        unsigned char *fb = trigger_and_read_fb();
        if (!fb) {
            write_result("{\"resp\":\"capture\",\"ok\":false,\"error\":\"timeout\"}");
            return;
        }

        int empty = page_is_empty(fb);
        int content_bottom = find_content_bottom(fb);
        log_msg("Capture: empty=%d content_bottom=%d\n", empty, content_bottom);

        if (empty) {
            free(fb);
            write_result("{\"resp\":\"capture\",\"ok\":true,\"empty\":true,\"content_bottom\":0}");
            return;
        }

        /* Save cropped PNG */
        if (save_cropped_png(fb, CAPTURE_OUT_PATH) != 0) {
            free(fb);
            write_result("{\"resp\":\"capture\",\"ok\":false,\"error\":\"png write failed\"}");
            return;
        }

        free(fb);
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "{\"resp\":\"capture\",\"ok\":true,\"empty\":false,"
                 "\"content_bottom\":%d,\"file\":\"%s\"}",
                 content_bottom, CAPTURE_OUT_PATH);
        write_result(resp);
        log_msg("Capture: saved %s\n", CAPTURE_OUT_PATH);
        return;
    }

    if (strncmp(cmd, "swipe", 5) == 0) {
        pthread_mutex_lock(&g_inject_lock);
        int ret = do_swipe_page();
        pthread_mutex_unlock(&g_inject_lock);
        if (ret == 0)
            write_result("{\"resp\":\"swipe\",\"ok\":true}");
        else
            write_result("{\"resp\":\"swipe\",\"ok\":false,\"error\":\"touch open failed\"}");
        return;
    }

    int file_len;
    const char *file_val = json_str(line, "file", &file_len);
    if (!file_val) {
        write_result("{\"resp\":\"?\",\"ok\":false,\"error\":\"missing file\"}");
        return;
    }
    char filepath[512];
    if (file_len >= (int)sizeof(filepath)) file_len = sizeof(filepath) - 1;
    memcpy(filepath, file_val, file_len);
    filepath[file_len] = '\0';

    int speed_ms = json_int(line, "speed", 5);
    int delay_us = speed_ms * 1000;

    char *json = NULL;
    long fsize = 0;
    if (load_file(filepath, &json, &fsize) != 0) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"resp\":\"%.*s\",\"ok\":false,\"error\":\"load: %s\"}",
                 cmd_len, cmd, strerror(errno));
        write_result(err);
        return;
    }

    log_msg("Loaded %ld bytes from %s (speed=%dms)\n", fsize, filepath, speed_ms);

    char resp[128];
    pthread_mutex_lock(&g_inject_lock);
    if (strncmp(cmd, "draw", 4) == 0) {
        int strokes = do_draw(g_dev_fd, json, delay_us);
        snprintf(resp, sizeof(resp), "{\"resp\":\"draw\",\"ok\":true,\"strokes\":%d}", strokes);
    } else if (strncmp(cmd, "erase", 5) == 0) {
        int points = do_erase(g_dev_fd, json, delay_us);
        snprintf(resp, sizeof(resp), "{\"resp\":\"erase\",\"ok\":true,\"points\":%d}", points);
    } else {
        snprintf(resp, sizeof(resp), "{\"resp\":\"?\",\"ok\":false,\"error\":\"unknown cmd\"}");
    }
    pthread_mutex_unlock(&g_inject_lock);

    free(json);
    write_result(resp);
}

/* ─── armed-state watcher ────────────────────────────────────────── */

static int read_armed_state(void) {
    FILE *fp = fopen(ARMED_PATH, "r");
    if (!fp) return 0;
    int val = 0;
    fscanf(fp, "%d", &val);
    fclose(fp);
    return val != 0;
}

/* ─── main loop ──────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            g_verbose = 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    g_dev_fd = open(DEV_PATH, O_WRONLY);
    if (g_dev_fd < 0) {
        log_msg("FATAL: cannot open %s: %s\n", DEV_PATH, strerror(errno));
        return 1;
    }

    log_msg("Started (pid %d)\n", getpid());

    int armed = read_armed_state();
    log_msg("Initial armed state: %d\n", armed);

    while (g_running) {
        /* Check armed state */
        int new_armed = read_armed_state();
        if (new_armed != armed) {
            armed = new_armed;
            log_msg("Armed state changed: %d\n", armed);
        }

        if (!armed) {
            usleep(500000);  /* Poll every 500ms when disarmed */
            continue;
        }

        /* When armed: check for pending command */
        struct stat st;
        if (stat(CMD_PATH, &st) == 0 && st.st_size > 0) {
            char *cmd_buf = NULL;
            long cmd_size = 0;
            if (load_file(CMD_PATH, &cmd_buf, &cmd_size) == 0 && cmd_size > 0) {
                /* Strip trailing newline */
                while (cmd_size > 0 && (cmd_buf[cmd_size-1] == '\n' || cmd_buf[cmd_size-1] == '\r'))
                    cmd_buf[--cmd_size] = '\0';

                if (cmd_size > 0) {
                    log_msg("Processing command: %s\n", cmd_buf);
                    handle_command(cmd_buf);
                }
                free(cmd_buf);
            }
            /* Remove the command file so we don't re-execute */
            unlink(CMD_PATH);
        }

        usleep(100000);  /* Poll every 100ms when armed */
    }

    close(g_dev_fd);
    if (g_log_fp) fclose(g_log_fp);
    log_msg("Stopped\n");
    return 0;
}
