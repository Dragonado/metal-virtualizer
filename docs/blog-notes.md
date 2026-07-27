# Blog Notes

Running notes and insights worth writing up later.

## Proto skew fails on a spectrum, and the quiet end is the dangerous one

The `.proto` is a compile-time artifact only. `protoc` bakes the generated
code into each binary; after that the wire never carries, exchanges, or
verifies a schema. There is no version handshake where client and server
compare protos. Each process runs whatever contract was frozen in at its own
build time. So if the server is up and you rebuild only the client with a
changed proto, the two disagree, and gRPC's reaction depends entirely on *how*
they disagree:

- New RPC (client calls a method the old server never registered): clean
  `UNIMPLEMENTED` (status 12). Loud, you notice immediately.
- Added fields on an existing message: no error at all. Unknown fields are
  ignored by the old side; absent fields read as proto3 defaults (0, "").
  This silence is deliberate: it is exactly what lets old and new binaries
  coexist during a rolling deploy.
- Renumbered or retyped fields: silently wrong data. Field *names* never
  travel the wire, only numbers. Renumber and the old side decodes your bytes
  under its old numbering, so values land in the wrong fields and the rest
  read as zero. No error, no warning. A `device_id` of 0 arriving at a server
  whose handle map starts at 1.

The uncomfortable point is that the property enabling zero-downtime upgrades
(tolerate the other side being a different version) is the same property that
makes local dev skew invisible. The forward/backward compatibility is not a
safety net catching your mistake, it is the thing hiding it.

Two rules fall out: restart the server after every proto change during dev,
and never reuse or renumber a field id once it has shipped in any running
binary. Field numbers are a permanent namespace; treat a retired number like a
dropped database column, not like a renamed local variable.

## Unified memory is the zero-copy freedom the network destroys

`buffer->contents()` returns a CPU pointer into the buffer. In the vector-add
program, `populate_random_float` writes inputs through it and `verify` reads
results through it. On Apple Silicon this works with zero copies because of
unified memory: the CPU and GPU share the same physical DRAM, so a Shared
buffer is one allocation both sides address directly.

That zero-copy freedom is exactly what the network will destroy and the shim
will have to rebuild. Once a wire sits between the client and the GPU, the
client's `contents()` pointer and the server's GPU memory are no longer the
same bytes. The remoting layer has to re-introduce the discrete-GPU copy model
(snapshot inputs at commit, blit outputs back on completion) on top of hardware
that is physically unified. The hardware gives it away for free; the network
takes it back.

## A GPU program is a submission protocol, not a function call

The mental unlock for all of Metal's apparent ceremony is that you are not
calling GPU code. The GPU is a physically separate processor that runs
asynchronously and consumes a stream of commands you produce for it. Every
object (queue, command buffer, encoder) exists to build that stream, ship it
across the CPU/GPU boundary, and learn when it finished. Read the API as "I am
a client writing a work order for a remote worker and handing it off," and the
objects stop looking like boilerplate and start looking like the necessary
parts of a hand-off protocol.

This is also why Metal maps so cleanly onto a remoting project: it is already a
remoting API in miniature, where the GPU is the remote. Building GPU
virtualization means inserting a network where Metal already drew the
client/worker line.

## GPU work often has no CPU-visible result

Submitting GPU work does not imply that the CPU will read its output. The
common vector-add demo does exactly that:

```text
GPU writes C -> CPU waits -> CPU reads C -> prints OK
```

Real GPU programs often form a GPU-only pipeline instead:

```text
GPU job A writes an intermediate buffer
GPU job B reads that buffer
GPU job C renders it to the display
CPU never reads the intermediate bytes
```

A renderer writes pixels into a drawable that the display presents. A machine
learning pipeline produces an intermediate tensor consumed by the next kernel.
A temporary upload buffer is input only. In each case, a CPU round trip to read
the bytes is pointless. The CPU can submit work, release temporary ownership,
and prepare the next job while the GPU runs.

That raises an important lifetime question. Suppose both A and B use the same
buffer, and the application drops its own reference after it submits them:

```text
application owns buffer
command buffer A retains buffer
command buffer B retains buffer

application releases buffer
A finishes and releases its reference
B runs, then releases its reference
buffer is destroyed only after B finishes
```

This is reference counting, not the GPU somehow guessing that B will need the
buffer. When the CPU encodes B and binds the buffer, B's command buffer records
that dependency and retains the resource. Each normal Metal command buffer
keeps strong references to the resources it needs by default. The application
may release its own reference once every future command buffer that needs the
resource has already taken a reference.

The order matters. If the application releases its last reference after A but
before it has encoded B, then B has no valid buffer pointer to bind. The buffer
can be destroyed when A finishes. The API cannot preserve a future dependency
that has not yet been expressed.

This explains two remoter rules. First, a scheduled `PendingJob` must retain its
server-side buffers, pipeline, and queue as soon as it enters the job queue.
Second, the remote `waitUntilCompleted()` byte copyback is an emulation detail,
not a property of native Metal. It is needed only when a client still owns a
shadow buffer and wants to read it. GPU-only intermediate results can stay
server-side for the next job.

That last sentence is the desired end state, not what the current prototype
implements. Today every `commit()` copies every bound client shadow buffer to
the server. If A writes buffer X and B immediately uses X without a wait, the
client's shadow copy of X is still old. B's commit uploads those old bytes and
can overwrite A's server-side result before B runs. The current correct path
for that dependency is therefore `A commit -> A wait/copyback -> B commit`.
Keeping GPU-only intermediates server-side needs a coherence rule: upload a
buffer only after the client has actually modified its shadow, not on every
commit. Detecting such CPU writes requires explicit modification calls, dirty
tracking, or a different server-authoritative buffer design.

