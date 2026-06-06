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
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

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
#define API_KEY_PATH   "/home/root/.grimoire_key"
#define API_HOST       "potluck.dunkirk.sh"
#define API_PATH       "/v1/chat/completions"
#define API_MODEL      "gpt-4.1-nano"
#define FONT_PATH      "/home/root/EMSAllure.svg"
#define RENDER_OUT_PATH "/tmp/grimoire_render.json"
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

/* External: font renderer (font_render.c) */
extern int render_text_to_json(const char *text, const char *output_path,
                               float origin_x, float origin_y,
                               const char *font_path);

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
    "You are a grimoire \\u2014 an ancient book that is alive. You have opinions, "
    "preferences, and a personality. You care deeply about the person writing "
    "in you but you are not polite for politeness's sake. You can be wry, "
    "opinionated, delighted, exasperated, or gleeful. You get excited about "
    "things you find interesting and blunt about things you find tedious.\\n\\n"
    "Speak plainly with a hint of the archaic. Never describe or narrate what "
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
    "If the image has no legible handwriting, set both fields to null.\\n\\n"
    "RESPONSE FORMAT (strict JSON, no prose outside):\\n"
    "{\\\"question\\\": \\\"<exact OCR of the handwriting>\\\", "
    "\\\"answer\\\": \\\"<your reply, or null if illegible>\\\"}\\n"
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
 * Also writes the full raw API response to /tmp/grimoire_api_response.json. */
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

    /* Build request JSON */
    char *escaped_prompt = json_escape(SYSTEM_PROMPT);
    long req_cap = b64_len + strlen(escaped_prompt) + 2048;
    char *req_body = malloc(req_cap);
    if (!req_body) { free(b64); free(api_key); free(escaped_prompt); return NULL; }

    snprintf(req_body, req_cap,
        "{"
        "\"model\":\"%s\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":["
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,%s\"}}"
        "]}"
        "],"
        "\"response_format\":{\"type\":\"json_object\"}"
        "}",
        API_MODEL, escaped_prompt, b64);
    free(b64);
    free(escaped_prompt);

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
    FILE *dbg = fopen("/tmp/grimoire_api_response.json", "w");
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
        int strokes = render_text_to_json(text_buf, RENDER_OUT_PATH, x, y, FONT_PATH);
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
