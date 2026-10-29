#ifndef LSM_MANUAL_H
#define LSM_MANUAL_H

#include <stddef.h>

typedef struct {
    const char *title;
    const char *body;
} ManualSection;

extern const ManualSection lsm_manual[];
extern const size_t lsm_manual_count;

#endif
