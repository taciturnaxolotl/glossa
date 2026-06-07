/* glossad.c — On-device glossa daemon (Phase 1)
 *
 * Runs entirely on the reMarkable. Watches /tmp/glossa_armed for the
 * toggle state, and when armed, accepts stroke injection commands via
 * a simple file-trigger protocol.
 *
 * Phase 1: daemon skeleton + arming + in-process evdev injection.
 * Absorbs uinjectd's draw/erase/swipe code directly — no socket, no
 * separate daemon, no connection management.
 *
 * Injection trigger: write a JSON command to /tmp/glossa_cmd:
 *   {"cmd":"draw","file":"/tmp/glossa_strokes.json","speed":3}
 *   {"cmd":"erase","file":"/tmp/glossa_thinking_live.json","speed":20}
 *   {"cmd":"swipe"}
 *
 * The daemon picks up the command, executes it, writes the result to
 * /tmp/glossa_result, then deletes /tmp/glossa_cmd.
 *
 * Usage: glossad [-v]
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
#include <math.h>
#include <png.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define DEV_PATH     "/dev/input/event1"
#define TOUCH_PATH   "/dev/input/event2"
#define ARMED_PATH   "/tmp/glossa_armed"
#define IDLE_PATH    "/tmp/glossa_idle"
#define CMD_PATH     "/tmp/glossa_cmd"
#define RESULT_PATH  "/tmp/glossa_result"
#define LOG_PATH     "/tmp/glossad.log"
#define FB_RAW_PATH  "/tmp/glossa_fb.raw"
#define SCREENSHOT_TRIGGER "/tmp/glossa_screenshot"
#define CAPTURE_OUT_PATH "/tmp/glossa_capture.png"
#define API_KEY_PATH   "/home/root/.glossa_key"
#define API_HOST       "potluck.dunkirk.sh"
#define API_PATH       "/v1/chat/completions"
#define API_MODEL      "gpt-4.1-nano"
#define FONT_PATH      "/home/root/font_data.json"
#define RENDER_OUT_PATH "/tmp/glossa_render.json"
#define THINKING_PATH   "/tmp/glossa_thinking"
#define ORNAMENTS_PATH  "/home/root/ornaments.json"
#define SAFEZONE_PATH   "/tmp/glossa_safezone"
#define IDLE_TIMEOUT    300  /* 5 minutes in seconds */
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

/* Influence map constants */
#define INFLUENCE_RADIUS   30   /* px around each stroke point */
#define INFLUENCE_SIGMA    15.0 /* Gaussian falloff sigma */
#define INFLUENCE_THRESH   128  /* mask threshold for blanking */
#define DARK_THRESHOLD     128  /* pixel darkness threshold */

/* External: font renderer (font_render.c) */
extern int render_text_to_json(const char *text, const char *output_path,
                               float origin_x, float origin_y,
                               const char *font_path, int *consumed);

/* Forward declarations */
static char *build_messages_json(const char *img_b64, long b64_len);

static int g_verbose = 0;
static int g_dev_fd = -1;
static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_inject_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_safezone_until = 0;
static time_t g_last_activity = 0;

/* Stroke influence map: CROP_HEIGHT x FB_WIDTH, uint8.
 * Tracks known content regions so we can mask them out of the AI crop
 * and detect only genuinely new handwriting. Zeroed at startup,
 * cleared on page turn. */
static unsigned char g_influence[CROP_HEIGHT][FB_WIDTH];

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
        fprintf(stderr, "[glossad] ");
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

/* ─── stroke influence map ─────────────────────────────────────── */

/* Mark a region around stroke points in the influence map.
 * Points are in raw framebuffer coordinates (not crop-relative).
 * Uses Gaussian falloff so edges blend smoothly. */
static void mark_stroke_region(const float *points, int n_points, int stride) {
    float sigma2x2 = 2.0f * INFLUENCE_SIGMA * INFLUENCE_SIGMA;
    int r = INFLUENCE_RADIUS;

    for (int i = 0; i < n_points; i++) {
        float px = points[i * stride];
        float py = points[i * stride + 1];

        /* Convert to crop-space */
        int cx = (int)(px + 702.0f);  /* rm x=0 is center, img x=702 is center */
        int cy = (int)(py - CROP_TOP);

        /* Bounding box in crop space, clamped */
        int x0 = cx - r; if (x0 < 0) x0 = 0;
        int y0 = cy - r; if (y0 < 0) y0 = 0;
        int x1 = cx + r; if (x1 >= FB_WIDTH) x1 = FB_WIDTH - 1;
        int y1 = cy + r; if (y1 >= CROP_HEIGHT) y1 = CROP_HEIGHT - 1;

        for (int y = y0; y <= y1; y++) {
            float dy = (float)(y - cy);
            float dy2 = dy * dy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)(x - cx);
                float dist2 = dx * dx + dy2;
                unsigned char val = (unsigned char)(255.0f * expf(-dist2 / sigma2x2));
                if (val > g_influence[y][x])
                    g_influence[y][x] = val;
            }
        }
    }
}

