/* font_render.c — EMSAllure SVG single-stroke font renderer for grimoired
 *
 * Ports the Python grimoire.py text_to_strokes + strokes_to_json to C.
 * Parses the SVG font file, renders text as stroke JSON for uinject.
 *
 * Key facts (from CRUSH.md):
 * - Origin is TOP-CENTER on reMarkable
 * - SVG uses Y-up, rm uses Y-down: sy = -gy * scale
 * - All paths are M/L only (no curves)
 * - Densify with min_seg_len=300 for device compatibility
 * - Point format: [x, y, speed, width, direction, pressure]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ─── constants (match Python) ───────────────────────────────────── */

#define DEFAULT_SCALE     0.06f
#define DEFAULT_X        -550.0f
#define DEFAULT_Y         200.0f
#define LINE_HEIGHT       1.4f
#define MAX_LINE_WIDTH    1100.0f
#define GLYPH_HEIGHT      800.0f
#define DENSIFY_MIN_SEG   300.0f
#define MAX_GLYPHS        256
#define MAX_POLYLINES     16
#define MAX_POINTS_PER_PL 256

/* ─── data structures ────────────────────────────────────────────── */

typedef struct {
    float x, y;
} Point2D;

typedef struct {
    Point2D pts[MAX_POINTS_PER_PL];
    int count;
} Polyline;

typedef struct {
    Polyline polylines[MAX_POLYLINES];
    int num_polylines;
    float advance;
} Glyph;

typedef struct {
    Glyph glyphs[MAX_GLYPHS];
    float advances[MAX_GLYPHS];
    int loaded;
} FontData;

/* Simple LCG RNG for deterministic jitter (matches Python seeding) */
typedef struct {
    unsigned long state;
} RNG;

static void rng_seed(RNG *rng, const char *seed_str) {
    /* Simple hash of seed string */
    unsigned long h = 5381;
    while (*seed_str) {
        h = ((h << 5) + h) + (unsigned char)*seed_str++;
    }
    rng->state = h;
}

static float rng_uniform(RNG *rng, float lo, float hi) {
    rng->state = rng->state * 1103515245 + 12345;
    float t = (float)((rng->state >> 16) & 0x7FFF) / 32767.0f;
    return lo + t * (hi - lo);
}

static int rng_randint(RNG *rng, int lo, int hi) {
    rng->state = rng->state * 1103515245 + 12345;
    int range = hi - lo + 1;
    return lo + (int)(((rng->state >> 16) & 0x7FFF) % range);
}

/* ─── SVG path parsing (M/L only) ────────────────────────────────── */

static int parse_path_d(const char *d, Polyline *polylines, int max_pl) {
    int num_pl = 0;
    const char *p = d;
    float cur_x = 0, cur_y = 0;

    while (*p && num_pl < max_pl) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        if (*p == 'M' || *p == 'm') {
            int relative = (*p == 'm');
            p++;
            /* Start new polyline */
            if (num_pl > 0 && polylines[num_pl-1].count >= 2) {
                /* Previous polyline is complete */
            } else if (num_pl > 0) {
                /* Discard empty/single-point polyline */
                num_pl--;
            }
            if (num_pl >= max_pl) break;
            polylines[num_pl].count = 0;

            while (*p == ' ') p++;
            float x, y;
            if (sscanf(p, "%f %f", &x, &y) != 2 &&
                sscanf(p, "%f,%f", &x, &y) != 2) break;
            if (relative) { x += cur_x; y += cur_y; }
            cur_x = x; cur_y = y;
            polylines[num_pl].pts[0].x = x;
            polylines[num_pl].pts[0].y = y;
            polylines[num_pl].count = 1;
            /* Advance past the numbers */
            while (*p && *p != ' ' && *p != ',' && *p != 'L' && *p != 'l' && *p != 'M' && *p != 'm') p++;
            num_pl++;
        } else if (*p == 'L' || *p == 'l') {
            int relative = (*p == 'l');
            p++;
            /* Add points to current polyline */
            int pl_idx = num_pl - 1;
            if (pl_idx < 0) continue;

            while (1) {
                while (*p == ' ' || *p == ',') p++;
                if (!*p || *p == 'M' || *p == 'm' || *p == 'L' || *p == 'l') break;
                float x, y;
                if (sscanf(p, "%f %f", &x, &y) != 2 &&
                    sscanf(p, "%f,%f", &x, &y) != 2) break;
                if (relative) { x += cur_x; y += cur_y; }
                cur_x = x; cur_y = y;
                int cnt = polylines[pl_idx].count;
                if (cnt < MAX_POINTS_PER_PL) {
                    polylines[pl_idx].pts[cnt].x = x;
                    polylines[pl_idx].pts[cnt].y = y;
                    polylines[pl_idx].count = cnt + 1;
                }
                /* Advance past numbers */
                while (*p && *p != ' ' && *p != ',' && *p != 'L' && *p != 'l' && *p != 'M' && *p != 'm') p++;
            }
        } else {
            p++;
        }
    }
    /* Final cleanup: discard trailing empty polyline */
    if (num_pl > 0 && polylines[num_pl-1].count < 2) num_pl--;
    return num_pl;
}

