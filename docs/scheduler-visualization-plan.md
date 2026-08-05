# Scheduler timeline and GPU activity visualization plan

## Status

Deferred follow-up work. This document describes the smallest implementation needed to produce a blog-ready scheduler timeline alongside macOS Activity Monitor's GPU History. It is a visualization and observability feature, not a new scheduling policy or a performance benchmark.

Estimated implementation and capture time: approximately 1.5–2.5 hours.

## Goal

Produce one final composite visual containing:

```text
┌───────────────────────────────────────────────┐
│ Terminal: all concurrent clients passed       │
├────────────────────────┬──────────────────────┤
│ Scheduler timeline SVG │ Activity Monitor GPU │
└────────────────────────┴──────────────────────┘
```

The three panels demonstrate different facts:

- The terminal proves that every controlled client completed and verified its result.
- The scheduler timeline shows when MAR enqueued, submitted, completed, and returned each job.
- Activity Monitor confirms aggregate activity on the physical Apple GPU during the experiment.

The timeline must label the interval between submission and completion as **Metal in-flight**, not **GPU executing**. MAR observes when it submits a command buffer and when Metal reports completion. Metal's driver controls the actual hardware schedule inside that interval.

## Minimal implementation

### 1. Instrument `server/server.cpp`

Add four monotonic timestamps to `Job`:

```cpp
std::chrono::steady_clock::time_point enqueued_time;
std::chrono::steady_clock::time_point submitted_time;
std::chrono::steady_clock::time_point completed_time;
std::chrono::steady_clock::time_point returned_time;
```

Use `std::chrono::steady_clock`, not wall-clock time. A monotonic clock cannot jump backward because of clock synchronization or a manual system-time change.

Record the timestamps at the existing state transitions:

| Timestamp | Location | Meaning |
| --- | --- | --- |
| `enqueued_time` | `CommitCommandBuffer()` after insertion into `ready_jobs_` | MAR accepted the validated job |
| `submitted_time` | `scheduler_loop()` immediately before `commit_job(job)` | MAR selected the job for submission |
| `completed_time` | `update_job_status_after_completion()` | Metal invoked the completion handler |
| `returned_time` | `WaitUntilCompleted()` after output copyback is prepared | MAR finished returning the result |

All timestamp writes must follow the same synchronization discipline as `Job::state`. Do not introduce unsynchronized reads and writes merely for instrumentation.

After successful result copyback, emit one complete record for the job:

```text
MAR_TIMELINE job=42 queue=7 enqueued_us=100 submitted_us=220 completed_us=510 returned_us=640 status=completed
```

Prefer one completed record per job over four separate log lines. A single record is easier to parse and reduces interleaved output from concurrent RPC handlers. Serialize the final output using the existing server mutex or a small dedicated logging mutex. Do not hold the global mutex while performing file I/O beyond emitting the already-formatted line.

The timestamps may be absolute `steady_clock` microseconds. The renderer will normalize them relative to the earliest observed enqueue time.

Failed jobs can optionally emit:

```text
MAR_TIMELINE job=43 queue=7 enqueued_us=... submitted_us=... failed_us=... status=failed
```

Failure visualization is not required for the first version.

### 2. Add `scripts/render_scheduler_timeline.py`

Implement a dependency-free Python script using only the standard library. It should:

1. Read a server log file.
2. Ignore every line that does not begin with `MAR_TIMELINE`.
3. Parse `key=value` fields.
4. Reject or skip incomplete records with a clear warning.
5. Sort jobs by enqueue time.
6. Normalize all times to the earliest enqueue time.
7. Render a standalone SVG file.

Suggested command:

```bash
python3 scripts/render_scheduler_timeline.py \
  /tmp/mar-server.log \
  /tmp/mar-scheduler-timeline.svg \
  --limit 30
```

The renderer should display a readable subset when hundreds of jobs exist. The first version can show the first 20–30 successfully returned jobs sorted by enqueue time.

Suggested visual encoding:

| Interval or marker | Color | Label |
| --- | --- | --- |
| Enqueued → submitted | Gray | Waiting in MAR |
| Submitted → completed | Blue | Metal in-flight |
| Completed → returned | Orange | Result copyback |
| Returned | Green marker | Client result ready |

Each row should be labeled with at least the job ID and command-queue ID. Add a horizontal millisecond axis and a legend. Include a subtitle that states that the chart shows MAR/Metal lifecycle timestamps rather than exact GPU hardware occupancy.

