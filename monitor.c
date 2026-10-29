#define _POSIX_C_SOURCE 200809L
#include <GL/freeglut.h>
#include "metrics.h"
#include "sampler.h"
#include "topology.h"
#include "explore.h"
#include "interaction.h"
#include "manual.h"
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const Snapshot *snapshot;
#define view_metrics (snapshot->hardware)
#define view_cores (snapshot->data.cpu_limit)
#define view_count (snapshot->hardware.process_count)
#define view_head (snapshot->data.history_head)
#define view_samples (snapshot->data.history_count)
#define view_cpu_history (snapshot->data.cpu_history)
#define view_mem_history (snapshot->data.mem_history)
#define view_net_history (snapshot->data.net_history)
#define view_disk_history (snapshot->data.disk_history)
#define view_memory (snapshot->data.memory_percent)
#define view_swap (snapshot->data.swap_percent)
#define view_memtotal (snapshot->data.memory_gib)
#define view_rxrate (snapshot->data.rx_rate)
#define view_txrate (snapshot->data.tx_rate)
#define view_readrate (snapshot->data.read_rate)
#define view_writerate (snapshot->data.write_rate)
#define view_uptime (snapshot->data.uptime)

static int topology_interacting(void);
static int width = 1440, height = 900, paused, smoke, frames, drag, mx, my;
static int history_window = 300, history_pinned, history_cursor_valid;
static double history_cursor_time;
static int tree_panel_size = 300, file_panel_size = 300, panel_drag;
static float overview_ratio = .666667f, gpu_ratio = .6f;
static unsigned layout_revision;
static float yaw = -35, pitch = 40, zoom = 26;
static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int sample(void) {
    if (!sampler_refresh(3000))
        return 0;
    snapshot = sampler_poll();
    return snapshot && snapshot->valid;
}

/* All interface elements and graphs are rendered by OpenGL; FreeType supplies
   glyph bitmaps only. Sampling is independent of view/filter/sort state. */
typedef struct {
    float r, g, b;
} Color;
static const Color foreground = {.86f, .9f, .95f}, muted = {.45f, .53f, .64f},
                   cyan = {.23f, .83f, .96f}, purple = {.65f, .54f, 1}, green = {.3f, .87f, .66f},
                   orange = {1, .65f, .36f}, red = {1, .36f, .4f}, panel = {.055f, .072f, .105f};
#define VIEW_COUNT 9
#define MANUAL_VIEW 8
static int tab, core_scroll, selected_core, gpu_selected, process_scroll, sort_column = 3,
                                                                          sort_desc = 1;
static int manual_scroll, manual_max_scroll;
static int topology_tab(void) {
    return tab == 6 || tab == 7;
}
static int selected_pid = -1, search_focus, visible_count, indices[MAX_PROCESSES], confirm_fd = -1,
           confirm_signal;
static uint64_t selected_start;
static Process confirm_process;
static char search[128], notice[256];
static double notice_until;
static int ui_failed;
static GLuint font_texture;
typedef struct {
    float u, v, w, h, bx, by, advance;
} Glyph;
static Glyph glyphs[128];
static int font_ready;
static void color(Color c) {
    glColor3f(c.r, c.g, c.b);
}
static void rect(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}
static void line(float x, float y, float xx, float yy) {
    glBegin(GL_LINES);
    glVertex2f(x, y);
    glVertex2f(xx, yy);
    glEnd();
}
static void font_init(void) {
    FT_Library library;
    FT_Face face;
    if (FT_Init_FreeType(&library))
        return;
    const char *paths[] = {getenv("MONITOR_FONT"), "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                           "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf", NULL};
    int ok = 0;
    for (int i = 0; i < 4; i++)
        if (paths[i] && !FT_New_Face(library, paths[i], 0, &face)) {
            ok = 1;
            break;
        }
    if (!ok) {
        FT_Done_FreeType(library);
        return;
    }
    FT_Set_Pixel_Sizes(face, 0, 18);
    unsigned char atlas[512 * 256] = {0};
    for (int c = 32; c < 127; c++) {
        if (FT_Load_Char(face, (unsigned)c, FT_LOAD_RENDER))
            continue;
        FT_GlyphSlot g = face->glyph;
        int x = (c % 16) * 32, y = (c / 16) * 32;
        int w = (int)g->bitmap.width, h = (int)g->bitmap.rows;
        if (w > 31)
            w = 31;
        if (h > 31)
            h = 31;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                atlas[(y + j) * 512 + x + i] = g->bitmap.buffer[j * g->bitmap.pitch + i];
        glyphs[c] = (Glyph){x / 512.f,
                            y / 256.f,
                            w / 512.f,
                            h / 256.f,
                            (float)g->bitmap_left,
                            (float)g->bitmap_top,
                            g->advance.x / 64.f};
    }
    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 512, 256, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    font_ready = 1;
}
static float text_width(const char *s, float size) {
    return (float)strlen(s) * (font_ready ? glyphs['M'].advance * size / 18 : 8);
}
static void label(float x, float y, float size, Color c, const char *s) {
    color(c);
    if (!font_ready) {
        glRasterPos2f(x, y);
        for (; *s; s++)
            glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (unsigned char)*s);
        return;
    }
    float scale = size / 18;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126)
            ch = '?';
        Glyph g = glyphs[ch];
        float left = x + g.bx * scale, top = y - g.by * scale, w = g.w * 512 * scale,
              h = g.h * 256 * scale;
        glTexCoord2f(g.u, g.v);
        glVertex2f(left, top);
        glTexCoord2f(g.u + g.w, g.v);
        glVertex2f(left + w, top);
        glTexCoord2f(g.u + g.w, g.v + g.h);
        glVertex2f(left + w, top + h);
        glTexCoord2f(g.u, g.v + g.h);
        glVertex2f(left, top + h);
        x += g.advance * scale;
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}
static void clip_text(float x, float y, float available, float size, Color c, const char *s) {
    char clipped[1024];
    snprintf(clipped, sizeof clipped, "%s", s);
    while (clipped[0] && text_width(clipped, size) > available)
        clipped[strlen(clipped) - 1] = 0;
    size_t n = strlen(clipped);
    if (n >= 3 && n < strlen(s))
        memcpy(clipped + n - 3, "...", 3);
    label(x, y, size, c, clipped);
}
static int manual_content(int draw) {
    int columns = (int)((width - 108) / fmaxf(1, text_width("M", 14)));
    if (columns < 12)
        columns = 12;
    if (columns > 255)
        columns = 255;
    int offset = 0;
    for (size_t section = 0; section < lsm_manual_count; section++) {
        if (draw)
            label(44, 214 + offset - manual_scroll, 14, cyan, lsm_manual[section].title);
        offset += 26;
        const char *remaining = lsm_manual[section].body;
        while (*remaining) {
            size_t length = strlen(remaining);
            if (length > (size_t)columns) {
                length = (size_t)columns;
                while (length && remaining[length] != ' ')
                    length--;
                if (!length)
                    length = (size_t)columns;
            }
            if (draw) {
                char text[256];
                memcpy(text, remaining, length);
                text[length] = 0;
                label(44, 214 + offset - manual_scroll, 14, foreground, text);
            }
            remaining += length;
            while (*remaining == ' ')
                remaining++;
            offset += 22;
        }
        offset += 20;
    }
    return offset;
}
static void manual_bounds(void) {
    manual_max_scroll = manual_content(0) - (height - 264);
    if (manual_max_scroll < 0)
        manual_max_scroll = 0;
    if (manual_scroll > manual_max_scroll)
        manual_scroll = manual_max_scroll;
    if (manual_scroll < 0)
        manual_scroll = 0;
}
static void overlay(void) {
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}
static void box(int x, int y, int w, int h) {
    color(panel);
    rect(x, y, w, h);
}
static void manual_view(void) {
    manual_bounds();
    label(24, 152, 21, foreground, "lsm / Linux System Monitor");
    clip_text(24, 176, width - 48, 12, muted,
              "Manual / controls, process actions, memory, 3D editing and command line");
    int viewport = height - 244;
    box(24, 190, width - 48, viewport);
    glEnable(GL_SCISSOR_TEST);
    glScissor(24, height - 190 - viewport, width - 48, viewport);
    manual_content(1);
    glDisable(GL_SCISSOR_TEST);
    if (manual_max_scroll) {
        float thumb = (float)viewport * viewport / (viewport + manual_max_scroll);
        if (thumb < 20)
            thumb = 20;
        color(cyan);
        rect(width - 31, 190 + (viewport - thumb) * manual_scroll / manual_max_scroll, 3, thumb);
    }
    char status[160];
    snprintf(status, sizeof status,
             "Wheel / arrows / PgUp / PgDn: scroll | Home / End | %d%% | man lsm",
             manual_max_scroll ? (int)(100.0 * manual_scroll / manual_max_scroll) : 100);
    clip_text(24, height - 34, width - 48, 11, muted, status);
}
static void button(int x, int y, int w, const char *s, Color c) {
    color((Color){c.r * .16f, c.g * .16f, c.b * .16f});
    rect(x, y, w, 34);
    float size = text_width(s, 14) > w - 20 ? 12 : 14;
    clip_text(x + 10, y + 22, w - 20, size, c, s);
}
static int inside(int x, int y, int xx, int yy, int w, int h) {
    return x >= xx && x < xx + w && y >= yy && y < yy + h;
}
static int split_left(float ratio) {
    int total = width - 64, left = (int)(total * ratio);
    if (left < 300)
        left = 300;
    if (left > total - 220)
        left = total - 220;
    return left;
}
static int topology_side_width(void) {
    return panel_sidebar_width(width, tab == 6 ? tree_panel_size : file_panel_size);
}
static int topology_side_x(void) {
    return width - 24 - topology_side_width();
}
static int panel_divider(int *x, int *y, int *h) {
    if (tab == 0 && width >= 900) {
        *x = 32 + split_left(overview_ratio);
        *y = 264;
        *h = height - 302;
        return 1;
    }
    if (tab == 2 && width >= 900 && view_metrics.gpu_count) {
        *x = 32 + split_left(gpu_ratio);
        *y = 196;
        *h = height - 238;
        return 2;
    }
    if (topology_tab() && width >= 1000) {
        *x = topology_side_x() - 8;
        *y = 264;
        *h = height - 304;
        return tab == 6 ? 3 : 4;
    }
    return 0;
}
static int panel_hit(int x, int y) {
    int xx, yy, h;
    int kind = panel_divider(&xx, &yy, &h);
    return kind && inside(x, y, xx - 5, yy, 10, h) ? kind : 0;
}
static void panel_adjust(int x) {
    if (panel_drag == 1 || panel_drag == 2) {
        float ratio = (float)(x - 32) / (width - 64);
        if (ratio < .25f)
            ratio = .25f;
        if (ratio > .8f)
            ratio = .8f;
        if (panel_drag == 1)
            overview_ratio = ratio;
        else
            gpu_ratio = ratio;
    }
    if (panel_drag == 3)
        tree_panel_size = panel_sidebar_width(width, width - 32 - x);
    if (panel_drag == 4)
        file_panel_size = panel_sidebar_width(width, width - 32 - x);
    layout_revision++;
}
static void panel_draw(void) {
    int x, y, h;
    if (!panel_divider(&x, &y, &h))
        return;
    color(panel_drag ? cyan : (Color){.16f, .23f, .3f});
    rect(x - 2, y, 4, h);
    color(muted);
    for (int i = -1; i <= 1; i++)
        rect(x - 4, y + h * .5f + i * 6, 8, 2);
}
static int latest(void) {
    return view_samples ? (view_head + HISTORY - 1) % HISTORY : view_head;
}
typedef struct {
    int x, y, w, h, stride, cursor, length;
    const float *values;
    const double *times;
    const char *unit;
} ChartHit;
#define CHART_LIMIT (MAX_CPUS + 16)
static ChartHit chart_hits[CHART_LIMIT];
static int chart_hit_count, chart_hit_tab, chart_hit_modal;
static uint64_t chart_hit_sequence;
static int inspector_is_open(void);
static int history_hit(int x, int y) {
    if (chart_hit_tab != tab || chart_hit_sequence != snapshot->sequence ||
        chart_hit_modal != inspector_is_open())
        return -1;
    if (!inspector_is_open() && (y < (tab == 2   ? (width < 900 ? 238 : 196)
                                      : tab == 4 ? 202
                                                 : 116) ||
                                 y >= height - 34))
        return -1;
    for (int i = chart_hit_count - 1; i >= 0; i--) {
        ChartHit *c = &chart_hits[i];
        if (inside(x, y, c->x + 16, c->y + 42, c->w - 31, c->h - 66))
            return i;
    }
    return -1;
}
static int history_hover(int x, int y) {
    if (history_pinned)
        return history_hit(x, y) >= 0;
    int hit = history_hit(x, y);
    history_cursor_valid = 0;
    if (hit >= 0) {
        ChartHit *c = &chart_hits[hit];
        if (c->length) {
            double newest = c->times[(c->cursor + HISTORY - 1) % HISTORY];
            double time = newest - history_window * (1 - (x - c->x - 16) / (double)(c->w - 32));
            int i = history_nearest(c->times, HISTORY, c->cursor, c->length, time);
            if (i >= 0) {
                history_cursor_time = c->times[i];
                history_cursor_valid = 1;
            }
        }
    }
    return hit >= 0;
}
static int history_click(int x, int y) {
    if (history_hit(x, y) < 0)
        return 0;
    if (history_pinned) {
        history_pinned = 0;
        history_hover(x, y);
    } else {
        history_hover(x, y);
        history_pinned = history_cursor_valid;
    }
    return 1;
}
static void history_clear(void) {
    history_pinned = history_cursor_valid = 0;
}
static float peak_range(const float *values, int cursor, int length, const double *times) {
    return history_visible_peak(values, 1, times, HISTORY, cursor, length, history_window);
}
static float peak(const float *values) {
    return peak_range(values, view_head, view_samples, snapshot->data.timestamps);
}
static void chart_history(int x, int y, int w, int h, const char *title, const float *values,
                          int stride, float maximum, Color c, int cursor, int length,
                          const double *timestamps, const char *unit) {
    box(x, y, w, h);
    clip_text(x + 16, y + 24, w - 32, 13, muted, title);
    if (h < 90)
        return;
    int top = y + 44, bottom = y + h - 28;
    color((Color){.12f, .16f, .21f});
    for (int i = 0; i < 4; i++)
        line(x + 16, top + (bottom - top) * i / 3.f, x + w - 16, top + (bottom - top) * i / 3.f);
    if (maximum <= 0)
        maximum = 1;
    double newest = length ? timestamps[(cursor + HISTORY - 1) % HISTORY] : 0;
    color(c);
    glLineWidth(1.5f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < length; i++) {
        int idx = (cursor - length + i + HISTORY) % HISTORY;
        float v = values[idx * stride];
        if (v < 0 || !isfinite(v)) {
            glEnd();
            glBegin(GL_LINE_STRIP);
            continue;
        }
        double fraction = 1 - (newest - timestamps[idx]) / history_window;
        if (fraction < 0)
            continue;
        glVertex2f(x + 16 + (w - 32) * fraction, bottom - (bottom - top) * fminf(v / maximum, 1));
    }
    glEnd();
    glLineWidth(1);
    if (length == 1 && values[((cursor + HISTORY - 1) % HISTORY) * stride] >= 0) {
        float v = values[((cursor + HISTORY - 1) % HISTORY) * stride];
        glPointSize(4);
        glBegin(GL_POINTS);
        glVertex2f(x + w - 16, bottom - (bottom - top) * fminf(v / maximum, 1));
        glEnd();
        glPointSize(1);
    }
    char s[160];
    snprintf(s, sizeof s, "-%dm", history_window / 60);
    label(x + 16, y + h - 9, 11, muted, s);
    label(x + w - 43, y + h - 9, 11, muted, paused ? "held" : "now");
    snprintf(s, sizeof s, "%.0f", maximum);
    label(x + w - 16 - text_width(s, 11), y + 24, 11, muted, s);
    if (history_cursor_valid && length && newest - history_cursor_time >= 0 &&
        newest - history_cursor_time <= history_window) {
        double fraction = 1 - (newest - history_cursor_time) / history_window;
        float xx = (float)(x + 16 + (w - 32) * fraction);
        color(history_pinned ? orange : foreground);
        line(xx, top, xx, bottom);
        int index = history_nearest(timestamps, HISTORY, cursor, length, history_cursor_time);
        if (index >= 0) {
            float v = values[index * stride];
            if (v >= 0) {
                snprintf(s, sizeof s, "%.2f %s | %.1fs ago%s", v, unit, newest - timestamps[index],
                         history_pinned ? " | pinned" : "");
                glPointSize(5);
                glBegin(GL_POINTS);
                glVertex2f(xx, bottom - (bottom - top) * fminf(v / maximum, 1));
                glEnd();
                glPointSize(1);
            } else
                snprintf(s, sizeof s, "Unavailable at selected time");
            clip_text(x + 16, y + 41, w - 32, 11, history_pinned ? orange : foreground, s);
        }
    }
    if (chart_hit_count < CHART_LIMIT)
        chart_hits[chart_hit_count++] =
            (ChartHit){x, y, w, h, stride, cursor, length, values, timestamps, unit};
}
static void chart(int x, int y, int w, int h, const char *title, const float *values, int stride,
                  float maximum, Color c) {
    const char *unit = values == view_disk_history || values == view_net_history ? "MiB/s" : "%";
    chart_history(x, y, w, h, title, values, stride, maximum, c, view_head, view_samples,
                  snapshot->data.timestamps, unit);
}
static const char *value(double v, const char *unit, char *s, size_t n) {
    if (v < 0)
        snprintf(s, n, "Unavailable");
    else
        snprintf(s, n, "%.0f%s", v, unit);
    return s;
}
static void info(int x, int y, int w, const char *key, const char *val) {
    clip_text(x, y, w / 2 - 10, 13, muted, key);
    clip_text(x + w / 2, y, w / 2, 13, foreground, val);
}
static void bar(int x, int y, int w, double v, Color c) {
    color((Color){.1f, .14f, .19f});
    rect(x, y, w, 5);
    if (v >= 0) {
        color(c);
        rect(x, y, w * fmin(v / 100, 1), 5);
    }
}
static void header(void) {
    color((Color){.04f, .054f, .08f});
    rect(0, 0, width, 116);
    label(24, 33, 22, foreground, "lsm 4.1");
    if (width > 540)
        label(166, 32, 12, muted, "RESOURCE DIAGNOSTICS");
    char s[256];
    snprintf(s, sizeof s, "%s  /  %s", view_metrics.hostname, view_metrics.kernel);
    clip_text(360, 32, width - 680, 12, muted, s);
    snprintf(s, sizeof s, "History %dm", history_window / 60);
    button(width - 288, 12, 144, s, history_pinned ? orange : cyan);
    label(width - 112, 32, 12,
          topology_interacting() ? cyan
          : paused               ? orange
          : snapshot->valid      ? green
                                 : red,
          topology_interacting() ? "o EDITING"
          : paused               ? "o FROZEN"
          : snapshot->valid      ? "o LIVE"
                                 : "o ERROR");
    const char *names[] = {width < 600 ? "1 Home" : "1 Overview",
                           "2 CPU",
                           "3 GPU",
                           width < 650 ? "4 Procs" : "4 Processes",
                           width < 650 ? "5 Wait" : "5 Pressure",
                           "6 RAM",
                           width < 700 ? "7Tree" : "7 Process tree",
                           width < 700 ? "8 FS" : "8 Filesystem",
                           "9 Manual"};
    int nav = (width - 48) / VIEW_COUNT;
    if (nav > 155)
        nav = 155;
    if (nav < 1)
        nav = 1;
    const char *compact[] = {"1", "2CPU", "3GPU", "4PID", "5PSI", "6RAM", "7Tree", "8FS", "9Help"};
    for (int i = 0; i < VIEW_COUNT; i++) {
        int x = 24 + i * nav;
        if (tab == i) {
            color((Color){.09f, .16f, .21f});
            rect(x, 61, nav - 6, 40);
            color(cyan);
            rect(x, 100, nav - 6, 2);
        }
        clip_text(x + 8, 86, nav - 12, 13, tab == i ? cyan : muted,
                  width < 700 ? compact[i] : names[i]);
    }
    snprintf(s, sizeof s, "%d CPUs / %d processes / sample %.1f ms", view_metrics.cpu_count,
             view_count, snapshot->collection_ms);
    if (width > 1800)
        label(width - 410, 85, 12, muted, s);
}
static void draw_3d(int x, int y, int w, int h, int gpu_mode) {
    box(x, y, w, h);
    label(x + 16, y + 25, 13, muted,
          gpu_mode ? "GPU ENGINE HISTORY / orbit with mouse" : "CPU HISTORY / orbit with mouse");
    int viewport_h = h - 65;
    if (viewport_h < 1)
        return;
    glViewport(x, height - y - h + 30, w, viewport_h);
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    double top = .1 * tan(45 * 3.141592653589793 / 360), aspect = (double)w / viewport_h;
    glFrustum(-top * aspect, top * aspect, -top, top, .1, 150);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, -1, -zoom);
    glRotatef(pitch, 1, 0, 0);
    glRotatef(yaw, 0, 1, 0);
    color((Color){.13f, .19f, .25f});
    glBegin(GL_LINES);
    for (int i = 0; i <= 10; i++) {
        float a = -8 + i * 1.6f;
        glVertex3f(a, 0, -6);
        glVertex3f(a, 0, 6);
    }
    for (int i = 0; i <= 12; i++) {
        glVertex3f(-8, 0, -6 + i);
        glVertex3f(8, 0, -6 + i);
    }
    for (int i = 0; i <= 5; i++) {
        glVertex3f(-8, i * 1.2f, -6);
        glVertex3f(8, i * 1.2f, -6);
    }
    glEnd();
    int shown = view_metrics.cpu_limit < 128 ? view_metrics.cpu_limit : 128;
    if (gpu_mode)
        shown = view_metrics.gpus[gpu_selected].engine_count;
    for (int c = 0; c < shown; c++) {
        if (!gpu_mode && !view_metrics.cpus[c].online)
            continue;
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < view_samples; i++) {
            int idx = (view_head - view_samples + i + HISTORY) % HISTORY;
            float raw = gpu_mode ? view_metrics.gpus[gpu_selected].engines[c].history[idx]
                                 : view_cpu_history[idx][c + 1];
            if (raw < 0)
                continue;
            double age = snapshot->data.timestamps[latest()] - snapshot->data.timestamps[idx];
            if (age > history_window)
                continue;
            float v = raw / 100;
            glColor3f(.18f + .8f * v, .78f - .35f * v, .97f - .55f * v);
            glVertex3f(shown > 1 ? -8 + 16.f * c / (shown - 1) : 0, v * 6,
                       -6 + 12.f * (1 - (float)(age / history_window)));
        }
        glEnd();
    }
    overlay();
    label(x + 16, y + h - 12, 11, muted,
          gpu_mode ? "X engine   Y engine usage %   Z time"
                   : "X logical CPU (first 128)   Y usage %   Z time");
}
static void overview(void) {
    int gap = 16, cols = width < 900 ? 2 : 4, w = (width - 48 - (cols - 1) * gap) / cols, y = 136;
    const char *titles[] = {"PROCESSOR", "MEMORY", "GPU / ENGINE", "NETWORK / TOTAL"};
    char values[4][128], details[4][128];
    snprintf(values[0], 128, "%.1f%%", view_cpu_history[latest()][0]);
    snprintf(details[0], 128, "%d cores / %d threads", view_metrics.physical_cores,
             view_metrics.cpu_count);
    snprintf(values[1], 128, "%.1f%%", view_memory);
    snprintf(details[1], 128, "%.1f / %.1f GiB", view_memory * view_memtotal / 100, view_memtotal);
    double gpu = -1;
    for (int i = 0; i < view_metrics.gpu_count; i++)
        if (view_metrics.gpus[i].busy > gpu)
            gpu = view_metrics.gpus[i].busy;
    value(gpu, "%", values[2], 128);
    snprintf(details[2], 128, "%d graphics device%s", view_metrics.gpu_count,
             view_metrics.gpu_count == 1 ? "" : "s");
    snprintf(values[3], 128, "%.2f MiB/s", (view_rxrate + view_txrate) / 1048576);
    snprintf(details[3], 128, "RX %.2f / TX %.2f", view_rxrate / 1048576, view_txrate / 1048576);
    Color colors[] = {cyan, purple, green, orange};
    for (int i = 0; i < 4; i++) {
        int x = 24 + (i % cols) * (w + gap), yy = y + (i / cols) * 102;
        box(x, yy, w, cols == 2 ? 90 : 112);
        label(x + 16, yy + 21, 11, muted, titles[i]);
        clip_text(x + 16, yy + 52, w - 32, width < 1100 ? 22 : 29, colors[i], values[i]);
        clip_text(x + 16, yy + (cols == 2 ? 76 : 92), w - 32, 11, muted, details[i]);
    }
    if (cols == 2) {
        draw_3d(24, 344, width - 48, height - 382, 0);
        return;
    }
    int left = split_left(overview_ratio), right = width - 64 - left, body_h = height - 302;
    draw_3d(24, 264, left, body_h, 0);
    int x = 40 + left, h = (body_h - 16) / 2;
    char s[128];
    snprintf(s, sizeof s, "RAM / %.1f%% / SWAP %.1f%%", view_memory, view_swap);
    chart(x, 264, right, h, s, view_mem_history, 1, 100, purple);
    snprintf(s, sizeof s, "DISK / R %.1f W %.1f MiB/s", view_readrate / 1048576,
             view_writerate / 1048576);
    chart(x, 280 + h, right, body_h - h - 16, s, view_disk_history, 1, peak(view_disk_history),
          orange);
}
static int pressure_scroll;
static void pressure_view(void) {
    label(24, 148, 20, foreground, "RESOURCE PRESSURE");
    clip_text(24, 175, width - 48, 12, muted,
              "Time tasks spend waiting for CPU, memory or storage. Lower is better.");
    int compact = width < 900, columns = compact ? 1 : 3,
        w = compact ? width - 48 : (width - 80) / 3;
    int card_h = compact ? 290 : height - 250;
    if (card_h < 280)
        card_h = 280;
    int content = (compact ? 3 * (card_h + 16) : card_h), room = height - 238,
        max_scroll = content - room;
    if (max_scroll < 0)
        max_scroll = 0;
    if (pressure_scroll > max_scroll)
        pressure_scroll = max_scroll;
    if (pressure_scroll < 0)
        pressure_scroll = 0;
    const char *names[] = {"CPU / runnable tasks waiting", "MEMORY / allocation stalls",
                           "I/O / storage stalls"};
    Color colors[] = {cyan, purple, orange};
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 34, width, height - 236 > 0 ? height - 236 : 1);
    for (int i = 0; i < 3; i++) {
        const Pressure *p = &snapshot->data.pressure[i];
        int x = 24 + (i % columns) * (w + 16),
            y = 202 + (i / columns) * (card_h + 16) - pressure_scroll;
        char s[160];
        box(x, y, w, card_h);
        clip_text(x + 16, y + 26, w - 32, 12, muted, names[i]);
        if (p->available)
            snprintf(s, sizeof s, "%.2f%%", p->avg10);
        else
            snprintf(s, sizeof s, "Unavailable");
        label(x + 16, y + 67, 28, colors[i], s);
        label(x + 16, y + 89, 11, muted, "average waiting over the last 10 seconds");
        if (p->available)
            snprintf(s, sizeof s, "60s %.2f%%   /   300s %.2f%%", p->avg60, p->avg300);
        else
            snprintf(s, sizeof s, "Kernel pressure counters not accessible.");
        clip_text(x + 16, y + 115, w - 32, 12, foreground, s);
        if (i > 0 && p->full_available)
            snprintf(s, sizeof s, "All active tasks stalled: %.2f%% (10s)", p->full10);
        else
            snprintf(s, sizeof s, "Source: /proc/pressure/%s",
                     i == 0   ? "cpu"
                     : i == 1 ? "memory"
                              : "io");
        clip_text(x + 16, y + 142, w - 32, 11, muted, s);
        chart(x + 8, y + 158, w - 16, card_h - 166, "WAITING / EACH SAMPLE (%)", p->history, 1, 100,
              colors[i]);
    }
    glDisable(GL_SCISSOR_TEST);
}
static int cpu_columns(void) {
    int n = (width - 48) / 285;
    return n > 0 ? n : 1;
}
static int cpu_rows(void) {
    int n = (height - 284) / 134;
    return n > 0 ? n : 1;
}
static int cpu_page(void) {
    return cpu_columns() * cpu_rows();
}
static void cpu_view(void) {
    clip_text(24, 145, width - 300, 20, foreground, view_metrics.cpu_name);
    char s[256], temp[64];
    snprintf(s, sizeof s, "%d physical / %d logical / %d socket(s)  |  governor %s  |  package %s",
             view_metrics.physical_cores, view_metrics.cpu_count, view_metrics.packages,
             view_metrics.governor,
             value(view_metrics.package_temperature, " C", temp, sizeof temp));
    clip_text(24, 174, width - 48, 13, muted, s);
    const CpuInfo *selected = &view_metrics.cpus[selected_core];
    snprintf(s, sizeof s,
             "CPU %d / core %d / %.0f-%.0f MHz / user %.1f%%  system %.1f%%  I/O wait %.1f%%",
             selected_core, selected->core, selected->min_mhz, selected->max_mhz, selected->user,
             selected->system, selected->iowait);
    clip_text(24, 207, width - 48, 13, cyan, s);
    snprintf(s, sizeof s, "%s | %s | cache %s | microcode %s", view_metrics.cpu_vendor,
             view_metrics.architecture, view_metrics.cache, view_metrics.microcode);
    clip_text(24, 233, width - 48, 12, muted, s);
    int cols = cpu_columns(), rows = cpu_rows(), w = (width - 48 - 12 * (cols - 1)) / cols;
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++) {
            int id = core_scroll + row * cols + col;
            if (id >= view_metrics.cpu_limit)
                continue;
            const CpuInfo *c = &view_metrics.cpus[id];
            int x = 24 + col * (w + 12), y = 250 + row * 134;
            box(x, y, w, 122);
            if (id == selected_core) {
                color(cyan);
                rect(x, y, 3, 122);
            }
            snprintf(s, sizeof s, "CPU %02d %s", id, c->online ? "" : " / offline");
            label(x + 14, y + 23, 13, foreground, s);
            snprintf(s, sizeof s, "%.1f%%", c->busy);
            label(x + w - 88, y + 24, 18, cyan, s);
            char freq[48], t[48];
            snprintf(s, sizeof s, "%s  /  %s", value(c->mhz, " MHz", freq, sizeof freq),
                     value(c->temperature, " C", t, sizeof t));
            label(x + 14, y + 47, 12, muted, s);
            snprintf(s, sizeof s, "Core %d  socket %d  max %.0f MHz", c->core, c->package,
                     c->max_mhz);
            clip_text(x + 14, y + 66, w - 28, 11, muted, s);
            bar(x + 14, y + 74, w - 28, c->busy, c->busy > 85 ? orange : cyan);
            color(cyan);
            glBegin(GL_LINE_STRIP);
            for (int i = 0; i < view_samples; i++) {
                int idx = (view_head - view_samples + i + HISTORY) % HISTORY;
                double age = snapshot->data.timestamps[latest()] - snapshot->data.timestamps[idx];
                if (age > history_window)
                    continue;
                glVertex2f(x + 14 + (w - 28) * (1 - age / history_window),
                           y + 113 - 27 * view_cpu_history[idx][id + 1] / 100);
            }
            glEnd();
            if (history_cursor_valid) {
                int index = history_nearest(snapshot->data.timestamps, HISTORY, view_head,
                                            view_samples, history_cursor_time);
                double age = snapshot->data.timestamps[latest()] - history_cursor_time;
                if (index >= 0 && age >= 0 && age <= history_window) {
                    float xx = (float)(x + 14 + (w - 28) * (1 - age / history_window));
                    color(history_pinned ? orange : foreground);
                    line(xx, y + 82, xx, y + 113);
                    snprintf(s, sizeof s, "%.1f%% @ cursor", view_cpu_history[index][id + 1]);
                    label(x + 14, y + 111, 10, history_pinned ? orange : foreground, s);
                }
            }
            if (chart_hit_count < CHART_LIMIT)
                chart_hits[chart_hit_count++] = (ChartHit){x - 2,
                                                           y + 40,
                                                           w + 4,
                                                           97,
                                                           MAX_CPUS + 1,
                                                           view_head,
                                                           view_samples,
                                                           &view_cpu_history[0][id + 1],
                                                           snapshot->data.timestamps,
                                                           "%"};
        }
    snprintf(
        s, sizeof s,
        "Showing CPU %d-%d of %d  |  click a core for breakdown  |  wheel / PgUp / PgDn to scroll",
        core_scroll,
        core_scroll + cpu_page() - 1 < view_metrics.cpu_limit ? core_scroll + cpu_page() - 1
                                                              : view_metrics.cpu_limit - 1,
        view_metrics.cpu_count);
    clip_text(24, height - 47, width - 48, 12, muted, s);
}
static int gpu_3d, gpu_scroll;
static void gpu_view(void) {
    if (!view_metrics.gpu_count) {
        clip_text(24, 160, width - 48, 18, foreground,
                  "No graphics devices exposed by DRM or NVML");
        clip_text(24, 195, width - 48, 12, muted, "Check the driver and device permissions.");
        return;
    }
    const Gpu *g = &view_metrics.gpus[gpu_selected];
    char s[256], v[80];
    int compact = width < 900;
    snprintf(s, sizeof s, "%s / %s", g->vendor, g->name);
    clip_text(24, 148, compact ? width - 48 : width - 390, compact ? 16 : 20, foreground, s);
    snprintf(s, sizeof s, "%s | %s | %s | %04x:%04x", g->card, g->driver, g->pci, g->vendor_id,
             g->device_id);
    clip_text(24, 176, width - 48, 12, muted, s);
    int button_y = compact ? 188 : 126;
    button(width - 340, button_y, 90, "< GPU", muted);
    button(width - 240, button_y, 90, "GPU >", muted);
    button(width - 140, button_y, 116, gpu_3d ? "2D view" : "3D view", cyan);
    int start = compact ? 238 : 196;
    int left = compact ? width - 48 : split_left(gpu_ratio),
        right = compact ? left : width - 64 - left, x = compact ? 24 : 40 + left,
        gh = compact ? 185 : (height - 270) / 2;
    if (gh < 140)
        gh = 140;
    int engine_h = 65 + 36 * (g->engine_count > 0 ? g->engine_count : 1), telemetry_h = 466;
    int content_h = compact ? gh + 16 + engine_h + 16 + telemetry_h
                            : (gh + 16 + engine_h > telemetry_h ? gh + 16 + engine_h : telemetry_h);
    int max_scroll = content_h - (height - start - 42);
    if (max_scroll < 0)
        max_scroll = 0;
    if (gpu_scroll > max_scroll)
        gpu_scroll = max_scroll;
    if (gpu_scroll < 0)
        gpu_scroll = 0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 34, width, height - start - 34 > 0 ? height - start - 34 : 1);
    int graph_y = start - gpu_scroll;
    if (g->busy >= 0)
        snprintf(s, sizeof s, "%s / %.1f%% / %d clients",
                 g->nv_index >= 0 ? "NVML" : "Busiest visible engine", g->busy, g->clients);
    else
        snprintf(s, sizeof s, "Utilization unavailable / %d clients", g->clients);
    if (gpu_3d)
        draw_3d(24, graph_y, left, gh, 1);
    else
        chart(24, graph_y, left, gh, s, g->history, 1, 100, green);
    int by = graph_y + gh + 16;
    box(24, by, left, engine_h);
    clip_text(40, by + 25, left - 32, 12, muted, "ENGINE ACTIVITY / capacity normalized");
    int ey = by + 53;
    for (int i = 0; i < g->engine_count; i++) {
        const Engine *e = &g->engines[i];
        snprintf(s, sizeof s, "%-17s x%u", e->name, e->capacity);
        label(40, ey, 13, foreground, s);
        label(24 + left - 100, ey, 13, green, value(e->busy, "%", v, sizeof v));
        bar(40, ey + 10, left - 32, e->busy, green);
        ey += 36;
    }
    if (!g->engine_count)
        clip_text(40, ey, left - 32, 12, muted,
                  g->nv_index >= 0 ? "Engine split unavailable through NVML"
                                   : "No accessible engine counters yet");
    int telemetry_y = compact ? by + engine_h + 16 : graph_y;
    box(x, telemetry_y, right, telemetry_h);
    label(x + 16, telemetry_y + 27, 13, muted, "DEVICE TELEMETRY");
    int yy = telemetry_y + 61, ww = right - 32;
