#include "health.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double health_threshold(HealthScale scale, HealthLevel level) {
    int danger = level == HEALTH_DANGER;
    switch (scale) {
    case HEALTH_UTILIZATION:
        return danger ? 95 : 85;
    case HEALTH_PRESSURE:
        return danger ? 25 : 10;
    case HEALTH_FULL_PRESSURE:
        return danger ? 15 : 5;
    case HEALTH_LOAD:
        return danger ? 1.5 : 1;
    default:
        return INFINITY;
    }
}

HealthLevel health_classify(double value, HealthScale scale) {
    if (scale == HEALTH_NONE || !isfinite(value) || value < 0)
        return HEALTH_UNKNOWN;
    if (value >= health_threshold(scale, HEALTH_DANGER))
        return HEALTH_DANGER;
    if (value >= health_threshold(scale, HEALTH_WARNING))
        return HEALTH_WARNING;
    return HEALTH_NORMAL;
}

typedef struct {
    HealthLevel level;
    const char *name, *unit;
    double value;
    int contention;
} Issue;

void health_evaluate(const Snapshot *frame, HealthReport *report) {
    *report = (HealthReport){.level = HEALTH_UNKNOWN,
                             .cpu = HEALTH_UNKNOWN,
                             .memory = HEALTH_UNKNOWN,
                             .gpu = HEALTH_UNKNOWN,
                             .gpu_memory = HEALTH_UNKNOWN,
                             .load = HEALTH_UNKNOWN,
                             .pressure = {HEALTH_UNKNOWN, HEALTH_UNKNOWN, HEALTH_UNKNOWN}};
    if (!frame || !frame->valid || frame->data.history_count < 1 ||
        frame->data.history_count > HISTORY || frame->data.history_head < 0 ||
        frame->data.history_head >= HISTORY)
        return;
    const SampleData *data = &frame->data;
    int latest = (data->history_head + HISTORY - 1) % HISTORY;
    double cpu = data->cpu_history[latest][0], gpu = -1, gpu_memory = -1;
    for (int i = 0; i < frame->hardware.gpu_count && i < MAX_GPUS; i++) {
        const Gpu *device = &frame->hardware.gpus[i];
        if (isfinite(device->busy) && device->busy > gpu)
            gpu = device->busy;
        if (isfinite(device->vram_total) && isfinite(device->vram_used) && device->vram_total > 0 &&
            device->vram_used >= 0) {
            double used = 100.0 * device->vram_used / device->vram_total;
            if (used > gpu_memory)
                gpu_memory = used;
        }
    }
    double load =
        frame->hardware.cpu_count > 0 ? data->load_average[0] / frame->hardware.cpu_count : -1;
    report->cpu = health_classify(cpu, HEALTH_UTILIZATION);
    report->memory = health_classify(data->memory_percent, HEALTH_UTILIZATION);
    report->gpu = health_classify(gpu, HEALTH_UTILIZATION);
    report->gpu_memory = health_classify(gpu_memory, HEALTH_UTILIZATION);
    report->load = health_classify(load, HEALTH_LOAD);
    Issue issues[10] = {{report->cpu, "CPU", "%", cpu, 0},
                        {report->memory, "RAM", "%", data->memory_percent, 0},
                        {report->gpu, "GPU", "%", gpu, 0},
                        {report->load, "1m load", "/CPU", load, 1},
                        {report->gpu_memory, "GPU VRAM", "%", gpu_memory, 0}};
    int count = 5;
    const char *names[] = {"CPU stalls", "RAM stalls", "I/O stalls"};
    const char *full_names[] = {"CPU full stalls", "RAM full stalls", "I/O full stalls"};
    for (int i = 0; i < 3; i++) {
        const Pressure *p = &data->pressure[i];
        HealthLevel some =
            p->available ? health_classify(p->avg10, HEALTH_PRESSURE) : HEALTH_UNKNOWN;
        HealthLevel full = i > 0 && p->full_available
                               ? health_classify(p->full10, HEALTH_FULL_PRESSURE)
                               : HEALTH_UNKNOWN;
        report->pressure[i] = some > full ? some : full;
        issues[count++] = (Issue){some, names[i], "% avg10", p->avg10, 1};
        if (i > 0)
            issues[count++] = (Issue){full, full_names[i], "% avg10", p->full10, 1};
        if (report->pressure[i] == HEALTH_DANGER)
            report->overloaded = 1;
    }
    if (report->load == HEALTH_DANGER)
        report->overloaded = 1;
    for (int i = 0; i < count; i++)
        if (issues[i].level > report->level)
            report->level = issues[i].level;
    /* Show overload causes before utilization so narrow windows retain the
       explanation for SYSTEM OVERLOAD even when several resources are busy. */
    for (int level = HEALTH_DANGER; level >= HEALTH_WARNING; level--)
        for (int contention = 1; contention >= 0; contention--)
            for (int i = 0; i < count; i++)
                if ((int)issues[i].level == level && issues[i].contention == contention) {
                    size_t used = strlen(report->reasons);
                    snprintf(report->reasons + used, sizeof report->reasons - used, "%s%s %.1f%s",
                             used ? " | " : "", issues[i].name, issues[i].value, issues[i].unit);
                }
}

