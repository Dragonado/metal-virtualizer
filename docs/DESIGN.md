# Metal API Remoter design

## Objective

Run a supported `metal-cpp` compute program against a Metal GPU owned by another
process or machine, then place command buffers from multiple clients behind one
server-side admission point.

The prototype operates entirely in user space. It does not modify macOS, the
Metal driver, or the GPU kernel interface.

## Interception boundary

CUDA remoting systems can replace a shared library that exports a flat C ABI.
Most Metal operations are Objective-C messages sent through `objc_msgSend`, and
`metal-cpp` compiles its inline wrappers into the application. There is no
per-method dynamic symbol table that can be replaced cleanly.

This project chooses compile-time header substitution. Bazel force-includes
`metal/metal_hijack.h`, which loads the declarations needed from `metal-cpp`
before mapping later `MTL::` tokens to `MetalShim`:

```cpp
#include "metal_shim.h"
#define MTL MetalShim
```

The client source still writes `MTL::Device`, `MTL::Buffer`, and related calls,
but the compiler emits calls to plain C++ proxy objects. This requires source
and recompilation. Supporting precompiled Metal binaries would instead require
Objective-C runtime proxies or method interception.

The build deliberately omits Metal's implementation symbols from shim clients.
If a client uses a method outside the replacement surface, the build fails at
link time instead of silently executing that operation locally.

## Create, record, execute

The supported calls fall into three groups:

1. Create calls synchronously allocate a real server object and return an
   opaque numeric handle.
2. Recording calls modify only client proxy state. They do not perform RPCs.
3. `commit()` serializes the recorded state and buffer snapshots into one RPC.

```text
create object -> synchronous RPC -> remote handle
record command -> local proxy state only
commit         -> command metadata + input snapshots
wait           -> completion status + output snapshots
```

This batching avoids one network round trip for every encoder method.

## Shadow buffers and copy at commit

The difficult Metal-specific boundary is `MTL::Buffer::contents()`. Native
shared buffers return a CPU pointer into unified memory. A write through that
pointer generates no function call, so a remote system cannot observe the
write when it happens.

The client proxy allocates a shadow buffer. At commit, it serializes the full
contents of every bound shadow buffer. The server copies each snapshot into a
real Metal buffer before encoding and submitting the command. On completion,
the server serializes the bound buffers and the client copies them back into the
same shadows.

The coherence contract is therefore:

```text
CPU writes before commit  -> visible to the submitted GPU job
GPU writes before finish  -> visible after wait completes
CPU reads before finish   -> may observe the old shadow contents
```

The baseline copies every bound buffer in both directions. Direction analysis,
dirty-page tracking, private storage, and chunked transfers are later work.

## Server state

The server owns maps from remote handles to real Metal objects:

```text
device ID  -> MTL::Device
queue ID   -> MTL::CommandQueue
buffer ID  -> MTL::Buffer
library ID -> MTL::Library
function ID -> MTL::Function
pipeline ID -> MTL::ComputePipelineState
```

gRPC may run handlers concurrently. One mutex currently protects all handle
maps, job state, and ID allocation. Long-running GPU waits do not hold that
mutex, and completed output bytes are copied after releasing it.

Handles are global for the controlled prototype. There is no tenant ownership
check or malicious-client isolation yet.

## Command ordering

Each committed proxy command buffer becomes a server `Job`. A dedicated thread
pops ready jobs in FIFO order and commits real Metal command buffers.

The scheduler must preserve this invariant:

```text
If c1 was committed before c2 to the same command queue,
the server must submit c1 before c2.
```

The server does not need to wait for c1 to complete before submitting c2.
Native Metal queue ordering preserves GPU-side dependencies between them.
Different queues have no implicit ordering without explicit synchronization.

The current implementation is FIFO infrastructure, not the final scheduling
result. It submits ready work quickly and leaves hardware execution decisions
to Metal. A demonstrated gap-filling scheduler still needs a workload with
measurable CPU-side gaps, explicit admission policy, and local-versus-remote
utilization measurements.

## Object lifetime

Server maps own real objects until a release RPC removes the public handle.
Real command buffers are retained across autorelease-pool drainage and released
from their completion callback.

A remaining semantic gap is queued-job resource ownership. Native Metal allows
an application to release its queue, pipeline, or buffers after commit because
the command buffer retains what it needs. The final remote design must resolve
and retain all required real objects when the job is enqueued. The current
implementation detects a missing handle and fails the job rather than crashing,
but it does not yet preserve native execution in that race.

Client proxies are plain C++ objects, not Objective-C objects. Foundation's
autorelease pools cannot manage them. The current command-buffer proxy uses a
documented one-commit/one-wait lifetime; native-compatible repeated waits and
release-before-wait require client-side reference counting.

## Scope

The current implementation is intentionally compute-only and one-dimensional.
The exact supported API surface and observable limitations are listed in the
root README. The roadmap in `TODO.md` separates current MVP work from later
storage modes, session isolation, scheduler policy, and measurement.