SVG is preferred because it requires no plotting dependency, stays sharp in the blog, and can be opened directly in a browser or Preview.

### 3. Make the visualization workload large enough

The normal adder contains only 100 elements and is too short for a readable Activity Monitor graph. Preserve 100 as the default while allowing a larger visualization build in `src/main.cpp`:

```cpp
#ifndef MAR_ARRAY_SIZE
#define MAR_ARRAY_SIZE 100
#endif

constexpr size_t MAXN = MAR_ARRAY_SIZE;
```

Build the visualization client with a larger array:

```bash
bazel build \
  --//src:shim=true \
  --copt=-DMAR_ARRAY_SIZE=262144 \
  //src:adder
```

Three buffers at this size contain approximately 3 MiB in total:

```text
262,144 elements × 4 bytes × 3 buffers = 3,145,728 bytes
```

That remains below gRPC's commonly configured 4 MiB default message limit for the commit payload. Confirm the actual serialized request size during implementation; protobuf metadata adds some overhead. Reduce the array size if the request approaches the configured limit.

If the larger array still produces an unreadably short GPU graph, run controlled batches for 15–30 seconds. Avoid changing the normal adder defaults or presenting the synthetic workload as a performance benchmark.

## Optional convenience script

If manual capture becomes cumbersome, add `scripts/run_scheduler_visualization.sh`. It may:

1. Build the larger shim-enabled adder once.
2. Launch a configurable number of concurrent clients in batches.
3. Capture and verify each client's output.
4. Report failures.
5. Invoke `render_scheduler_timeline.py` after the server log is complete.

This wrapper is optional. Do not add it until the three-file manual workflow works.

## Manual capture workflow

Build the instrumented server and visualization client:

```bash
bazel build //server:server
bazel build \
  --//src:shim=true \
  --copt=-DMAR_ARRAY_SIZE=262144 \
  //src:adder
```

Start the server and capture its diagnostic stream:

```bash
./bazel-bin/server/server 2> /tmp/mar-server.log
```

Open the macOS GPU graph:

```text
Activity Monitor → Window → GPU History
```

Run the controlled concurrent workload. Keep it active for long enough to make the GPU History graph readable. Verify every client result exactly as `scripts/test_concurrent_adders.sh` already does.

After the run, generate the timeline:

```bash
python3 scripts/render_scheduler_timeline.py \
  /tmp/mar-server.log \
  /tmp/mar-scheduler-timeline.svg \
  --limit 30
```

Arrange the successful-client terminal, timeline SVG, and Activity Monitor GPU History in one screenshot.

## Interpretation and claim boundaries

The finished visual can support these claims:

- Multiple client processes completed correct remote computations.
- MAR admitted jobs and submitted them through its FIFO scheduler.
- Metal reported completion for those command buffers.
- The physical Apple GPU showed aggregate activity during the controlled run.

It cannot by itself support these claims:

- A specific command buffer occupied the GPU continuously from submit to completion.
- MAR improved utilization relative to native Metal.
- MAR improved throughput or latency.
- FIFO is an intelligent scheduling policy.
- Activity Monitor proves causation for each individual job.

Any future utilization claim requires a controlled comparison using the same workload, transport, buffer sizes, and measurement window under two scheduling policies.

## Expected files

Required:

- `server/server.cpp` — lifecycle timestamps and machine-readable log record
- `scripts/render_scheduler_timeline.py` — log parser and SVG renderer
- `src/main.cpp` — compile-time visualization array-size override

Optional:

- `scripts/run_scheduler_visualization.sh` — reproducible orchestration
- `docs/assets/` or another artifact directory — generated example SVG, if the repository should preserve it

The final blog asset should be copied separately into the website repository only after the experiment and caption are finalized.

## Verification checklist

- Normal `MAXN == 100` build still compiles and passes.
- Native build with `--//src:shim=false` still compiles and passes.
- Remote adder still verifies its result.
- Existing 100-client script still passes.
- Timeline instrumentation does not change scheduler ordering.
- Timeline output remains parseable under concurrent completion.
- SVG opens in a browser and labels intervals accurately.
- Activity Monitor capture lasts long enough to be legible.
- No screenshot exposes credentials, tokens, private keys, instance addresses, or unrelated personal information.
