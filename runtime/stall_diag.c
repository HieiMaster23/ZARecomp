#include "stall_diag.h"
#include "platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef STALL_DIAG_QUEUE_CAP
#define STALL_DIAG_QUEUE_CAP 256
#endif
#ifndef STALL_DIAG_RECORD_CAP
#define STALL_DIAG_RECORD_CAP 4096
#endif

_Atomic bool g_stall_diag_enabled;
typedef struct {
    uint64_t frame, thread, begin, end, blocked, game_cpu, render_cpu, gpu_wait;
    uint64_t cause[STALL_CAUSES];
} StallRecord;
typedef struct {
    StallSpan *active;
    uint64_t totals[STALL_CAUSES], previous[STALL_CAUSES];
    uint64_t frame, time, blocked, game_cpu, render_cpu, gpu_wait;
} StallThread;
static __thread StallThread local;
static StallRecord records[STALL_DIAG_QUEUE_CAP];
static unsigned head, count, accepted;
static _Atomic uint64_t lost_buffer, lost_limit;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t writer;
static bool started, stopping;
static FILE *output;

static uint64_t delta(uint64_t a, uint64_t b) { return a >= b ? a - b : 0; }

void stall_diag_enter(StallSpan *span, StallCause cause, uint64_t now) {
    span->parent = local.active;
    span->start = now;
    span->children = 0;
    span->cause = cause;
    local.active = span;
}

void stall_diag_leave(StallSpan *span, uint64_t now) {
    uint64_t elapsed = delta(now, span->start);
    local.totals[span->cause] += delta(elapsed, span->children);
    local.active = span->parent;
    if (span->parent) span->parent->children += elapsed;
    span->active = false;
}

void stall_diag_frame(uint64_t frame, uint64_t thread, uint64_t now, uint64_t blocked,
                      uint64_t game_cpu, uint64_t render_cpu, uint64_t gpu_wait) {
    if (!atomic_load_explicit(&g_stall_diag_enabled, memory_order_relaxed)) return;
    bool continuous = local.time && now >= local.time && frame > local.frame;
    StallRecord r = {.frame = frame, .thread = thread, .begin = local.time, .end = now,
                     .blocked = delta(blocked, local.blocked), .game_cpu = delta(game_cpu, local.game_cpu),
                     .render_cpu = delta(render_cpu, local.render_cpu), .gpu_wait = delta(gpu_wait, local.gpu_wait)};
    for (int i = 0; i < STALL_CAUSES; i++) {
        r.cause[i] = delta(local.totals[i], local.previous[i]);
        local.previous[i] = local.totals[i];
    }
    local.frame = frame;
    local.time = now;
    local.blocked = blocked;
    local.game_cpu = game_cpu;
    local.render_cpu = render_cpu;
    local.gpu_wait = gpu_wait;
    if (!continuous || now - r.begin <= 100000000ull) return;
    if (pthread_mutex_trylock(&lock)) {
        atomic_fetch_add_explicit(&lost_buffer, 1, memory_order_relaxed);
        return;
    }
    if (stopping || accepted >= STALL_DIAG_RECORD_CAP) atomic_fetch_add_explicit(&lost_limit, 1, memory_order_relaxed);
    else if (count == STALL_DIAG_QUEUE_CAP) atomic_fetch_add_explicit(&lost_buffer, 1, memory_order_relaxed);
    else {
        records[(head + count) % STALL_DIAG_QUEUE_CAP] = r;
        count++;
        accepted++;
        pthread_cond_signal(&ready);
    }
    pthread_mutex_unlock(&lock);
}

static void write_record(const StallRecord *r) {
    fprintf(output, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu", (unsigned long long)r->frame,
            (unsigned long long)r->thread, (unsigned long long)r->begin, (unsigned long long)r->end,
            (unsigned long long)r->blocked, (unsigned long long)r->game_cpu,
            (unsigned long long)r->render_cpu, (unsigned long long)r->gpu_wait);
    uint64_t classified = 0;
    for (int i = 0; i < STALL_CAUSES; i++) {
        fprintf(output, ",%llu", (unsigned long long)r->cause[i]);
        classified += r->cause[i];
    }
    fprintf(output, ",%llu,%llu,%llu\n", (unsigned long long)delta(r->blocked, classified),
            (unsigned long long)atomic_load_explicit(&lost_buffer, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&lost_limit, memory_order_relaxed));
}

static void *write_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!count && !stopping) pthread_cond_wait(&ready, &lock);
        if (!count && stopping) { pthread_mutex_unlock(&lock); break; }
        StallRecord r = records[head];
        head = (head + 1) % STALL_DIAG_QUEUE_CAP;
        count--;
        pthread_mutex_unlock(&lock);
        write_record(&r);
    }
    fprintf(output, "# end accepted=%u lost_buffer=%llu lost_limit=%llu\n", accepted,
            (unsigned long long)atomic_load_explicit(&lost_buffer, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&lost_limit, memory_order_relaxed));
    if (ferror(output) || fflush(output)) fprintf(stderr, "[stall-diag] output failed; capture is incomplete\n");
    fclose(output);
    return NULL;
}

void stall_diag_init(void) {
#ifdef _WIN32
    const char *e = getenv("LARECOMP_STALL_DIAG");
    if (started || !e || strcmp(e, "1")) return;
    LARGE_INTEGER frequency, before, after;
    FILETIME utc;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before)) return;
    GetSystemTimePreciseAsFileTime(&utc);
    if (!QueryPerformanceCounter(&after)) return;
    mkdir("logs", 0755);
    char path[128];
    snprintf(path, sizeof path, "logs/stall-diag-%lu-%llu.csv", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)before.QuadPart);
    output = fopen(path, "wx");
    if (!output) { fprintf(stderr, "[stall-diag] output unavailable; diagnostic disabled\n"); return; }
    fprintf(output, "# stall_diag_version=1 clock=QPC_ns pid=%lu\n# qpc_frequency=%llu qpc_before=%llu "
                    "utc_filetime_100ns=%llu qpc_after=%llu\n",
            (unsigned long)GetCurrentProcessId(), (unsigned long long)frequency.QuadPart,
            (unsigned long long)before.QuadPart, (unsigned long long)(((uint64_t)utc.dwHighDateTime << 32) | utc.dwLowDateTime),
            (unsigned long long)after.QuadPart);
    fprintf(output, "frame,guest_thread,begin_qpc_ns,end_qpc_ns,blocked_ns,game_thread_cpu_ns,render_thread_cpu_ns,"
                    "gpu_wait_all_threads_ns,mutex_ns,condvar_ns,arbiter_ns,event_vsync_only_ns,event_other_ns,sleep_ns,"
                    "queue_room_ns,present_ready_ns,blocked_unclassified_ns,lost_buffer_total,lost_limit_total\n");
    if (pthread_create(&writer, NULL, write_main, NULL)) {
        fprintf(stderr, "[stall-diag] writer unavailable; diagnostic disabled\n");
        fclose(output);
        return;
    }
    started = true;
    atomic_store_explicit(&g_stall_diag_enabled, true, memory_order_relaxed);
    atexit(stall_diag_shutdown);
#endif
}

void stall_diag_shutdown(void) {
    if (!started) return;
    atomic_store_explicit(&g_stall_diag_enabled, false, memory_order_relaxed);
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
    pthread_join(writer, NULL);
    started = false;
}