#define GPU_INFO(key, val)                                                                         \
    do {                                                                                           \
        info(x + 16, yy, ww, key, val);                                                            \
        yy += 30;                                                                                  \
    } while (0)
    GPU_INFO("GPU clock", value(g->clock, " MHz", v, sizeof v));
    GPU_INFO("Min clock", value(g->min_clock, " MHz", v, sizeof v));
    GPU_INFO("Max clock", value(g->max_clock, " MHz", v, sizeof v));
    GPU_INFO("Temperature", value(g->temperature, " C", v, sizeof v));
    GPU_INFO("Board power", value(g->power, " W", v, sizeof v));
    GPU_INFO("Power limit", value(g->power_limit, " W", v, sizeof v));
    if (g->vram_total >= 0)
        snprintf(s, sizeof s, "%.0f / %.0f MiB", g->vram_used / 1048576, g->vram_total / 1048576);
    else
        snprintf(s, sizeof s, "Shared / not exposed");
    GPU_INFO("Dedicated VRAM", s);
    if (g->gtt_total >= 0)
        snprintf(s, sizeof s, "%.0f / %.0f MiB", g->gtt_used / 1048576, g->gtt_total / 1048576);
    else
        snprintf(s, sizeof s, "Unavailable");
    GPU_INFO("GTT memory", s);
    if (g->memory_available)
        snprintf(s, sizeof s, "%.1f MiB", g->client_memory / 1048576);
    else
        snprintf(s, sizeof s, "Unavailable");
    GPU_INFO("Client resident sum", s);
    GPU_INFO("PCIe speed", g->link[0] ? g->link : "Unavailable");
