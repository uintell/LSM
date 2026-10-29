#ifndef LSM_COREPLOT_H
#define LSM_COREPLOT_H

#include "metrics.h"

#define CORE_PLOT_CPUS 128
#define CORE_PLOT_POINTS (CORE_PLOT_CPUS * HISTORY)

typedef struct {
    double x, y, depth, time;
    float value;
    int cpu, connected;
} CorePlotPoint;
typedef struct {
    int valid, count, x, y, w, h;
    CorePlotPoint points[CORE_PLOT_POINTS];
} CorePlot;
typedef struct {
    int valid, cpu;
    double time, value, x, y;
} CorePlotHit;
typedef struct {
    int count;
    double mean, peak;
} CoreHistoryStats;

void core_plot_begin(CorePlot *plot, int x, int y, int w, int h);
void core_plot_add(CorePlot *plot, CorePlotPoint point);
CorePlotHit core_plot_pick(const CorePlot *plot, double x, double y, double radius);
CoreHistoryStats core_history_stats(const float *values, int stride, const double *times,
                                    int cursor, int count, double window);
int core_plot_tests(void);

#endif