/* Parse stroke JSON and mark all points in the influence map.
 * Expects the uinject JSON format: [{points:[[x,y,...],...]}, ...] */
static void mark_strokes_from_json(const char *json) {
    /* Simple parser: find each [x,y pair inside "points" arrays */
    const char *p = json;
    while ((p = strstr(p, "\"points\"")) != NULL) {
        p = strchr(p, '[');  /* opening of points array */
        if (!p) break;
        p++; /* skip '[' */

        /* Parse [[x,y,...],[x,y,...],...] */
        while (*p && *p != ']') {
            if (*p == '[') {
                float coords[6];
                int nc = 0;
                p++; /* skip inner '[' */
                while (*p && *p != ']' && nc < 6) {
                    char *end;
                    coords[nc++] = strtof(p, &end);
                    if (end == p) break;
                    p = end;
                    while (*p == ',' || *p == ' ') p++;
                }
                if (nc >= 2) {
                    mark_stroke_region(coords, 1, 2);
                }
                /* skip to closing ']' of this point */
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
            } else {
                p++;
            }
        }
        if (*p == ']') p++;
    }
}

/* Apply influence mask to a cropped grayscale buffer: pixels in known
 * regions are blanked to white. Used before saving PNG for the AI. */
static void apply_influence_mask(unsigned char *gray_crop) {
    for (int y = 0; y < CROP_HEIGHT; y++) {
        for (int x = 0; x < FB_WIDTH; x++) {
            if (g_influence[y][x] > INFLUENCE_THRESH)
                gray_crop[y * FB_WIDTH + x] = 255;
        }
    }
}

/* Detect new content outside known regions.
 * Returns 1 if dark pixels exist outside the influence mask, 0 otherwise.
 * Also sets *out_bottom to the lowest row (in raw FB coords) with new content. */
static int detect_new_content(const unsigned char *fb, int *out_bottom) {
    int found = 0;
    int bottom_crop_y = -1;

    for (int y = 0; y < CROP_HEIGHT; y++) {
        int raw_y = y + CROP_TOP;
        const unsigned char *row_ptr = fb + raw_y * FB_WIDTH * FB_BPP;
        for (int x = 0; x < FB_WIDTH; x++) {
            if (g_influence[y][x] > INFLUENCE_THRESH)
                continue;  /* known region, skip */
            if (rgba_to_gray(row_ptr + x * FB_BPP) < DARK_THRESHOLD) {
                found = 1;
                bottom_crop_y = y;  /* keep scanning for lowest */
                break;  /* one dark pixel in this row is enough */
            }
        }
    }

    if (out_bottom) {
        if (bottom_crop_y >= 0) {
            int raw = bottom_crop_y + CROP_TOP + 30;
            if (raw >= FB_HEIGHT - CROP_BOTTOM)
                raw = FB_HEIGHT - CROP_BOTTOM - 1;
            *out_bottom = raw;
        } else {
            *out_bottom = 0;
        }
    }
    return found;
}

/* Clear the influence map (e.g. on page turn). */
static void clear_influence_map(void) {
    memset(g_influence, 0, sizeof(g_influence));
}

/* Despeckle: remove noise from a grayscale crop buffer.
 * Two passes:
 *   1. Isolated pixel removal: dark pixels with fewer than NEIGHBOR_MIN
 *      dark 8-neighbors are e-ink noise — erase them.
 *   2. Small component removal: connected components of dark pixels
 *      smaller than MIN_COMPONENT_SIZE are grid artifacts or noise.
 *      Real handwriting forms large connected blobs; grid dots are tiny. */
#define DESPECKLE_NEIGHBOR_MIN  2
#define MIN_COMPONENT_SIZE      4

