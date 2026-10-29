#ifndef MONITOR_METRICS_H
#define MONITOR_METRICS_H
#include <stdint.h>
#include <sys/types.h>
#define MAX_CPUS 1024
#define MAX_PROCESSES 32768
#define MAX_GPUS 16
#define MAX_ENGINES 16
#define HISTORY 900

typedef struct {
    int id, online, package, core, type;
    double busy, user, system, iowait, mhz, min_mhz, max_mhz, temperature;
    uint64_t total, idle, user_ticks, system_ticks, wait_ticks;
} CpuInfo;
typedef struct {
    int pid, ppid, threads, priority, nice, io_available;
    uid_t uid;
    char name[128], command[512], state;
    uint64_t ticks, start, rss_kib, read_bytes, write_bytes;
    double cpu, read_rate, write_rate;
} Process;
typedef struct {
    char name[48];
    double busy;
    unsigned capacity;
    float history[HISTORY];
} Engine;
typedef struct {
    char card[32], name[160], driver[64], pci[32], vendor[32];
    char link[64], max_link[64];
    unsigned vendor_id, device_id;
    double clock, min_clock, max_clock, temperature, power, power_limit;
    double busy, vram_total, vram_used, gtt_total, gtt_used, client_memory;
    Engine engines[MAX_ENGINES];
    int engine_count, clients, restricted, nv_index, memory_available;
    float history[HISTORY];
} Gpu;
typedef struct {
    char cpu_name[160], kernel[160], hostname[80], governor[64];
    char architecture[32], cpu_vendor[64], cache[64], microcode[32];
    CpuInfo cpus[MAX_CPUS];
    int cpu_count, cpu_limit, packages, physical_cores, process_count, gpu_count;
    double package_temperature;
    Process processes[MAX_PROCESSES];
    Gpu gpus[MAX_GPUS];
} Metrics;
void metrics_copy(Metrics *destination);
int metrics_init(void);
int metrics_update(double dt, int baseline, int history_slot);
int process_read(int pid, Process *p);
int process_handle(const Process *p);
int process_signal(int handle, int signal_number);
int metrics_tests(void);
void metrics_close(void);
#endif
