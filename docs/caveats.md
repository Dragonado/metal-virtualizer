# Caveats

These are known boundaries of the current implementation, not failures of the
controlled demo. The demo uses supported calls, keeps its proxy resources alive
through completion, waits exactly once, and trusts every client.

## Cached Foundation strings

`NS::String::string(...)` returns an autoreleased object. If `Device` stores
that pointer in a member, its constructor should retain it; otherwise it may
disappear when an autorelease pool drains.

## Remote IDs are untrusted input

The server must validate every object ID received over RPC. Using
`object_map[id]` for a missing ID inserts a null entry, and dereferencing it
crashes the server. Use a lookup that can return `NOT_FOUND` instead.

## Creation failures need local error objects

`newLibrary` and `newComputePipelineState` report failure through an
`NS::Error**`. A network failure or server-side Metal error must become a
client-local error object when that output pointer is supplied. Returning
`nullptr` while leaving the caller's `NS::Error*` uninitialized makes normal
error handling unsafe.

## Multiple device objects still target one physical GPU

Calling `MTL::CreateSystemDefaultDevice()` repeatedly still reaches the same
physical GPU. The current server gives each client-facing device ID its own
real `MTL::Device` object. That is sufficient for the demo: the FIFO scheduler,
not object identity, is the central submission point.

The server could cache one real device object later, but then a client device
release must remove only that client's handle and must not destroy an object
still used by another client. The cache is an ownership optimization, not a
requirement for sharing the GPU.

## Concurrent mid-flight CPU and GPU access is outside the contract

The client sees a shadow allocation while the server GPU uses a different real
buffer. CPU writes made after `commit()` are not sent to that already-submitted
job, and GPU writes are not visible in the shadow until
`waitUntilCompleted()` returns. Programs that concurrently mutate or inspect a
buffer while GPU work is in flight can therefore differ from native unified
memory behavior. The controlled demo writes before commit and reads after wait.

## gRPC handlers may run concurrently

Once multiple clients are active, shared handle maps and the next-ID counter
need synchronization. Without it, concurrent create or release RPCs can race,
corrupt bookkeeping, or reuse IDs incorrectly.

Use one shared mutex for every access to the maps and `counter_` as the first
correct implementation. Separate mutexes for readers and writers do not help:
they must lock the same mutex to exclude one another. A single mutex also
serializes independent operations such as creating a buffer and releasing a
pipeline state, but that cost is small when the lock covers only map operations.
Never hold it across `waitUntilCompleted()` or another long-running GPU action,
or it accidentally serializes tenants. Finer-grained locks later require a
fixed lock order and an explicit rule for object lifetime after map lookup.

## A queued command buffer must retain its resources

This is valid Metal code when using the normal `commandBuffer()` path:

```cpp
command_buffer->commit();
buffer->release();
```

`release()` drops the application's reference; it does not necessarily destroy
the real `MTL::Buffer` immediately. A normal native Metal command buffer keeps
strong references to the resources it needs until GPU execution finishes.

A scheduler changes the timing. `CommitCommandBuffer` can return after placing
a job in a pending queue, before the server has created and encoded the real
`MTL::CommandBuffer`. If a client then releases a buffer, queue, or pipeline,
the scheduler cannot later look it up through its public ID and use it safely.

At enqueue time, the `PendingJob` must retain every real object it will need:
the command queue, compute pipeline state, and bound buffers. A release RPC may
remove the public handle from its map, but the real object must remain alive
until the queued or running job completes. The scheduler then releases the
job-held references after completion. This preserves normal local Metal
lifetime semantics rather than requiring clients to wait before calling
`release()`.

The current `WaitUntilCompleted()` implementation copies every bound server
buffer into the response by looking up its public ID in `buffer_map_`. A
scheduler must instead copy results from the `PendingJob`'s retained buffers
into job-owned result bytes when the job completes. `WaitUntilCompleted()` then
returns those stored bytes without consulting the public maps.

Copyback is only useful for a client shadow buffer that still exists. If a
client releases its last `Buffer` handle after `commit()`, local Metal may still
finish the GPU work, but the application no longer owns a valid buffer through
which to observe the result. The remoter may discard that buffer's returned
bytes. The current client command buffer stores raw `Buffer*` values and
dereferences them during `waitUntilCompleted()`, so it needs explicit lifetime
handling before this release-before-wait case is supported.

## The scheduler must preserve one client's command order

Native Metal preserves the order of command buffers committed to the same
`MTL::CommandQueue`. This makes a GPU-only dependency valid without a CPU wait:

```text
A commits: writes buffer X
B commits: reads buffer X

same queue => B observes A's write
```

The scheduler may choose how to interleave independent clients, but it must not
submit a later job from one client before that client's earlier job. If client A
commits `A1` and then `A2`, `A1` must be submitted before `A2`. Reversing them
can change a valid Metal program's result.

Different Metal queues have no such implicit dependency. Cross-queue work needs
explicit synchronization, such as a shared event. For the first scheduler, one
server submission queue and one scheduler thread are the simplest way to make
the order deterministic.