#undef GPU_INFO
    clip_text(x + 16, yy + 10, right - 32, 11, muted,
              "Resident sum can count shared buffers twice.");
    clip_text(x + 16, yy + 30, right - 32, 11, muted, "Integrated GPUs share system RAM.");
    clip_text(x + 16, yy + 50, right - 32, 11, g->restricted ? orange : muted,
              g->restricted ? "Some users' GPU clients are inaccessible."
                            : "Activity reflects accessible client counters.");
    glDisable(GL_SCISSOR_TEST);
    if (max_scroll > 0) {
        color(muted);
        rect(width - 12, start + (height - start - 42) * gpu_scroll / (float)(max_scroll + 1), 3,
             18);
    }
}
static int compare_index(const void *a, const void *b) {
    const Process *p = &view_metrics.processes[*(const int *)a],
                  *q = &view_metrics.processes[*(const int *)b];
    int c = 0;
#define CMP(field) ((p->field > q->field) - (p->field < q->field))
    switch (sort_column) {
    case 0:
        c = CMP(pid);
        break;
    case 1:
        c = CMP(uid);
        break;
    case 2:
        c = strcmp(p->name, q->name);
        break;
    case 3:
        c = CMP(cpu);
        break;
    case 4:
        c = CMP(rss_kib);
        break;
    case 5:
        c = CMP(threads);
        break;
    case 6:
        c = (p->state > q->state) - (p->state < q->state);
        break;
    }
#undef CMP
    return c ? (sort_desc ? -c : c) : (p->pid > q->pid) - (p->pid < q->pid);
}
static int contains(const char *haystack, const char *needle) {
    if (!*needle)
        return 1;
    for (; *haystack; haystack++) {
        const char *a = haystack, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b)
            return 1;
    }
    return 0;
}
static int process_rows(void) {
    int rows = (height - 410) / 29;
    return rows > 0 ? rows : 1;
}
static void filter_processes(void) {
    visible_count = 0;
    for (int i = 0; i < view_count; i++) {
        char pid[32];
        snprintf(pid, sizeof pid, "%d", view_metrics.processes[i].pid);
        if (contains(view_metrics.processes[i].name, search) ||
            contains(view_metrics.processes[i].command, search) || contains(pid, search))
            indices[visible_count++] = i;
    }
    qsort(indices, (size_t)visible_count, sizeof indices[0], compare_index);
    int max = visible_count - process_rows();
    if (max < 0)
        max = 0;
    if (process_scroll > max)
        process_scroll = max;
    if (process_scroll < 0)
        process_scroll = 0;
}
static const Process *selection(void) {
    for (int i = 0; i < view_count; i++)
        if (view_metrics.processes[i].pid == selected_pid &&
            view_metrics.processes[i].start == selected_start)
            return &view_metrics.processes[i];
    return NULL;
}
static void message(const char *s) {
    snprintf(notice, sizeof notice, "%s", s);
    notice_until = seconds() + 7;
}
static void begin_signal(int sig) {
    const Process *p = selection();
    if (!p) {
        message("Select a running process first.");
        return;
    }
    int fd = process_handle(p);
    if (fd < 0) {
        char s[256];
        snprintf(s, sizeof s, "Cannot open PID %d: %s", p->pid, strerror(errno));
        message(s);
        return;
    }
    if (confirm_fd >= 0)
        close(confirm_fd);
    confirm_fd = fd;
    confirm_signal = sig;
    confirm_process = *p;
}
static void cancel_signal(void) {
    if (confirm_fd >= 0)
        close(confirm_fd);
    confirm_fd = -1;
}
static void accept_signal(void) {
    if (confirm_fd < 0)
        return;
    char s[256];
    if (process_signal(confirm_fd, confirm_signal))
        snprintf(s, sizeof s, "PID %d: %s", confirm_process.pid, strerror(errno));
    else
        snprintf(s, sizeof s, "Sent %s to %s (PID %d).",
                 confirm_signal == SIGTERM ? "SIGTERM" : "SIGKILL", confirm_process.name,
                 confirm_process.pid);
    cancel_signal();
    message(s);
}
static struct {
    int open, metric, pid, write_index, history_count;
    uint64_t start, sequence;
    Process last_process;
    float cpu[HISTORY], ram[HISTORY], io[HISTORY];
    double time[HISTORY];
} inspector;
static int inspector_is_open(void) {
    return inspector.open;
}
static void inspector_track(void) {
    if (!inspector.open || inspector.sequence == snapshot->data.sample_number)
        return;
    inspector.sequence = snapshot->data.sample_number;
    const Process *p = selection();
    int slot = inspector.write_index;
    if (p && p->pid == inspector.pid && p->start == inspector.start) {
        inspector.last_process = *p;
        inspector.cpu[slot] = (float)p->cpu;
        inspector.ram[slot] = (float)(p->rss_kib / 1024.);
        inspector.io[slot] = p->read_rate >= 0 && p->write_rate >= 0
                                 ? (float)((p->read_rate + p->write_rate) / 1024)
                                 : -1;
    } else
        inspector.cpu[slot] = inspector.ram[slot] = inspector.io[slot] = -1;
    inspector.time[slot] = snapshot->collected_at;
    inspector.write_index = (slot + 1) % HISTORY;
    if (inspector.history_count < HISTORY)
        inspector.history_count++;
}
static void memory_ui_reset(void);
static void inspector_open(void) {
    const Process *p = selection();
    if (!p) {
        message("Select a running process first.");
        return;
    }
    memory_ui_reset();
    memset(&inspector, 0, sizeof inspector);
    inspector.open = 1;
    inspector.pid = p->pid;
    inspector.start = p->start;
    inspector.last_process = *p;
    search_focus = 0;
    inspector_track();
}
static void inspector_geometry(int *x, int *y, int *w, int *h) {
    *w = width - 32;
    if (*w > 1000)
        *w = 1000;
    *h = height - 32;
    if (*h > 720)
        *h = 720;
    *x = (width - *w) / 2;
    *y = (height - *h) / 2;
}
static int map_indices[MEMORY_REGIONS], map_count, map_scroll, map_filter, map_sort = 1,
                                                                           byte_scroll;
static MemoryRegion chosen_region;
static int region_chosen;
static uint64_t byte_serial, byte_address;
static void memory_ui_reset(void) {
    region_chosen = 0;
    byte_serial = 0;
    map_scroll = 0;
    byte_scroll = 0;
    map_filter = 0;
    sampler_memory_select(0, 0);
}
static int memory_tab_width(int w) {
    int b = (w - 56) / 5;
    return b > 108 ? 108 : b;
}
static void inspector_set_metric(int metric) {
    if (metric >= 3)
        sampler_memory_select(inspector.pid, inspector.start);
    else
        sampler_memory_select(0, 0);
    inspector.metric = metric;
}
static void inspector_close(void) {
    inspector.open = 0;
    sampler_memory_select(0, 0);
}
static const MemoryReport *inspector_report(void) {
    const MemoryReport *m = &snapshot->memory;
    return m->pid == inspector.pid && m->start == inspector.start ? m : NULL;
}
static int map_compare(const void *aa, const void *bb) {
    const MemoryReport *m = &snapshot->memory;
    const MemoryRegion *a = &m->regions[*(const int *)aa], *b = &m->regions[*(const int *)bb];
    if (map_sort && a->rss != b->rss)
        return a->rss > b->rss ? -1 : 1;
    return (a->begin > b->begin) - (a->begin < b->begin);
}
static int map_rows(int h) {
    int n = (h - 400) / 20;
    return n > 0 ? n : 1;
}
static void maps_filter(int h) {
    const MemoryReport *m = inspector_report();
    map_count = 0;
    if (m && m->available)
        for (int i = 0; i < m->stored; i++)
            if (!map_filter || (int)m->regions[i].kind == map_filter - 1)
                map_indices[map_count++] = i;
    qsort(map_indices, (size_t)map_count, sizeof map_indices[0], map_compare);
    int max = map_count - map_rows(h);
    if (max < 0)
        max = 0;
    if (map_scroll > max)
        map_scroll = max;
    if (map_scroll < 0)
        map_scroll = 0;
}
static void inspector_request_bytes(uint64_t address) {
    if (!region_chosen) {
        message("Select a memory region in Maps first.");
        inspector_set_metric(3);
        return;
    }
    if (chosen_region.permissions[0] != 'r') {
        message("This mapping is not readable.");
        return;
    }
    if (address < chosen_region.begin || address >= chosen_region.end)
        return;
    size_t n = chosen_region.end - address < MEMORY_BYTES ? (size_t)(chosen_region.end - address)
                                                          : MEMORY_BYTES;
    sampler_memory_select(inspector.pid, inspector.start);
    byte_serial = sampler_memory_peek(inspector.pid, inspector.start, address, n);
    byte_address = address;
    byte_scroll = 0;
    inspector.metric = 4;
}
static void inspector_maps(int x, int y, int w, int h) {
    const MemoryReport *m = inspector_report();
    char s[768];
    if (paused && !m) {
        label(x + 20, y + 207, 13, orange, "View is frozen. Space resumes collection.");
        return;
    }
    if (!m) {
        label(x + 20, y + 207, 13, muted, "Loading memory maps...");
        return;
    }
    if (!m->available) {
        snprintf(s, sizeof s, "Memory maps unavailable: %s", strerror(m->error ? m->error : EIO));
        clip_text(x + 20, y + 207, w - 40, 13, orange, s);
        return;
    }
    if (m->accounting)
        snprintf(s, sizeof s, "RSS %.1f / PSS %.1f / private %.1f / shared %.1f MiB",
                 m->rss / 1024., m->pss / 1024., m->private_kib / 1024., m->shared_kib / 1024.);
    else
        snprintf(s, sizeof s, "Address map available; per-region accounting unavailable.");
    clip_text(x + 20, y + 195, w - 170, 12, cyan, s);
    snprintf(s, sizeof s, "%s >",
             map_filter ? memory_kind_name((MemoryKind)(map_filter - 1)) : "All types");
    button(x + w - 140, y + 174, 120, s, muted);
    if (w >= 700) {
        label(x + 20, y + 224, 11, muted, "ADDRESS");
        label(x + 150, y + 224, 11, muted, "PERM");
        label(x + 218, y + 224, 11, muted, "RSS MiB");
        label(x + 292, y + 224, 11, muted, "PSS MiB");
        label(x + 375, y + 224, 11, muted, "MAPPING / FILE");
    } else {
        label(x + 20, y + 224, 11, muted, "PERM");
        label(x + 76, y + 224, 11, muted, "RSS MiB");
        label(x + 160, y + 224, 11, muted, "MAPPING / FILE");
    }
    maps_filter(h);
    for (int row = 0; row < map_rows(h) && row + map_scroll < map_count; row++) {
        const MemoryRegion *r = &m->regions[map_indices[row + map_scroll]];
        int yy = y + 236 + row * 20;
        color(region_chosen && r->begin == chosen_region.begin ? (Color){.07f, .17f, .23f}
              : row % 2                                        ? (Color){.04f, .055f, .08f}
                                                               : panel);
        rect(x + 12, yy, w - 24, 19);
        if (w >= 700) {
            snprintf(s, sizeof s, "%012llx", (unsigned long long)r->begin);
            label(x + 20, yy + 14, 11, muted, s);
            label(x + 150, yy + 14, 11, foreground, r->permissions);
            if (m->accounting)
                snprintf(s, sizeof s, "%.1f", r->rss / 1024.);
            else
                snprintf(s, sizeof s, "-");
            label(x + 218, yy + 14, 11, cyan, s);
            if (m->accounting)
                snprintf(s, sizeof s, "%.1f", r->pss / 1024.);
            else
                snprintf(s, sizeof s, "-");
            label(x + 292, yy + 14, 11, purple, s);
            clip_text(x + 375, yy + 14, w - 395, 11, foreground,
                      r->name[0] ? r->name : memory_kind_name(r->kind));
        } else {
            label(x + 20, yy + 14, 11, foreground, r->permissions);
            if (m->accounting)
                snprintf(s, sizeof s, "%.1f", r->rss / 1024.);
            else
                snprintf(s, sizeof s, "-");
            label(x + 76, yy + 14, 11, cyan, s);
            clip_text(x + 160, yy + 14, w - 180, 11, foreground,
                      r->name[0] ? r->name : memory_kind_name(r->kind));
        }
    }
    if (region_chosen) {
        snprintf(s, sizeof s, "%llx-%llx / %s", (unsigned long long)chosen_region.begin,
                 (unsigned long long)chosen_region.end, chosen_region.permissions);
        clip_text(x + 20, y + h - 132, w - 180, 11, cyan, s);
        clip_text(x + 20, y + h - 111, w - 40, 11, muted,
                  chosen_region.name[0] ? chosen_region.name
                                        : memory_kind_name(chosen_region.kind));
        button(x + w - 154, y + h - 150, 134, "Read bytes", cyan);
    } else {
        snprintf(s, sizeof s, "%d / %d mappings%s. Select a region to inspect.", map_count,
                 m->total_regions, m->truncated ? " (list capped)" : "");
        clip_text(x + 20, y + h - 125, w - 40, 11, muted, s);
    }
}
static void inspector_bytes(int x, int y, int w, int h) {
    char s[768];
    const MemoryBytes *b = &snapshot->bytes;
    button(x + 20, y + 174, 98, "< 256 B", muted);
    button(x + 128, y + 174, 98, "256 B >", muted);
    button(x + w - 130, y + 174, 110, "Reload", cyan);
    if (!byte_serial) {
        label(x + 20, y + 238, 12, muted, "Select a mapping in Maps, then click Read bytes.");
        return;
    }
    if (b->serial != byte_serial || b->pid != inspector.pid || b->start != inspector.start) {
        label(x + 20, y + 238, 12, muted,
              paused ? "View is frozen. Space loads the requested bytes."
                     : "Reading selected range...");
        return;
    }
    if (b->error) {
        snprintf(s, sizeof s, "Read denied or unavailable: %s", strerror(b->error));
        clip_text(x + 20, y + 238, w - 40, 12, orange, s);
        clip_text(x + 20, y + 263, w - 40, 11, muted, "Linux process-debugging permissions apply.");
        return;
    }
    snprintf(s, sizeof s, "%d bytes at 0x%llx / read-only snapshot", b->count,
             (unsigned long long)b->address);
    clip_text(x + 20, y + 230, w - 40, 12, cyan, s);
    int columns = w < 700 ? 8 : 16, visible = (h - 365) / 18;
    if (visible < 1)
        visible = 1;
    int lines = (b->count + columns - 1) / columns, max_scroll = lines - visible;
    if (max_scroll < 0)
        max_scroll = 0;
    if (byte_scroll > max_scroll)
        byte_scroll = max_scroll;
    if (byte_scroll < 0)
        byte_scroll = 0;
    for (int row = 0; row < visible && row + byte_scroll < lines; row++) {
        int offset = (row + byte_scroll) * columns,
            used = snprintf(s, sizeof s, "%012llx  ", (unsigned long long)(b->address + offset));
        for (int col = 0; col < columns; col++) {
            if (offset + col < b->count)
                used += snprintf(s + used, sizeof s - (size_t)used, "%02x ", b->data[offset + col]);
            else
                used += snprintf(s + used, sizeof s - (size_t)used, "   ");
        }
        s[used++] = ' ';
        for (int col = 0; col < columns && offset + col < b->count; col++) {
            unsigned char ch = b->data[offset + col];
            s[used++] = ch >= 32 && ch < 127 ? (char)ch : '.';
        }
        s[used] = 0;
        clip_text(x + 20, y + 254 + row * 18, w - 40, w < 700 ? 10 : 12, foreground, s);
    }
    clip_text(x + 20, y + h - 111, w - 40, 11, muted,
              "Hex bytes / ASCII. Wheel scrolls this page. No writes are performed.");
}
static int ram_indices[MAX_PROCESSES], ram_count;
static int ram_compare(const void *aa, const void *bb) {
    const Process *a = &view_metrics.processes[*(const int *)aa],
                  *b = &view_metrics.processes[*(const int *)bb];
    if (a->rss_kib != b->rss_kib)
        return a->rss_kib > b->rss_kib ? -1 : 1;
    return (a->pid > b->pid) - (a->pid < b->pid);
}
static int ram_table_top(void) {
    return height < 650 ? 362 : 496;
}
static int ram_rows(void) {
    int n = (height - ram_table_top() - 36) / 26;
    return n > 0 ? n : 1;
}
static void ram_view(void) {
    const SystemMemory *m = &snapshot->data.system_memory;
    char s[256];
    label(24, 148, 20, foreground, "INSIDE RAM");
    clip_text(24, 174, width - 48, 12, muted,
              "Click a process to explore its memory mappings and readable contents.");
    int w = (width - 64) / 2;
    box(24, 194, w, 126);
    box(40 + w, 194, w, 126);
    label(40, 220, 12, muted, "RAM CATEGORIES");
    snprintf(s, sizeof s, "Anonymous %.0f / cache %.0f MiB", m->anonymous / 1024.,
             m->cached / 1024.);
    clip_text(40, 249, w - 32, 13, foreground, s);
    snprintf(s, sizeof s, "Kernel slab %.0f / page tables %.0f MiB", m->slab / 1024.,
             m->page_tables / 1024.);
    clip_text(40, 276, w - 32, 12, muted, s);
    snprintf(s, sizeof s, "Shared / tmpfs %.0f MiB", m->shared / 1024.);
    clip_text(40, 303, w - 32, 12, muted, s);
    int xx = 56 + w;
    label(xx, 220, 12, muted, "CAPACITY");
    snprintf(s, sizeof s, "%.1f / %.1f GiB used", view_memory * view_memtotal / 100, view_memtotal);
    clip_text(xx, 249, w - 32, width < 700 ? 14 : 18, purple, s);
    snprintf(s, sizeof s, "Available %.0f / free %.0f MiB", m->available / 1024., m->free / 1024.);
    clip_text(xx, 276, w - 32, 12, muted, s);
    snprintf(s, sizeof s, "Swap %.0f / %.0f MiB", (m->swap_total - m->swap_free) / 1024.,
             m->swap_total / 1024.);
    clip_text(xx, 303, w - 32, 12, muted, s);
    if (height >= 650)
        chart(24, 334, width - 48, 106, "RESIDENT MEMORY / SYSTEM (%)", view_mem_history, 1, 100,
              purple);
    label(24, ram_table_top() - 34, 11, muted,
          "Counters overlap. RSS per process includes shared pages.");
    label(32, ram_table_top() - 11, 11, muted, "PID");
    label(120, ram_table_top() - 11, 11, muted, "RSS MiB");
    label(230, ram_table_top() - 11, 11, muted, "LARGEST PROCESSES / click");
    ram_count = view_count;
    for (int i = 0; i < ram_count; i++)
        ram_indices[i] = i;
    qsort(ram_indices, (size_t)ram_count, sizeof ram_indices[0], ram_compare);
    for (int i = 0; i < ram_count && i < ram_rows(); i++) {
        const Process *p = &view_metrics.processes[ram_indices[i]];
        int yy = ram_table_top() + i * 26;
        box(24, yy, width - 48, 25);
        snprintf(s, sizeof s, "%d", p->pid);
        label(32, yy + 17, 12, foreground, s);
        snprintf(s, sizeof s, "%.1f", p->rss_kib / 1024.);
        label(120, yy + 17, 12, purple, s);
        clip_text(230, yy + 17, width - 262, 12, foreground, p->name);
    }
}
static int process_action_width(void) {
    int w = (width - 92) / 3;
    return w < 144 ? w : 144;
}
static int process_action_x(int i) {
    int w = process_action_width();
    return width - 40 - (3 * w + 16) + i * (w + 8);
}
static void inspector_view(void) {
    if (!inspector.open)
        return;
    int x, y, w, h;
    inspector_geometry(&x, &y, &w, &h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0, 0, 0, .8f);
    rect(0, 0, width, height);
    glDisable(GL_BLEND);
    box(x, y, w, h);
    char s[768];
    const Process *live = selection();
    const Process *p = &inspector.last_process;
    clip_text(x + 20, y + 36, w - 145, 20, foreground, p->name);
    button(x + w - 106, y + 12, 88, "Close", muted);
    struct passwd *pw = getpwuid(p->uid);
    char user[64];
    if (pw)
        snprintf(user, sizeof user, "%.63s", pw->pw_name);
    else
        snprintf(user, sizeof user, "%u", (unsigned)p->uid);
    snprintf(s, sizeof s, "PID %d | %s | %s | PPID %d", p->pid, user,
             live ? "RUNNING / VISIBLE" : "EXITED / HIDDEN", p->ppid);
    clip_text(x + 20, y + 65, w - 40, 12, live ? cyan : orange, s);
    snprintf(s, sizeof s, "State %c | %d threads | nice %d | priority %d", p->state, p->threads,
             p->nice, p->priority);
    clip_text(x + 20, y + 88, w - 40, 12, muted, s);
    clip_text(x + 20, y + 113, w - 40, 11, muted, p->command);
    const char *titles[] = {"CPU", "RAM", "I/O", "Maps", "Bytes"};
    int tabw = memory_tab_width(w);
    for (int i = 0; i < 5; i++)
        button(x + 20 + i * (tabw + 4), y + 131, tabw, titles[i],
               inspector.metric == i ? cyan : muted);
    if (inspector.metric == 3)
        inspector_maps(x, y, w, h);
    else if (inspector.metric == 4)
        inspector_bytes(x, y, w, h);
    else {
        const float *values = inspector.metric == 0   ? inspector.cpu
                              : inspector.metric == 1 ? inspector.ram
                                                      : inspector.io;
        if (inspector.metric == 0)
            snprintf(s, sizeof s, "CPU / %.1f%% / 100%% = one logical CPU", p->cpu);
        else if (inspector.metric == 1)
            snprintf(s, sizeof s, "RESIDENT RAM / %.1f MiB", p->rss_kib / 1024.);
        else if (p->read_rate >= 0 && p->write_rate >= 0)
            snprintf(s, sizeof s, "DISK / R %.1f + W %.1f KiB/s", p->read_rate / 1024,
                     p->write_rate / 1024);
        else
            snprintf(s, sizeof s, "DISK I/O / unavailable with current permissions");
        float visible_peak =
            peak_range(values, inspector.write_index, inspector.history_count, inspector.time);
        float maximum = inspector.metric == 0 ? fmaxf(100, visible_peak) : visible_peak;
        chart_history(x + 12, y + 178, w - 24, h - 286, s, values, 1, maximum,
                      inspector.metric == 0   ? cyan
                      : inspector.metric == 1 ? purple
                                              : orange,
                      inspector.write_index, inspector.history_count, inspector.time,
                      inspector.metric == 0   ? "%"
                      : inspector.metric == 1 ? "MiB"
                                              : "KiB/s");
    }
    clip_text(x + 20, y + h - 83, w - 40, 11, muted,
              inspector.metric >= 3
                  ? "Wheel: scroll | S: sort maps | M: maps | B: read selected bytes"
                  : "Hover/click graph: inspect/pin | V: window | C: clear | Space: freeze");
    button(x + w - 316, y + h - 60, 140, "End / SIGTERM", orange);
    button(x + w - 164, y + h - 60, 140, "Kill / SIGKILL", red);
}
static void process_columns(int col[8]) {
    col[0] = 24;
    col[1] = 104;
    col[2] = 212;
    col[3] = width - 490;
    col[4] = width - 380;
    col[5] = width - 255;
    col[6] = width - 135;
    col[7] = width - 24;
    if (width < 900) {
        col[1] = col[2] = 94;
        col[3] = width - 204;
        col[4] = width - 116;
        col[5] = col[6] = col[7] = width - 24;
    }
}
static void process_view(void) {
    filter_processes();
    char s[512];
    label(24, 148, 21, foreground, "Processes");
    snprintf(s, sizeof s, "%d matched / %d total  |  CPU 100%% = one logical CPU", visible_count,
             view_count);
    clip_text(170, 148, width - 194, 12, muted, s);
    box(24, 166, width - 48, 42);
    if (search_focus) {
        color(cyan);
        rect(24, 206, width - 48, 2);
    }
    snprintf(s, sizeof s, "%s%s", search[0] ? search : "Search name, command or PID...",
             search_focus ? "_" : "");
    clip_text(38, 193, width - 80, 14, search[0] ? foreground : muted, s);
    int col[8];
    process_columns(col);
    const char *names[] = {"PID", "USER", "NAME", "CPU %", "RAM MiB", "THREADS", "STATE"};
    for (int i = 0; i < 7; i++) {
        if (width < 900 && (i == 1 || i == 5 || i == 6))
            continue;
        snprintf(s, sizeof s, "%s%s", names[i], sort_column == i ? (sort_desc ? " v" : " ^") : "");
        label(col[i] + 8, 239, 12, sort_column == i ? cyan : muted, s);
    }
    for (int row = 0; row < process_rows() && row + process_scroll < visible_count; row++) {
        const Process *p = &view_metrics.processes[indices[row + process_scroll]];
        int y = 250 + row * 29;
        color(p->pid == selected_pid && p->start == selected_start ? (Color){.075f, .18f, .23f}
              : row % 2                                            ? (Color){.045f, .06f, .09f}
                                                                   : panel);
        rect(24, y, width - 48, 28);
        snprintf(s, sizeof s, "%d", p->pid);
        label(col[0] + 8, y + 19, 13, foreground, s);
        if (width >= 900) {
            struct passwd *pw = getpwuid(p->uid);
            if (pw)
                snprintf(s, sizeof s, "%.11s", pw->pw_name);
            else
                snprintf(s, sizeof s, "%u", (unsigned)p->uid);
            clip_text(col[1] + 8, y + 19, col[2] - col[1] - 16, 12, muted, s);
        }
        clip_text(col[2] + 8, y + 19, col[3] - col[2] - 16, 13, foreground, p->name);
        snprintf(s, sizeof s, "%.1f", p->cpu);
        label(col[3] + 8, y + 19, 13, p->cpu > 50 ? orange : cyan, s);
        snprintf(s, sizeof s, "%.1f", p->rss_kib / 1024.);
        label(col[4] + 8, y + 19, 13, foreground, s);
        if (width >= 900) {
            snprintf(s, sizeof s, "%d", p->threads);
            label(col[5] + 8, y + 19, 13, muted, s);
            snprintf(s, sizeof s, "%c", p->state);
            label(col[6] + 8, y + 19, 13, p->state == 'R' ? green : muted, s);
        }
    }
    const Process *p = selection();
    int y = height - 146;
    box(24, y, width - 48, 112);
    if (p) {
        char io[100];
        if (p->read_rate >= 0 && p->write_rate >= 0)
            snprintf(io, sizeof io, "disk R %.1f / W %.1f KiB/s", p->read_rate / 1024,
                     p->write_rate / 1024);
        else
            snprintf(io, sizeof io, "disk I/O unavailable");
        snprintf(s, sizeof s, "PID %d | PPID %d | nice %d | priority %d | %s", p->pid, p->ppid,
                 p->nice, p->priority, io);
        clip_text(38, y + 23, width - 76, 12, foreground, s);
        clip_text(38, y + 44, width - 76, 12, muted, p->command);
    } else
        label(38, y + 33, 13, muted,
              selected_pid > 0 ? "Selected process has exited."
                               : "Select a row to inspect or end a process.");
    int bw = process_action_width();
    button(process_action_x(0), y + 62, bw, "Inspect / I", cyan);
    button(process_action_x(1), y + 62, bw, width < 650 ? "End / T" : "End / SIGTERM", orange);
    button(process_action_x(2), y + 62, bw, width < 650 ? "Kill / K" : "Kill / SIGKILL", red);
}
static void dialog(void) {
    if (confirm_fd < 0)
        return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0, 0, 0, .78f);
    rect(0, 0, width, height);
    glDisable(GL_BLEND);
    int w = width - 32;
    if (w > 640)
        w = 640;
    int x = (width - w) / 2, y = (height - 270) / 2;
    box(x, y, w, 270);
    Color c = confirm_signal == SIGTERM ? orange : red;
    label(x + 24, y + 38, width < 600 ? 19 : 22, c,
          confirm_signal == SIGTERM ? "End this process?" : "Force kill this process?");
    char s[256];
    snprintf(s, sizeof s, "%s  /  PID %d  /  UID %u", confirm_process.name, confirm_process.pid,
             (unsigned)confirm_process.uid);
    clip_text(x + 24, y + 76, w - 48, 14, foreground, s);
    label(x + 24, y + 113, 12, muted,
          confirm_signal == SIGTERM ? "SIGTERM requests a graceful exit."
                                    : "SIGKILL ends the process immediately.");
    label(x + 24, y + 137, 12, muted,
          confirm_signal == SIGTERM ? "The application may ignore this request."
                                    : "Unsaved work can be lost.");
    label(x + 24, y + 165, 11, muted, "Children are not automatically ended.");
    button(x + w - 304, y + 208, 128, "Cancel / Esc", muted);
    button(x + w - 164, y + 208, 140, "Confirm / Enter", c);
}
/* The same tree layout and camera drive process ancestry and directory trees. */
static Topology scene;
static int scene_kind = -1, tree_mine, scene_dirty = 1, scene_fit = 1, tree_focus_pid,
           tree_hover = -1;