/* ─── densify ────────────────────────────────────────────────────── */

static int densify(const Point2D *in, int in_count, Point2D *out, int max_out,
                   float min_seg_len) {
    if (in_count < 2) return 0;
    int oi = 0;
    out[oi++] = in[0];
    for (int i = 0; i < in_count - 1 && oi < max_out - 1; i++) {
        float dx = in[i+1].x - in[i].x;
        float dy = in[i+1].y - in[i].y;
        float dist = sqrtf(dx*dx + dy*dy);
        int steps = (int)ceilf(dist / min_seg_len);
        if (steps < 2) steps = 2;
        for (int s = 1; s <= steps && oi < max_out; s++) {
            float t = (float)s / steps;
            out[oi].x = in[i].x + dx * t;
            out[oi].y = in[i].y + dy * t;
            oi++;
        }
    }
    return oi;
}

/* ─── SVG font parser ────────────────────────────────────────────── */

/* Extract attribute value from an XML tag string */
static const char *get_attr(const char *tag, const char *name, char *buf, int bufsz) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "%s=\"", name);
    const char *p = strstr(tag, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    int i = 0;
    while (*p && *p != '"' && i < bufsz - 1) buf[i++] = *p++;
    buf[i] = '\0';
    return buf;
}

static int load_svg_font(FontData *font, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    /* Read entire file */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *data = malloc(sz + 1);
    if (!data) { fclose(fp); return -1; }
    fread(data, 1, sz, fp);
    data[sz] = '\0';
    fclose(fp);

    /* Find default horiz-adv-x from <font> tag */
    float default_adv = 378.0f;
    char *font_tag = strstr(data, "<font ");
    if (font_tag) {
        char buf[32];
        if (get_attr(font_tag, "horiz-adv-x", buf, sizeof(buf)))
            default_adv = atof(buf);
    }

    /* Initialize all advances to default */
    for (int i = 0; i < MAX_GLYPHS; i++) {
        font->advances[i] = default_adv;
        font->glyphs[i].num_polylines = 0;
    }

    /* Parse each <glyph> tag */
    char *p = data;
    while ((p = strstr(p, "<glyph ")) != NULL) {
        /* Find end of tag */
        char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        char *next = tag_end + 1;

        /* Extract unicode */
        char uni_buf[16] = {0};
        if (!get_attr(p, "unicode", uni_buf, sizeof(uni_buf))) {
            p = next;
            continue;
        }

        /* Handle XML entities */
        unsigned char ch = 0;
        if (strncmp(uni_buf, "&#x", 3) == 0) {
            ch = (unsigned char)strtol(uni_buf + 3, NULL, 16);
        } else if (strncmp(uni_buf, "&#", 2) == 0) {
            ch = (unsigned char)atoi(uni_buf + 2);
        } else if (uni_buf[0] && !uni_buf[1]) {
            ch = (unsigned char)uni_buf[0];
        } else if (strcmp(uni_buf, "&amp;") == 0) {
            ch = '&';
        } else if (strcmp(uni_buf, "&lt;") == 0) {
            ch = '<';
        } else if (strcmp(uni_buf, "&gt;") == 0) {
            ch = '>';
        } else if (strcmp(uni_buf, "&apos;") == 0) {
            ch = '\'';
        } else if (strcmp(uni_buf, "&quot;") == 0) {
            ch = '"';
        } else {
            /* Multi-byte or unknown — skip */
            p = next;
            continue;
        }

        /* Extract horiz-adv-x */
        char adv_buf[32];
        if (get_attr(p, "horiz-adv-x", adv_buf, sizeof(adv_buf)))
            font->advances[ch] = atof(adv_buf);

        /* Extract path data */
        char d_buf[4096];
        if (get_attr(p, "d", d_buf, sizeof(d_buf))) {
            font->glyphs[ch].num_polylines = parse_path_d(
                d_buf, font->glyphs[ch].polylines, MAX_POLYLINES);
        }

        p = next;
    }

    free(data);
    font->loaded = 1;
    return 0;
}

/* ─── text → strokes JSON ────────────────────────────────────────── */

/* Render text to stroke JSON file. Returns number of strokes written,
 * or -1 on error. */
