# Windows stall diagnostic

`LARECOMP_STALL_DIAG=1` enables a ROM-independent, opt-in diagnostic in the next
process launch. Unset it or use `0` to disable it. It does not attach to a running
process or change pacing, resolution, waits, timeouts or game speed. Other values
are disabled. Non-Windows builds leave this diagnostic disabled.

Output is `logs/stall-diag-<pid>-<qpc>.csv`, never an existing file. Only the writer
thread writes records. The presenting thread uses a bounded nonblocking queue;
contention/full queues drop diagnostic records instead of delaying the game.
There are at most 4096 records per process and 256 queued records. Header and file
creation happen at startup, outside the frame path. Normal shutdown drains the
queue; a crash or forced exit can leave an incomplete capture. A missing `# end`
footer or an output error means it must not be treated as complete.

Schema version 1 contains integer nanoseconds and numeric identifiers only:

- `frame,guest_thread`: runtime presentation counter and guest thread identifier.
- `begin_qpc_ns,end_qpc_ns`: the two presentation-boundary samples from Windows'
  `plat_now_ns`, which is `floor(QPC * 1e9 / frequency)`. Convert PresentMon raw QPC
  ticks with the same formula; these are not wall-clock or SDL-relative seconds.
  The header brackets a UTC FILETIME anchor between two raw QPC reads and records
  their frequency. The bracket gives the anchor's uncertainty.
- `blocked_ns`: existing blocked counter delta on this presenting thread.
- `game_thread_cpu_ns,render_thread_cpu_ns,gpu_wait_all_threads_ns`: existing CPU
  and GPU-wait counter deltas, not mutually exclusive parts of blocked time.
- `mutex_ns,condvar_ns,arbiter_ns,event_vsync_only_ns,event_other_ns,sleep_ns`:
  exclusive measured waits on the presenting thread. Vsync means a wait set with
  exactly the vsync event; mixed sets are other events, not proof of which event
  ultimately caused a wake. Sleep measures actual elapsed time, not requested time.
- `queue_room_ns,present_ready_ns`: render-queue capacity and the gate requiring
  both room and completion of the previous present. These are condition waits,
  not the whole enqueue call or its mutex acquisition.
- `blocked_unclassified_ns`: nonnegative remainder of existing blocked time.
  It can include backend acquire/slot/pacing waits; it is not automatically CPU
  work or IO. Nested diagnostic spans charge only their exclusive duration.
- `lost_buffer_total,lost_limit_total`: cumulative diagnostic losses, also reported
  in the footer, including events after the 4096-record limit. They are not game
  frame drops. Lost records are never silently replaced with averages.

Only gaps **strictly greater than 100 ms** are emitted, without the existing slow
log's 20-lines/minute or loading suppression. Ordinary frames still update the
previous snapshot, so a hitch reports one interval, not time since the last hitch.
A counter rollback/in-place reboot starts a new baseline. No content, paths,
raw handles, inputs or stacks are recorded. Completed asynchronous GPU work can
overlap CPU work; do not add the durations together or infer a cause from a single
category without checking the event semantics.

With the flag off there is no diagnostic file, worker, allocation or extra clock
read on the frame/wait path. With it on, overhead is clocks at the queue wait
boundary, TLS counter updates and per-frame snapshots; it is not zero. It does not
instrument individual draws or change GPU timing instrumentation.

Run `python tools/test_stall_diag.py` with `CC` naming a native Clang toolchain and
`python tools/test_fence_sequence.py`. The harness uses synthetic clocks/data and
the production kernel, not the game. Its microbenchmark prints off/on costs and
checks conservative budgets of 100/1000 ns per span-plus-frame on the test device;
this is not an in-game overhead or FPS guarantee.
