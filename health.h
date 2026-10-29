#ifndef LSM_HEALTH_H
#define LSM_HEALTH_H

#include "sampler.h"

typedef enum { HEALTH_UNKNOWN = -1, HEALTH_NORMAL, HEALTH_WARNING, HEALTH_DANGER } HealthLevel;

typedef enum {
    HEALTH_NONE,
    HEALTH_UTILIZATION,
    HEALTH_PRESSURE,
    HEALTH_FULL_PRESSURE,
    HEALTH_LOAD
} HealthScale;

typedef struct {
    HealthLevel level, cpu, memory, gpu, gpu_memory, load, pressure[3];
    int overloaded;
    char reasons[256];
} HealthReport;

double health_threshold(HealthScale scale, HealthLevel level);
HealthLevel health_classify(double value, HealthScale scale);
void health_evaluate(const Snapshot *frame, HealthReport *report);
int health_tests(void);

#endif
