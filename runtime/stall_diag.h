#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    STALL_MUTEX, STALL_CONDVAR, STALL_ARBITER, STALL_EVENT_VSYNC, STALL_EVENT_OTHER,
    STALL_SLEEP, STALL_QUEUE_ROOM, STALL_PRESENT_READY, STALL_CAUSES
} StallCause;

typedef struct StallSpan {
    struct StallSpan *parent;
    uint64_t start, children;
    StallCause cause;
    bool active;
} StallSpan;

extern _Atomic bool g_stall_diag_enabled;
void stall_diag_init(void);
void stall_diag_shutdown(void);
void stall_diag_enter(StallSpan *span, StallCause cause, uint64_t now);
void stall_diag_leave(StallSpan *span, uint64_t now);
void stall_diag_frame(uint64_t frame, uint64_t thread, uint64_t now, uint64_t blocked,
                      uint64_t game_cpu, uint64_t render_cpu, uint64_t gpu_wait);

static inline void stall_diag_begin(StallSpan *span, StallCause cause, uint64_t now) {
    span->active = atomic_load_explicit(&g_stall_diag_enabled, memory_order_relaxed);
    if (span->active) stall_diag_enter(span, cause, now);
}

static inline void stall_diag_end(StallSpan *span, uint64_t now) {
    if (span->active) stall_diag_leave(span, now);
}