int health_tests(void) {
    int ok = health_classify(84.99, HEALTH_UTILIZATION) == HEALTH_NORMAL &&
             health_classify(85, HEALTH_UTILIZATION) == HEALTH_WARNING &&
             health_classify(94.99, HEALTH_UTILIZATION) == HEALTH_WARNING &&
             health_classify(95, HEALTH_UTILIZATION) == HEALTH_DANGER &&
             health_classify(NAN, HEALTH_UTILIZATION) == HEALTH_UNKNOWN &&
             health_classify(INFINITY, HEALTH_UTILIZATION) == HEALTH_UNKNOWN &&
             health_classify(-1, HEALTH_UTILIZATION) == HEALTH_UNKNOWN &&
             health_classify(900, HEALTH_NONE) == HEALTH_UNKNOWN;
    Snapshot *frame = calloc(1, sizeof *frame);
    if (!frame)
        return 0;
    HealthReport report;
    health_evaluate(frame, &report);
    ok = ok && report.level == HEALTH_UNKNOWN;
    frame->valid = 1;
    frame->data.history_count = 1;
    frame->data.history_head = 1;
    frame->hardware.cpu_count = 8;
    frame->data.load_average[0] = 7.9;
    health_evaluate(frame, &report);
    ok = ok && report.level == HEALTH_NORMAL && report.gpu == HEALTH_UNKNOWN && !report.overloaded;
    frame->data.cpu_history[0][0] = 98;
    health_evaluate(frame, &report);
    ok =
        ok && report.cpu == HEALTH_DANGER && !report.overloaded && strstr(report.reasons, "CPU 98");
    frame->data.cpu_history[0][0] = 0;
    frame->data.load_average[0] = 8;
    health_evaluate(frame, &report);
    ok = ok && report.load == HEALTH_WARNING && !report.overloaded;
    frame->data.load_average[0] = 12;
    frame->data.cpu_history[0][0] = 98;
    health_evaluate(frame, &report);
    ok = ok && report.load == HEALTH_DANGER && report.overloaded &&
         !strncmp(report.reasons, "1m load", 7);
    frame->data.cpu_history[0][0] = 0;
    frame->data.load_average[0] = 0;
    frame->data.memory_percent = 95;
    frame->data.pressure[1] = (Pressure){.avg10 = 25, .available = 1};
    health_evaluate(frame, &report);
    ok = ok && report.memory == HEALTH_DANGER && report.pressure[1] == HEALTH_DANGER &&
         report.overloaded && strstr(report.reasons, "RAM stalls");
    frame->data.pressure[1] = (Pressure){.full10 = 15, .full_available = 1};
    health_evaluate(frame, &report);
    ok = ok && report.overloaded && strstr(report.reasons, "RAM full stalls");
    frame->data.memory_percent = 0;
    frame->data.pressure[1] = (Pressure){.avg10 = 99, .full10 = 99};
    frame->hardware.gpu_count = 1;
    frame->hardware.gpus[0].busy = -1;
    frame->data.swap_percent = 100;
    health_evaluate(frame, &report);
    ok = ok && report.level == HEALTH_NORMAL && !report.overloaded;
    frame->hardware.gpus[0].busy = 99;
    health_evaluate(frame, &report);
    ok = ok && report.gpu == HEALTH_DANGER && !report.overloaded;
    frame->hardware.gpus[0].busy = NAN;
    frame->hardware.cpu_count = 0;
    health_evaluate(frame, &report);
    ok = ok && report.gpu == HEALTH_UNKNOWN && report.load == HEALTH_UNKNOWN;
    frame->hardware.gpus[0].vram_total = 1024;
    frame->hardware.gpus[0].vram_used = 1000;
    health_evaluate(frame, &report);
    ok = ok && report.gpu_memory == HEALTH_DANGER && strstr(report.reasons, "GPU VRAM");
    frame->data.history_head = HISTORY;
    health_evaluate(frame, &report);
    ok = ok && report.level == HEALTH_UNKNOWN;
    free(frame);
    printf(
        "%s: severity boundaries, missing counters, normalized load and pressure overload alerts\n",
        ok ? "PASS" : "FAIL");
    return ok;
}