Apple documents the default retained-reference behavior for command buffers in
[its Metal documentation](https://developer.apple.com/documentation/metal/mtlcommandbufferdescriptor/retainedreferences?changes=__1).

## One queue is also an ordering contract

GPU-only chaining works because command buffers committed to the same native
Metal queue preserve their order. The CPU can express a dependency without
waiting for A on the CPU:

```cpp
// A writes intermediate_buffer.
command_buffer_a->commit();

// B reads intermediate_buffer.
command_buffer_b->commit();

// No A->waitUntilCompleted() is needed here.
```

```text
same command queue:
  A commits: write X
  B commits: read X

  Metal executes A's commands before B can observe X
```

This is not an accidental implementation detail. It is the ordering contract
that makes the pipeline correct. A scheduler must preserve it. It may insert
client B's independent work between client A's two jobs, but it must never
submit A2 before A1:

```text
valid:   A1 -> B1 -> A2
invalid: A2 -> B1 -> A1
```

The latter can make A2 read data that A1 was supposed to write first. A single
scheduler thread submitting to one real server queue gives this first version a
simple ordering point. Different native Metal queues do not automatically order
one another; cross-queue dependencies need explicit synchronization, such as a
shared event. Apple describes same-queue command ordering in its
[command-structure documentation](https://developer.apple.com/documentation/Metal/setting-up-a-command-structure)
and [`commit()` documentation](https://developer.apple.com/documentation/metal/mtlcommandbuffer/commit%28%29?changes=__6&language=objc).

## A command queue is an ordered asynchronous submission lane

The queue is not the GPU, and it is not one GPU job. A command buffer is one
job; a command queue is the ordered lane that accepts many jobs:

```text
one command queue Q
  command buffer A1
  command buffer A2
  command buffer A3
```

That is why submission is `command_buffer->commit()`, not `queue->commit()`.
The command buffer is saying, "submit this particular job to the queue that
created me." A queue can have many unfinished command buffers, so
`queue->commit()` would not say which job should be submitted.

The important benefit is dependent asynchronous work. If A writes X and B reads
X, the CPU can submit both to the same queue and continue doing unrelated work:

```cpp
command_buffer_a->commit(); // A writes X.
command_buffer_b->commit(); // B reads X.

// The CPU does not need A->waitUntilCompleted() here.
```

```text
same queue:
  A writes X -> B reads X
  Metal preserves this order

different queues:
  A writes X -> B reads X
  no automatic ordering; use an explicit event
```

The queue's core purpose is therefore an ordered asynchronous submission lane.
It creates command buffers, gives them a common ordering scope, and hands their
work to Metal for scheduling. It does not own a GPU, reserve GPU time, promise
a dedicated resource slice, or make every calculation physically run one after
another. It guarantees the observable command order; the driver and hardware
can still pipeline internal execution.

An API could have hidden one global queue and still support asynchronous GPU
work. Metal exposes queues because applications often need separate ordered
streams and explicit control over how their jobs enter the GPU.

## You cannot compile GPU code ahead of time

Why is the Metal kernel compiled at runtime instead of bundled into the
executable like the rest of the program? Because the final GPU machine code
does not exist until you are on the actual machine. Two reasons.

First, the CPU and GPU are different processors with different instruction sets,
so one compiler invocation cannot emit both. That is why you cannot `#include` a
`.metal` file.

Second, and deeper, the GPU ISA is deliberately unstable and private: M1, M2,
M3 differ, and it can shift across driver versions. So Apple refuses to let you
ship final GPU code. The pipeline is staged. Offline, MSL source compiles to
AIR, a portable LLVM-based bytecode (a `.metallib` is a bag of AIR). At runtime,
on the target machine, the driver compiles AIR to the exact ISA for this GPU and
this driver. That last step is irreducibly runtime, because only the installed
driver knows the real ISA.

This is not a Metal quirk. It is universal: Vulkan ships SPIR-V, CUDA ships PTX,
D3D12 ships DXIL. Everyone ships portable IR and JITs the last mile in the
driver, to decouple the app from the hardware generation. Even with offline
compilation you still pay the AIR-to-ISA cost at pipeline-state creation. The
JIT was never avoidable.

## Metal's verbosity is a map of where to put your network

The thing that feels like bureaucracy (device, queue, library, pipeline state,
command buffer, encoder) is really an amortization schedule made explicit.
Modern GPU APIs split expensive-and-rare from cheap-and-frequent, then make the
reused object immutable so the driver validates it once. Compiling a pipeline,
creating a queue, allocating buffers: expensive, done once. Encoding commands
and committing: cheap, done constantly.

For anyone building a remoting layer this structure is a gift, because the seams
Metal chose for amortization are exactly the seams to cut the wire along. The
create-group handles (device, queue, library, pipeline state, buffers) get
established once per session and cached by handle: that is the control plane.
The record and execute groups (encoder calls, commit) stream across per
invocation: that is the command plane. Metal already did the hard architectural
work of finding where the cheap/expensive boundary sits. The virtualization
layer should put its network on the same line.

## Which call goes in which plane, and the one test that decides

The split (control vs command) only helps once you can put each concrete method
on one side. The tempting test is "does the client look at the result?" but
that is wrong: it describes what one particular client program happens to do,
not the method itself. Bet your class layout on it and a different client that
inspects something you filed under command-plane forces you to rebuild, not add
a method.

The real test is about the return value itself: **can the client make up the
answer on its own, or does only the server know it?**

- If only the server knows it, the client has to stop and wait for the real
  answer before it can go on. That is a round trip. Control plane. Examples: did
  the shader compile or fail? what is the device name? did the allocation
  succeed?
- If the return value is just a ticket that the client only ever hands back to
  the server later, the client can make up the ticket itself (pick a number,
  agree the server will bind it to the real object at replay) and keep going.
  Record the call, send it later. Command plane.

The "does the client look at it" thing is just a side effect of this. The client
checks the pipeline state for null because only the server knows whether the
compile worked. It never checks the command buffer because there is nothing to
know: it is just a ticket.

Control plane (each waits for a real answer only the server can give):

- `MTLCreateSystemDefaultDevice` -> the whole session hangs off this; client
  checks it is non-null.
- `device->newCommandQueue()` -> a lasting object made once and reused every
  dispatch. It has to exist on the server before any commit can name it as where
  to submit.
- `device->newLibrary(source, ...)` -> the client needs to know if the shader
  compiled. Also where the MSL source crosses to the Mac to be compiled.
- `library->newFunction(name)` -> can fail if the name is not found; client
  needs to know.
- `device->newComputePipelineState(func, &err)` -> can fail; also where the
  final GPU-specific compile actually runs on the server.
- `device->newBuffer(length, options)` -> allocation the server has to really
  do, and the client is about to write inputs into it through `contents()`.
- `device->name()` and other queries -> the value only the server has.

Command plane (recorded locally, nothing sent until commit):

- `queue->commandBuffer()` -> starts an empty recording. Nothing on the server
  yet; the proxy just remembers which queue to submit to later.
- `commandBuffer->computeCommandEncoder()` -> a recorder that writes into the
  command buffer's list. Local only.
- `encoder->setComputePipelineState(pso)` -> writes down a step naming the
  pipeline state (already made in the control plane).
- `encoder->setBuffer(buf, offset, index)` -> writes down a step naming a buffer
  plus a couple numbers.
- `encoder->dispatchThreads(grid, threadgroup)` -> writes down a step of
  numbers.
- `encoder->endEncoding()` -> writes down an end marker.

The two at the boundary:

- `commit()` is the one command-plane call that actually goes on the wire. It is
  the flush: it sends the queue id and the whole recorded list (plus the input
  buffers the client wrote into, which is the coherence part) in a single round
  trip, and the server replays the steps against the real objects.
- `waitUntilCompleted()` is where the client waits for the GPU to finish. It
  might be free (if commit already waited) or a second wait on the wire, but
  either way it is not a per-step round trip.

One method looks like it should go on the wire and does not:
`pso->maxTotalThreadsPerThreadgroup()`. Only the server knows the value, so by
the test it should be a round trip. But it is a fixed property of the pipeline
state you just created, so you send it back on that creation response and it
costs nothing extra. Worth looking for this in general: a read that is really a
constant fact about an object you just made never needs its own trip.

The count for one dispatch: about six round trips for all the setup, then one
commit per dispatch carrying the whole recorded batch, plus at most one wait.
The naive "every method is a round trip" version pays nine or more per dispatch.
That gap is the whole reason the command plane records instead of calling.

A caveat on all of this, and it is the honest one. Even "can the client make up
the answer" is read off this one workload. The fully general version does not
label methods at all: it defers everything, hands back a made-up ticket for
every call, and only stops to wait when the client asks for something only the
server knows. Then no new client can ever break the layout, because the rule is
just "wait when someone needs a real answer." That is promise pipelining, the
same idea behind async/await. The MVP is allowed to hard-label each method only
because the same person writes every client, so the set of calls is known and
fixed. If that stopped being true, the labels would have to become a runtime
decision. And the messy case that shows the seam: `newBuffer` returns a ticket
(could be made up) but can also fail (only the server knows). When a call is
both, the failure wins and it round-trips, unless you gamble that it will not
fail and handle errors late.

## N handles, one GPU: the gap where virtualization lives

An Apple Silicon Mac has exactly one GPU. There is no call anywhere in Metal
that manufactures a GPU, because hardware is hardware. `MTLCreateSystemDefaultDevice`,
despite the "Create" in its name, does not create a GPU: it hands you a handle
to the one that already exists. Call it twice and both handles refer to the same
silicon.

This is precisely the seam GPU virtualization lives in. Each tenant's client
calls `MTLCreateSystemDefaultDevice` and believes it owns a private device. That
belief is the product. The interposer hands each client a proxy handle; behind
the wire every proxy funnels to the one physical GPU, time-sliced by a
scheduler. "More devices than GPUs" is not an error to avoid, it is the entire
point. Metal will never do that multiplexing for you; it just keeps returning
the same real device. The virtualization layer is the thing that makes two
GPU-less tenants each think they own the card. The distance between how many
handles clients hold (many) and how many GPUs exist (one) is the whole space the
system operates in.

## A mutex protects an agreement, not a map

When two gRPC handlers run at once, the server's maps and `counter_` are shared
memory. A mutex does not somehow attach protection to a `std::map`. It works
only because every path that reads or writes that shared state agrees to lock
the *same* mutex first. If a reader locks `reader_mutex` and a writer locks
`writer_mutex`, they can still access the same map at the same time. The two
locks protect different things, so the map is still racing.

The easiest correct first implementation is one `maps_mutex` for `counter_` and
all handle maps. It is deliberately coarse: while `CreateBufferShim` holds it,
`ReleaseComputePipelineStateShim` must wait even though those operations touch
different maps. That is unnecessary serialization, but not necessarily a
meaningful performance problem. The critical sections should only find, insert,
erase, and allocate IDs; those are short CPU operations. Do not hold that mutex
while waiting for the GPU or doing a blocking RPC, because then a tiny
bookkeeping lock becomes a full tenant-serialization lock.

Per-map mutexes can recover concurrency later. They also create a harder
problem: handlers such as `CommitCommandBuffer` need a queue, a pipeline, and
several buffers, so they may need several locks. Then the code needs a fixed
lock order to avoid deadlocks, and it needs an object-lifetime rule so another
thread cannot release a buffer just after commit finds its raw pointer. One
mutex is the right first correctness tool; finer locks are an optimization once
the scheduler is measured.

### A condition-variable notification is not stored

A shutdown flag and a condition variable must follow the same mutex agreement.
Making the flag atomic prevents a data race on the flag itself, but it does not
prevent a lost wakeup. This interleaving is possible if the destructor changes
the predicate without taking the mutex used by the waiting thread:

```text
scheduler:
  holds mtx_
  checks is_server_shutdown
  sees false

destructor:
  sets is_server_shutdown = true
  calls notify_one()
  nobody is sleeping yet, so notification disappears

scheduler:
  starts sleeping
  no future notification arrives
  sleeps forever
```

The scheduler checks the predicate again after it wakes, but it missed the only
notification and therefore never gets that chance. A condition variable is a
doorbell, not a mailbox: ringing it does not leave a token for a future waiter.

The fix is to change the predicate while holding the same mutex, then notify:

```cpp
{
    std::lock_guard<std::mutex> lock(mtx_);
    is_server_shutdown = true;
}
scheduler_cv_.notify_one();
```

If the scheduler owns `mtx_`, the destructor must wait. The scheduler then
atomically releases the mutex and begins waiting, after which the destructor
can set the flag and wake it. If the destructor gets the mutex first, the
scheduler later sees `true` and never sleeps. Those are the only two possible
orders, so the wakeup cannot fall into the gap between checking and sleeping.

### Reference counting: atomicity and memory ordering are different promises

A portable C++ proxy can copy Metal's `retain()`/`release()` ownership model by
keeping an atomic counter inside the object:

```cpp
class RefCounted {
  public:
    void retain() {
        reference_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void release() {
        if (reference_count_.fetch_sub(
                1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

  protected:
    virtual ~RefCounted() = default;

  private:
    std::atomic<uint32_t> reference_count_{1};
};
```

The object begins with one owner. `retain()` adds an owner. `release()` removes
one, and `fetch_sub()` returns the old value, so an old value of one means this
thread changed the count to zero and is the only thread allowed to delete the
object.

Declaring the counter atomic is not enough if the update is written as a
separate load and store. Two threads can both load one and both store two,
losing an owner. `fetch_add`, `fetch_sub`, and the atomic `++`/`--` operators are
single read-modify-write operations, so no increment or decrement is lost.

Atomicity answers whether the counter update can be torn or lost. Memory
ordering answers how the counter operation constrains reads and writes to other
memory. Atomic `++reference_count_` and `--reference_count_` use the strongest
default ordering, `memory_order_seq_cst`. All sequentially consistent atomic
operations participate in one global order observed by every thread. This is
simple and correct, but stronger than reference-count increments normally need.

`retain()` only needs to add another owner to an object for which the caller
already has a valid reference. It does not use that increment to publish the
object's other fields to a new thread, so `fetch_add(1,
memory_order_relaxed)` keeps the counter operation atomic without imposing
additional ordering on unrelated memory.

`release()` is different. An owner may modify the object before dropping its
reference, and the thread removing the final reference is about to run the
destructor. `memory_order_acq_rel` keeps earlier operations before the release
and lets the final owner acquire the effects published by prior owners before
destroying the object. The simpler `--reference_count_ == 0` version is also
correct; it uses stronger sequentially consistent ordering and is a reasonable
first implementation when clarity matters more than this small optimization.

This C++ counter cannot simply inherit Foundation's autorelease behavior.
metal-cpp's `NS::Referencing` assumes that `this` is a real Objective-C object
and forwards `retain`, `release`, and `autorelease` through `objc_msgSend`. A
plain C++ proxy allocated with `new` has no Objective-C `isa` pointer, and
sending it those messages can crash. A portable Linux client therefore needs
its own C++ reference counting and, if exact `commandBuffer()` naming semantics
are required, its own lightweight autorelease-pool mechanism.

### The 100-client experiment: the race happened

It is tempting to look at a ten-client run that happens to pass and conclude
that the mutex is unnecessary. To test that assumption, I commented out every
`std::lock_guard<std::mutex> lock(mtx_)` in the server and changed the
concurrent-adder script from ten clients to one hundred. The result was not a
subtle benchmark regression. Three clients failed, and the logs caught the
direct symptom:

```text
adder 17: Buffer ID: 903
adder 92: Buffer ID: 903

[SHIM] ERROR: 5: Could not find buffer.
Assertion failed: (b), function commit, file metal_impl.cpp, line 374.
```

Two independent clients received the same supposedly unique buffer ID. The
reason is that `counter_++` is not one indivisible action. Without a lock, two
gRPC handler threads can interleave like this:

```text
thread A reads counter_ = 902
thread B reads counter_ = 902
thread A writes counter_ = 903 and returns ID 903
thread B writes counter_ = 903 and returns ID 903
```

The same test also concurrently inserted into and searched `buffer_map_`.
`std::map` does not allow a writer and another writer or reader to access it at
the same time. That is a C++ data race and therefore undefined behavior: the
map can be corrupted, a lookup can fail unexpectedly, or the process can
crash. In this run, a later `CommitCommandBuffer` could not find a buffer. The
server returned gRPC `NOT_FOUND`; the client correctly checked that status and
aborted rather than continuing with an invalid response.

gRPC is designed to let a server handle more than one RPC concurrently. In this
synchronous server, gRPC can call methods on the one shared `ShimmerImpl`
service object from different worker threads at the same time. It makes RPC
dispatch concurrent; it does **not** make the service object's C++ members
thread-safe. `counter_`, `buffer_map_`, and every other handle map belong to
the service object, so the service code must provide their synchronization.

That is the exact job of the lock guard:

```cpp
std::lock_guard<std::mutex> lock(mtx_);
```

It locks `mtx_` when the handler enters and reliably unlocks it when the handler
returns, including an early error return. When every handler uses that same
guard before touching shared state, only one handler can mutate or inspect the
maps and counter at a time. The mutex does not make gRPC serial; it protects the
server-owned state that concurrent gRPC handlers share.

## A correct global lock can erase the reason to have a scheduler

The first ten-client test made this visible. The same ten 100-element vector
adds all completed correctly, but the timed client phase took 3.373 seconds
through the remote shim and 1.182 seconds when the same program used local
Metal directly: about 2.85 times slower remotely. The timer starts after Bazel
has built the selected binary, so compilation is not included in either number.

That 2.85x is not a measurement of mutex overhead alone. The remote path also
pays for gRPC, protobuf serialization, copying each buffer into a message,
crossing into the server process, and creating Metal setup objects separately
for every adder. The workload is so small that these fixed costs dominate the
kernel itself.

The global mutex nevertheless has one specific, damaging effect. It covers the
entire `WaitUntilCompleted` handler. A client that has committed a job then
locks the server while it waits for the GPU. Another client's later `commit`
cannot enter the server to make its already-recorded job available. The lock
turns a short bookkeeping rule into admission control, and can reduce the
system to whole-job serialization.

This matters because the local GPU already time-shares native Metal work:

```text
native apps:
  app A ─┐
  app B ─┼→ Metal driver decides scheduling
  app C ─┘

your remoter:
  remote client A ─┐
  remote client B ─┼→ your server decides admission/order → Metal driver → GPU
  remote client C ─┘
```

The remoter does not create the GPU's ability to share work. Its value is that
GPU-less or remote clients can use that one GPU, and that the server can choose
an explicit policy for their committed command buffers. Without concurrent
admission and a policy that keeps another client's work ready during CPU gaps,
the server is mostly a slower path to the same driver scheduler. The
transparency experiment still matters, but the utilization claim needs the
scheduler.

## Thunder-style sole tenancy versus command-buffer multiplexing

Thunder Compute's publicly described model is temporal oversubscription, not
simultaneous multi-tenancy on one card. While a process is using a GPU, that
process gets the complete card: all compute and all VRAM. When the process exits
or Thunder decides that it is idle, the GPU can be detached and assigned to a
different workload. Several persistent CPU instances can therefore share a
smaller fleet of GPUs over time without two tenants occupying the same card at
the same instant. Thunder reports this as a fleet-capacity result, not a claim
that one remote job runs faster than native CUDA. Its public write-up reports
about 1.8 times as many users served by its fleet while acknowledging that some
remote workloads can run as much as roughly twice as slowly as native execution.

```text
time ---------------------------------------------------------------------->

physical GPU:
  [ client A owns the whole card ][handoff][ client B owns the whole card ]
```

This is an exclusive GPU lease. The closest simple equivalent in this project
would assign the remoter's GPU to one `device_id` from `CreateDevice` until that
client calls `Device::release()`. Commits from other device IDs would wait. On
release, the server would finish outstanding work, destroy that client's Metal
objects, clear the owner, and wake the next client. A condition variable would
represent the waiting queue; a mutex alone is not a lease policy.

Thunder also says it can hand a GPU away when a process "sits idle," but the
public material does not define that idle threshold or explain how live VRAM
state is preserved. Transparent handoff from a still-running process is the
hard version. The system must either keep its resources resident, which denies
the next tenant the full VRAM guarantee, or save those resources to host memory,
release them, and recreate them when the first process returns. For this
prototype, an explicit process-lifetime lease would be the honest
Thunder-equivalent baseline.

The command-buffer design makes the opposite choice. Multiple clients keep
their server-side resources alive together. Every completed command buffer is a
point where another ready client's work can be admitted:

```text
finish A1 -> run B1 -> run A2 -> run C1
```

This is finer than a process lease, but it is not preemption. Metal does not let
the server pause command buffer A1 halfway through, execute B1, and resume A1.
A five-second command buffer can still block every client behind it for five
seconds. The scheduler controls which ready command buffer it submits next;
Metal's driver retains final control over how submitted work executes on the
hardware.

This limitation is intended and acceptable. The scheduler chooses the next
command buffer to submit; it does not and cannot pause one halfway through GPU
work. A long submitted command buffer therefore still delays work behind it.
The server controls admission order, while Metal's driver controls execution on
the hardware.

| Property | Thunder-style exclusive lease | Command-buffer multiplexing |
| --- | --- | --- |
| Scheduling unit | Process or idle lease | Completed command-buffer boundary |
| Tenants resident at once | One | Several |
| VRAM promise | Whole card for the owner | Aggregate working sets must fit |
| Switching | Coarse and potentially expensive | Fine and potentially fast |
| Performance | Predictable while the lease is held | Contention and interference |
| Cleanup | Reset one tenant at handoff | Track many live object graphs |
| Isolation | Stronger boundary and memory wipe | Must be enforced in software |
| Best workload | Large models that need most VRAM | Bursty jobs with smaller working sets |
| Fleet behavior | Assign whole GPUs across a cluster | Order ready jobs on one GPU |

The VRAM tradeoff is fundamental. If an 80 GB GPU serves two clients that each
need 60 GB, an exclusive lease can run them one after the other. A resident
command-buffer scheduler cannot hold both 60 GB working sets. The second
allocation must fail, or the server must implement eviction and restoration.
Once eviction exists, the design has reintroduced much of the hard state
management that sole-tenancy handoff requires.

The design choice in its clearest form:

**Thunder's design deliberately chooses:**

- full VRAM;
- predictable performance;
- simpler cleanup;
- stronger isolation;
- support for huge models;
- cluster-wide GPU assignment.

**This command-buffer design deliberately chooses:**

- faster switching;
- better opportunity for fine-grained gap filling;
- shared resident state;
- more contention;
- harder fairness and security.

Neither list dominates the other. Thunder optimizes fleet capacity while
preserving the experience of a dedicated card. This project explores whether
giving up that dedicated-card guarantee can reclaim smaller gaps inside long
process lifetimes.

### Concurrency is not yet security isolation

Sole tenancy is not the only possible security mechanism, but the current
prototype cannot claim tenant isolation. Its handles are global sequential
integers, every RPC reaches shared maps, there is no authenticated client
identity, and the server does not verify that a queue, pipeline, and buffers all
belong to the same tenant. Some message relationships are checked with
`assert`, packed byte lengths are not comprehensively validated before
`memcpy`, and all remote tenants are represented inside one trusted server
process. A client that guesses another client's ID can attempt to reference its
objects.

A credible software-enforced API isolation layer would need:

1. an authenticated session identity on every RPC;
2. a separate handle namespace per session, or unguessable capability handles;
3. ownership and cross-object consistency checks on every request;
4. complete bounds validation before all copies;
5. per-tenant memory, command-size, and execution-time quotas;
6. safe cleanup after normal exit, timeout, or broken connection;
7. zeroing before memory is reused by another tenant;
8. authenticated and encrypted transport outside a trusted local network.

Even after those changes, the honest phrase is "software-enforced API-level
isolation," not hardware isolation. Tenants still share caches, memory
bandwidth, execution units, the Metal driver, and the server process. Timing
side channels and denial-of-service remain possible. Apple exposes no public
MIG-like hardware partition in this design.

The encouraging part is that basic client isolation is practical here. Every
operation already passes through the remoting server, so the server is a
natural enforcement point. Giving every client an authenticated session ID,
associating every Metal object with its owning session, and rejecting RPCs that
refer to another session's objects would stop ordinary cross-client access
through the API. Bounds checks, quotas, disconnect cleanup, and clearing memory
before reuse complete a useful prototype-level isolation boundary.

That work should not be confused with production-grade security. A hostile
client can also try malicious shaders, excessive allocations, very long command
buffers, malformed messages, timing measurements, and driver bugs. Preventing
one client from guessing another client's buffer ID is comparatively
straightforward. Remaining secure and available under every hostile input is a
much larger project. The accurate claim is:

> Software-enforced resource and memory isolation is practical for this design.
> Basic ownership isolation is straightforward, but production-grade security
> and side-channel resistance require considerably more work.

### Shared resident state creates more concurrency than an exclusive lease

Thunder's exclusive-lease model can largely reason about one owner at a time:

> Client A owns the GPU. Only A's GPU state matters until the lease ends.

This command-buffer design keeps A's, B's, and C's resources alive together,
and their commands can be submitted in changing orders. That creates many more
possible interleavings for the server to handle:

- A can release a buffer while one of A's submitted command buffers still uses
  it;
- A can wait for completion while B commits more work;
- several clients can create and release objects at the same time;
- their combined allocations can exceed available VRAM;
- one client can submit a long command buffer and delay everyone else;
- a client can disconnect while its commands are queued or executing;
- each result must be delivered to the client that submitted that job.

This is substantially more concurrency complexity than sole tenancy, but it
does not require uncontrolled parallel code in every RPC. The server can keep
the design understandable by using short mutex-protected sections for maps and
counters, retaining objects while submitted commands use them, and routing
ready work through one scheduler thread. Each submitted job can have its own
completion state so that clients wait independently without holding the global
mutex.

```text
RPC threads -> validate and enqueue jobs -> one scheduler -> Metal -> per-job results
```

Metal and its driver still manage execution on the GPU. The remoter manages the
lifetime, ordering, memory pressure, and fairness of several clients whose
state remains resident at the same time. That additional complexity is the
price of faster switching and finer gap filling.

### The claim worth testing

The defensible claim is not that this scheduler is categorically faster or more
powerful than Thunder Compute. Thunder implements broad CUDA compatibility,
fleet orchestration, state cleanup, and production security that this prototype
does not. The specific claim is narrower and technically interesting:

> I built a remote Metal API shim with command-buffer-granularity concurrent
> admission, trading sole tenancy and full-VRAM guarantees for finer gap filling
> across bursty tenants.

The right experiment compares two server policies with the same remoting and
copy overhead: an exclusive process lease versus concurrent command-buffer
admission. Two tenants that each alternate between equal GPU work and CPU gaps
give a useful ceiling. Exclusive leasing leaves roughly half of each lease idle;
fine-grained admission can place B's ready command buffer into A's CPU gap and
approach twice the aggregate throughput. Native local Metal remains the
lower-overhead reference, not the intentionally weak baseline that the remoter
must beat.

Thunder references: [How Thunder Compute works (GPU-over-TCP)](https://www.thundercompute.com/blog/how-thunder-compute-works-gpu-over-tcp)
and [GPU Virtualization: Approaches and Tradeoffs](https://www.thundercompute.com/blog/why-network-based-gpu-virtualization-is-the-future).

## The contract: preserve Metal's meaning, not its timing

The bold goal of this project is semantic transparency for the part of Metal it
implements:

> **If a valid Metal program compiles against the supported shim and completes
> successfully, it produces the same observable result as local Metal. The shim
> guarantees semantics, not performance, scheduling, or resource availability.
> Unsupported APIs fail at compile time, and malicious clients are outside the
> threat model.**

Every qualification in that statement defines an important boundary.

**"Supported shim" means only the implemented Metal subset.** The project does
not yet implement the entire Metal API. If render encoding or another missing
feature is absent from the shim headers, code that uses it can fail to compile.
That is acceptable. The dangerous outcome is for unsupported code to compile
and then silently behave differently, for example because a required method is
implemented as a no-op.

**"Valid Metal program" is stronger than "C++ code that compiles."** A compiler
cannot prove that a program obeys object lifetimes, stays within buffer bounds,
synchronizes correctly, or avoids races inside a shader. Native Metal does not
assign useful semantics to every invalid program, so the remoter cannot promise
to reproduce them. The baseline is a program that works correctly through
native Metal on the corresponding server GPU.

**"Honest client" defines the threat model.** An honest client uses the C++ shim
normally. It does not forge protobuf messages, guess another client's object
IDs, deliberately send malformed lengths, or try to attack the server. Global
handles without ownership checks are acceptable under this limited research
assumption. Software ownership isolation can be added later without changing
the semantic-transparency goal, but defending against hostile tenants is a
separate security project.

**"Same observable result" describes what must be preserved.** A successful
remote execution should produce the same buffer contents, command ordering,
object relationships, lifetime behavior, and supported query results as the
native program. Equivalent supported failures should also be reported as
failures instead of allowing the client to use an invalid response.

The promise does not require equal pointer addresses or internal handle IDs.
Those values are implementation details. It also cannot promise bit-identical
output for a native program whose shader contains a data race or whose result
is otherwise nondeterministic. Device-specific queries describe the server GPU,
because that is the GPU doing the work.

**"Completes successfully" is necessary because resources are not guaranteed.**
Several resident clients can exhaust VRAM. The network can disconnect, the
server can stop, or a requested allocation can fail. The remoter therefore
cannot promise that every accepted source program finishes. It can promise that
once an operation is reported as successful, its observable result matches the
native Metal operation for the same input and device.

**Performance is explicitly outside the equivalence.** RPC latency, data copies,
queueing, and scheduling can make remote execution slower. A future scheduler
may also change which client's command runs first. Neither difference is a
semantic error as long as each client's synchronization and command-ordering
rules are preserved. The contract reproduces what the program computes, not how
long the computation takes.

This gives the project a strict implementation rule:

> If the shim lets the user express a valid Metal program, the shim must
> implement that program correctly. Missing functionality should be clearly
> unavailable, not accepted and silently ignored.

## This is Metal remoting, not the existing container-GPU paths

There are already good ways to accelerate llama.cpp from a Linux container on
an Apple Silicon Mac. They are useful comparisons, but they solve at a different
layer than this project.

| Path | Remoted interface | What runs in the guest/client | What the host ultimately runs | Difference from this project |
| --- | --- | --- | --- | --- |
| Podman/libkrun GPU path | Vulkan | A Vulkan application, including llama.cpp's Vulkan backend | Vulkan is forwarded through Venus/virglrenderer and translated by MoltenVK to Metal | General virtual-GPU plumbing, but the application must speak Vulkan; Metal is an implementation detail below the guest API. |
| GGML-VirtGPU / APIR | GGML backend operations: buffers, tensors, and graph execution | llama.cpp or another GGML application | The host can load `ggml-metal` and execute the graph on the Apple GPU | A narrow, efficient LLM/tensor remoting ABI, not an implementation of the Metal API. |
| llama.cpp RPC | GGML device and compute operations | llama.cpp on a different machine | A remote `ggml-rpc-server`, optionally backed by Metal | Network-distributed inference, not VM GPU virtualization or a general Metal shim. |
| This project | `metal-cpp` objects and command recording | A Metal compute program rebuilt against a compatible shim | Native Metal directly | The broadest API target here: it can serve non-GGML custom Metal workloads, but inherits Metal resource, command, synchronization, and coherence semantics. |

The architecture picture makes the distinction concrete:

```text
this project:       metal-cpp -> shim -> wire -> native Metal
Podman/libkrun:     Linux Vulkan -> VirtGPU/Venus -> host Vulkan -> MoltenVK -> Metal
GGML-VirtGPU/APIR:  GGML -> VirtGPU/APIR -> ggml-metal -> Metal
```

`virtio-gpu` (often shortened to VirtGPU in this discussion) is the virtual
device and shared-memory transport exposed to a Linux guest. API Remoting
(APIR) is the protocol layer that can carry a higher-level API across that
device boundary. Podman is the container tool; on macOS it uses a Linux VM, and
libkrun/krunkit provides that VM's virtual hardware. None of these is itself a
Metal API implementation.

This gives the post an honest positioning: the project is not claiming to be
the first route from a Linux workload to an Apple GPU. Its point is to explore
the *Metal* factory/record/execute boundary directly, instead of accepting the
semantics of Vulkan or narrowing the interface to GGML. That choice buys a
native-Metal server and generality; it also makes the implementation surface
much larger. For a llama.cpp-only product, GGML-VirtGPU is the more sensible
seam. For a generic Metal-remoting experiment, it is precisely the wrong seam.

The milestone that matters is not one remote vector add. It is **two
independent GPU-less Metal tenants, no source changes beyond rebuilding with the
shim, correct results, and measurable better total throughput/latency than naive
whole-job serialization.**

References: [Podman macOS GPU path](https://podman-desktop.io/docs/podman/gpu),
[GGML-VirtGPU backend](https://android.googlesource.com/platform/external/ggml-org/llama.cpp/%2B/refs/tags/studio-2026.1.1/docs/backend/VirtGPU.md),
and [llama.cpp RPC](https://github.com/ggml-org/llama.cpp/blob/master/tools/rpc/README.md).

## Why I cannot transparently intercept Metal the way Thunder intercepts CUDA

Thunder can run an already-compiled, unmodified CUDA binary against a remote
GPU. I cannot do the same trick for a metal-cpp binary. The reason is not that
one library is dynamic and the other is not. Both are dynamically loaded at
runtime (libcuda.so for CUDA, Metal.framework for Metal). The difference is the
shape of the API at that dynamic boundary.

CUDA's driver API is a flat table of C function symbols in a shared library.
Every operation (allocate, memcpy, launch) is a named C symbol resolved by the
dynamic linker at load time. The linker lets you shadow named symbols
(LD_PRELOAD on Linux, DYLD_INSERT_LIBRARIES on macOS). Substitute the whole
table and you have intercepted 100 percent of the API, cleanly, on an unmodified
binary. That is exactly what Thunder does: it supplies a replacement libcuda, and
an unmodified process loads it without knowing.

Metal is an Objective-C framework. Its API is not a table of C symbols. Almost
every operation is an Objective-C message dispatched through objc_msgSend by
selector, not a distinct linker symbol. Two consequences.

First, metal-cpp is header-only C++. The wrappers are inline and get compiled
into the client binary, where each one becomes an objc_msgSend call. In an
already-compiled binary there is no metal-cpp library sitting at a dynamic
boundary to substitute. It is baked into the client's own machine code.

Second, the only genuinely interposable C symbols Metal exports are entry points
like MTLCreateSystemDefaultDevice. Everything the returned device does (newBuffer,
newCommandQueue, commit) is ObjC message dispatch. Dynamic-linker interposition
shadows named C symbols, so it can catch the front door but is blind to the
individual operations, which are funneled through a single objc_msgSend rather
than exposed as separate symbols.

So to get Thunder-style transparency on an unmodified Metal binary, I would have
to intercept at the Objective-C runtime layer: method swizzling or NSProxy-style
forwarding on the Metal protocol objects. That is possible but deep and fragile,
a fundamentally different mechanism from symbol substitution.

The decision: go the header route instead. Ship shim headers that declare the
same MTL:: API but implement it as proxies that forward over the wire. The
client source is unchanged; it just recompiles against my headers instead of
Apple's metal-cpp. This sidesteps all runtime-interception machinery. The cost
is a recompile, which is free for me because I write every client.

The general principle worth remembering: transparent binary interposition is
only as clean as the API boundary is a flat C symbol table. A C-ABI shared
library is interposable by design. An Objective-C framework, or any header-only
C++ that inlines into the caller, is not.

TODO (diagram): draw the two layer stacks side by side and mark the interception
layer for each.
- CUDA: [client binary] -> dynamic link -> [libcuda.so: flat C symbol table] ->
  driver -> GPU. Thunder intercepts at the libcuda symbol boundary (LD_PRELOAD).
- Metal: [client binary with metal-cpp inlined to objc_msgSend] -> [libobjc
  objc_msgSend funnel] -> [Metal.framework ObjC classes] -> driver -> GPU. The
  only interposable C symbol is MTLCreateSystemDefaultDevice. Transparent
  interception would have to happen at the objc runtime (swizzling); I instead
  intercept at compile time with a header shim, above the whole stack.

## The header shim is not cheating

The obvious objection to the header route: the client's compiled binary is
different from one built against Apple's real metal-cpp, so I am not really
intercepting "their" program. Half true, and worth stating precisely.

What is actually given up: transparency to prebuilt artifacts. Hand me a Metal
binary someone already compiled and I cannot run it. Thunder can run the CUDA
equivalent because it swaps a loaded library under a byte-identical binary. I
swap the binary itself via recompile. That is a real limitation and I should
name it rather than paper over it.

Why it is still not cheating. The substitution mechanism (compile-time link vs
load-time preload) is a small part of the system and not the interesting part.
The transport, the copy-at-commit coherence shim, the scheduler, the multi-tenant
utilization work are identical either way. The shim genuinely proxies every call
over the wire, genuinely snapshots buffers at commit, genuinely multiplexes two
tenants onto one GPU. What I skipped is Objective-C runtime surgery, which is
orthogonal to GPU virtualization. I dodged a dispatch problem, not a
virtualization problem.

The deeper point: both approaches are the same illusion. Thunder's client also
believes it is calling a local libcuda and it is not. The program is not running
against what it thinks in either case, because that illusion is the product:
make a process believe it has a local GPU when it does not. The only thing that
differs is the granularity of the swap. Thunder swaps a dynamically loaded .so;
I swap a recompiled binary. Same lie, different seam.

And recompiling against a compatible SDK is a normal, legitimate integration
model, not a workaround: Vulkan layers and ICD loaders, ANGLE standing in for
OpenGL, MPI implementations you relink against, allocator replacements like
jemalloc. "Build against my drop-in that implements the same interface" is a
standard seam.

Where the critique becomes valid: the moment the write-up claims the wrong
thing. "Transparent Metal GPU virtualization" would oversell, because it is not
transparent to prebuilt binaries. The honest and more interesting claim is
"Metal GPU remoting via a link-time shim; full binary transparency would require
ObjC-runtime interception, which I scoped out, and here is the CUDA-vs-Metal ABI
reason why." That framing is more rigorous and it foregrounds the actual insight
(C symbol table vs objc_msgSend). So: a deliberate scope cut on an axis (artifact
transparency) that is a distraction from the real thesis (multiplexing one GPU
across tenants to raise utilization). Just do not let the write-up quietly claim
the axis I gave up.

## linker error

A header file never creates a object file. So we need to create a separate shim binary (not just a header).

```c++
// --- INVISIBLE STEP 1: Compiler processes metal_shim.h first ---
#include <Metal/Metal.hpp> // 1. Compiler reads Metal.hpp. NO PASSWORD YET. 
                           // It generates blueprints and marks Metal.hpp as "already read".
#define MTL MetalShim

// END OF INJECTED CODE

// --- STEP 2: Compiler finally starts reading main.cpp ---
#define MTL_PRIVATE_IMPLEMENTATION // 2. You provide the password here but its useless.
#include <Metal/Metal.hpp>         // 3. Compiler says: "I already read Metal.hpp in Step 1! Skipping!"
```

so these are the following constraints:
- Original cpp code written by user cannot be modified.
- I have to include MTL_PRIVATE_IMPLEMENTATION somewhere or else the linker will fail.
- I have to include the MTL_PRIVATE_IMPLEMENTATION before the actual #include <Metal/Metal.hpp> or else its useless.
- There should be exactly one .o file generated that has the apple metal objects.
- You cannot include the same header twice because of header guards/compilation errors, etc,.

So the only natural solution is to create another translation unit that inherits all the metal implementation and generates the .o file. All subsequent files that depend on metal objects will refer to this file via the linker.


This is called "STB-Style" Header Pattern developed by a guy who developed library for graphics and now its used everywhere.

## The repo layout is the architecture diagram

After moving the server out of metal/, the directory tree reads as the system
design. Each top-level folder is one box in the picture, and the dependency
rules between folders are the arrows:

```
src/          client program (unmodified Metal code, GPU-less)
metal/        the shim: hijack header + gRPC proxy impl
server/       standalone GPU-owning server
proto/        the only thing shim and server share
third_party/  metal-cpp headers + opt-in foundation impl
```

The picture itself:

```
 +--------+     hijacked      +-------------+
 | Client | ----------------> | Shim header |
 +--------+                   +-------------+
       \_________  __________/       |
                 \/                   |  separated
      compiled into ONE binary        |  by network
                                      v
                              ~~~~~~~~~~~~~~~~~
                                gRPC (proto/)
                              ~~~~~~~~~~~~~~~~~
                                      |
                                      v
                                 +--------+
                                 | Server |  <- real GPU
                                 +--------+
```

Two boundaries, two different kinds of "separate":

- Client and shim are separate *folders* but the same *binary*. The hijack is a
  compile-time trick (force-included header, `#define MTL MetalShim`), so the
  client's unmodified source and my proxy classes get welded together by the
  linker into one executable. src/ never includes metal/ by name; the build
  system does the splice.
- Shim and server are separate *processes* on opposite ends of a wire. They
  share zero code, zero headers, zero linked symbols. The only artifact both
  sides consume is proto/, the wire contract. Nothing else in the tree is
  visible across that line.

The folder split earns its keep because you can see the deployment story in
`ls`: everything above the wavy line ships to the GPU-less tenant, everything
below it stays on the one Mac that owns the GPU.

## nothing apple survives in the client

Once the shim is finished, the client binary contains: my proxy classes (plain
C++), a recorded command script, shadow buffers (plain malloc memory), and a
gRPC stub. The MSL shader source travels as a string and gets compiled
server-side on the Mac. Nothing Apple-specific survives on the client: no
Metal.framework, no ObjC runtime, no Apple SDK.

Right now that is false. `otool -L bazel-bin/metal/adder` on the current shim
shows Metal.framework, Foundation.framework, and libobjc.A.dylib all linked,
because the current shim delegates in-process: `_realDevice->newCommandQueue()`
is a real metal-cpp call that emits objc_msgSend into Metal.framework. The
"nothing apple survives" claim is the end state, after the delegation is replaced
by serialization. metal-cpp is header-only, so it only drags in Metal.framework
when you actually call one of its methods. Kill the calls and the linker has
nothing to resolve against Apple; those three lines fall out of the link map on
their own.

The reason every piece ends up as plain C++ is one physical fact: there is no
Apple GPU in the client's laptop. Not "no library", a hardware absence. And it is
the premise of the whole system, not a constraint I am working around. If the
client had a usable Apple GPU there would be no reason to remote anything. Every
plain-C++ property is that single fact propagating outward:

- the shadow buffer is malloc memory because there is no VRAM or unified memory
  to allocate,
- the command list is a recording because there is no command queue to submit to,
- the MSL is a string because there is no GPU to run it and no Metal compiler
  present to compile it,
- the proxy holds a handle instead of a device pointer because there is no real
  device to point at.

So MTL:: cannot exist in the client for the most basic reason possible. Device,
Buffer, CommandQueue, Encoder, PipelineState, Library are all handles onto GPU
state, and there is no GPU to hold that state. Not a linking problem, a hardware
problem.

The subtle part is the boundary where that argument stops. NS:: is not a GPU
thing. NS::String and NS::Error come from Foundation, and Foundation does not
need a GPU; a GPU-less machine runs it fine. So "no GPU" is airtight for MTL:: and
says nothing about NS::. NS:: gets shimmed for a different reason: hauling a whole
Foundation/ObjC stack onto the client just to carry a string and an error code is
pure dependency cost with no capability gained, and a ten-line std::string-backed
NS::String does everything the client needs, because the client only ever carries
these values to serialize them. The ObjC runtime and Foundation are not even
Apple-exclusive (GNUstep and swift-corelibs-foundation exist on Linux), but
metal-cpp's NS wrappers are bound to Apple's objc_msgSend ABI, so they would not
bind to a Linux Foundation anyway. Either way the shim is strictly less work than
porting a Foundation.

So the clean split, two different kinds of "must not be here":

- MTL:: is shimmed because there is no GPU. Physical, absolute, non-negotiable.
- NS:: is shimmed because it is a pointless dependency for holding a string.
  Practical, a judgment call that happens to be obvious.

The payoff is that the finished client links only gRPC, protobuf, libc++, and my
own code, which means it can be built and run on Linux, on a box that has never
heard of Apple. The GPU-less tenant is not a crippled Mac, it is a machine with
no Apple anything. All the Mac-ness is confined to the one server process. Run
`otool -L` on the finished client and the three Apple lines are gone, having
moved to the server binary. That relocation, not elimination, is the actual
trick: confine every scrap of Apple to one process on one Mac, and hand everyone
else a plain-C++ view of it over the wire.

## The wire follows Metal's execution boundary

The easy mistake in a remote API is to make every method call an RPC. That is
not what Metal is doing. A command encoder is a local notebook: calls such as
`setBuffer`, `setComputePipelineState`, and `dispatchThreads` only describe
future work. They do not touch the GPU. The shim should keep the same shape,
recording those calls locally as a serializable command list and sending that
list once at `commit()`. The committed command buffer is then both the network
submission unit and the scheduler's unit of work.

The opposite category is create and query. `newLibrary`, `newFunction`, and
`newComputePipelineState` need real server-side objects now, so they are
synchronous RPCs returning opaque IDs. A query follows the same rule only when
the answer is not already known locally. Immutable properties are better packed
into their creation response: device creation returns both `device_id` and the
device name; pipeline creation returns both its ID and its thread limit. The
client proxy stores those values and its query methods become local reads.

Pointers do not cross the wire. A proxy-object pointer becomes a server handle
ID. An `NS::String*` becomes its UTF-8 bytes. A buffer pointer becomes a byte
snapshot at `commit()`, because the application can write through `contents()`
without making another interceptable API call. An `NS::Error**` becomes a status
and error payload that the shim turns back into a local error object if needed.
The general question is not "how do I send this pointer?" but "what does this
pointer mean?"

## Keeping Foundation is a valid intermediate step

`NS::String` is not a C++ string container. metal-cpp's `NS::String*` is a
typed pointer to a Foundation-owned Objective-C `NSString` object. The wrapper
headers know how to issue Objective-C messages, while the actual `NSString`
implementation lives in Apple's precompiled `Foundation.framework`.

That makes a useful staging choice. A macOS client can receive the device-name
bytes over gRPC, construct a new local `NSString`, retain it in the proxy, and
return that cached `NS::String*` from `Device::name()`. It preserves the Metal
source interface without adding a network round trip. It does not make the
client portable: Foundation and the Objective-C runtime are Apple platform
dependencies. Replacing `NS::String` with a small `std::string`-backed shim is
a later portability step, not a prerequisite for proving Metal remoting.