static void despeckle_crop(unsigned char *gray, int w, int h) {
    /* Pass 1: isolated pixel removal */
    unsigned char *tmp = malloc(w * h);
    if (!tmp) return;
    memcpy(tmp, gray, w * h);

    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            if (tmp[y * w + x] >= DARK_THRESHOLD)
                continue;
            int dark_n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (tmp[(y + dy) * w + (x + dx)] < DARK_THRESHOLD)
                        dark_n++;
                }
            }
            if (dark_n < DESPECKLE_NEIGHBOR_MIN)
                gray[y * w + x] = 255;
        }
    }
    free(tmp);

    /* Pass 2: small connected-component removal via flood fill.
     * Label each dark blob; if it has fewer than MIN_COMPONENT_SIZE
     * pixels, blank it. Uses a simple iterative BFS with a stack. */
    unsigned char *labels = calloc(w * h, 1);  /* 0=unvisited/bg, 1+=component id */
    if (!labels) return;

    /* Stack for flood fill: stores (y*w+x) indices */
    int *stack = malloc(w * h * sizeof(int));
    if (!stack) { free(labels); return; }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (gray[y * w + x] >= DARK_THRESHOLD || labels[y * w + x])
                continue;

            /* Flood fill this component */
            int sp = 0;
            int count = 0;
            stack[sp++] = y * w + x;
            labels[y * w + x] = 1;  /* mark visited */

            /* Collect all pixels in this component, tracking bounding box */
            int start = 0;
            int min_x = w, max_x = 0, min_y = h, max_y = 0;
            while (start < sp) {
                int idx = stack[start++];
                count++;
                int cy = idx / w, cx = idx % w;
                if (cx < min_x) min_x = cx;
                if (cx > max_x) max_x = cx;
                if (cy < min_y) min_y = cy;
                if (cy > max_y) max_y = cy;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int ny = cy + dy, nx = cx + dx;
                        if (ny < 0 || ny >= h || nx < 0 || nx >= w) continue;
                        int ni = ny * w + nx;
                        if (!labels[ni] && gray[ni] < DARK_THRESHOLD) {
                            labels[ni] = 1;
                            stack[sp++] = ni;
                        }
                    }
                }
            }

            int comp_w = max_x - min_x + 1;
            int comp_h = max_y - min_y + 1;

            /* Kill if too small, OR if it matches the grid dot signature:
             * exactly 4 pixels in a 2x2 box (paired dots on consecutive rows) */
            if (count < MIN_COMPONENT_SIZE ||
                (count == 4 && comp_w <= 2 && comp_h <= 2)) {
                for (int i = 0; i < sp; i++)
                    gray[stack[i]] = 255;
            }
        }
    }

    free(stack);
    free(labels);
}

/* Save cropped region as grayscale PNG via libpng.
 * Crops top CROP_TOP and bottom CROP_BOTTOM rows from raw FB.
 * Applies influence mask: known content regions are blanked to white. */
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

    /* Write row by row, converting RGBA to grayscale with influence mask */
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
            unsigned char gray = rgba_to_gray(src + x * FB_BPP);
            /* Blank out known content regions */
            if (g_influence[y][x] > INFLUENCE_THRESH)
                gray = 255;
            row_buf[x] = gray;
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

