// Host-build shim for pico/mutex.h. The hosttest harness is single-threaded
// (no core1, no background task), so every mutex op is a no-op.
#pragma once
#include <stdbool.h>

typedef struct { int _unused; } mutex_t;

static inline void mutex_init(mutex_t *m)              { (void)m; }
static inline void mutex_enter_blocking(mutex_t *m)    { (void)m; }
static inline void mutex_exit(mutex_t *m)              { (void)m; }
static inline bool mutex_try_enter(mutex_t *m, void *_unused) { (void)m; (void)_unused; return true; }
