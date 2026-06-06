/* uinjectd.c — Persistent injection + event daemon for grimoire
 *
 * Runs ON the reMarkable.  Listens on a TCP socket; the host connects
 * once and keeps the connection open for the whole session.  This kills
 * the per-call SSH handshake for injection AND the 500ms idle polling.
 *
 * Two directions over one socket (newline-delimited JSON):
 *
 *   Host -> daemon (commands):
 *     {"cmd":"draw","file":"/tmp/grimoire_strokes.json","speed":3}
 *     {"cmd":"erase","file":"/tmp/grimoire_thinking_live.json","speed":20}
 *     {"cmd":"ping"}
 *
 *   Daemon -> host (responses + pushed events):
 *     {"resp":"draw","ok":true,"strokes":40}
 *     {"resp":"erase","ok":true,"points":36}
 *     {"resp":"ping","ok":true}
 *     {"event":"idle","ts":172...}            <- pushed, no request
 *
 * The daemon watches /tmp/grimoire_idle (written by the xovi extension)
 * and pushes an "idle" event whenever it changes.  The host no longer
 * polls.
 *
 * Usage: uinjectd [-v] [-p PORT]   (default port 9999)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

#define DEV_PATH   "/dev/input/event1"
#define TOUCH_PATH "/dev/input/event2"
#define IDLE_PATH  "/tmp/grimoire_idle"
#define DEFAULT_PORT 9999
#define MAX_CMD    4096

static int g_verbose = 0;
static int g_dev_fd = -1;
static int g_client_fd = -1;
static pthread_mutex_t g_inject_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;

#define vlog(...) do { if (g_verbose) fprintf(stderr, "[uinjectd] " __VA_ARGS__); } while (0)

/* ─── evdev helpers ──────────────────────────────────────────────── */

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

/* ─── minimal JSON parsing (flat objects only) ───────────────────── */

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

/* ─── socket write (thread-safe, newline-framed) ─────────────────── */

static void send_line(const char *msg) {
    pthread_mutex_lock(&g_write_lock);
    int fd = g_client_fd;
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        write(fd, "\n", 1);
    }
    pthread_mutex_unlock(&g_write_lock);
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

/* ─── injection ──────────────────────────────────────────────────── */

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

        emit_event(fd, EV_ABS, ABS_PRESSURE, 0);
        emit_event(fd, EV_ABS, ABS_DISTANCE, 86);
        emit_syn(fd);
        emit_event(fd, EV_KEY, BTN_TOUCH, 0);
        emit_event(fd, EV_KEY, BTN_TOOL_PEN, 0);
        emit_syn(fd);

        stroke_count++;
        vlog("Stroke %d: %d points\n", stroke_count, point_count);
        usleep(50000);
    }
    return stroke_count;
}

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
        vlog("Erase pass %d/%d: %d points\n", pass+1, num_passes, pass_emitted);
    }
    return total_emitted;
}

/* ─── page swipe (touchscreen MT-B) ─────────────────────────────── */

static int do_swipe_page(void) {
    int fd = open(TOUCH_PATH, O_WRONLY);
    if (fd < 0) { vlog("swipe: open %s failed\n", TOUCH_PATH); return -1; }

    /* Right-to-left swipe across bottom third of screen (next page).
     * Touch coords match display: X 0-1403, Y 0-1871.
     * Swipe from (1200, 1400) to (200, 1400) in ~20 steps. */
    const int x0 = 1200, x1 = 200, y = 1400;
    const int steps = 20;
    const int step_delay = 8000;  /* 8ms per step = ~160ms total */

    /* Slot 0, tracking ID 1 */
    emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x0);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit_event(fd, EV_ABS, ABS_MT_PRESSURE, 128);
    emit_syn(fd);
    usleep(step_delay);

    for (int i = 1; i <= steps; i++) {
        int cx = x0 + (x1 - x0) * i / steps;
        emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
        emit_event(fd, EV_ABS, ABS_MT_POSITION_X, cx);
        emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
        emit_syn(fd);
        usleep(step_delay);
    }

    /* Release: tracking ID = -1 */
    emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit_syn(fd);

    close(fd);
    vlog("Swipe page done\n");
    return 0;
}

/* ─── command handling ───────────────────────────────────────────── */