static uint64_t tree_focus_start, scene_epoch;
static int tree_follow, follow_exit_notified;
static int files_running, file_selected = -1, file_depth = 2, file_hidden;
static uint64_t file_request_serial;
static const FileTree *file_frame;
static char file_root[TOPOLOGY_PATH], file_input[TOPOLOGY_PATH], tree_query[128];
static int file_input_focus, tree_query_focus;
static float tree_yaw = -25, tree_pitch = 22, tree_distance = 40, tree_pan_x, tree_pan_y,
             tree_center[3];
static int graph_x, graph_y, graph_w, graph_h;
static uint64_t scene_scope;
static PickFrame presented;
static LayoutEditor layout_editor;
static int presented_indices[TOPOLOGY_NODES], injected_modifiers;
static NodeKey file_selection_key;
static int file_selection_valid;
typedef struct {
    NodeKey key;
    char label[128];
    float cpu, base_radius;
    unsigned uid;
    int directory, symlink, error, selectable;
} SceneMeta;
static SceneMeta scene_meta[TOPOLOGY_NODES];
static struct {
    int active, button, mode, x, y, moved, empty_click, had_before;
    float yaw, pitch, distance, pan_x, pan_y;
    PickNode target;
    LayoutKey layout;
    double previous[3];
    PickFrame camera;
} tree_gesture;
static int topology_interacting(void) {
    return tree_gesture.active;
}
static GLdouble graph_model[16], graph_projection[16];
static GLint graph_viewport[4];
static int graph_ready;
static int tree_child_pid[16], tree_child_rows;
static uint64_t tree_child_start[16];
static int process_index(int pid) {
    int lo = 0, hi = view_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (view_metrics.processes[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < view_count && view_metrics.processes[lo].pid == pid ? lo : -1;
}
static void files_load(const char *path) {
    if (!path || !*path || strlen(path) >= TOPOLOGY_PATH) {
        message("Path is empty or too long.");
        return;
    }
    if (!files_running) {
        if (!files_start()) {
            message("Cannot start filesystem collector.");
            return;
        }
        files_running = 1;
        file_frame = files_poll();
    }
    char requested[TOPOLOGY_PATH];
    snprintf(requested, sizeof requested, "%s", path);
    int same_root = file_frame && file_frame->count && !strcmp(requested, file_frame->root);
    uint64_t request = files_request(requested, file_depth, file_hidden);
    if (!request) {
        message("Path is empty or too long.");
        return;
    }
    file_request_serial = request;
    snprintf(file_input, sizeof file_input, "%s", requested);
    if (!same_root) {
        file_selected = -1;
        file_selection_valid = 0;
    }
    presented.valid = 0;
    scene_dirty = 1;
    if (!same_root)
        scene_fit = 1;
    tree_hover = -1;
    graph_ready = 0;
}
static void files_parent(void) {
    if (!file_frame || file_frame->serial != file_request_serial || !file_frame->count)
        return;
    char path[TOPOLOGY_PATH];
    snprintf(path, sizeof path, "%s", file_frame->root);
    char *slash = strrchr(path, '/');
    if (slash == path)
        slash[1] = 0;
    else if (slash)
        *slash = 0;
    else
        snprintf(path, sizeof path, "/");
    files_load(path);
}
static void files_enter(void) {
    if (!file_frame || file_frame->serial != file_request_serial || file_selected < 0 ||
        file_selected >= file_frame->count)
        return;
    const FileNode *n = &file_frame->nodes[file_selected];
    if (!n->directory || n->symlink) {
        message(n->symlink ? "Symlink shown without traversal. Enter an explicit path to follow it."
                           : "Select a directory to enter.");
        return;
    }
    char path[TOPOLOGY_PATH];
    if (files_path(file_frame, file_selected, path, sizeof path))
        files_load(path);
    else
        message("Selected path is too long.");
}
static int is_descendant(int source, int pid, uint64_t start) {
    for (int steps = 0; source >= 0 && steps < view_count; steps++) {
        const Process *p = &view_metrics.processes[source];
        if (p->pid == pid)
            return p->start == start;
        int parent = process_index(p->ppid);
        if (parent == source)
            return 0;
        source = parent;
    }
    return 0;
}
static void topology_measure_scene(void) {
    float low[3] = {0}, high[3] = {0};
    int found = 0;
    for (int i = 0; i < scene.count; i++)
        if (scene.nodes[i].depth >= 0) {
            float p[3] = {scene.nodes[i].x, scene.nodes[i].y, scene.nodes[i].z};
            for (int k = 0; k < 3; k++) {
                if (!found || p[k] < low[k])
                    low[k] = p[k];
                if (!found || p[k] > high[k])
                    high[k] = p[k];
            }
            found = 1;
        }
    for (int k = 0; k < 3; k++)
        scene.center[k] = (low[k] + high[k]) * .5f;
    scene.extent = 1;
    for (int i = 0; i < scene.count; i++)
        if (scene.nodes[i].depth >= 0) {
            float x = scene.nodes[i].x - scene.center[0], y = scene.nodes[i].y - scene.center[1],
                  z = scene.nodes[i].z - scene.center[2];
            float r = sqrtf(x * x + y * y + z * z) + scene.nodes[i].radius;
            if (r > scene.extent)
                scene.extent = r;
        }
}
static void topology_apply_layout(int kind) {
    for (int i = 0; i < scene.count; i++)
        if (scene.nodes[i].depth >= 0 && scene_meta[i].selectable) {
            double p[3];
            if (layout_get(&layout_editor, (LayoutKey){kind, scene_scope, scene_meta[i].key}, p)) {
                scene.nodes[i].x = (float)p[0];
                scene.nodes[i].y = (float)p[1];
                scene.nodes[i].z = (float)p[2];
            }
        }
    topology_measure_scene();
}
static NodeKey file_node_key(const FileTree *f, int i) {
    char path[TOPOLOGY_PATH];
    if (!files_path(f, i, path, sizeof path))
        return (NodeKey){0, 0};
    return node_path_key(path);
}
static void process_scene_build(void) {
    char context[128];
    snprintf(context, sizeof context, "process:%d:%llu:%d", tree_focus_pid,
             (unsigned long long)tree_focus_start, tree_mine);
    scene_scope = node_path_key(context).a;
    int reverse[MAX_PROCESSES];
    for (int i = 0; i < view_count; i++)
        reverse[i] = -1;
    scene.count = 1;
    scene.nodes[0] = (TopologyNode){.parent = -1, .source = -1, .id = 0, .radius = .4f};
    scene_meta[0] = (SceneMeta){0};
    snprintf(scene_meta[0].label, sizeof scene_meta[0].label, "Visible roots");
    for (int i = 0; i < view_count && scene.count < TOPOLOGY_NODES; i++) {
        const Process *p = &view_metrics.processes[i];
        if (tree_mine && p->uid != getuid())
            continue;
        if (tree_focus_pid && !is_descendant(i, tree_focus_pid, tree_focus_start))
            continue;
        int index = scene.count++;
        scene_meta[index] = (SceneMeta){.key = {(uint64_t)p->pid, p->start},
                                        .cpu = (float)p->cpu,
                                        .uid = (unsigned)p->uid,
                                        .selectable = 1};
        snprintf(scene_meta[index].label, sizeof scene_meta[index].label, "%d %.90s", p->pid,
                 p->name);
        reverse[i] = index;
        scene.nodes[index] =
            (TopologyNode){.parent = 0,
                           .source = i,
                           .id = p->pid,
                           .identity = p->start,
                           .radius = .18f + fminf(.2f, sqrtf((float)p->cpu) * .016f)};
    }
    int focus = 0;
    for (int i = 1; i < scene.count; i++) {
        TopologyNode *n = &scene.nodes[i];
        int p = process_index(view_metrics.processes[n->source].ppid);
        if (p >= 0 && reverse[p] >= 0)
            n->parent = reverse[p];
        if (n->id == tree_focus_pid && n->identity == tree_focus_start)
            focus = i;
    }
    topology_layout(&scene, focus);
    topology_apply_layout(6);
}
static void file_scene_build(void) {
    if (file_selection_valid)
        file_selected = -1;
    scene_scope = file_frame ? node_path_key(file_frame->root).a : 0;
    scene.count = file_frame ? file_frame->count : 0;
    for (int i = 0; i < scene.count; i++) {
        const FileNode *n = &file_frame->nodes[i];
        scene_meta[i] = (SceneMeta){.key = file_node_key(file_frame, i),
                                    .directory = n->directory,
                                    .symlink = n->symlink,
                                    .error = n->error,
                                    .uid = n->uid,
                                    .selectable = 1};
        const char *name = n->name;
        if (!i) {
            const char *slash = strrchr(file_frame->root, '/');
            name = slash && slash[1] ? slash + 1 : file_frame->root;
        }
        snprintf(scene_meta[i].label, sizeof scene_meta[i].label, "%.127s", name);
        if (file_selection_valid && node_key_equal(scene_meta[i].key, file_selection_key))
            file_selected = i;
        scene_meta[i].base_radius =
            n->directory ? .27f : .14f + fminf(.15f, log1pf((float)n->bytes) * .006f);
        scene.nodes[i] = (TopologyNode){
            .parent = n->parent,
            .source = i,
            .id = i,
            .identity = file_frame->serial,
            .radius = n->directory ? .27f : .14f + fminf(.15f, log1pf((float)n->bytes) * .006f)};
    }
    if (scene.count) {
        topology_layout(&scene, 0);
        topology_apply_layout(7);
    }
}
static void topology_focus(void) {
    const Process *p = selection();
    if (!p) {
        message("Select a running process first.");
        return;
    }
    tree_follow = 0;
    tree_focus_pid = p->pid;
    tree_focus_start = p->start;
    tree_mine = 0;
    scene_dirty = scene_fit = 1;
}
static void topology_follow_toggle(void) {
    if (tree_follow) {
        tree_follow = 0;
        return;
    }
    if (!selection()) {
        message("Select a running process to follow.");
        return;
    }
    topology_focus();
    tree_follow = 1;
    follow_exit_notified = 0;
    scene_fit = 1;
}
typedef struct {
    uint64_t sequence, start;
    int pid, members;
    double cpu, rss;
} FamilySummary;
static FamilySummary family_summary;
static const FamilySummary *topology_family(const Process *p) {
    if (family_summary.sequence == snapshot->data.sample_number && family_summary.pid == p->pid &&
        family_summary.start == p->start)
        return &family_summary;
    family_summary =
        (FamilySummary){.sequence = snapshot->data.sample_number, .start = p->start, .pid = p->pid};
    for (int i = 0; i < view_count; i++)
        if (is_descendant(i, p->pid, p->start)) {
            family_summary.members++;
            family_summary.cpu += view_metrics.processes[i].cpu;
            family_summary.rss += view_metrics.processes[i].rss_kib / 1024.;
        }
    return &family_summary;
}
static void topology_parent(void) {
    const Process *p = selection();
    if (!p)
        return;
    int parent = process_index(p->ppid);
    if (parent < 0) {
        message("Parent is outside the visible process snapshot.");
        return;
    }
    selected_pid = view_metrics.processes[parent].pid;
    selected_start = view_metrics.processes[parent].start;
    tree_follow = 0;
    tree_focus_pid = selected_pid;
    tree_focus_start = selected_start;
    tree_mine = 0;
    scene_dirty = scene_fit = 1;
}
static void topology_find(void) {
    if (!tree_query[0]) {
        message("Enter a process name or PID.");
        return;
    }
    for (int i = 0; i < view_count; i++) {
        char pid[32];
        const Process *p = &view_metrics.processes[i];
        snprintf(pid, sizeof pid, "%d", p->pid);
        if (!strcmp(pid, tree_query) || contains(p->name, tree_query)) {
            selected_pid = p->pid;
            selected_start = p->start;
            topology_focus();
            tree_query_focus = 0;
            return;
        }
    }
    message("No matching process in the current snapshot.");
}
static void topology_geometry(void) {
    graph_x = 24;
    graph_y = 264;
    graph_w = width >= 1000 ? width - 64 - topology_side_width() : width - 48;
    graph_h = width >= 1000 ? height - 304 : height - 384;
}
static int topology_project(const TopologyNode *n, double *x, double *y, double *depth) {
    double p[3] = {n->x, n->y, n->z}, eye;
    return pick_project(&presented, p, x, y, depth, &eye);
}
static int topology_pick(int x, int y) {
    if (presented.kind != tab || !graph_ready)
        return -1;
    int hit = pick_target(&presented, x, y);
    return hit >= 0 ? presented.nodes[hit].source : -1;
}
static int topology_select_key(int kind, NodeKey key) {
    if (kind == 6) {
        int index = process_index((int)key.a);
        if (index < 0 || view_metrics.processes[index].start != key.b) {
            message("That process exited or changed before selection.");
            return 0;
        }
        selected_pid = (int)key.a;
        selected_start = key.b;
        return 1;
    }
    if (!file_frame || file_frame->serial != file_request_serial)
        return 0;
    for (int i = 0; i < file_frame->count; i++)
        if (node_key_equal(file_node_key(file_frame, i), key)) {
            file_selected = i;
            file_selection_key = key;
            file_selection_valid = 1;
            return 1;
        }
    message("That filesystem entry is no longer in the current snapshot.");
    return 0;
}
static void topology_select(int source) {
    for (int i = 0; i < presented.count; i++)
        if (presented.nodes[i].source == source) {
            topology_select_key(presented.kind, presented.nodes[i].key);
            return;
        }
}
static Color topology_color(int i) {
    const SceneMeta *m = &scene_meta[i];
    if ((scene_kind == 6 && m->selectable && m->key.a == (uint64_t)selected_pid &&
         m->key.b == selected_start) ||
        (scene_kind == 7 && file_selection_valid && node_key_equal(m->key, file_selection_key)))
        return orange;
    if (i == tree_hover)
        return foreground;
    if (scene_kind == 7)
        return m->error ? red : m->symlink ? orange : m->directory ? cyan : purple;
    if (!m->selectable)
        return muted;
    return m->cpu > 50 ? red : m->uid == getuid() ? cyan : purple;
}
static const char *topology_node_name(int i, char *out, size_t size) {
    snprintf(out, size, "%s", scene_meta[i].label);
    return out;
}
static void topology_draw(void) {
    topology_geometry();
    box(graph_x, graph_y, graph_w, graph_h);
    if (graph_h < 65 || graph_w < 80 || !scene.count) {
        graph_ready = 0;
        return;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(graph_x, height - graph_y - graph_h, graph_w, graph_h);
    glViewport(graph_x, height - graph_y - graph_h, graph_w, graph_h);
    float aspect = (float)graph_w / graph_h, extent = scene.extent;
    if (scene_fit && !tree_gesture.active) {
        topology_measure_scene();
        extent = scene.extent;
        memcpy(tree_center, scene.center, sizeof tree_center);
        tree_pan_x = tree_pan_y = 0;
        float half = .39269908f;
        if (aspect < 1)
            half = atanf(tanf(half) * aspect);
        tree_distance = extent / sinf(half) * 1.12f + 1;
        scene_fit = 0;
    }
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    double top = .1 * tan(.3926990817);
    glFrustum(-top * aspect, top * aspect, -top, top, .1, fmax(300, tree_distance + extent * 5));
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(tree_pan_x, tree_pan_y, -tree_distance);
    glRotatef(tree_pitch, 1, 0, 0);
    glRotatef(tree_yaw, 0, 1, 0);
    glTranslatef(-tree_center[0], -tree_center[1], -tree_center[2]);
    glGetDoublev(GL_MODELVIEW_MATRIX, graph_model);
    glGetDoublev(GL_PROJECTION_MATRIX, graph_projection);
    glGetIntegerv(GL_VIEWPORT, graph_viewport);
    graph_ready = 1;
    pick_frame_begin(&presented, scene_kind, scene_scope, graph_model, graph_projection,
                     graph_viewport, height);
    if (scene_kind == 6)
        for (int i = 0; i < scene.count; i++) {
            TopologyNode *node = &scene.nodes[i];
            if (node->depth < 0)
                continue;
            double eye_z = graph_model[2] * node->x + graph_model[6] * node->y +
                           graph_model[10] * node->z + graph_model[14];
            float base = node->id ? .18f + fminf(.2f, sqrtf(scene_meta[i].cpu) * .016f) : .4f;
            node->radius = fmaxf(base, (float)(-eye_z * 2 * tan(.3926990817) * 3.5 / graph_h));
        }
    if (scene_kind == 7)
        for (int i = 0; i < scene.count; i++)
            if (scene.nodes[i].depth >= 0) {
                TopologyNode *n = &scene.nodes[i];
                double eye_z = graph_model[2] * n->x + graph_model[6] * n->y +
                               graph_model[10] * n->z + graph_model[14];
                n->radius = fmaxf(scene_meta[i].base_radius,
                                  (float)(-eye_z * 2 * tan(.3926990817) * 3.5 / graph_h));
            }
    for (int i = 0; i < scene.count; i++) {
        presented_indices[i] = -1;
        if (scene.nodes[i].depth < 0 || !scene_meta[i].selectable)
            continue;
        double p[3] = {scene.nodes[i].x, scene.nodes[i].y, scene.nodes[i].z};
        presented_indices[i] = pick_add(&presented, scene_meta[i].key, i, scene_meta[i].directory,
                                        p, scene.nodes[i].radius);
    }
    unsigned char ancestor[TOPOLOGY_NODES] = {0};
    if (scene_kind == 6)
        for (int i = 0; i < scene.count; i++)
            if (scene.nodes[i].id == selected_pid && scene.nodes[i].identity == selected_start) {
                for (int j = i, steps = 0; j >= 0 && steps < scene.count;
                     j = scene.nodes[j].parent, steps++)
                    ancestor[j] = 1;
                break;
            }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBegin(GL_LINES);
    for (int i = 0; i < scene.count; i++) {
        const TopologyNode *n = &scene.nodes[i];
        int p = n->parent;
        if (n->depth <= 0 || p < 0 || scene.nodes[p].depth < 0)
            continue;
        if (scene_kind == 6 && ancestor[i])
            glColor4f(1, .67f, .3f, .95f);
        else
            glColor4f(.25f, .61f, .78f,
                      (scene_kind == 6 ? (n->id == selected_pid && n->identity == selected_start)
                                       : file_selection_valid &&
                                             node_key_equal(scene_meta[i].key, file_selection_key))
                          ? .95f
                          : .36f);
        glVertex3f(n->x, n->y, n->z);
        glVertex3f(scene.nodes[p].x, scene.nodes[p].y, scene.nodes[p].z);
        float dx = n->x - scene.nodes[p].x, dy = n->y - scene.nodes[p].y,
              dz = n->z - scene.nodes[p].z, len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 0 && !(scene_kind == 6 && !scene.nodes[p].id)) {
            float ux = dx / len, uy = dy / len, uz = dz / len;
            float tx = n->x - ux * (n->radius + .05f), ty = n->y - uy * (n->radius + .05f),
                  tz = n->z - uz * (n->radius + .05f);
            float ex = tx - ux * .4f, ey = ty - uy * .4f, ez = tz - uz * .4f, nx = -uz * .13f,
                  nz = ux * .13f;
            glVertex3f(tx, ty, tz);
            glVertex3f(ex + nx, ey, ez + nz);
            glVertex3f(tx, ty, tz);
            glVertex3f(ex - nx, ey, ez - nz);
        }
    }
    glEnd();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    const GLfloat light[] = {-12, 25, 35, 1};
    glLightfv(GL_LIGHT0, GL_POSITION, light);
    for (int i = 0; i < scene.count; i++) {
        const TopologyNode *n = &scene.nodes[i];
        if (n->depth < 0)
            continue;
        color(topology_color(i));
        glPushMatrix();
        glTranslatef(n->x, n->y, n->z);
        if (scene_kind == 7 && scene_meta[i].directory)
            glutSolidCube(n->radius * 1.6);
        else if (!scene_meta[i].selectable)
            glutWireSphere(n->radius, 10, 7);
        else
            glutSolidSphere(n->radius, 10, 7);
        glPopMatrix();
    }
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    overlay();
    int labels = 0;
    for (int i = 0; i < scene.count; i++) {
        const TopologyNode *n = &scene.nodes[i];
        if (n->depth < 0)
            continue;
        int selected = scene_kind == 6 ? n->id == selected_pid && n->identity == selected_start
                                       : file_selection_valid &&
                                             node_key_equal(scene_meta[i].key, file_selection_key);
        if (i != tree_hover && !selected && i != scene.root && (scene.visible > 55 || n->depth > 2))
            continue;
        double x, y, d;
        if (!topology_project(n, &x, &y, &d) ||
            !inside((int)x, (int)y, graph_x, graph_y, graph_w, graph_h))
            continue;
        char name[128];
        topology_node_name(i, name, sizeof name);
        double label_width = fmin(180, text_width(name, 11)),
               radius = presented_indices[i] >= 0
                            ? presented.nodes[presented_indices[i]].pixel_radius
                            : 4;
        double lx = x + fmax(8, radius + 6), ly = y - fmax(8, radius * .5 + 4);
        if (lx + label_width > graph_x + graph_w - 8)
            lx = x - radius - 6 - label_width;
        lx = fmax(graph_x + 6, fmin(lx, graph_x + graph_w - label_width - 6));
        ly = fmax(graph_y + 18, fmin(ly, graph_y + graph_h - 8));
        color((Color){.035f, .05f, .075f});
        rect((float)lx - 3, (float)ly - 12, (float)label_width + 6, 16);
        clip_text((float)lx, (float)ly, 180, 11, selected ? orange : foreground, name);
        pick_label_add(&presented, presented_indices[i], lx - 3, ly - 12, label_width + 6, 16);
        if (++labels >= 60)
            break;
    }
    for (int i = 0; i < presented.count; i++) {
        const PickNode *n = &presented.nodes[i];
        int selected = scene_kind == 6
                           ? n->key.a == (uint64_t)selected_pid && n->key.b == selected_start
                           : file_selection_valid && node_key_equal(n->key, file_selection_key);
        if (!selected && n->source != tree_hover)
            continue;
        color(selected ? orange : cyan);
        glLineWidth(1.5f);
        glBegin(GL_LINE_LOOP);
        double radius = fmax(8, n->pixel_radius + 4);
        for (int j = 0; j < 40; j++) {
            double angle = j * 6.28318530718 / 40;
            glVertex2d(n->sx + cos(angle) * radius, n->sy + sin(angle) * radius);
        }
        glEnd();
        glLineWidth(1);
    }
    if (tree_gesture.active)
        clip_text(graph_x + 14, graph_y + 24, graph_w - 28, 12, cyan,
                  tree_gesture.mode == 1   ? "Moving node | Esc cancels"
                  : tree_gesture.mode == 2 ? "Orbiting camera | Esc cancels"
                                           : "Panning camera | Esc cancels");
    glDisable(GL_SCISSOR_TEST);
}
static void topology_details(void) {
    int wide = width >= 1000, x = wide ? topology_side_x() : 24, y = wide ? 264 : height - 108,
        w = wide ? topology_side_width() : width - 48, h = wide ? height - 304 : 72;
    box(x, y, w, h);
    char s[768];
    tree_child_rows = 0;
    if (scene_kind == 6) {
        const Process *p = selection();
        if (!p) {
            clip_text(x + 14, y + 24, w - 28, 12, muted, "Click a node to inspect its process.");
            return;
        }
        snprintf(s, sizeof s, "%s / PID %d", p->name, p->pid);
        clip_text(x + 14, y + 24, w - 28, 14, cyan, s);
        snprintf(s, sizeof s, "PPID %d | CPU %.1f%% | RAM %.1f MiB", p->ppid, p->cpu,
                 p->rss_kib / 1024.);
        clip_text(x + 14, y + 48, w - 28, 12, foreground, s);
        if (!wide)
            return;
        snprintf(s, sizeof s, "UID %u | state %c | %d threads", (unsigned)p->uid, p->state,
                 p->threads);
        clip_text(x + 14, y + 74, w - 28, 12, muted, s);
        clip_text(x + 14, y + 101, w - 28, 11, muted, p->command);
        const FamilySummary *family = topology_family(p);
        snprintf(s, sizeof s, "Family %d | CPU %.1f%%", family->members, family->cpu);
        clip_text(x + 14, y + 128, w - 28, 12, cyan, s);
        snprintf(s, sizeof s, "RSS sum %.1f MiB (includes shared)", family->rss);
        clip_text(x + 14, y + 152, w - 28, 11, muted, s);
        button(x + 14, y + 169, w - 28, "Inspect process / I", cyan);
        label(x + 14, y + 228, 12, muted, "DIRECT CHILDREN / click to select");
        for (int i = 0; i < view_count && tree_child_rows < 16; i++)
            if (view_metrics.processes[i].ppid == p->pid &&
                y + 255 + tree_child_rows * 24 < y + h - 95) {
                const Process *c = &view_metrics.processes[i];
                tree_child_pid[tree_child_rows] = c->pid;
                tree_child_start[tree_child_rows] = c->start;
                snprintf(s, sizeof s, "%d  %.25s", c->pid, c->name);
                clip_text(x + 14, y + 255 + tree_child_rows * 24, w - 28, 12, foreground, s);
                tree_child_rows++;
            }
        if (!tree_child_rows)
            label(x + 14, y + 255, 12, muted, "No visible direct children.");
        label(x + 14, y + h - 65, 11, muted, "Cyan: yours | purple: others");
        label(x + 14, y + h - 43, 11, muted, "Orange: selected | red: busy");
        label(x + 14, y + h - 21, 11, muted, "Visible roots is a grouping node.");
    } else {
        if (!file_frame || file_selected < 0 || file_selected >= file_frame->count) {
            clip_text(x + 14, y + 24, w - 28, 12, muted, "Click a file or folder for details.");
            return;
        }
        const FileNode *f = &file_frame->nodes[file_selected];
        clip_text(x + 14, y + 24, w - 28, 14, cyan, f->name);
        snprintf(s, sizeof s, "%s | %.1f KiB logical",
                 f->symlink     ? "Symlink"
                 : f->directory ? "Directory"
                                : "File",
                 f->bytes / 1024.);
        clip_text(x + 14, y + 48, w - 28, 12, foreground, s);
        if (!wide)
            return;
        snprintf(s, sizeof s, "Allocated %.1f KiB | mode %04o", f->allocated / 1024.,
                 f->mode & 07777);
        clip_text(x + 14, y + 77, w - 28, 12, muted, s);
        snprintf(s, sizeof s, "UID %u | depth %d", f->uid, f->depth);
        label(x + 14, y + 102, 12, muted, s);
        char path[TOPOLOGY_PATH];
        if (files_path(file_frame, file_selected, path, sizeof path))
            clip_text(x + 14, y + 132, w - 28, 11, foreground, path);
        if (f->symlink) {
            snprintf(s, sizeof s, "Link -> %s", f->target);
            clip_text(x + 14, y + 160, w - 28, 11, orange, s);
        } else if (f->directory)
            button(x + 14, y + 150, w - 28, "Enter directory / Enter", cyan);
        if (f->boundary)
            clip_text(x + 14, y + 213, w - 28, 11, orange, "Mount boundary: enter to explore.");
        if (f->error)
            clip_text(x + 14, y + 240, w - 28, 11, orange, strerror(f->error));
        label(x + 14, y + h - 50, 11, muted, "Cyan: folders | purple: files");
        label(x + 14, y + h - 27, 11, muted, "Orange: selection / symlink");
    }
}
static void topology_view(int kind) {
    static unsigned scene_layout_revision;
    if (scene_layout_revision != layout_revision) {
        scene_fit = 1;
        graph_ready = 0;
        scene_layout_revision = layout_revision;
    }
    if (kind == 7 && !files_running)
        files_load(file_root);
    uint64_t epoch = kind == 6 ? snapshot->sequence : file_frame ? file_frame->serial : 0;
    if (!tree_gesture.active && (scene_kind != kind || scene_dirty || scene_epoch != epoch)) {
        if (scene_kind != kind)
            scene_fit = 1;
        scene_kind = kind;
        scene_epoch = epoch;
        tree_hover = -1;
        graph_ready = 0;
        if (kind == 6) {
            int old_count = scene.visible;
            float old_extent = scene.extent;
            process_scene_build();
            if (tree_follow &&
                (old_count != scene.visible || fabsf(old_extent - scene.extent) > .2f))
                scene_fit = 1;
            if (tree_follow && scene.count == 1 && !follow_exit_notified) {
                message("Followed process exited or is unavailable. Press A to return to all "
                        "processes.");
                follow_exit_notified = 1;
            }
        } else
            file_scene_build();
        scene_dirty = 0;
    }
    char s[TOPOLOGY_PATH + 128];
    clip_text(24, 148, kind == 6 && tree_follow ? width - 230 : width - 48, 20, foreground,
              kind == 6 ? "3D PROCESS TREE" : "3D FILESYSTEM");
    if (kind == 6) {
        if (tree_follow) {
            snprintf(s, sizeof s, "FOLLOW PID %d", tree_focus_pid);
            clip_text(width - 190, 148, 166, 11, cyan, s);
        }
        box(24, 166, width - 48, 40);
        snprintf(s, sizeof s, "%s%s", tree_query[0] ? tree_query : "Find process name or PID... /",
                 tree_query_focus ? "_" : "");
        clip_text(36, 192, width - 72, 13, tree_query_focus ? cyan : muted, s);
        button(24, 218, 116, "Focus / Enter", cyan);
        button(150, 218, 104, "Parent", muted);
        button(264, 218, 86, "All / A", muted);
        if (width >= 600)
            button(360, 218, 116, tree_mine ? "Mine: ON" : "Mine / U", tree_mine ? cyan : muted);
        snprintf(s, sizeof s, "%d shown / %d processes | PPID parent -> child",
                 scene.visible > 0 ? scene.visible - (!tree_focus_pid) : 0, view_count);
        if (width >= 640)
            button(486, 218, 104, tree_follow ? "Follow ON" : "Follow / L",
                   tree_follow ? cyan : muted);
        if (width >= 760)
            button(600, 218, 108, "Reset / G", muted);
        if (width >= 860)
            button(718, 218, 80, "Fit / R", muted);
        if (width > 1550)
            label(820, 240, 12, muted, s);
    } else {
        box(24, 166, width - 48, 40);
        snprintf(s, sizeof s, "%s%s",
                 file_input_focus ? file_input
                 : file_frame && file_frame->serial == file_request_serial && file_frame->root[0]
                     ? file_frame->root
                     : file_input,
                 file_input_focus ? "_" : "");
        clip_text(36, 192, width - 72, 13, file_input_focus ? cyan : muted, s);
        button(24, 218, 64, "Up", muted);
        button(98, 218, 80, "Home", muted);
        button(188, 218, 64, "/", muted);
        button(262, 218, 100, "Refresh", cyan);
        if (width > 600) {
            snprintf(s, sizeof s, "Depth %d", file_depth);
            button(372, 218, 98, s, muted);
            button(480, 218, 118, file_hidden ? "Hidden ON" : "Hidden OFF", muted);
        }
        if (file_frame && file_frame->serial == file_request_serial) {
            if (file_frame->error) {
                snprintf(s, sizeof s, "Cannot scan: %s", strerror(file_frame->error));
                clip_text(24, 258, width - 48, 11, red, s);
            } else {
                snprintf(s, sizeof s,
                         "%d entries | %d folders scanned | depth %d%s | symlinks unexpanded",
                         file_frame->count, file_frame->scanned, file_frame->depth,
                         file_frame->truncated ? " | LIMIT" : "");
                clip_text(24, 258, width - 48, 11, muted, s);
            }
        } else
            label(24, 258, 11, orange, "Scanning directory metadata in the background...");
    }
    if (kind == 7 && width >= 760)
        button(608, 218, 108, "Reset / G", muted);
    if (kind == 7 && width >= 860)
        button(726, 218, 80, "Fit / R", muted);
    topology_draw();
    topology_details();
    if (tree_focus_pid && kind == 6 && scene.count == 1)
        clip_text(40, 300, graph_w - 32, 13, orange,
                  "Focused process exited or is unavailable. Press A for all.");
    if (tree_hover >= 0 && tree_hover < scene.count) {
        double x, y, d;
        if (topology_project(&scene.nodes[tree_hover], &x, &y, &d)) {
            char name[128];
            clip_text(graph_x + 14, graph_y + 24, graph_w - 28, 13, orange,
                      topology_node_name(tree_hover, name, sizeof name));
        }
    }
}
static void topology_pointer_begin(int button, int x, int y, int modifiers);
static void topology_layout_reset(void);
static void topology_layout_undo(void);
static void topology_click(int x, int y) {
    if (width >= 760 && inside(x, y, tab == 6 ? 600 : 608, 218, 108, 34)) {
        topology_layout_reset();
        return;
    }
    if (width >= 860 && inside(x, y, tab == 6 ? 718 : 726, 218, 80, 34)) {
        scene_fit = 1;
        return;
    }
    if (tab == 6) {
        if (inside(x, y, 24, 166, width - 48, 40)) {
            tree_query_focus = 1;
            return;
        }
        tree_query_focus = 0;
        if (width >= 640 && inside(x, y, 486, 218, 104, 34))
            topology_follow_toggle();
        if (inside(x, y, 24, 218, 116, 34))
            topology_focus();
        if (inside(x, y, 150, 218, 104, 34))
            topology_parent();
        if (inside(x, y, 264, 218, 86, 34)) {
            tree_follow = 0;
            tree_focus_pid = 0;
            tree_mine = 0;
            scene_dirty = scene_fit = 1;
        }
        if (width >= 600 && inside(x, y, 360, 218, 116, 34)) {
            tree_follow = 0;
            tree_mine = !tree_mine;
            tree_focus_pid = 0;
            scene_dirty = scene_fit = 1;
        }
        if (width >= 1000 &&
            inside(x, y, topology_side_x() + 14, 433, topology_side_width() - 28, 34))
            inspector_open();
        if (width >= 1000 && inside(x, y, topology_side_x() + 14, 505, topology_side_width() - 28,
                                    tree_child_rows * 24)) {
            int row = (y - 505) / 24;
            if (row < tree_child_rows) {
                selected_pid = tree_child_pid[row];
                selected_start = tree_child_start[row];
            }
        }
    } else {
        if (inside(x, y, 24, 166, width - 48, 40)) {
            if (file_frame && file_frame->serial == file_request_serial && file_frame->root[0])
                snprintf(file_input, sizeof file_input, "%s", file_frame->root);
            file_input_focus = 1;
            return;
        }
        file_input_focus = 0;
        if (inside(x, y, 24, 218, 64, 34))
            files_parent();
        if (inside(x, y, 98, 218, 80, 34))
            files_load(getenv("HOME") ? getenv("HOME") : "/");
        if (inside(x, y, 188, 218, 64, 34))
            files_load("/");
        if (inside(x, y, 262, 218, 100, 34))
            files_load(file_frame && file_frame->serial == file_request_serial &&
                               file_frame->root[0]
                           ? file_frame->root
                           : file_input);
        if (width > 600 && inside(x, y, 372, 218, 98, 34)) {
            file_depth = file_depth % 4 + 1;
            files_load(file_frame && file_frame->serial == file_request_serial &&
                               file_frame->root[0]
                           ? file_frame->root
                           : file_input);
        }
        if (width > 600 && inside(x, y, 480, 218, 118, 34)) {
            file_hidden = !file_hidden;
            files_load(file_frame && file_frame->serial == file_request_serial &&
                               file_frame->root[0]
                           ? file_frame->root
                           : file_input);
        }
        if (width >= 1000 &&
            inside(x, y, topology_side_x() + 14, 414, topology_side_width() - 28, 34))
            files_enter();
    }
    if (inside(x, y, graph_x, graph_y, graph_w, graph_h))
        topology_pointer_begin(GLUT_LEFT_BUTTON, x, y,
                               smoke ? injected_modifiers : glutGetModifiers());
}
static void topology_gesture_cancel(void) {
    if (!tree_gesture.active)
        return;
    if (tree_gesture.mode == 1 && tree_gesture.moved) {
        if (tree_gesture.had_before)
            layout_set(&layout_editor, tree_gesture.layout, tree_gesture.previous);
        else
            layout_remove(&layout_editor, tree_gesture.layout);
        int i = tree_gesture.target.source;
        if (i >= 0 && i < scene.count) {
            scene.nodes[i].x = (float)tree_gesture.target.position[0];
            scene.nodes[i].y = (float)tree_gesture.target.position[1];
            scene.nodes[i].z = (float)tree_gesture.target.position[2];
        }
        scene_dirty = 1;
    }
    tree_yaw = tree_gesture.yaw;
    tree_pitch = tree_gesture.pitch;
    tree_distance = tree_gesture.distance;
    tree_pan_x = tree_gesture.pan_x;
    tree_pan_y = tree_gesture.pan_y;
    tree_gesture.active = 0;
    glutPostRedisplay();
}
static void topology_pointer_begin(int button, int x, int y, int modifiers) {
    if (button != GLUT_LEFT_BUTTON && button != GLUT_MIDDLE_BUTTON && button != GLUT_RIGHT_BUTTON)
        return;
    if (!presented.valid || presented.kind != tab ||
        !inside(x, y, graph_x, graph_y, graph_w, graph_h))
        return;
    if (tree_gesture.active)
        return;
    if (tab == 7 && (!file_frame || file_frame->serial != file_request_serial))
        return;
    memset(&tree_gesture, 0, sizeof tree_gesture);
    tree_gesture.active = 1;
    tree_gesture.button = button;
    tree_gesture.x = x;
    tree_gesture.y = y;
    tree_gesture.yaw = tree_yaw;
    tree_gesture.pitch = tree_pitch;
    tree_gesture.distance = tree_distance;
    tree_gesture.pan_x = tree_pan_x;
    tree_gesture.pan_y = tree_pan_y;
    pick_frame_begin(&tree_gesture.camera, presented.kind, presented.scope, presented.model,
                     presented.projection, presented.viewport, presented.window_height);
    int hit = pick_target(&presented, x, y);
    if (button == GLUT_RIGHT_BUTTON)
        tree_gesture.mode = 2;
    else if (button == GLUT_MIDDLE_BUTTON || (modifiers & GLUT_ACTIVE_SHIFT))
        tree_gesture.mode = 3;
    else if (button == GLUT_LEFT_BUTTON && hit >= 0) {
        tree_gesture.mode = 1;
        tree_gesture.target = presented.nodes[hit];
        tree_gesture.layout =
            (LayoutKey){presented.kind, presented.scope, presented.nodes[hit].key};
        tree_gesture.had_before =
            layout_get(&layout_editor, tree_gesture.layout, tree_gesture.previous);
        if (!topology_select_key(presented.kind, presented.nodes[hit].key)) {
            tree_gesture.active = 0;
            return;
        }
        tree_hover = presented.nodes[hit].source;
    } else {
        tree_gesture.mode = 3;
        tree_gesture.empty_click = button == GLUT_LEFT_BUTTON;
    }
    glutPostRedisplay();
}
static void topology_pointer_motion(int x, int y) {
    if (!tree_gesture.active)
        return;
    if (!tree_gesture.moved && !gesture_moved(tree_gesture.x, tree_gesture.y, x, y))
        return;
    if (!tree_gesture.moved) {
        tree_gesture.moved = 1;
        tree_follow = 0;
    }
    double dx = (double)x - tree_gesture.x, dy = (double)y - tree_gesture.y;
    if (tree_gesture.mode == 1) {
        double delta[3], p[3];
        if (!pick_drag_delta(&tree_gesture.camera, tree_gesture.target.position, tree_gesture.x,
                             tree_gesture.y, x, y, delta))
            return;
        for (int k = 0; k < 3; k++)
            p[k] = tree_gesture.target.position[k] + delta[k];
        if (!layout_set(&layout_editor, tree_gesture.layout, p)) {
            message("No more layout storage is available. Reset the layout to clear it.");
            return;
        }
        int i = tree_gesture.target.source;
        if (i >= 0 && i < scene.count) {
            scene.nodes[i].x = (float)p[0];
            scene.nodes[i].y = (float)p[1];
            scene.nodes[i].z = (float)p[2];
            topology_measure_scene();
        }
    } else if (tree_gesture.mode == 2) {
        tree_yaw = tree_gesture.yaw + (float)dx * .35f;
        tree_pitch = fmaxf(-85, fminf(85, tree_gesture.pitch + (float)dy * .35f));
    } else {
        double scale =
            2 * tree_gesture.distance * tan(.3926990817) / tree_gesture.camera.viewport[3];
        tree_pan_x = tree_gesture.pan_x + (float)(dx * scale);
        tree_pan_y = tree_gesture.pan_y - (float)(dy * scale);
    }
    glutPostRedisplay();
}
static void topology_release(int button, int x, int y) {
    if (!tree_gesture.active || button != tree_gesture.button)
        return;
    if (tree_gesture.moved)
        topology_pointer_motion(x, y);
    if (tree_gesture.mode == 1 && tree_gesture.moved) {
        double p[3];
        if (layout_get(&layout_editor, tree_gesture.layout, p))
            layout_commit(&layout_editor, tree_gesture.layout, tree_gesture.had_before,
                          tree_gesture.previous, p);
    } else if (tree_gesture.empty_click && !tree_gesture.moved) {
        if (tab == 6) {
            selected_pid = -1;
            selected_start = 0;
        } else {
            file_selected = -1;
            file_selection_valid = 0;
        }
    }
    tree_gesture.active = 0;
    glutPostRedisplay();
}
static void topology_layout_reset(void) {
    topology_gesture_cancel();
    layout_reset(&layout_editor, scene_kind, scene_scope);
    scene_dirty = scene_fit = 1;
}
static void topology_layout_undo(void) {
    if (layout_undo(&layout_editor, scene_kind, scene_scope)) {
        scene_dirty = 1;
        glutPostRedisplay();
    }
}
static void topology_passive(int x, int y) {
    if (confirm_fd < 0)
        history_hover(x, y);
    if ((tab == 6 || tab == 7) && !tree_gesture.active && !inspector.open && confirm_fd < 0) {
        int hit = topology_pick(x, y);
        if (hit != tree_hover) {
            tree_hover = hit;
            glutPostRedisplay();
        }
    }
}
static void keyboard(unsigned char k, int x, int y);
static void mouse(int b, int state, int x, int y);
static void motion(int x, int y);
static void advanced_smoke_before(void);
static void editor_smoke_before(void);
static void poll_views(void);
static int editor_phase;
static int advanced_phase;
static int manual_phase;
static void manual_smoke_before(void);
static void special(int k, int x, int y);
static int smoke_actions(void) {
    int ok = 1;
    tab = 3;
    search_focus = 0;
    cancel_signal();
    for (int i = 0; i < 2; i++) {
        pid_t child = fork();
        if (child < 0)
            return 0;
        if (child == 0) {
            signal(SIGTERM, SIG_DFL);
            for (;;)
                pause();
        }
        if (!sample()) {
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            return 0;
        }
        snprintf(search, sizeof search, "%d", child);
        filter_processes();
        process_scroll = 0;
        int row = -1;
        for (int j = 0; j < visible_count; j++)
            if (view_metrics.processes[indices[j]].pid == child)
                row = j;
        if (row < 0 || row >= process_rows()) {
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            return 0;
        }
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, 40, 260 + row * 29);
        keyboard(i ? 'k' : 't', 0, 0);
        if (confirm_fd < 0 || confirm_process.pid != child) {
            cancel_signal();
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            return 0;
        }
        /* Cancel must leave the target alive, then reopening must preserve identity. */
        keyboard(27, 0, 0);
        if (confirm_fd >= 0 || kill(child, 0))
            ok = 0;
        keyboard(i ? 'k' : 't', 0, 0);
        if (confirm_fd < 0 || confirm_process.pid != child) {
            cancel_signal();
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            return 0;
        }
        if (i)
            keyboard('\r', 0, 0);
        else {
            int w = width - 32;
            if (w > 640)
                w = 640;
            mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, (width - w) / 2 + w - 100, (height - 270) / 2 + 220);
        }
        int status;
        if (confirm_fd >= 0) {
            cancel_signal();
            kill(child, SIGKILL);
            ok = 0;
        }
        waitpid(child, &status, 0);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != (i ? SIGKILL : SIGTERM))
            ok = 0;
    }
    search[0] = 0;
    notice[0] = 0;
    selected_pid = -1;
    filter_processes();
    printf(
        "%s: process row selection, cancel, End button and Kill keyboard action on test children\n",
        ok ? "PASS" : "FAIL");
    return ok;
}
static char smoke_directory[TOPOLOGY_PATH];
static void smoke_directory_cleanup(void) {
    if (!smoke_directory[0])
        return;
    int fd = open(smoke_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        unlinkat(fd, "folder/file.txt", 0);
        unlinkat(fd, "readme.txt", 0);
        unlinkat(fd, "a-before.txt", 0);
        unlinkat(fd, ".hidden", 0);
        unlinkat(fd, "cycle", 0);
        unlinkat(fd, "folder", AT_REMOVEDIR);
        close(fd);
    }
    rmdir(smoke_directory);
}
static int topology_smoke_pick(void) {
    if (topology_pick(-500, -500) != -1)
        return 0;
    for (int i = 0; i < scene.count; i++) {
        if (scene.nodes[i].depth < 0 || (scene_kind == 6 && !scene.nodes[i].id))
            continue;
        double x, y, d;
        if (!topology_project(&scene.nodes[i], &x, &y, &d))
            continue;
        if (topology_pick((int)lround(x), (int)lround(y)) == i) {
            topology_select(i);
            return 1;
        }
    }
    return 0;
}
static int smoke_confirm_test;
static void smoke_step(void) {
    /* Traverse every view, narrow layouts, sorting, filtering and signal dialog.
       The render test never sends a signal to a real user process. */
    if (frames == 0) {
        tab = 0;
    }
    if (frames == 1) {
        keyboard('2', 0, 0);
        if (tab != 1)
            ui_failed = 1;
    }
    if (frames == 2) {
        keyboard('3', 0, 0);
        if (tab != 2)
            ui_failed = 1;
    }
    if (frames == 3) {
        gpu_3d = 1;
    }
    if (frames == 4) {
        tab = 3;
        sort_column = 3;
        filter_processes();
        if (visible_count > 0) {
            const Process *p = &view_metrics.processes[indices[0]];
            selected_pid = p->pid;
            selected_start = p->start;
        }
    }
    if (frames == 5) {
        snprintf(search, sizeof search, "lsm-no-matches-xyz");
        filter_processes();
        if (visible_count)
            ui_failed = 1;
        search[0] = 0;
    }
    if (frames == 6 && !smoke_confirm_test) {
        for (int i = 0; i < view_count; i++)
            if (view_metrics.processes[i].pid > 1 && view_metrics.processes[i].pid != getpid() &&
                view_metrics.processes[i].uid == getuid()) {
                selected_pid = view_metrics.processes[i].pid;
                selected_start = view_metrics.processes[i].start;
                begin_signal(SIGTERM);
                break;
            }
        smoke_confirm_test = 1;
    }
    if (frames == 7) {
        cancel_signal();
        glutReshapeWindow(1000, 720);
        tab = 1;
    }
    if (frames == 8) {
        tab = 2;
        gpu_scroll = 500;
    }
    if (frames == 9)
        tab = 3;
    if (frames == 10) {
        glutFullScreen();
        tab = 0;
    }
    if (frames == 11)
        tab = 1;
    if (frames == 12) {
        tab = 2;
        gpu_3d = 0;
        gpu_scroll = 0;
    }
    if (frames == 13)
        tab = 3;
    if (frames == 14) {
        begin_signal(SIGKILL);
    }
    if (frames == 16) {
        keyboard('5', 0, 0);
        if (tab != 4)
            ui_failed = 1;
    }
    if (frames == 17)
        pressure_scroll = 300;
    if (frames == 18) {
        tab = 3;
        filter_processes();
        if (visible_count) {
            const Process *p = &view_metrics.processes[indices[0]];
            selected_pid = p->pid;
            selected_start = p->start;
        }
        keyboard('i', 0, 0);
        if (!inspector.open)
            ui_failed = 1;
    }
    if (frames == 19) {
        keyboard('2', 0, 0);
        if (inspector.metric != 1)
            ui_failed = 1;
    }
    if (frames == 20) {
        keyboard('3', 0, 0);
        if (inspector.metric != 2)
            ui_failed = 1;
    }
    if (frames == 22) {
        uint64_t held = snapshot->sequence;
        keyboard(' ', 0, 0);
        if (!paused || !sampler_refresh(3000) || snapshot->sequence != held)
            ui_failed = 1;
        keyboard(' ', 0, 0);
        if (paused || snapshot->sequence <= held)
            ui_failed = 1;
    }
    if (frames == 23) {
        keyboard('i', 0, 0);
        int before = inspector.history_count;
        if (!inspector.open || !sample())
            ui_failed = 1;
        inspector_track();
        if (inspector.history_count <= before)
            ui_failed = 1;
        keyboard(27, 0, 0);
        tab = 4;
        pressure_scroll = 0;
        puts(ui_failed ? "FAIL: pressure navigation, process inspector histories and frozen view"
                       : "PASS: pressure navigation, process inspector histories and frozen view");
    }
    if (frames == 25) {
        keyboard('6', 0, 0);
        if (tab != 5)
            ui_failed = 1;
    }
    if (frames == 26) {
        if (!sample())
            ui_failed = 1;
        selected_pid = getpid();
        for (int i = 0; i < view_count; i++)
            if (view_metrics.processes[i].pid == getpid())
                selected_start = view_metrics.processes[i].start;
        inspector_open();
        keyboard('m', 0, 0);
        for (int i = 0; i < 4 && (!inspector_report() || !inspector_report()->available); i++)
            if (!sample())
                ui_failed = 1;
        if (!inspector_report() || !inspector_report()->available || !inspector_report()->stored)
            ui_failed = 1;
    }
    if (frames == 27) {
        static const unsigned char probe[MEMORY_BYTES] =
            "lsm byte-view test: controlled bytes, not other application memory.";
        const MemoryReport *m = inspector_report();
        uint64_t address = (uint64_t)(uintptr_t)probe;
        region_chosen = 0;
        if (m)
            for (int i = 0; i < m->stored; i++)
                if (address >= m->regions[i].begin && address < m->regions[i].end &&
                    sizeof probe <= m->regions[i].end - address) {
                    chosen_region = m->regions[i];
                    region_chosen = 1;
                    break;
                }
        inspector_request_bytes(address);
        for (int i = 0; i < 4 && snapshot->bytes.serial != byte_serial; i++)
            if (!sample())
                ui_failed = 1;
        if (!region_chosen || snapshot->bytes.error || snapshot->bytes.count != sizeof probe ||
            memcmp(snapshot->bytes.data, probe, sizeof probe))
            ui_failed = 1;
    }
    if (frames == 28) {
        inspector_close();
        tab = 5;
        puts(ui_failed ? "FAIL: RAM view, memory maps and known-byte viewer"
                       : "PASS: RAM view, memory maps and known-byte viewer");
    }
    if (frames == 30) {
        keyboard('7', 0, 0);
        if (tab != 6)
            ui_failed = 1;
    }
    if (frames == 32) {
        keyboard('\r', 0, 0);
        if (tree_focus_pid != selected_pid)
            ui_failed = 1;
    }
    if (frames == 33) {
        int before = tree_mine;
        topology_click(380, 230);
        if (width >= 600 && tree_mine == before)
            ui_failed = 1;
        keyboard('a', 0, 0);
        if (tree_mine)
            ui_failed = 1;
        if (tree_focus_pid)
            ui_failed = 1;
    }
    if (frames == 34) {
        snprintf(smoke_directory, sizeof smoke_directory, "/tmp/lsm-3d-ui-XXXXXX");
        if (!mkdtemp(smoke_directory)) {
            ui_failed = 1;
            smoke_directory[0] = 0;
        } else {
            int fd = open(smoke_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0)
                ui_failed = 1;
            else {
                if (mkdirat(fd, "folder", 0700))
                    ui_failed = 1;
                const char *names[] = {"readme.txt", "folder/file.txt", ".hidden"};
                for (int i = 0; i < 3; i++) {
                    int file = openat(fd, names[i], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                    if (file < 0)
                        ui_failed = 1;
                    else {
                        if (write(file, "3D metadata test", 16) != 16)
                            ui_failed = 1;
                        close(file);
                    }
                }
                if (symlinkat(".", fd, "cycle"))
                    ui_failed = 1;
                close(fd);
            }
            files_load(smoke_directory);
            if (!files_wait(file_request_serial, 3000))
                ui_failed = 1;
            file_frame = files_poll();
        }
        keyboard('8', 0, 0);
        if (tab != 7 || !file_frame || file_frame->error)
            ui_failed = 1;
    }
    if (frames == 36 && file_frame) {
        for (int i = 1; i < file_frame->count; i++)
            if (!strcmp(file_frame->nodes[i].name, "folder"))
                file_selected = i;
        keyboard('\r', 0, 0);
        if (!files_wait(file_request_serial, 3000))
            ui_failed = 1;
        file_frame = files_poll();
        if (!strstr(file_frame->root, "/folder"))
            ui_failed = 1;
    }
    if (frames == 37 && file_frame) {
        keyboard(8, 0, 0);
        if (!files_wait(file_request_serial, 3000))
            ui_failed = 1;
        file_frame = files_poll();
        if (strcmp(file_frame->root, smoke_directory))
            ui_failed = 1;
        keyboard('h', 0, 0);
        if (!files_wait(file_request_serial, 3000))
            ui_failed = 1;
        file_frame = files_poll();
        if (!file_frame->hidden)
            ui_failed = 1;
    }
    if (frames == 39) {
        glutLeaveFullScreen();
        glutReshapeWindow(900, 700);
        tab = 6;
        scene_fit = scene_dirty = 1;
    }
    if (frames == 21) {
        keyboard(27, 0, 0);
        if (inspector.open)
            ui_failed = 1;
    }
    if (frames == 15) {
        cancel_signal();
        if (!smoke_actions())
            ui_failed = 1;
    }
    if (frames >= 42) {
        if (advanced_phase < 10)
            advanced_smoke_before();
        else if (editor_phase < 12)
            editor_smoke_before();
        else
            manual_smoke_before();
    }
}
static void advanced_smoke_before(void) {
    if (frames > 260 && advanced_phase < 10) {
        ui_failed = 1;
        advanced_phase = 10;
        puts("FAIL: advanced UI could not obtain a usable window");
        return;
    }
    if (advanced_phase == 0) {
        glutFullScreen();
        advanced_phase = 1;
        return;
    }
    if (advanced_phase == 1) {
        if (width < 1000 || height < 720)
            return;
        inspector_close();
        search_focus = tree_query_focus = file_input_focus = 0;
        tab = 0;
        if (!paused)
            keyboard(' ', 0, 0);
        history_clear();
        advanced_phase = 2;
        return;
    }
    if (advanced_phase == 2) {
        if (chart_hit_count < 2 || chart_hit_tab != 0) {
            ui_failed = 1;
            advanced_phase = 10;
            return;
        }
        ChartHit c = chart_hits[0];
        int x = c.x + c.w - 16, y = c.y + 46;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, y);
        if (!history_pinned || !history_cursor_valid ||
            history_cursor_time != c.times[(c.cursor + HISTORY - 1) % HISTORY])
            ui_failed = 1;
        int old = history_window;
        keyboard('v', 0, 0);
        if (history_window != history_next_window(old))
            ui_failed = 1;
        advanced_phase = 3;
        return;
    }
    if (advanced_phase == 3) {
        int x, y, h;
        if (panel_divider(&x, &y, &h) != 1)
            ui_failed = 1;
        float old = overview_ratio;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y + 20);
        motion(x + 90, y + 20);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, x + 90, y + 20);
        if (overview_ratio == old || panel_drag)
            ui_failed = 1;
        advanced_phase = 4;
        return;
    }
    if (advanced_phase == 4) {
        tab = 6;
        int index = process_index(1);
        if (index < 0)
            index = process_index(getpid());
        if (index < 0) {
            ui_failed = 1;
            advanced_phase = 10;
            return;
        }
        selected_pid = view_metrics.processes[index].pid;
        selected_start = view_metrics.processes[index].start;
        keyboard('l', 0, 0);
        if (!tree_follow || tree_focus_pid != selected_pid || tree_focus_start != selected_start)
            ui_failed = 1;
        advanced_phase = 5;
        return;
    }
    if (advanced_phase == 5) {
        int x, y, h;
        if (panel_divider(&x, &y, &h) != 3)
            ui_failed = 1;
        int old = tree_panel_size;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y + 20);
        motion(x - 90, y + 20);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, x - 90, y + 20);
        if (tree_panel_size == old || panel_drag)
            ui_failed = 1;
        const Process *p = selection();
        const FamilySummary *f = p ? topology_family(p) : NULL;
        if (!f || f->members < 1 || f->cpu < p->cpu || f->rss < p->rss_kib / 1024.)
            ui_failed = 1;
        uint64_t identity = tree_focus_start;
        tree_focus_start++;
        process_scene_build();
        if (scene.count != 1)
            ui_failed = 1;
        tree_focus_start = identity;
        scene_dirty = 1;
        keyboard('l', 0, 0);
        if (tree_follow)
            ui_failed = 1;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, 510, 230);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, 510, 230);
        if (!tree_follow)
            ui_failed = 1;
        advanced_phase = 6;
        return;
    }
    if (advanced_phase == 6) {
        keyboard('c', 0, 0);
        if (history_pinned || history_cursor_valid)
            ui_failed = 1;
        tab = 7;
        advanced_phase = 7;
        return;
    }
    if (advanced_phase == 7) {
        int x, y, h;
        if (panel_divider(&x, &y, &h) != 4)
            ui_failed = 1;
        int old = file_panel_size, other = tree_panel_size;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y + 20);
        motion(x - 60, y + 20);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, x - 60, y + 20);
        if (file_panel_size == old || tree_panel_size != other || panel_drag)
            ui_failed = 1;
        tab = 2;
        gpu_3d = 0;
        advanced_phase = 8;
        return;
    }
    if (advanced_phase == 8) {
        if (view_metrics.gpu_count) {
            int x, y, h;
            if (panel_divider(&x, &y, &h) != 2)
                ui_failed = 1;
            float old = gpu_ratio;
            mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y + 24);
            motion(x - 70, y + 24);
            mouse(GLUT_LEFT_BUTTON, GLUT_UP, x - 70, y + 24);
            if (gpu_ratio == old || panel_drag)
                ui_failed = 1;
        }
        gpu_scroll = 500;
        advanced_phase = 9;
        return;
    }
    if (advanced_phase == 9) {
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, 50, 80);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, 50, 80);
        if (tab != 0)
            ui_failed = 1;
        advanced_phase = 10;
        printf("%s: shared cursor pin/clear, history windows, all panel dividers, family totals, "
               "Follow identity and controls\n",
               ui_failed ? "FAIL" : "PASS");
    }
}
static PickNode editor_test_node;
static LayoutKey editor_test_key;
static double editor_test_position[3], editor_test_screen[2];
static int editor_test_x, editor_test_y;
static float editor_test_yaw, editor_test_pitch;
static int editor_find_target(int kind, NodeKey key, PickNode *out) {
    if (presented.kind != kind)
        return 0;
    for (int i = 0; i < presented.count; i++)
        if (node_key_equal(presented.nodes[i].key, key)) {
            *out = presented.nodes[i];
            return 1;
        }
    return 0;
}
static void editor_smoke_before(void) {
    if (frames > 340 && editor_phase < 12) {
        ui_failed = 1;
        editor_phase = 12;
        puts("FAIL: editor tests timed out");
        return;
    }
    if (editor_phase == 0) {
        inspector_close();
        tab = 6;
        paused = 0;
        injected_modifiers = 0;
        if (!sample())
            ui_failed = 1;
        int index = process_index(getpid());
        if (index < 0) {
            ui_failed = 1;
            editor_phase = 12;
            return;
        }
        selected_pid = getpid();
        selected_start = view_metrics.processes[index].start;
        topology_focus();
        editor_test_key = (LayoutKey){6, 0, {(uint64_t)selected_pid, selected_start}};
        editor_phase = 1;
        return;
    }
    if (editor_phase == 1) {
        if (!editor_find_target(6, editor_test_key.node, &editor_test_node)) {
            ui_failed = 1;
            editor_phase = 12;
            return;
        }
        editor_test_key.scope = presented.scope;
        editor_test_x = (int)lround(editor_test_node.sx);
        editor_test_y = (int)lround(editor_test_node.sy);
        editor_test_yaw = tree_yaw;
        editor_test_pitch = tree_pitch;
        int count = layout_editor.count;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, editor_test_x, editor_test_y);
        motion(editor_test_x + 3, editor_test_y + 3);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, editor_test_x + 3, editor_test_y + 3);
        if (tree_gesture.active || tree_yaw != editor_test_yaw || tree_pitch != editor_test_pitch ||
            layout_editor.count != count || selected_pid != (int)editor_test_key.node.a)
            ui_failed = 1;
        for (int i = 0; i < presented.label_count; i++) {
            PickLabel *label = &presented.labels[i];
            if (node_key_equal(presented.nodes[label->target].key, editor_test_key.node)) {
                int lx = (int)(label->x + 5), ly = (int)(label->y + 8);
                mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, lx, ly);
                mouse(GLUT_LEFT_BUTTON, GLUT_UP, lx, ly);
                if (selected_pid != (int)editor_test_key.node.a ||
                    selected_start != editor_test_key.node.b)
                    ui_failed = 1;
                break;
            }
        }
        editor_phase = 2;
        return;
    }
    if (editor_phase == 2) {
        /* Reproduce the original bug: publication arrives after rendering and
           before the press, then another update arrives before release. */
        if (!sample())
            ui_failed = 1;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, editor_test_x, editor_test_y);
        if (!tree_gesture.active || tree_gesture.mode != 1 ||
            selected_pid != (int)editor_test_key.node.a)
            ui_failed = 1;
        if (!sample())
            ui_failed = 1;
        motion(editor_test_x + 70, editor_test_y + 35);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, editor_test_x + 70, editor_test_y + 35);
        if (!layout_get(&layout_editor, editor_test_key, editor_test_position) ||
            tree_gesture.active || tree_yaw != editor_test_yaw || tree_pitch != editor_test_pitch ||
            selected_start != editor_test_key.node.b)
            ui_failed = 1;
        double x, y, z, eye;
        if (!pick_project(&presented, editor_test_position, &x, &y, &z, &eye) ||
            fabs(x - (editor_test_node.sx + 70)) > .75 ||
            fabs(y - (editor_test_node.sy + 35)) > .75)
            ui_failed = 1;
        scene_dirty = 1;
        editor_phase = 3;
        return;
    }
    if (editor_phase == 3) {
        PickNode node;
        if (!editor_find_target(6, editor_test_key.node, &node) ||
            fabs(node.sx - (editor_test_node.sx + 70)) > .75 ||
            fabs(node.sy - (editor_test_node.sy + 35)) > .75)
            ui_failed = 1;
        int x = graph_x + graph_w / 2, y = graph_y + graph_h / 2;
        mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
        motion(x + 50, y - 20);
        mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x + 50, y - 20);
        double p[3];
        if (tree_gesture.active || tree_yaw == editor_test_yaw || tree_pitch == editor_test_pitch ||
            !layout_get(&layout_editor, editor_test_key, p) ||
            memcmp(p, editor_test_position, sizeof p))
            ui_failed = 1;
        editor_test_yaw = tree_yaw;
        editor_test_pitch = tree_pitch;
        editor_phase = 4;
        return;
    }
    if (editor_phase == 4) {
        int x = graph_x + graph_w / 2, y = graph_y + graph_h / 2;
        float pan = tree_pan_x;
        mouse(GLUT_MIDDLE_BUTTON, GLUT_DOWN, x, y);
        motion(x + 60, y + 30);
        mouse(GLUT_MIDDLE_BUTTON, GLUT_UP, x + 60, y + 30);
        if (tree_pan_x == pan || tree_yaw != editor_test_yaw || tree_pitch != editor_test_pitch ||
            tree_gesture.active)
            ui_failed = 1;
        editor_phase = 5;
        return;
    }
    if (editor_phase == 5) {
        int x = graph_x + graph_w / 2, y = graph_y + graph_h / 2;
        float pan = tree_pan_x;
        injected_modifiers = GLUT_ACTIVE_SHIFT;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
        motion(x - 20, y + 10);
        uint64_t held = snapshot->sequence;
        if (!sampler_refresh(3000))
            ui_failed = 1;
        poll_views();
        if (snapshot->sequence != held)
            ui_failed = 1;
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, x - 20, y + 10);
        injected_modifiers = 0;
        poll_views();
        if (tree_pan_x == pan || tree_gesture.active || snapshot->sequence <= held ||
            selected_pid != (int)editor_test_key.node.a)
            ui_failed = 1;
        editor_phase = 6;
        return;
    }
    if (editor_phase == 6) {
        PickNode node;
        if (!editor_find_target(6, editor_test_key.node, &node)) {
            ui_failed = 1;
            editor_phase = 12;
            return;
        }
        int x = (int)lround(node.sx), y = (int)lround(node.sy);
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
        motion(x - 40, y + 20);
        keyboard(27, 0, 0);
        double p[3];
        if (tree_gesture.active || !layout_get(&layout_editor, editor_test_key, p) ||
            memcmp(p, editor_test_position, sizeof p))
            ui_failed = 1;
        editor_phase = 7;
        return;
    }
    if (editor_phase == 7) {
        keyboard(26, 0, 0);
        double p[3];
        if (layout_get(&layout_editor, editor_test_key, p))
            ui_failed = 1;
        keyboard('r', 0, 0);
        editor_phase = 8;
        return;
    }
    if (editor_phase == 8) {
        tab = 7;
        files_load(smoke_directory);
        if (!files_wait(file_request_serial, 3000))
            ui_failed = 1;
        file_frame = files_poll();
        editor_phase = 9;
        return;
    }
    if (editor_phase == 9) {
        char path[TOPOLOGY_PATH];
        size_t root_length = strlen(smoke_directory);
        if (root_length + sizeof "/readme.txt" > sizeof path) {
            ui_failed = 1;
            editor_phase = 12;
            return;
        }
        memcpy(path, smoke_directory, root_length);
        memcpy(path + root_length, "/readme.txt", sizeof "/readme.txt");
        NodeKey key = node_path_key(path);
        PickNode node;
        if (!editor_find_target(7, key, &node)) {
            ui_failed = 1;
            editor_phase = 12;
            return;
        }
        editor_test_key = (LayoutKey){7, presented.scope, key};
        int x = (int)lround(node.sx), y = (int)lround(node.sy);
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
        motion(x + 55, y - 25);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, x + 55, y - 25);
        if (!layout_get(&layout_editor, editor_test_key, editor_test_position))
            ui_failed = 1;
        double screen_depth, screen_eye;
        pick_project(&presented, editor_test_position, &editor_test_screen[0],
                     &editor_test_screen[1], &screen_depth, &screen_eye);
        struct stat st;
        if (stat(path, &st) || st.st_size != 16)
            ui_failed = 1;
        int root = open(smoke_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (root < 0)
            ui_failed = 1;
        else {
            int fd = openat(root, "a-before.txt", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0)
                ui_failed = 1;
            else
                close(fd);
            close(root);
        }
        files_load(smoke_directory);
        if (!files_wait(file_request_serial, 3000))
            ui_failed = 1;
        file_frame = files_poll();
        editor_phase = 10;
        return;
    }
    if (editor_phase == 10) {
        PickNode node;
        if (!editor_find_target(7, editor_test_key.node, &node) || !file_selection_valid ||
            !node_key_equal(file_selection_key, editor_test_key.node) ||
            fabs(node.sx - editor_test_screen[0]) > .75 ||
            fabs(node.sy - editor_test_screen[1]) > .75)
            ui_failed = 1;
        else
            for (int k = 0; k < 3; k++)
                if (fabs(node.position[k] - editor_test_position[k]) > 1e-5)
                    ui_failed = 1;
        keyboard('g', 0, 0);
        double p[3];
        if (layout_get(&layout_editor, editor_test_key, p))
            ui_failed = 1;
        editor_phase = 11;
        return;
    }
    if (editor_phase == 11) {
        editor_phase = 12;
        printf("%s: selection across updates, jitter, direct node drag/persistence, orbit/pan, "
               "capture, cancel, undo and file refresh\n",
               ui_failed ? "FAIL" : "PASS");
    }
}
static void manual_smoke_before(void) {
    if (manual_phase == 0) {
        keyboard('9', 0, 0);
        if (tab != MANUAL_VIEW)
            ui_failed = 1;
        manual_scroll = 0;
        manual_phase = 1;
        return;
    }
    if (manual_phase == 1) {
        float distance = tree_distance;
        mouse(4, GLUT_DOWN, 100, 300);
        if (manual_scroll <= 0 || tree_distance != distance)
            ui_failed = 1;
        special(GLUT_KEY_END, 0, 0);
        if (manual_scroll != manual_max_scroll || !manual_scroll)
            ui_failed = 1;
        int x, y, h;
        if (panel_divider(&x, &y, &h))
            ui_failed = 1;
        manual_phase = 2;
        return;
    }
    if (manual_phase == 2) {
        special(GLUT_KEY_HOME, 0, 0);
        if (manual_scroll)
            ui_failed = 1;
        special(GLUT_KEY_PAGE_DOWN, 0, 0);
        if (manual_scroll <= 0)
            ui_failed = 1;
        special(GLUT_KEY_PAGE_UP, 0, 0);
        if (manual_scroll)
            ui_failed = 1;
        int nav = (width - 48) / VIEW_COUNT;
        if (nav > 155)
            nav = 155;
        mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, 24 + 8 * nav + nav / 2, 80);
        mouse(GLUT_LEFT_BUTTON, GLUT_UP, 24 + 8 * nav + nav / 2, 80);
        if (tab != MANUAL_VIEW)
            ui_failed = 1;
        glutLeaveFullScreen();
        glutReshapeWindow(640, 540);
        manual_phase = 3;
        return;
    }
    if (manual_phase == 3) {
        /* Tiling window managers may ignore glutReshapeWindow. Check wrapping
           at a narrow width independently of whether the request was honored. */
        int saved_width = width;
        int content_height = manual_content(0);
        width = 640;
        int narrow_height = manual_content(0);
        width = saved_width;
        if (saved_width >= 640 && narrow_height < content_height)
            ui_failed = 1;
        manual_bounds();
        if (manual_scroll || manual_max_scroll <= 0)
            ui_failed = 1;
        manual_phase = 4;
        printf(
            "%s: Manual tab keyboard/click navigation, scrolling, bounds and responsive wrapping\n",
            ui_failed ? "FAIL" : "PASS");
    }
}
static void capture_frame(void) {
    const char *directory = getenv("LSM_CAPTURE_DIR");
    if (!directory || !smoke)
        return;
    if (frames != 0 && frames != 1 && frames != 2 && frames != 4 && frames != 6 && frames != 8 &&
        frames != 11 && frames != 12 && frames != 13 && frames != 14 && frames != 16 &&
        frames != 17 && frames != 18 && frames != 19 && frames != 20 && frames != 25 &&
        frames != 26 && frames != 27 && frames != 30 && frames != 32 && frames != 34 &&
        frames != 36 && frames != 38 && frames != 40 &&
        !(frames >= 42 && advanced_phase >= 2 && advanced_phase <= 9) &&
        !(editor_phase >= 1 && editor_phase <= 11) && !manual_phase)
        return;
    size_t bytes = (size_t)width * height * 3;
    unsigned char *pixels = malloc(bytes);
    if (!pixels)
        return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    char path[4096];
    if (manual_phase)
        snprintf(path, sizeof path, "%s/lsm-manual-%d.ppm", directory, manual_phase);
    else if (editor_phase >= 1)
        snprintf(path, sizeof path, "%s/lsm-editor-%d.ppm", directory, editor_phase);
    else if (frames >= 42)
        snprintf(path, sizeof path, "%s/lsm-advanced-%d.ppm", directory, advanced_phase);
    else
        snprintf(path, sizeof path, "%s/lsm-%02d.ppm", directory, frames);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        for (int y = height - 1; y >= 0; y--)
            fwrite(pixels + (size_t)y * width * 3, 1, (size_t)width * 3, f);
        fclose(f);
    }
    free(pixels);
}
static void display(void) {
    if (smoke)
        smoke_step();
    glClearColor(.025f, .035f, .055f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    overlay();
    chart_hit_count = 0;
    chart_hit_tab = tab;
    chart_hit_sequence = snapshot->sequence;
    chart_hit_modal = inspector.open;
    header();
    if (width < 400 || height < 440) {
        clip_text(16, 155, width - 32, 12, foreground, "Enlarge window or press F for fullscreen");
    } else
        switch (tab) {
        case 0:
            overview();
            break;
        case 1:
            cpu_view();
            break;
        case 2:
            gpu_view();
            break;
        case 3:
            process_view();
            break;
        case 4:
            pressure_view();
            break;
        case 5:
            ram_view();
            break;
        case 6:
            topology_view(6);
            break;
        case 7:
            topology_view(7);
            break;
        case MANUAL_VIEW:
            manual_view();
            break;
        }
    const char *help = tab == 3 ? "Click headers: sort  |  /: search  |  Wheel: scroll  |  T: end  "
                                  "K: kill | I: inspect | Space: freeze"
                                : "1-9: views | F: fullscreen | Drag: orbit | Wheel: scroll / zoom "
                                  "| Space: freeze | R: reset";
    if (width < 900)
        help = tab == 3   ? "/: search | I: inspect | T: end | K: kill"
               : tab == 2 ? "Wheel: scroll | +/-: zoom | F: fullscreen"
                          : "Wheel: scroll | F: fullscreen | 1-9: views";
    if (tab < 6 && chart_hit_count)
        help = "Hover graph: sample | Click: pin/unpin | C: clear | V: 1/5/15m | Drag divider: "
               "resize | Space: freeze";
    if (tab == MANUAL_VIEW)
        help = "9: manual | 1-8: monitor views | F: fullscreen | Escape: exit";
    clip_text(24, height - 12, width - 48, 11, muted, help);
    if (notice[0] && seconds() < notice_until) {
        color((Color){.14f, .12f, .08f});
        rect(24, height - 157, width - 48, 38);
        clip_text(36, height - 133, width - 72, 13, orange, notice);
    }
    if (topology_tab()) {
        color((Color){.025f, .035f, .055f});
        rect(0, height - 28, width, 28);
        clip_text(24, height - 12, width - 48, 11, muted,
                  tab == 6 ? "Left: select/move | Right: orbit | Shift/middle: pan | Ctrl+Z: undo "
                             "| G: reset layout | R: fit"
                           : "Left: select/move | Right: orbit | Shift/middle: pan | Ctrl+Z: undo "
                             "| Enter: folder | G: reset");
    }
    panel_draw();
    if (inspector.open)
        chart_hit_count = 0;
    inspector_view();
    dialog();
    if (smoke && (frames == 31 || frames == 35) && !topology_smoke_pick())
        ui_failed = 1;
    if (smoke && frames == 40)
        printf("%s: process/file 3D picking, subtree focus, folder entry, parent navigation and "
               "hidden files\n",
               ui_failed ? "FAIL" : "PASS");
    capture_frame();
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error %u on tab %d\n", error, tab);
        ui_failed = 1;
    }
    glutSwapBuffers();
    if (smoke && ++frames >= 42 && advanced_phase >= 10 && editor_phase >= 12 &&
        manual_phase >= 4) {
        printf("%s: all views, 3D graphs, process filter, selection dialog and resize rendering\n",
               ui_failed ? "FAIL" : "PASS");
        glutLeaveMainLoop();
    }
}
static void poll_views(void) {
    if (!paused && !tree_gesture.active) {
        snapshot = sampler_poll();
        if (files_running)
            file_frame = files_poll();
    }
    inspector_track();
}
static void timer(int n) {
    (void)n;
    poll_views();
    glutPostRedisplay();
    glutTimerFunc(16, timer, 0);
}
static void reshape(int w, int h) {
    topology_gesture_cancel();
    presented.valid = 0;
    panel_drag = 0;
    layout_revision++;
    width = w > 0 ? w : 1;
    height = h > 0 ? h : 1;
}
static void reset_camera(void) {
    yaw = -35;
    pitch = 40;
    zoom = 26;
}
static void keyboard(unsigned char k, int x, int y) {
    (void)x;
    (void)y;
    if (confirm_fd >= 0) {
        if (k == 27)
            cancel_signal();
        else if (k == '\r')
            accept_signal();
        glutPostRedisplay();
        return;
    }
    if (tree_gesture.active) {
        if (k == 27 || k == 26)
            topology_gesture_cancel();
        return;
    }
    if (inspector.open) {
        if (k == 'v' || k == 'V')
            history_window = history_next_window(history_window);
        if (k == 'c' || k == 'C')
            history_clear();
        if (k == 27 || k == 'i' || k == 'I')
            inspector_close();
        if (k >= '1' && k <= '4')
            inspector_set_metric(k - '1');
        if (k == '5' || k == 'b' || k == 'B')
            inspector_request_bytes(region_chosen ? chosen_region.begin : 0);
        if (k == 'm' || k == 'M')
            inspector_set_metric(3);
        if (k == 's' || k == 'S') {
            map_sort = !map_sort;
            map_scroll = 0;
        }
        if (k == 't' || k == 'T')
            begin_signal(SIGTERM);
        if (k == 'k' || k == 'K')
            begin_signal(SIGKILL);
        if (k == ' ') {
            paused = !paused;
            if (!paused) {
                snapshot = sampler_poll();
                inspector_track();
            }
        }
        glutPostRedisplay();
        return;
    }
    if ((tab == 6 && tree_query_focus) || (tab == 7 && file_input_focus)) {
        char *buffer = tab == 6 ? tree_query : file_input;
        size_t capacity = tab == 6 ? sizeof tree_query : sizeof file_input;
        if (k == 27) {
            tree_query_focus = file_input_focus = 0;
        } else if (k == '\r') {
            if (tab == 6)
                topology_find();
            else {
                files_load(file_input);
                file_input_focus = 0;
            }
        } else if (k == 8 || k == 127) {
            size_t n = strlen(buffer);
            if (n)
                buffer[n - 1] = 0;
        } else if (k >= 32 && k < 127 && strlen(buffer) < capacity - 1) {
            size_t n = strlen(buffer);
            buffer[n] = k;
            buffer[n + 1] = 0;
        }
        glutPostRedisplay();
        return;
    }
    if (search_focus && tab == 3) {
        if (k == 27 || k == '\r')
            search_focus = 0;
        else if (k == 8 || k == 127) {
            size_t n = strlen(search);
            if (n)
                search[n - 1] = 0;
            process_scroll = 0;
        } else if (k >= 32 && k < 127 && strlen(search) < sizeof search - 1) {
            size_t n = strlen(search);
            search[n] = k;
            search[n + 1] = 0;
            process_scroll = 0;
        }
        glutPostRedisplay();
        return;
    }
    if (k == 'v' || k == 'V')
        history_window = history_next_window(history_window);
    if (k == 'c' || k == 'C')
        history_clear();
    if (k >= '1' && k <= '9') {
        tab = k - '1';
        search_focus = 0;
    }
    if (k == 27 && topology_tab()) {
        selected_pid = -1;
        selected_start = 0;
        file_selected = -1;
        file_selection_valid = 0;
        tree_hover = -1;
        glutPostRedisplay();
        return;
    }
    if (k == 27)
        glutLeaveMainLoop();
    if (k == ' ') {
        paused = !paused;
        if (!paused)
            snapshot = sampler_poll();
        inspector_track();
    }
    if (k == 'r' || k == 'R') {
        if (topology_tab()) {
            tree_yaw = -25;
            tree_pitch = 22;
            scene_fit = 1;
        } else
            reset_camera();
    }
    if (topology_tab()) {
        if (k == 26)
            topology_layout_undo();
        if (k == 'g' || k == 'G')
            topology_layout_reset();
        if (k == '/') {
            if (tab == 6)
                tree_query_focus = 1;
            else {
                if (file_frame && file_frame->serial == file_request_serial && file_frame->root[0])
                    snprintf(file_input, sizeof file_input, "%s", file_frame->root);
                file_input_focus = 1;
            }
        }
        if (k == '\r') {
            if (tab == 6)
                topology_focus();
            else
                files_enter();
        }
        if (k == 8 || k == 127) {
            if (tab == 6)
                topology_parent();
            else
                files_parent();
        }
        if (tab == 6) {
            if (k == 'l' || k == 'L')
                topology_follow_toggle();
            if (k == 'a' || k == 'A') {
                tree_follow = 0;
                tree_focus_pid = 0;
                tree_mine = 0;
                scene_dirty = scene_fit = 1;
            }
            if (k == 'u' || k == 'U') {
                tree_follow = 0;
                tree_mine = !tree_mine;
                tree_focus_pid = 0;
                scene_dirty = scene_fit = 1;
            }
            if (k == 'i' || k == 'I')
                inspector_open();
            if (k == 't' || k == 'T')
                begin_signal(SIGTERM);
            if (k == 'k' || k == 'K')
                begin_signal(SIGKILL);
        }
        if (tab == 7 && (k == 'h' || k == 'H')) {
            file_hidden = !file_hidden;
            files_load(file_frame && file_frame->serial == file_request_serial &&
                               file_frame->root[0]
                           ? file_frame->root
                           : file_input);
        }
        if (k == '+' || k == '=')
            tree_distance = fmaxf(1, tree_distance * .9f);
        if (k == '-')
            tree_distance = fminf(100000, tree_distance * 1.1f);
    }
    if (k == 'f' || k == 'F')
        glutFullScreenToggle();
    if (k == '+' || k == '=')
        zoom = fmaxf(12, zoom - 1);
    if (k == '-')
        zoom = fminf(60, zoom + 1);
    if (tab == 3) {
        if (k == '/')
            search_focus = 1;
        if (k == 'i' || k == 'I')
            inspector_open();
        if (k == 't' || k == 'T')
            begin_signal(SIGTERM);
        if (k == 'k' || k == 'K')
            begin_signal(SIGKILL);
    }
    glutPostRedisplay();
}
static void scroll(int amount) {
    if (tab == MANUAL_VIEW) {
        manual_scroll += amount * 54;
        manual_bounds();
        return;
    }
    if (topology_tab()) {
        tree_distance = fmaxf(.7f, fminf(100000, tree_distance * powf(1.1f, (float)amount)));
        return;
    }
    if (tab == 1) {
        core_scroll += amount * cpu_columns();
        if (core_scroll < 0)
            core_scroll = 0;
        int max = ((view_metrics.cpu_limit - 1) / cpu_columns()) * cpu_columns();
        if (core_scroll > max)
            core_scroll = max;
    } else if (tab == 3) {
        process_scroll += amount * 3;
        filter_processes();
    } else if (tab == 4) {
        pressure_scroll += amount * 60;
        if (pressure_scroll < 0)
            pressure_scroll = 0;
    } else if (tab == 2) {
        gpu_scroll += amount * 45;
        if (gpu_scroll < 0)
            gpu_scroll = 0;
    } else
        zoom = fmaxf(12, fminf(60, zoom + amount));
}
static void special(int k, int x, int y) {
    (void)x;
    (void)y;
    if (confirm_fd >= 0 || search_focus || inspector.open || tree_gesture.active)
        return;
    if (tab == MANUAL_VIEW) {
        if (k == GLUT_KEY_PAGE_DOWN)
            manual_scroll += height > 296 ? height - 276 : 22;
        if (k == GLUT_KEY_PAGE_UP)
            manual_scroll -= height > 296 ? height - 276 : 22;
        if (k == GLUT_KEY_DOWN)
            manual_scroll += 22;
        if (k == GLUT_KEY_UP)
            manual_scroll -= 22;
        if (k == GLUT_KEY_HOME)
            manual_scroll = 0;
        if (k == GLUT_KEY_END)
            manual_scroll = manual_content(0);
        manual_bounds();
        glutPostRedisplay();
        return;
    }
    if (k == GLUT_KEY_PAGE_DOWN)
        scroll(tab == 1 ? cpu_rows() : process_rows() / 3);
    if (k == GLUT_KEY_PAGE_UP)
        scroll(tab == 1 ? -cpu_rows() : -process_rows() / 3);
    if (k == GLUT_KEY_DOWN)
        scroll(1);
    if (k == GLUT_KEY_UP)
        scroll(-1);
    glutPostRedisplay();
}
static void mouse(int b, int state, int x, int y) {
    if (state == GLUT_DOWN && panel_drag)
        return;
    if (state == GLUT_UP) {
        if (panel_drag) {
            panel_drag = 0;
            glutPostRedisplay();
            return;
        }
        if (topology_tab())
            topology_release(b, x, y);
        drag = 0;
        return;
    }
    if (confirm_fd >= 0) {
        int w = width - 32;
        if (w > 640)
            w = 640;
        int left = (width - w) / 2, top = (height - 270) / 2;
        if (b == GLUT_LEFT_BUTTON) {
            if (inside(x, y, left + w - 304, top + 208, 128, 34))
                cancel_signal();
            else if (inside(x, y, left + w - 164, top + 208, 140, 34))
                accept_signal();
        }
        return;
    }
    if (inspector.open) {
        if (b == GLUT_LEFT_BUTTON && history_click(x, y))
            return;
        if (b == 3 || b == 4) {
            int amount = b == 3 ? -3 : 3;
            if (inspector.metric == 3) {
                map_scroll += amount;
                if (map_scroll < 0)
                    map_scroll = 0;
            }
            if (inspector.metric == 4) {
                byte_scroll += amount;
                if (byte_scroll < 0)
                    byte_scroll = 0;
            }
            return;
        }
        if (b != GLUT_LEFT_BUTTON)
            return;
        int xx, yy, ww, hh;
        inspector_geometry(&xx, &yy, &ww, &hh);
        if (inside(x, y, xx + ww - 106, yy + 12, 88, 34))
            inspector_close();
        int tw = memory_tab_width(ww);
        for (int i = 0; i < 5; i++)
            if (inside(x, y, xx + 20 + i * (tw + 4), yy + 131, tw, 34)) {
                if (i == 4)
                    inspector_request_bytes(region_chosen ? chosen_region.begin : 0);
                else
                    inspector_set_metric(i);
            }
        if (inspector.metric == 3) {
            if (inside(x, y, xx + ww - 140, yy + 174, 120, 34)) {
                map_filter = (map_filter + 1) % (MEM_KINDS + 1);
                map_scroll = 0;
            }
            if (inside(x, y, xx + 12, yy + 236, ww - 24, map_rows(hh) * 20)) {
                maps_filter(hh);
                int row = (y - yy - 236) / 20 + map_scroll;
                const MemoryReport *m = inspector_report();
                if (m && row < map_count) {
                    chosen_region = m->regions[map_indices[row]];
                    region_chosen = 1;
                }
            }
            if (inside(x, y, xx + ww - 154, yy + hh - 150, 134, 34))
                inspector_request_bytes(region_chosen ? chosen_region.begin : 0);
        }
        if (inspector.metric == 4) {
            if (inside(x, y, xx + 20, yy + 174, 98, 34) && region_chosen &&
                byte_address > chosen_region.begin)
                inspector_request_bytes(byte_address - chosen_region.begin > MEMORY_BYTES
                                            ? byte_address - MEMORY_BYTES
                                            : chosen_region.begin);
            if (inside(x, y, xx + 128, yy + 174, 98, 34) && region_chosen &&
                chosen_region.end - byte_address > MEMORY_BYTES)
                inspector_request_bytes(byte_address + MEMORY_BYTES);
            if (inside(x, y, xx + ww - 130, yy + 174, 110, 34))
                inspector_request_bytes(byte_address);
        }
        if (inside(x, y, xx + ww - 316, yy + hh - 60, 140, 34))
            begin_signal(SIGTERM);
        if (inside(x, y, xx + ww - 164, yy + hh - 60, 140, 34))
            begin_signal(SIGKILL);
        return;
    }
    if (tree_gesture.active)
        return;
    if (b == 3 || b == 4) {
        scroll(b == 3 ? -1 : 1);
        return;
    }
    if (b != GLUT_LEFT_BUTTON) {
        if (topology_tab())
            topology_pointer_begin(b, x, y, smoke ? injected_modifiers : glutGetModifiers());
        return;
    }
    if (inside(x, y, width - 288, 12, 144, 34)) {
        history_window = history_next_window(history_window);
        return;
    }
    if (history_click(x, y))
        return;
    int divider = panel_hit(x, y);
    if (divider) {
        panel_drag = divider;
        return;
    }
    int nav = (width - 48) / VIEW_COUNT;
    if (nav > 155)
        nav = 155;
    if (nav < 1)
        nav = 1;
    if (y >= 61 && y <= 102 && x >= 24 && x < 24 + VIEW_COUNT * nav) {
        int t = (x - 24) / nav;
        if (t < VIEW_COUNT)
            tab = t;
        search_focus = 0;
        return;
    }
    if (width < 400 || height < 440)
        return;
    if (topology_tab()) {
        topology_click(x, y);
        return;
    }
    if (tab == 5) {
        if (inside(x, y, 24, ram_table_top(), width - 48, ram_rows() * 26)) {
            int row = (y - ram_table_top()) / 26;
            if (row < ram_count) {
                const Process *p = &view_metrics.processes[ram_indices[row]];
                selected_pid = p->pid;
                selected_start = p->start;
                inspector_open();
                inspector_set_metric(3);
            }
        }
        return;
    }
    if (tab == 1) {
        int cols = cpu_columns(), w = (width - 48 - 12 * (cols - 1)) / cols;
        if (x >= 24 && x < width - 24 && y >= 250 && y < 250 + cpu_rows() * 134) {
            int col = (x - 24) / (w + 12), row = (y - 250) / 134,
                id = core_scroll + row * cols + col;
            if (id < view_metrics.cpu_limit)
                selected_core = id;
        }
        return;
    }
    if (tab == 2) {
        int gy = width < 900 ? 188 : 126;
        if (inside(x, y, width - 340, gy, 90, 34) && view_metrics.gpu_count)
            gpu_selected = (gpu_selected + view_metrics.gpu_count - 1) % view_metrics.gpu_count;
        if (inside(x, y, width - 240, gy, 90, 34) && view_metrics.gpu_count)
            gpu_selected = (gpu_selected + 1) % view_metrics.gpu_count;
        if (inside(x, y, width - 140, gy, 116, 34))
            gpu_3d = !gpu_3d;
    }
    if (tab == 3) {
        search_focus = inside(x, y, 24, 166, width - 48, 42);
        if (search_focus)
            return;
        if (inside(x, y, 24, 216, width - 48, 32)) {
            int col[8];
            process_columns(col);
            for (int i = 0; i < 7; i++)
                if (!(width < 900 && (i == 1 || i == 5 || i == 6)) && x >= col[i] &&
                    x < col[i + 1]) {
                    if (sort_column == i)
                        sort_desc = !sort_desc;
                    else {
                        sort_column = i;
                        sort_desc = i == 3 || i == 4 || i == 5;
                    }
                    process_scroll = 0;
                    filter_processes();
                }
            return;
        }
        if (inside(x, y, 24, 250, width - 48, process_rows() * 29)) {
            filter_processes();
            int row = (y - 250) / 29 + process_scroll;
            if (row < visible_count) {
                const Process *p = &view_metrics.processes[indices[row]];
                selected_pid = p->pid;
                selected_start = p->start;
            }
            return;
        }
        if (inside(x, y, process_action_x(0), height - 84, process_action_width(), 34))
            inspector_open();
        if (inside(x, y, process_action_x(1), height - 84, process_action_width(), 34))
            begin_signal(SIGTERM);
        if (inside(x, y, process_action_x(2), height - 84, process_action_width(), 34))
            begin_signal(SIGKILL);
        return;
    }
    if ((tab == 0 && y > (width < 900 ? 344 : 264)) || (tab == 2 && gpu_3d && y > 196)) {
        drag = 1;
        mx = x;
        my = y;
    }
}
static void motion(int x, int y) {
    if (panel_drag) {
        if (inspector.open || confirm_fd >= 0) {
            panel_drag = 0;
            return;
        }
        panel_adjust(x);
        glutPostRedisplay();
        return;
    }
    if (tree_gesture.active) {
        topology_pointer_motion(x, y);
        return;
    }
    if (drag) {
        yaw += (x - mx) * .4f;
        pitch = fmaxf(-85, fminf(85, pitch + (y - my) * .4f));
        mx = x;
        my = y;
        glutPostRedisplay();
    }
}
static void usage(FILE *stream, const char *program) {
    fprintf(stream,
            "Usage: %s [--check | --smoke] [--view VIEW] [--root PATH]\n"
            "       %s --help | --version\n\n"
            "Views: overview, cpu, gpu, processes, pressure, ram, tree, files, manual\n\n"
            "  --view VIEW  Open the named view (default: overview)\n"
            "  --root PATH  Set the initial filesystem root (default: current directory)\n"
            "  --check      Run checks without a graphical display\n"
            "  --smoke      Run graphical checks using temporary test processes/files\n"
            "  -h, --help   Show this help\n"
            "  --version    Show the version\n\n"
            "Press 9 in the app for the manual. Terminal manual: man lsm\n"
            "From the build directory: man -l ./lsm.1\n",
            program, program);
}
int main(int argc, char **argv) {
    if (!getcwd(file_root, sizeof file_root))
        snprintf(file_root, sizeof file_root, "/");
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("lsm 4.1 | Linux System Monitor | C / OpenGL");
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout, argv[0]);
        return 0;
    }
    int check = 0;
    for (int argument = 1; argument < argc; argument++) {
        if (!strcmp(argv[argument], "--check"))
            check = 1;
        else if (!strcmp(argv[argument], "--smoke"))
            smoke = 1;
        else if (!strcmp(argv[argument], "--root") && argument + 1 < argc) {
            if (strlen(argv[++argument]) >= sizeof file_root) {
                fprintf(stderr, "Path too long\n");
                return 2;
            }
            snprintf(file_root, sizeof file_root, "%s", argv[argument]);
        } else if (!strcmp(argv[argument], "--view") && argument + 1 < argc) {
            const char *names[] = {"overview", "cpu",  "gpu",   "processes", "pressure",
                                   "ram",      "tree", "files", "manual"};
            int found = 0;
            argument++;
            for (int i = 0; i < VIEW_COUNT; i++)
                if (!strcmp(argv[argument], names[i])) {
                    tab = i;
                    found = 1;
                }
            if (!found) {
                fprintf(stderr, "Unknown view: %s\n", argv[argument]);
                return 2;
            }
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (!sampler_start()) {
        fprintf(stderr, "Cannot read Linux /proc metrics\n");
        sampler_stop();
        return 1;
    }
    snapshot = sampler_poll();
    if (check || smoke) {
        struct timespec delay = {0, 150000000};
        nanosleep(&delay, NULL);
        if (!sample()) {
            sampler_stop();
            return 1;
        }
    }
    if (check) {
        int ok = view_samples == 1 && view_memory >= 0 && view_memory <= 100 &&
                 isfinite(view_rxrate) && isfinite(view_readrate);
        for (int c = 0; c <= view_cores; c++)
            if (!isfinite(view_cpu_history[0][c]) || view_cpu_history[0][c] < 0 ||
                view_cpu_history[0][c] > 100)
                ok = 0;
        for (int i = 0; i < view_count; i++)
            if (!isfinite(view_metrics.processes[i].cpu) || view_metrics.processes[i].cpu < 0)
                ok = 0;
        printf("%s: %s / %d cores / %d threads / %.2f GiB / %d processes\n", ok ? "PASS" : "FAIL",
               view_metrics.cpu_name, view_metrics.physical_cores, view_metrics.cpu_count,
               view_memtotal, view_count);
        for (int i = 0; i < view_metrics.gpu_count; i++) {
            const Gpu *g = &view_metrics.gpus[i];
            printf("GPU: %s %s / %s / %s / %d engines / %d visible clients / %.0f MHz\n", g->vendor,
                   g->name, g->driver, g->pci, g->engine_count, g->clients, g->clock);
        }
        ok = sampler_tests() && ok;
        sampler_stop();
        ok = metrics_tests() && ok;
        ok = memory_tests() && ok;
        ok = topology_tests() && ok;
        ok = files_tests() && ok;
        ok = explore_tests() && ok;
        ok = interaction_tests() && ok;
        return ok ? 0 : 1;
    }
    int ac = 1;
    glutInit(&ac, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH);
    glutInitWindowSize(width, height);
    glutCreateWindow("lsm 4.1 | Linux System Monitor | C / OpenGL");
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    font_init();
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutSpecialFunc(special);
    glutMouseFunc(mouse);
    glutMotionFunc(motion);
    glutPassiveMotionFunc(topology_passive);
    glutTimerFunc(0, timer, 0);
    glutMainLoop();
    cancel_signal();
    if (files_running)
        files_stop();
    if (smoke)
        smoke_directory_cleanup();
    sampler_stop();
    return ui_failed ? 1 : 0;
}