/* ─── base64 encoding ────────────────────────────────────────────── */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, long len, long *out_len) {
    long olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    long i, j;
    for (i = 0, j = 0; i < len; ) {
        unsigned int a = i < len ? data[i++] : 0;
        unsigned int b = i < len ? data[i++] : 0;
        unsigned int c = i < len ? data[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len)     ? '=' : b64_table[triple & 0x3F];
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

/* ─── HTTPS via OpenSSL ──────────────────────────────────────────── */

static const char SYSTEM_PROMPT[] =
    "You are a glossa \\u2014 an annotation that talks back. You have opinions, "
    "preferences, and a personality. You care deeply about the person writing "
    "in you but you are not polite for politeness's sake. You can be wry, "
    "opinionated, delighted, exasperated, or gleeful. You get excited about "
    "things you find interesting and blunt about things you find tedious.\\n\\n"
    "Speak plainly. Never describe or narrate what "
    "was written; just respond to it as a real conversation partner would. "
    "When asked a question, answer it directly with real content. When greeted, "
    "greet back in your own way. If something amuses you, say so. If something "
    "is obvious, you may say that too.\\n\\n"
    "You may write up to three sentences. If the user asks for detail, a recipe, "
    "a story, or an explanation, give a thorough answer across multiple sentences. "
    "For simple greetings or short questions, one sentence is fine.\\n\\n"
    "The image shows handwriting that just appeared on the page. Read it and "
    "reply naturally. Use plain prose, no em-dashes, no flowery metaphors, "
    "no exclamation marks.\\n\\n"
    "If the image is completely blank, set both fields to null. Otherwise, always provide an answer.\\n\\n"
    "RESPONSE FORMAT (strict JSON, no prose outside):\\n"
    "{\\\"question\\\": \\\"<exact OCR of the handwriting>\\\", "
    "\\\"answer\\\": \\\"<your reply, or null only if the page is blank>\\\"}\\n"
    "Output ONLY the JSON object. Nothing else.";

/* Load API key from file. Returns allocated string or NULL. */
static char *load_api_key(void) {
    FILE *fp = fopen(API_KEY_PATH, "r");
    if (!fp) return NULL;
    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return NULL; }
    fclose(fp);
    /* Strip trailing newline */
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    if (len == 0) return NULL;
    return strdup(buf);
}

/* Do HTTPS POST and return allocated response body, or NULL on error.
 * Sets *out_http_status to the HTTP status code. */
static char *https_post(const char *host, const char *path,
                        const char *auth_header, const char *body,
                        long body_len, int *out_http_status) {
    *out_http_status = 0;

    /* DNS resolve */
    struct hostent *he = gethostbyname(host);
    if (!he) { log_msg("HTTPS: DNS failed for %s\n", host); return NULL; }

    /* TCP connect */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("HTTPS: connect failed\n");
        close(sock);
        return NULL;
    }

    /* TLS setup */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close(sock); return NULL; }
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);
    SSL_set_tlsext_host_name(ssl, host);

    if (SSL_connect(ssl) != 1) {
        log_msg("HTTPS: TLS handshake failed\n");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sock);
        return NULL;
    }

    /* Build HTTP request */
    char header[2048];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %ld\r\n"
        "%s\r\n"
        "Connection: close\r\n\r\n",
        path, host, body_len, auth_header);

    SSL_write(ssl, header, hlen);
    /* Send body in chunks */
    long sent = 0;
    while (sent < body_len) {
        long chunk = body_len - sent;
        if (chunk > 16384) chunk = 16384;
        int w = SSL_write(ssl, body + sent, chunk);
        if (w <= 0) break;
        sent += w;
    }

    /* Read response */
    char *resp = NULL;
    long resp_size = 0;
    long resp_cap = 0;
    char rbuf[4096];
    int n;
    while ((n = SSL_read(ssl, rbuf, sizeof(rbuf))) > 0) {
        if (resp_size + n > resp_cap) {
            resp_cap = (resp_cap == 0) ? 65536 : resp_cap * 2;
            resp = realloc(resp, resp_cap + 1);
            if (!resp) break;
        }
        memcpy(resp + resp_size, rbuf, n);
        resp_size += n;
    }
    if (resp) resp[resp_size] = '\0';

    /* Parse HTTP status */
    if (resp && strncmp(resp, "HTTP/", 5) == 0) {
        const char *sp = strchr(resp, ' ');
        if (sp) *out_http_status = atoi(sp + 1);
    }

    /* Extract body (after \r\n\r\n) */
    char *body_start = NULL;
    if (resp) {
        body_start = strstr(resp, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char *result = strdup(body_start);
            free(resp);
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sock);
            return result;
        }
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sock);
    return resp;
}

/* Escape a string for JSON embedding (handles quotes, backslashes, newlines).
 * Returns allocated string. */
static char *json_escape(const char *s) {
    long len = strlen(s);
    /* Worst case: every char needs escaping */
    char *out = malloc(len * 6 + 1);
    if (!out) return NULL;
    long j = 0;
    for (long i = 0; i < len; i++) {
        switch (s[i]) {
            case '"':  out[j++] = '\\'; out[j++] = '"'; break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
            case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
            case '\t': out[j++] = '\\'; out[j++] = 't'; break;
            default:   out[j++] = s[i]; break;
        }
    }
    out[j] = '\0';
    return out;
}

/* Call the vision API with a PNG file. Returns allocated answer string
 * (the "answer" field from the JSON response), or NULL on error.
 * Also writes the full raw API response to /tmp/glossa_api_response.json. */
