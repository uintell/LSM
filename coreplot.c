#include "coreplot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void core_plot_begin(CorePlot *plot, int x, int y, int w, int h) {
    plot->count = 0;
    plot->x = x;
    plot->y = y;
    plot->w = w;
    plot->h = h;
    plot->valid = w > 0 && h > 0;
}

void core_plot_add(CorePlot *plot, CorePlotPoint point) {
    if (!plot->valid || plot->count >= CORE_PLOT_POINTS || !isfinite(point.x) ||
        !isfinite(point.y) || !isfinite(point.depth) || !isfinite(point.time) ||
        !isfinite(point.value) || point.value < 0 || point.value > 100 || point.depth < -1 ||
        point.depth > 1 || point.cpu < 0 || point.cpu >= CORE_PLOT_CPUS)
        return;
    plot->points[plot->count++] = point;
}

CorePlotHit core_plot_pick(const CorePlot *plot, double x, double y, double radius) {
    CorePlotHit hit = {0};
    if (!plot->valid || !isfinite(x) || !isfinite(y) || !isfinite(radius) || radius <= 0 ||
        x < plot->x || y < plot->y || x >= plot->x + plot->w || y >= plot->y + plot->h)
        return hit;
    double best = radius * radius, front = INFINITY;
    for (int i = 0; i < plot->count; i++) {
        const CorePlotPoint *b = &plot->points[i], *a = b;
        if (b->connected && i > 0 && plot->points[i - 1].cpu == b->cpu)
            a = &plot->points[i - 1];
        double dx = b->x - a->x, dy = b->y - a->y, length = dx * dx + dy * dy;
        double t = length > 0 ? ((x - a->x) * dx + (y - a->y) * dy) / length : 0;
        t = fmax(0, fmin(1, t));
        double sx = a->x + t * dx, sy = a->y + t * dy;
        double distance = (x - sx) * (x - sx) + (y - sy) * (y - sy);
        double depth = a->depth + t * (b->depth - a->depth);
        if (distance > radius * radius || distance > best + .01)
            continue;
        if (fabs(distance - best) <= .01 && depth >= front)
            continue;
        best = distance;
        front = depth;
        const CorePlotPoint *sample = t < .5 ? a : b;
        hit = (CorePlotHit){1, sample->cpu, sample->time, sample->value, sample->x, sample->y};
    }
    return hit;
}

CoreHistoryStats core_history_stats(const float *values, int stride, const double *times,
                                    int cursor, int count, double window) {
    CoreHistoryStats stats = {0};
    if (!values || !times || stride < 1 || cursor < 0 || cursor >= HISTORY || count < 1 ||
        count > HISTORY || !isfinite(window) || window <= 0)
        return stats;
    double now = times[(cursor + HISTORY - 1) % HISTORY];
    if (!isfinite(now))
        return stats;
    for (int i = 0; i < count; i++) {
        int index = (cursor - count + i + HISTORY) % HISTORY;
        float v = values[index * stride];
        if (!isfinite(times[index]) || now - times[index] < 0 || now - times[index] > window ||
            !isfinite(v) || v < 0 || v > 100)
            continue;
        stats.count++;
        stats.mean += v;
        if (v > stats.peak)
            stats.peak = v;
    }
    if (stats.count)
        stats.mean /= stats.count;
    return stats;
}

int core_plot_tests(void) {
    CorePlot *plot = calloc(1, sizeof *plot);
    if (!plot)
        return 0;
    core_plot_begin(plot, 100, 50, 300, 200);
    core_plot_add(plot, (CorePlotPoint){120, 100, .5, 10, 20, 3, 0});
    core_plot_add(plot, (CorePlotPoint){220, 100, .5, 20, 80, 3, 1});
    CorePlotHit hit = core_plot_pick(plot, 160, 103, 8);
    int ok = hit.valid && hit.cpu == 3 && hit.time == 10 && hit.value == 20;
    hit = core_plot_pick(plot, 190, 103, 8);
    ok = ok && hit.valid && hit.time == 20 && hit.value == 80;
    /* Crossing lines pick the front trace, preserving a recorded endpoint. */
    core_plot_add(plot, (CorePlotPoint){120, 100, -.5, 10, 25, 7, 0});
    core_plot_add(plot, (CorePlotPoint){220, 100, -.5, 20, 75, 7, 1});
    hit = core_plot_pick(plot, 190, 100, 8);
    ok = ok && hit.valid && hit.cpu == 7 && hit.value == 75;
    ok = ok && !core_plot_pick(plot, 190, 110, 8).valid && !core_plot_pick(plot, 10, 100, 8).valid;
    core_plot_begin(plot, 100, 50, 300, 200);
    core_plot_add(plot, (CorePlotPoint){120, 100, 0, 10, 20, 3, 0});
    core_plot_add(plot, (CorePlotPoint){220, 100, 0, 20, 80, 3, 0});
    ok = ok && !core_plot_pick(plot, 170, 100, 8).valid;
    core_plot_add(plot, (CorePlotPoint){NAN, 100, 0, 30, 10, 3, 1});
    ok = ok && plot->count == 2;
    float values[HISTORY] = {0};
    double times[HISTORY] = {0};
    values[HISTORY - 1] = 90;
    times[HISTORY - 1] = 1;
    values[0] = 20;
    times[0] = 10;
    values[1] = NAN;
    times[1] = 11;
    values[2] = 40;
    times[2] = 15;
    CoreHistoryStats stats = core_history_stats(values, 1, times, 3, 4, 5);
    ok = ok && stats.count == 2 && stats.mean == 30 && stats.peak == 40;
    stats = core_history_stats(values, 1, times, 3, 4, 20);
    ok = ok && stats.count == 3 && stats.mean == 50 && stats.peak == 90;
    free(plot);
    printf(
        "%s: core trace picking, frontmost overlap, missing samples and windowed core statistics\n",
        ok ? "PASS" : "FAIL");
    return ok;
}