int render_text_to_json(const char *text, const char *output_path,
                        float origin_x, float origin_y,
                        const char *font_path) {
    static FontData font;
    if (!font.loaded) {
        if (load_svg_font(&font, font_path) != 0) {
            fprintf(stderr, "[font] Failed to load %s\n", font_path);
            return -1;
        }
    }

    /* Seed RNG deterministically */
    char seed[512];
    snprintf(seed, sizeof(seed), "%s%.0f", text, origin_x);
    RNG rng;
    rng_seed(&rng, seed);

    float cursor_x = origin_x;
    float cursor_y = origin_y;
    float space_adv = font.advances[' '] * DEFAULT_SCALE;

    FILE *out = fopen(output_path, "w");
    if (!out) return -1;

    fprintf(out, "[");
    int total_strokes = 0;
    int first_stroke = 1;

    const char *word_start = text;
    while (*word_start) {
        /* Skip spaces between words */
        while (*word_start == ' ') word_start++;
        if (!*word_start) break;

        /* Find word end */
        const char *word_end = word_start;
        while (*word_end && *word_end != ' ') word_end++;

        /* Word wrap check */
        float word_width = 0;
        for (const char *c = word_start; c < word_end; c++) {
            unsigned char ch = (unsigned char)*c;
            word_width += font.advances[ch] * DEFAULT_SCALE;
        }
        if (cursor_x + word_width > origin_x + MAX_LINE_WIDTH && cursor_x > origin_x) {
            cursor_x = origin_x;
            cursor_y += GLYPH_HEIGHT * DEFAULT_SCALE * LINE_HEIGHT;
        }

        /* Render each character */
        for (const char *c = word_start; c < word_end; c++) {
            unsigned char ch = (unsigned char)*c;
            float adv = font.advances[ch] * DEFAULT_SCALE;
            Glyph *g = &font.glyphs[ch];

            if (g->num_polylines == 0) {
                cursor_x += adv;
                continue;
            }

            float x_jitter = rng_uniform(&rng, -2.0f, 2.0f);
            float y_jitter = rng_uniform(&rng, -1.5f, 0.5f);
            float slant = rng_uniform(&rng, -0.015f, 0.015f);

            for (int pl = 0; pl < g->num_polylines; pl++) {
                Polyline *src = &g->polylines[pl];
                if (src->count < 2) continue;

                /* Densify */
                Point2D dense[MAX_POINTS_PER_PL * 4];
                int dense_count = densify(src->pts, src->count, dense,
                                          MAX_POINTS_PER_PL * 4, DENSIFY_MIN_SEG);
                if (dense_count < 2) continue;

                /* Emit stroke JSON */
                if (!first_stroke) fprintf(out, ",");
                first_stroke = 0;

                float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
                fprintf(out, "{\"points\":[");

                for (int j = 0; j < dense_count; j++) {
                    float gx = dense[j].x;
                    float gy = dense[j].y;

                    /* Coordinate transform */
                    float sx = gx * DEFAULT_SCALE;
                    float sy = -gy * DEFAULT_SCALE;  /* Y flip */
                    sx += sy * slant;
                    float wobble = sinf(cursor_x * 0.01f + j * 0.3f) * 0.8f;
                    float px = cursor_x + sx + x_jitter;
                    float py = cursor_y + sy + y_jitter + wobble;

                    int speed = 4 + rng_randint(&rng, 0, 8);
                    int direction = 80 + rng_randint(&rng, 0, 30);
                    int width = 10 + rng_randint(&rng, 0, 3);
                    int pressure = 160 + rng_randint(&rng, 0, 30);

                    if (j > 0) fprintf(out, ",");
                    fprintf(out, "[%.1f,%.1f,%d,%d,%d,%d]",
                            px, py, speed, width, direction, pressure);

                    if (px < min_x) min_x = px;
                    if (py < min_y) min_y = py;
                    if (px > max_x) max_x = px;
                    if (py > max_y) max_y = py;
                }

                fprintf(out, "],\"rgba\":4278190080,\"color\":0,"
                            "\"bounds\":[%.1f,%.1f,%.1f,%.1f],"
                            "\"tool\":15,\"maskScale\":2.0,\"thickness\":2.0}",
                        min_x, min_y, max_x - min_x, max_y - min_y);

                total_strokes++;
            }

            /* Advance cursor with micro-jitter */
            cursor_x += adv + fmaxf(-2.0f, x_jitter * 0.1f) + fabsf(rng_uniform(&rng, -1.0f, 1.0f));
        }

        /* Space after word */
        cursor_x += space_adv + rng_uniform(&rng, -2.0f, 2.0f);
        word_start = word_end;
    }

    fprintf(out, "]");
    fclose(out);
    return total_strokes;
}