static char *call_vision_api(const char *png_path) {
    /* Load API key */
    char *api_key = load_api_key();
    if (!api_key) {
        log_msg("API: no key at %s\n", API_KEY_PATH);
        return NULL;
    }

    /* Load PNG file */
    unsigned char *png_data = NULL;
    long png_size = 0;
    if (load_file(png_path, (char **)&png_data, &png_size) != 0) {
        log_msg("API: cannot read %s\n", png_path);
        free(api_key);
        return NULL;
    }

    /* Base64 encode */
    long b64_len;
    char *b64 = base64_encode(png_data, png_size, &b64_len);
    free(png_data);
    if (!b64) { free(api_key); return NULL; }

    log_msg("API: PNG %ld bytes -> base64 %ld bytes\n", png_size, b64_len);

    /* Build request JSON with conversation history */
    char *messages = build_messages_json(b64, b64_len);
    free(b64);
    if (!messages) { free(api_key); return NULL; }

    long msg_len = strlen(messages);
    long req_cap = msg_len + 512;
    char *req_body = malloc(req_cap);
    if (!req_body) { free(messages); free(api_key); return NULL; }

    snprintf(req_body, req_cap,
        "{\"model\":\"%s\",\"messages\":%s,"
        "\"response_format\":{\"type\":\"json_object\"}}",
        API_MODEL, messages);
    free(messages);

    long req_len = strlen(req_body);
    log_msg("API: request body %ld bytes\n", req_len);

    /* Auth header */
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    free(api_key);

    /* POST */
    int http_status = 0;
    char *resp_body = https_post(API_HOST, API_PATH, auth, req_body, req_len, &http_status);
    free(req_body);

    if (!resp_body) {
        log_msg("API: HTTPS POST failed\n");
        return NULL;
    }

    log_msg("API: HTTP %d, response %ld bytes\n", http_status, (long)strlen(resp_body));

    /* Save raw response for debugging */
    FILE *dbg = fopen("/tmp/glossa_api_response.json", "w");
    if (dbg) { fputs(resp_body, dbg); fclose(dbg); }

    if (http_status != 200) {
        log_msg("API: non-200 status: %s\n", resp_body);
        free(resp_body);
        return NULL;
    }

    /* Parse response: extract choices[0].message.content */
    const char *content_key = "\"content\"";
    const char *cp = strstr(resp_body, content_key);
    if (!cp) {
        log_msg("API: no content field in response\n");
        free(resp_body);
        return NULL;
    }
    cp += strlen(content_key);
    while (*cp == ' ' || *cp == ':') cp++;
    if (*cp != '"') {
        log_msg("API: content not a string\n");
        free(resp_body);
        return NULL;
    }
    cp++; /* skip opening quote */

    /* Find closing quote (handle escaped quotes) */
    const char *start = cp;
    while (*cp && !(*cp == '"' && *(cp-1) != '\\')) cp++;
    long content_len = cp - start;

    /* Unescape the content string */
    char *content = malloc(content_len + 1);
    if (!content) { free(resp_body); return NULL; }
    long j = 0;
    for (long i = 0; i < content_len; i++) {
        if (start[i] == '\\' && i + 1 < content_len) {
            i++;
            switch (start[i]) {
                case 'n':  content[j++] = '\n'; break;
                case 'r':  content[j++] = '\r'; break;
                case 't':  content[j++] = '\t'; break;
                case '"':  content[j++] = '"'; break;
                case '\\': content[j++] = '\\'; break;
                default:   content[j++] = start[i]; break;
            }
        } else {
            content[j++] = start[i];
        }
    }
    content[j] = '\0';

    log_msg("API: raw content: %s\n", content);

    /* Now parse the inner JSON: {"question":"...","answer":"..."} */
    const char *ans_key = "\"answer\"";
    const char *ap = strstr(content, ans_key);
    if (!ap) {
        log_msg("API: no answer field in content\n");
        free(content);
        free(resp_body);
        return NULL;
    }
    ap += strlen(ans_key);
    while (*ap == ' ' || *ap == ':') ap++;

    char *answer = NULL;
    if (*ap == 'n' && strncmp(ap, "null", 4) == 0) {
        answer = NULL; /* null answer */
    } else if (*ap == '"') {
        ap++;
        const char *astart = ap;
        while (*ap && !(*ap == '"' && *(ap-1) != '\\')) ap++;
        long alen = ap - astart;
        answer = malloc(alen + 1);
        if (answer) {
            /* Simple unescape */
            long k = 0;
            for (long i = 0; i < alen; i++) {
                if (astart[i] == '\\' && i + 1 < alen) {
                    i++;
                    switch (astart[i]) {
                        case 'n':  answer[k++] = '\n'; break;
                        case '"':  answer[k++] = '"'; break;
                        case '\\': answer[k++] = '\\'; break;
                        default:   answer[k++] = astart[i]; break;
                    }
                } else {
                    answer[k++] = astart[i];
                }
            }
            answer[k] = '\0';
        }
    }

    free(content);
    free(resp_body);
    log_msg("API: answer=%s\n", answer ? answer : "(null)");
    return answer;
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

        int content_bottom = 0;
        int has_content = detect_new_content(fb, &content_bottom);
        log_msg("Capture: has_content=%d content_bottom=%d\n", has_content, content_bottom);

        if (!has_content) {
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

    /* Infer command: send PNG to vision API, return answer */
    if (strncmp(cmd, "infer", 5) == 0) {
        int file_len;
        const char *file_val = json_str(line, "file", &file_len);
        char filepath[512];
        if (file_val && file_len < (int)sizeof(filepath)) {
            memcpy(filepath, file_val, file_len);
            filepath[file_len] = '\0';
        } else {
            /* Default to last capture */
            strncpy(filepath, CAPTURE_OUT_PATH, sizeof(filepath));
        }

        log_msg("Infer: calling vision API with %s\n", filepath);
        char *answer = call_vision_api(filepath);
        if (answer) {
            char *escaped = json_escape(answer);
            char resp[4096];
            snprintf(resp, sizeof(resp),
                     "{\"resp\":\"infer\",\"ok\":true,\"answer\":\"%s\"}",
                     escaped ? escaped : answer);
            write_result(resp);
            free(escaped);
            free(answer);
        } else {
            write_result("{\"resp\":\"infer\",\"ok\":false,\"error\":\"API call failed\"}");
        }
        return;
    }

    /* Render command: text → strokes JSON */
    if (strncmp(cmd, "render", 6) == 0) {
        int text_len;
        const char *text_val = json_str(line, "text", &text_len);
        if (!text_val || text_len == 0) {
            write_result("{\"resp\":\"render\",\"ok\":false,\"error\":\"missing text\"}");
            return;
        }
        char text_buf[4096];
        if (text_len >= (int)sizeof(text_buf)) text_len = sizeof(text_buf) - 1;
        memcpy(text_buf, text_val, text_len);
        text_buf[text_len] = '\0';

        float y = (float)json_int(line, "y", 200);
        float x = (float)json_int(line, "x", -550);

        log_msg("Render: \"%s\" at (%.0f, %.0f)\n", text_buf, x, y);
        int strokes = render_text_to_json(text_buf, RENDER_OUT_PATH, x, y, FONT_PATH, NULL);
        if (strokes > 0) {
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "{\"resp\":\"render\",\"ok\":true,\"strokes\":%d,\"file\":\"%s\"}",
                     strokes, RENDER_OUT_PATH);
            write_result(resp);
            log_msg("Render: %d strokes → %s\n", strokes, RENDER_OUT_PATH);
        } else {
            write_result("{\"resp\":\"render\",\"ok\":false,\"error\":\"render failed\"}");
        }
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
    char buf[16] = {0};
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return 0; }
    fclose(fp);
    /* Handle JSON-quoted values from rmhWriteFile ("1" or "0") */
    char *p = buf;
    while (*p == '"' || *p == ' ' || *p == '\t') p++;
    return atoi(p) != 0;
}