static void handle_command(const char *line) {
    int cmd_len;
    const char *cmd = json_str(line, "cmd", &cmd_len);
    if (!cmd) {
        send_line("{\"resp\":\"?\",\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }

    if (strncmp(cmd, "ping", 4) == 0) {
        send_line("{\"resp\":\"ping\",\"ok\":true}");
        return;
    }

    if (strncmp(cmd, "swipe", 5) == 0) {
        pthread_mutex_lock(&g_inject_lock);
        int ret = do_swipe_page();
        pthread_mutex_unlock(&g_inject_lock);
        if (ret == 0)
            send_line("{\"resp\":\"swipe\",\"ok\":true}");
        else
            send_line("{\"resp\":\"swipe\",\"ok\":false,\"error\":\"touch open failed\"}");
        return;
    }

    /* Capture: trigger screenshot, wait for FB dump, stream raw bytes */
    if (strncmp(cmd, "capture", 7) == 0) {
        /* Trigger the xovi watch thread to dump the framebuffer */
        unlink("/tmp/grimoire_fb.raw");
        int tf = open("/tmp/grimoire_screenshot", O_CREAT|O_WRONLY, 0644);
        if (tf >= 0) close(tf);

        /* Wait for the dump (watch thread checks every ~1s worst case) */
        const long expected_size = 1404L * 1872 * 4;
        char *fb = NULL;
        long fb_size = 0;
        for (int i = 0; i < 20; i++) {
            usleep(100000);  /* 100ms polls */
            FILE *fp = fopen("/tmp/grimoire_fb.raw", "r");
            if (!fp) continue;
            fseek(fp, 0, SEEK_END);
            fb_size = ftell(fp);
            if (fb_size >= expected_size) {
                fseek(fp, 0, SEEK_SET);
                fb = malloc(fb_size);
                if (fb) fread(fb, 1, fb_size, fp);
                fclose(fp);
                break;
            }
            fclose(fp);
        }

        if (!fb || fb_size < expected_size) {
            send_line("{\"resp\":\"capture\",\"ok\":false,\"error\":\"timeout\"}");
            free(fb);
            return;
        }

        /* Send JSON header with size, then raw bytes */
        char hdr[128];
        snprintf(hdr, sizeof(hdr),
                 "{\"resp\":\"capture\",\"ok\":true,\"size\":%ld}", fb_size);
        pthread_mutex_lock(&g_write_lock);
        int cfd = g_client_fd;
        if (cfd >= 0) {
            write(cfd, hdr, strlen(hdr));
            write(cfd, "\n", 1);
            /* Stream raw bytes in chunks */
            long sent = 0;
            while (sent < fb_size) {
                long chunk = fb_size - sent;
                if (chunk > 65536) chunk = 65536;
                ssize_t w = write(cfd, fb + sent, chunk);
                if (w <= 0) break;
                sent += w;
            }
        }
        pthread_mutex_unlock(&g_write_lock);
        free(fb);
        vlog("Captured and streamed %ld bytes\n", fb_size);
        return;
    }

    int file_len;
    const char *file_val = json_str(line, "file", &file_len);
    if (!file_val) {
        send_line("{\"resp\":\"?\",\"ok\":false,\"error\":\"missing file\"}");
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
        send_line(err);
        return;
    }

    vlog("Loaded %ld bytes from %s (speed=%dms)\n", fsize, filepath, speed_ms);

    char resp[128];
    /* Serialize all pen access: only one stroke job touches the
     * digitizer at a time, so concurrent draw/erase can't interleave. */
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
    send_line(resp);
}

/* ─── idle-event watcher thread ──────────────────────────────────── */

static void *idle_watch_thread(void *arg) {
    (void)arg;
    long long last_ts = -1;

    while (g_client_fd >= 0) {
        FILE *fp = fopen(IDLE_PATH, "r");
        if (fp) {
            long long ts = 0;
            if (fscanf(fp, "%lld", &ts) == 1 && ts != last_ts) {
                last_ts = ts;
                char evt[64];
                snprintf(evt, sizeof(evt), "{\"event\":\"idle\",\"ts\":%lld}", ts);
                send_line(evt);
                vlog("Pushed idle event ts=%lld\n", ts);
            }
            fclose(fp);
        }
        usleep(200000);  /* poll the local file every 200ms (no network) */
    }
    return NULL;
}

/* ─── client session ─────────────────────────────────────────────── */

static void serve_client(int client_fd) {
    g_client_fd = client_fd;

    /* Disable Nagle so small JSON commands go out immediately */
    int one = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Spawn idle watcher for this session */
    pthread_t watcher;
    pthread_create(&watcher, NULL, idle_watch_thread, NULL);
    pthread_detach(watcher);

    send_line("{\"event\":\"ready\"}");
    vlog("Client connected\n");

    char buf[MAX_CMD];
    char line[MAX_CMD];
    int line_len = 0;

    while (1) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n <= 0) break;  /* client disconnected */

        /* Accumulate into lines (newline-delimited framing) */
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) handle_command(line);
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = buf[i];
            }
        }
    }

    g_client_fd = -1;
    close(client_fd);
    vlog("Client disconnected\n");
}

/* ─── main ───────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            g_verbose = 1;
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    signal(SIGPIPE, SIG_IGN);

    g_dev_fd = open(DEV_PATH, O_WRONLY);
    if (g_dev_fd < 0) {
        perror("open " DEV_PATH);
        return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 1) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "[uinjectd] Listening on :%d (dev=%s%s)\n",
            port, DEV_PATH, g_verbose ? " verbose" : "");

    /* Accept one client at a time; reconnect loop for robustness */
    while (1) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int client_fd = accept(srv, (struct sockaddr *)&cli, &clen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        serve_client(client_fd);
    }

    close(srv);
    close(g_dev_fd);
    return 0;
}
