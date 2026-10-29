#ifndef LSM_MEMORY_H
#define LSM_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define MEMORY_REGIONS 4096
#define MEMORY_BYTES 256

typedef struct {
    uint64_t total, available, free, cached, buffers, slab, reclaimable, unreclaimable;
    uint64_t anonymous, shared, page_tables, kernel_stack, swap_total, swap_free, dirty, writeback;
} SystemMemory;
typedef enum {
    MEM_HEAP,
    MEM_STACK,
    MEM_FILE,
    MEM_SHARED,
    MEM_ANON,
    MEM_SPECIAL,
    MEM_KINDS
} MemoryKind;
typedef struct {
    uint64_t begin, end, offset, rss, pss, private_kib, shared_kib, anonymous, swap, dirty;
    char permissions[5], name[384];
    MemoryKind kind;
} MemoryRegion;
typedef struct {
    int pid, error, available, accounting, stored, total_regions, truncated;
    uint64_t start, rss, pss, private_kib, shared_kib, anonymous, swap, virtual_kib;
    uint64_t category_rss[MEM_KINDS];
    MemoryRegion regions[MEMORY_REGIONS];
} MemoryReport;
typedef struct {
    int pid, error, count;
    uint64_t start, address, serial;
    unsigned char data[MEMORY_BYTES];
} MemoryBytes;
void memory_system_line(SystemMemory *m, const char *line);
int memory_report_read(int pid, uint64_t start, MemoryReport *report);
int memory_peek(int pid, uint64_t start, uint64_t address, unsigned char *data, size_t length);
const char *memory_kind_name(MemoryKind kind);
int memory_tests(void);
#endif