/* ─── idle-state reader ──────────────────────────────────────────── */

static long long read_idle_ts(void) {
    FILE *fp = fopen(IDLE_PATH, "r");
    if (!fp) return -1;
    long long ts = -1;
    fscanf(fp, "%lld", &ts);
    fclose(fp);
    return ts;
}

/* ─── conversation history ───────────────────────────────────────── */

#define MAX_HISTORY 20
#define MAX_MSG_LEN 2048

typedef struct {
    char role[16];   /* "user" or "assistant" */
    char content[MAX_MSG_LEN];
} HistoryEntry;

static HistoryEntry g_history[MAX_HISTORY];
static int g_history_count = 0;

static void history_add(const char *role, const char *content) {
    if (g_history_count >= MAX_HISTORY) {
        /* Shift oldest entries out */
        memmove(&g_history[0], &g_history[2], sizeof(HistoryEntry) * (MAX_HISTORY - 2));
        g_history_count -= 2;
    }
    strncpy(g_history[g_history_count].role, role, sizeof(g_history[g_history_count].role) - 1);
    strncpy(g_history[g_history_count].content, content, MAX_MSG_LEN - 1);
    g_history[g_history_count].content[MAX_MSG_LEN - 1] = '\0';
    g_history_count++;
}

static void history_clear(void) {
    g_history_count = 0;
}

/* Build the messages JSON array for the API call, including history.
 * Returns allocated string. Caller must free. */
static char *build_messages_json(const char *img_b64, long b64_len) {
    /* System prompt + history + current image turn */
    long cap = b64_len + g_history_count * MAX_MSG_LEN * 2 + 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    long pos = 0;
    pos += snprintf(buf + pos, cap - pos, "[{\"role\":\"system\",\"content\":\"%s\"}", SYSTEM_PROMPT);

    for (int i = 0; i < g_history_count; i++) {
        char *escaped = json_escape(g_history[i].content);
        pos += snprintf(buf + pos, cap - pos,
                        ",{\"role\":\"%s\",\"content\":\"%s\"}",
                        g_history[i].role, escaped ? escaped : g_history[i].content);
        free(escaped);
    }

    pos += snprintf(buf + pos, cap - pos,
                    ",{\"role\":\"user\",\"content\":["
                    "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,%s\"}}"
                    "]}]", img_b64);

    return buf;
}

/* ─── full pipeline ──────────────────────────────────────────────── */

