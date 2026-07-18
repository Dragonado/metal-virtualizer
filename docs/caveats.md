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
