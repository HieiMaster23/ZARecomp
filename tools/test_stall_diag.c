#define STALL_DIAG_QUEUE_CAP 4
#define STALL_DIAG_RECORD_CAP 64
#include "../runtime/stall_diag.c"
#include <assert.h>

static void reset_test(void) {
    memset(&local, 0, sizeof local);
    head = count = accepted = 0;
    stopping = false;
    atomic_store(&lost_buffer, 0);
    atomic_store(&lost_limit, 0);
    atomic_store(&g_stall_diag_enabled, true);
}

static StallRecord take(void) {
    assert(count);
    StallRecord r = records[head];
    head = (head + 1) % STALL_DIAG_QUEUE_CAP;
    count--;
    return r;
}

static void frame_at(uint64_t frame) {
    stall_diag_frame(frame, 7, frame * 150000000ull, frame * 10000000ull,
                     frame * 1000000ull, frame * 2000000ull, frame * 3000000ull);
}

static void *thread_test(void *arg) {
    (void)arg;
    frame_at(1);
    StallSpan s;
    stall_diag_begin(&s, STALL_EVENT_VSYNC, 150000000);
    stall_diag_end(&s, 155000000);
    stall_diag_frame(2, 9, 300000000, 5000000, 0, 0, 0);
    return NULL;
}

static void unit_tests(void) {
    reset_test();
    frame_at(1);
    for (int i = 0; i < STALL_CAUSES; i++) {
        StallSpan s;
        stall_diag_begin(&s, (StallCause)i, 150000000 + i * 1000);
        stall_diag_end(&s, 150000000 + i * 1000 + 100);
    }
    frame_at(2);
    StallRecord r = take();
    assert(r.frame == 2 && r.thread == 7 && r.begin == 150000000 && r.end == 300000000);
    for (int i = 0; i < STALL_CAUSES; i++) assert(r.cause[i] == 100);
    assert(r.game_cpu == 1000000 && r.render_cpu == 2000000 && r.gpu_wait == 3000000);
    StallSpan outer, inner;
    stall_diag_begin(&outer, STALL_MUTEX, 310000000);
    stall_diag_begin(&inner, STALL_SLEEP, 320000000);
    stall_diag_end(&inner, 350000000);
    stall_diag_end(&outer, 410000000);
    frame_at(3);
    r = take();
    assert(r.cause[STALL_MUTEX] == 70000000 && r.cause[STALL_SLEEP] == 30000000);
    pthread_t t;
    assert(!pthread_create(&t, NULL, thread_test, NULL));
    pthread_join(t, NULL);
    r = take();
    assert(r.thread == 9 && r.cause[STALL_EVENT_VSYNC] == 5000000 && !r.cause[STALL_MUTEX]);
    frame_at(1);
    assert(!count);
    frame_at(2);
    r = take();
    assert(!r.cause[STALL_MUTEX] && r.game_cpu == 1000000);

    reset_test();
    frame_at(1);
    pthread_mutex_lock(&lock);
    frame_at(2);
    pthread_mutex_unlock(&lock);
    assert(atomic_load(&lost_buffer) == 1 && !count);
    for (int i = 3; i <= 7; i++) frame_at(i);
    assert(count == 4 && atomic_load(&lost_buffer) == 2);
    for (int i = 0; i < 4; i++) take();

    reset_test();
    frame_at(1);
    for (int i = 2; i <= 71; i++) {
        frame_at(i);
        if (count) take();
    }
    assert(accepted == 64 && atomic_load(&lost_limit) == 6 && !atomic_load(&lost_buffer));
    reset_test();
    frame_at(1);
    stall_diag_frame(2, 7, 250000000, 0, 0, 0, 0);
    assert(!count);
    stall_diag_frame(3, 7, 350000001, 0, 0, 0, 0);
    assert(count == 1);
    r = take();
    assert(!r.game_cpu && !r.render_cpu && !r.gpu_wait);
    atomic_store(&g_stall_diag_enabled, false);
    uint64_t before = local.totals[STALL_SLEEP];
    StallSpan off;
    stall_diag_begin(&off, STALL_SLEEP, 0);
    stall_diag_end(&off, 999999999);
    frame_at(100);
    assert(local.totals[STALL_SLEEP] == before && !count);
    puts("PASS: categories, nesting, thread isolation, reset, threshold, contention, overflow, record cap and off");
}

#ifdef _WIN32
static uint64_t ticks(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint64_t)q.QuadPart;
}

static void bench(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (int on = 0; on <= 1; on++) {
        double best = 1e30;
        for (int round = 0; round < 3; round++) {
            reset_test();
            atomic_store(&g_stall_diag_enabled, on);
            uint64_t t0 = ticks();
            for (uint64_t i = 1; i <= 1000000; i++) {
                StallSpan s;
                stall_diag_begin(&s, STALL_SLEEP, i);
                stall_diag_end(&s, i + 1);
                stall_diag_frame(i, 7, i + 2, i, i, i, i);
            }
            double ns = (ticks() - t0) * 1e9 / f.QuadPart / 1000000;
            if (ns < best) best = ns;
        }
        printf("BENCH enabled=%d best_ns_per_span_and_frame=%.2f budget_ns=%d\n", on, best, on ? 1000 : 100);
        assert(best < (on ? 1000 : 100));
    }
}
#endif

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "emit")) {
        stall_diag_init();
        if (!atomic_load(&g_stall_diag_enabled)) return 2;
        for (uint64_t i = 1; i <= 41; i++) {
            StallSpan s;
            stall_diag_begin(&s, STALL_EVENT_OTHER, i * 150000000ull);
            stall_diag_end(&s, i * 150000000ull + 1000000);
            frame_at(i);
#ifdef _WIN32
            Sleep(2);
#endif
        }
        stall_diag_shutdown();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "off")) {
        stall_diag_init();
        assert(!atomic_load(&g_stall_diag_enabled) && !started && !output);
        frame_at(1);
        frame_at(2);
        return 0;
    }
    unit_tests();
#ifdef _WIN32
    bench();
#endif
    return 0;
}