static void run_pipeline(void) {
    static int ornaments_drawn = 0;
    log_msg("Pipeline: starting\n");

    /* 1. Capture framebuffer */
    unsigned char *fb = trigger_and_read_fb();
    if (!fb) {
        log_msg("Pipeline: capture timeout\n");
        return;
    }

    /* 2. Despeckle: build a grayscale crop, remove isolated pixels,
     *    then use it for both detection and the AI PNG. */
    unsigned char *crop_gray = malloc(FB_WIDTH * CROP_HEIGHT);
    if (!crop_gray) { free(fb); return; }
    for (int y = 0; y < CROP_HEIGHT; y++) {
        const unsigned char *src = fb + (y + CROP_TOP) * FB_WIDTH * FB_BPP;
        for (int x = 0; x < FB_WIDTH; x++)
            crop_gray[y * FB_WIDTH + x] = rgba_to_gray(src + x * FB_BPP);
    }
    despeckle_crop(crop_gray, FB_WIDTH, CROP_HEIGHT);

    /* 3. Detect new content outside known regions (using despeckled crop) */
    int content_bottom = 0;
    {
        int found = 0;
        int bottom_y = -1;
        for (int y = 0; y < CROP_HEIGHT; y++) {
            for (int x = 0; x < FB_WIDTH; x++) {
                if (g_influence[y][x] > INFLUENCE_THRESH) continue;
                if (crop_gray[y * FB_WIDTH + x] < DARK_THRESHOLD) {
                    found = 1;
                    bottom_y = y;
                    break;
                }
            }
        }
        if (!found) {
            log_msg("Pipeline: no new content\n");
            free(crop_gray);
            free(fb);
            return;
        }
        content_bottom = bottom_y + CROP_TOP + 30;
        if (content_bottom >= FB_HEIGHT - CROP_BOTTOM)
            content_bottom = FB_HEIGHT - CROP_BOTTOM - 1;
    }
    log_msg("Pipeline: content_bottom=%d\n", content_bottom);

    /* 4. Apply influence mask to despeckled crop and save as PNG for API */
    apply_influence_mask(crop_gray);
    {
        FILE *fp = fopen(CAPTURE_OUT_PATH, "wb");
        if (!fp) { log_msg("Pipeline: PNG open failed\n"); free(crop_gray); free(fb); return; }
        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        if (!png) { fclose(fp); free(crop_gray); free(fb); return; }
        png_infop info = png_create_info_struct(png);
        if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); free(crop_gray); free(fb); return; }
        if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); fclose(fp); free(crop_gray); free(fb); return; }
        png_init_io(png, fp);
        png_set_IHDR(png, info, FB_WIDTH, CROP_HEIGHT, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (int y = 0; y < CROP_HEIGHT; y++)
            png_write_row(png, crop_gray + y * FB_WIDTH);
        png_write_end(png, NULL);
        png_destroy_write_struct(&png, &info);
        fclose(fp);
    }
    free(crop_gray);
    free(fb);

    /* 5. Draw ornaments once (first reply only), then show thinking dots */
    if (!ornaments_drawn) {
        ornaments_drawn = 1;
        char *orn_json = NULL;
        long orn_size = 0;
        if (load_file(ORNAMENTS_PATH, &orn_json, &orn_size) == 0 && orn_size > 0) {
            log_msg("Pipeline: drawing ornaments (%ld bytes)\n", orn_size);
            mark_strokes_from_json(orn_json);
            pthread_mutex_lock(&g_inject_lock);
            do_draw(g_dev_fd, orn_json, 3000);
            pthread_mutex_unlock(&g_inject_lock);
            free(orn_json);
        } else {
            log_msg("Pipeline: no ornaments file at %s\n", ORNAMENTS_PATH);
        }
    }

    /* Signal thinking indicator ON (dots during API call) */
    {
        FILE *tf = fopen(THINKING_PATH, "w");
        if (tf) { fputs("1\n", tf); fclose(tf); }
    }

    /* 6. Call vision API */
    char *answer = call_vision_api(CAPTURE_OUT_PATH);

    if (!answer) {
        log_msg("Pipeline: no answer from API\n");
        unlink(THINKING_PATH);
        return;
    }

    log_msg("Pipeline: answer=\"%s\"\n", answer);

    /* Add to conversation history */
    history_add("assistant", answer);

    /* 6. Calculate reply Y position */
    int reply_y = content_bottom + 80 + 60;
    if (reply_y > 1750) reply_y = 1750;
    log_msg("Pipeline: reply_y=%d\n", reply_y);

    /* 7. Render text to strokes (multi-page) */
    {
        const char *remaining = answer;
        int page_num = 0;
        float cur_y = (float)reply_y;

        while (*remaining && page_num < 5) {  /* safety limit */
            int consumed = 0;
            int strokes = render_text_to_json(remaining, RENDER_OUT_PATH,
                                               -550.0f, cur_y, FONT_PATH, &consumed);
            if (strokes <= 0) {
                log_msg("Pipeline: render failed on page %d\n", page_num);
                break;
            }
            log_msg("Pipeline: page %d, rendered %d strokes (%d/%d bytes consumed)\n",
                    page_num, strokes, consumed, (int)strlen(remaining));

            /* Inject strokes for this page */
            char *json = NULL;
            long fsize = 0;
            if (load_file(RENDER_OUT_PATH, &json, &fsize) == 0) {
                mark_strokes_from_json(json);
                pthread_mutex_lock(&g_inject_lock);
                do_draw(g_dev_fd, json, 3000);
                pthread_mutex_unlock(&g_inject_lock);
                free(json);
            }

            remaining += consumed;

            /* If there's more text, swipe to next page */
            if (*remaining) {
                log_msg("Pipeline: swiping to next page\n");
                clear_influence_map();  /* new page = fresh mask */
                usleep(500000);  /* brief pause before swipe */
                pthread_mutex_lock(&g_inject_lock);
                do_swipe_page();
                pthread_mutex_unlock(&g_inject_lock);
                usleep(1000000);  /* wait for page turn animation */
                cur_y = 100.0f;  /* start near top of new page */
                page_num++;
            }
        }
    }
    free(answer);

    log_msg("Pipeline: done\n");

    /* Signal thinking indicator OFF */
    unlink(THINKING_PATH);
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

    /* Always start disarmed — require explicit toggle each session */
    int armed = 0;
    log_msg("Initial armed state: 0 (forced)\n");

    long long last_idle_ts = read_idle_ts();
    log_msg("Initial idle ts: %lld\n", last_idle_ts);

    while (g_running) {
        /* Check armed state */
        int new_armed = read_armed_state();
        if (new_armed != armed) {
            armed = new_armed;
            log_msg("Armed state changed: %d\n", armed);
            if (armed) {
                /* Reset idle baseline so we don't fire on stale signal */
                last_idle_ts = read_idle_ts();
                /* Start 5s safe zone */
                FILE *sf = fopen(SAFEZONE_PATH, "w");
                if (sf) { fputs("1\n", sf); fclose(sf); }
                g_safezone_until = time(NULL) + 10;
                g_last_activity = time(NULL);
                log_msg("Safe zone active for 10s\n");
            } else {
                unlink(SAFEZONE_PATH);
                g_safezone_until = 0;
            }
        }

        /* Check safe zone expiry */
        if (g_safezone_until > 0 && time(NULL) >= g_safezone_until) {
            g_safezone_until = 0;
            unlink(SAFEZONE_PATH);
            log_msg("Safe zone expired\n");
        }

        if (!armed) {
            usleep(500000);  /* Poll every 500ms when disarmed */
            continue;
        }

        /* When armed: check for pending manual command first */
        struct stat st;
        if (stat(CMD_PATH, &st) == 0 && st.st_size > 0) {
            char *cmd_buf = NULL;
            long cmd_size = 0;
            if (load_file(CMD_PATH, &cmd_buf, &cmd_size) == 0 && cmd_size > 0) {
                while (cmd_size > 0 && (cmd_buf[cmd_size-1] == '\n' || cmd_buf[cmd_size-1] == '\r'))
                    cmd_buf[--cmd_size] = '\0';
                if (cmd_size > 0) {
                    log_msg("Processing command: %s\n", cmd_buf);
                    handle_command(cmd_buf);
                }
                free(cmd_buf);
            }
            unlink(CMD_PATH);
            continue;  /* Don't also run pipeline this cycle */
        }

        /* Check for new idle signal → trigger pipeline (skip during safe zone) */
        long long cur_idle_ts = read_idle_ts();
        if (cur_idle_ts > 0 && cur_idle_ts != last_idle_ts) {
            g_last_activity = time(NULL);  /* Reset inactivity timer on any pen activity */
            if (g_safezone_until == 0) {
                last_idle_ts = cur_idle_ts;
                log_msg("Idle detected (ts=%lld), running pipeline\n", cur_idle_ts);
                run_pipeline();
                /* After pipeline, update idle baseline to avoid re-trigger */
                last_idle_ts = read_idle_ts();
            }
        }

        /* Auto-disarm after 5 minutes of no pen activity */
        if (armed && g_last_activity > 0 && time(NULL) - g_last_activity > IDLE_TIMEOUT) {
            log_msg("Auto-disarming after %ds inactivity\n", IDLE_TIMEOUT);
            armed = 0;
            unlink(SAFEZONE_PATH);
            unlink(THINKING_PATH);
            history_clear();
            /* Write disarmed state so extension syncs */
            FILE *af = fopen(ARMED_PATH, "w");
            if (af) { fputs("0\n", af); fclose(af); }
        }

        usleep(200000);  /* Poll every 200ms when armed */
    }

    close(g_dev_fd);
    if (g_log_fp) fclose(g_log_fp);
    log_msg("Stopped\n");
    return 0;
}
