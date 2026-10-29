#ifndef LSM_SAMPLER_H
#define LSM_SAMPLER_H
#include "metrics.h"
#include "memory.h"
typedef struct {
    double avg10, avg60, avg300, full10;
    uint64_t total;
    int available, full_available;
    float history[HISTORY];
} Pressure;
typedef struct {
    float cpu_history[HISTORY][MAX_CPUS + 1];
    float mem_history[HISTORY], net_history[HISTORY], disk_history[HISTORY];
    double timestamps[HISTORY];
    Pressure pressure[3];
    SystemMemory system_memory;
    int cpu_limit, history_head, history_count;
    uint64_t sample_number;
    double memory_percent, swap_percent, memory_gib;
    double rx_rate, tx_rate, read_rate, write_rate, load_average[3], uptime;
} SampleData;
typedef struct {
    Metrics hardware;
    SampleData data;
    MemoryReport memory;
    MemoryBytes bytes;
    uint64_t sequence;
    double collected_at, collection_ms;
    int valid;
} Snapshot;
/* Poll only on the UI thread. The returned frame remains immutable until the
   next poll, even when collection continues while the view is frozen. */
int sampler_start(void);
const Snapshot *sampler_poll(void);
int sampler_refresh(unsigned timeout_ms);
void sampler_memory_select(int pid, uint64_t start);
uint64_t sampler_memory_peek(int pid, uint64_t start, uint64_t address, size_t length);
void sampler_stop(void);
int sampler_tests(void);
#endif
