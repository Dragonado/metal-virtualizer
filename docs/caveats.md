# Caveats

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

## One physical device should have one server owner

Calling `MTL::CreateSystemDefaultDevice()` repeatedly still reaches the same
physical GPU, but the server should create and retain one real device for its
lifetime. Client-facing device IDs can all refer to that shared server object.
This makes the single scheduling point explicit.

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
