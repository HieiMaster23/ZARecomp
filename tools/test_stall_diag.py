"""ROM-free stall diagnostic schema, bounded output and production kernel checks."""
import csv
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = shlex.split(os.environ.get('CC', 'clang'))
common = ['-std=gnu11', '-O2', '-pthread', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
env = os.environ.copy()
env.pop('LARECOMP_STALL_DIAG', None)

with tempfile.TemporaryDirectory(prefix='stall-diag-') as temporary:
    temporary = Path(temporary)
    exe = temporary / ('diag.exe' if os.name == 'nt' else 'diag')
    subprocess.run(cc + common + [str(root/'tools/test_stall_diag.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=temporary, env=env, check=True, timeout=20)
    for value in [None, '0', 'false', '2']:
        off = env.copy()
        if value is not None:
            off['LARECOMP_STALL_DIAG'] = value
        subprocess.run([str(exe), 'off'], cwd=temporary, env=off, check=True)
    assert not (temporary/'logs').exists()
    if os.name == 'nt':
        subprocess.run([str(exe), 'emit'], cwd=temporary, env=env | {'LARECOMP_STALL_DIAG':'1'}, check=True)
        output, = (temporary/'logs').glob('stall-diag-*.csv')
        text = output.read_text()
        lines = text.splitlines()
        assert lines[0].startswith('# stall_diag_version=1 clock=QPC_ns pid=')
        anchor = dict(re.findall(r'(\w+)=(\d+)', lines[1]))
        assert int(anchor['qpc_frequency']) > 0
        assert int(anchor['qpc_before']) <= int(anchor['qpc_after'])
        assert int(anchor['utc_filetime_100ns']) > 0
        rows = list(csv.DictReader(line for line in lines if not line.startswith('#')))
        summary = re.fullmatch(r'# end accepted=(\d+) lost_buffer=(\d+) lost_limit=(\d+)', lines[-1])
        assert summary and len(rows) == int(summary[1])
        assert len(rows) + int(summary[2]) + int(summary[3]) == 40
        assert len(rows) > 20
        assert len(rows) <= 64 and all(len(row) == 19 for row in rows)
        for row in rows:
            assert all(re.fullmatch(r'\d+', value) for value in row.values())
            assert int(row['end_qpc_ns']) - int(row['begin_qpc_ns']) == 150000000
        assert not any(s in text.lower() for s in ['savedata', 'romfs', 'password', 'input', 'handle', 'c:\\', 'd:\\'])

    kernel_source = (root/'tools/test_condvar.c').read_text()
    kernel_source = kernel_source.replace('int main(void)', 'int original_condvar_main(void)')
    kernel_source = kernel_source.replace('uint32_t vi_vsync_handle(void) { abort(); }',
                                          'uint32_t vi_vsync_handle(void) { return test_vsync; }')
    kernel_source = kernel_source.replace('#include "../runtime/', '#include "' + root.as_posix() + '/runtime/')
    production_kernel = (root/'runtime/kernel.c').read_text()
    signature = 'static bool kwait(int64_t timeout_ns, const struct timespec *ts) {'
    assert production_kernel.count(signature) == 1
    production_kernel = production_kernel.replace(signature, signature + '\n    if (t_cur && t_cur->wait_kind == WAIT_MUTEX) test_mutex_reentries++;')
    kernel_source = kernel_source.replace('#include "' + root.as_posix() + '/runtime/kernel.c"', production_kernel)
    harness = '#include <stdint.h>\nstatic uint32_t test_vsync;\nstatic unsigned test_mutex_reentries;\n' + kernel_source
    harness += '\n#include "' + root.as_posix() + '/runtime/stall_diag.c"\n'
    harness += r'''
void audio_wait_dump(void) { abort(); }
void *hle_alloc(uint64_t size) { (void)size; abort(); }
static int extra_wake;
static void *handoff_waiter(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_klock);
        bool ready = g_waitlist == &waiter && waiter.wait_kind == WAIT_CONDVAR;
        if (ready) mutex_word = 0x222;
        pthread_mutex_unlock(&g_klock);
        if (ready) break;
        sched_yield();
    }
    svc_signal_process_wide_key((uint64_t)&condition_word, 1);
    pthread_mutex_lock(&g_klock);
    assert(waiter.wait_kind == WAIT_MUTEX);
    unsigned before = test_mutex_reentries;
    if (extra_wake) pthread_cond_signal(&waiter.cond);
    pthread_mutex_unlock(&g_klock);
    if (extra_wake) {
        for (;;) {
            pthread_mutex_lock(&g_klock);
            bool sleeping_again = test_mutex_reentries > before;
            pthread_mutex_unlock(&g_klock);
            if (sleeping_again) break;
            sched_yield();
        }
    }
    struct timespec delay = {0, 5000000};
    nanosleep(&delay, NULL);
    pthread_mutex_lock(&g_klock);
    mutex_release((uint64_t)&mutex_word);
    pthread_mutex_unlock(&g_klock);
    return NULL;
}
int main(void) {
    assert(original_condvar_main() == 0);
    pthread_cond_init(&waiter.cond, NULL);
    atomic_store(&g_stall_diag_enabled, true);
    t_cur = &waiter;
    for (extra_wake = 0; extra_wake <= 1; extra_wake++) {
        mutex_word = waiter.handle;
        uint64_t m = local.totals[STALL_MUTEX], c = local.totals[STALL_CONDVAR], b = g_blocked_ns;
        pthread_t signaler;
        assert(!pthread_create(&signaler, NULL, handoff_waiter, NULL));
        assert(svc_wait_process_wide_key((uint64_t)&mutex_word, (uint64_t)&condition_word,
                                         waiter.handle, -1) == R_OK);
        pthread_join(signaler, NULL);
        assert(mutex_word == waiter.handle);
        assert(local.totals[STALL_MUTEX] - m >= 4000000);
        assert(local.totals[STALL_CONDVAR] > c);
        assert(local.totals[STALL_MUTEX] - m + local.totals[STALL_CONDVAR] - c == g_blocked_ns - b);
        pthread_mutex_lock(&g_klock);
        mutex_release((uint64_t)&mutex_word);
        pthread_mutex_unlock(&g_klock);
    }
    for (int k = WAIT_MUTEX; k <= WAIT_ARBITER; k++) {
        waiter.wait_kind = (WaitKind)k;
        pthread_mutex_lock(&g_klock);
        struct timespec ts;
        deadline(&ts, 1000000);
        assert(!kwait(1000000, &ts));
        pthread_mutex_unlock(&g_klock);
        assert(local.totals[k - 1] > 0);
    }
    waiter.wait_kind = WAIT_NONE;
    uint32_t write_handle, handle;
    KEvent *event = kernel_create_event_pair(&write_handle, &handle);
    test_vsync = handle;
    int32_t index = -1;
    assert(svc_wait_synchronization((uint64_t)&handle, 1, 1000000, &index) == R_TIMEOUT);
    assert(local.totals[STALL_EVENT_VSYNC] > 0);
    test_vsync = 0;
    assert(svc_wait_synchronization((uint64_t)&handle, 1, 1000000, &index) == R_TIMEOUT);
    assert(local.totals[STALL_EVENT_OTHER] > 0);
    uint64_t before[STALL_CAUSES];
    memcpy(before, local.totals, sizeof before);
    waiter.cancel = true;
    assert(svc_wait_synchronization((uint64_t)&handle, 1, -1, &index) == R_CANCELLED);
    assert(!memcmp(before, local.totals, sizeof before));
    pthread_mutex_lock(&g_klock);
    kernel_signal(event);
    pthread_mutex_unlock(&g_klock);
    assert(svc_wait_synchronization((uint64_t)&handle, 1, -1, &index) == R_OK && index == 0);
    assert(!memcmp(before, local.totals, sizeof before));
    Cpu cpu = {0};
    cpu.x[0] = 1000000;
    svc_impl(&cpu, 0x0b, 0);
    assert(local.totals[STALL_SLEEP] > 0);
    pthread_cond_destroy(&waiter.cond);
    puts("PASS: production waits with diagnostic enabled preserve timeout, cancel and signaled event results");
    return 0;
}
'''
    cfile = temporary/'kernel.c'
    cfile.write_text(harness)
    kernel = temporary/('kernel.exe' if os.name == 'nt' else 'kernel')
    subprocess.run(cc + common + ['-I' + str(root/'runtime'), str(cfile), '-o', str(kernel)], check=True)
    subprocess.run([str(kernel)], cwd=temporary, env=env, check=True, timeout=20)

    queue_source = (root/'runtime/nvn/rqueue.c').read_text()
    wait = queue_source[queue_source.index('static void wait_until('):queue_source.index('\nstatic bool has_room(')]
    wait = wait.replace('&lock', '&queue_lock')
    harness = '#include "' + root.as_posix() + '/runtime/stall_diag.c"\n' + r'''
#include <assert.h>
#define LOG(...) ((void)0)
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int g_rt_unwind, presents_pending;
static uint64_t q_enq, q_done, q_cur;
static void rq_watch(const char *what, uint64_t arg) { (void)what; (void)arg; }
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
''' + wait + r'''
static int calls;
static bool available(uint64_t arg) { (void)arg; return ++calls >= 3; }
int main(void) {
    atomic_store(&g_stall_diag_enabled, true);
    pthread_mutex_lock(&queue_lock);
    wait_until(&cv, available, 0, false, "queue full");
    assert(local.totals[STALL_QUEUE_ROOM] > 0 && !local.totals[STALL_PRESENT_READY]);
    calls = 0;
    wait_until(&cv, available, 0, true, "present (the previous one to finish)");
    assert(local.totals[STALL_PRESENT_READY] > 0);
    g_rt_unwind = 1;
    calls = 0;
    wait_until(&cv, available, 0, true, "present (the previous one to finish)");
    assert(calls == 2);
    pthread_mutex_unlock(&queue_lock);
    puts("PASS: production queue wait attribution and unwind");
    return 0;
}
'''
    cfile.write_text(harness)
    queue = temporary/('queue.exe' if os.name == 'nt' else 'queue')
    subprocess.run(cc + common + [str(cfile), '-o', str(queue)], check=True)
    subprocess.run([str(queue)], cwd=temporary, env=env, check=True, timeout=5)

pace = (root/'runtime/nvn/pace.c').read_text()
assert pace.index('stall_diag_frame(') < pace.index('if (first || p > 100000000ull)')
print('PASS: schema/privacy, disabled output, >20 hitches, production kernel and >100 ms diagnostic hook')
