# Metal API Remoter

Metal API Remoter is a user-space GPU remoting prototype for Apple Silicon. A
client program records a supported subset of `metal-cpp` calls locally, sends a
committed command buffer and its buffer snapshots over gRPC, and executes the
work on a macOS server that owns the real Metal GPU.

The project explores two systems problems:

- reconstructing explicit network transfers for Metal's implicit shared-memory
  `Buffer::contents()` interface; and
- admitting command buffers from multiple clients through one server-side
  submission path while preserving command-queue order.

This is an active research prototype. The current scheduler performs
thread-safe FIFO submission. Metadata-aware gap filling and a measured
utilization improvement are goals, not completed benchmark claims.

## Architecture

```text
client metal-cpp source
        |
        | compile-time header substitution
        v
MetalShim proxy objects
        |
        | gRPC: handles, encoded commands, buffer bytes
        v
server handle registry -> FIFO submission thread -> Metal.framework -> GPU
        ^
        | completed output bytes
        +------------------------------------------------------------
```

The client and server share only the protobuf contract in `proto/`. The client
does not link Metal's implementation. A supported call must go through the
shim; a call that escapes the supported surface fails at link time instead of
silently executing on a local GPU.

See [DESIGN.md](docs/DESIGN.md) for the interception mechanism, copy-at-commit
coherence model, and scheduling invariant.

## How the shim works

`metal/metal_hijack.h` is force-included when the shim build flag is enabled.
It first includes the real `metal-cpp` declarations needed for common value
types, then defines:

```cpp
#define MTL MetalShim
```

Every later `MTL::` token in the client translation unit therefore names a
plain C++ proxy class. This is compile-time API substitution, not dyld
interposition, and it requires recompiling the client. Include order is
intentional: defining the macro before parsing Apple's headers would rewrite
their own namespace declarations.

## Copy at commit

Native shared Metal buffers expose a CPU pointer. Writes through that pointer
produce no API call that a network shim can intercept. Each remote `Buffer`
therefore owns a client-side shadow allocation:

1. `contents()` returns the shadow pointer.
2. `commit()` snapshots every bound buffer and sends the bytes with the encoded
   command buffer.
3. The server copies those bytes into real Metal buffers and submits the work.
4. `waitUntilCompleted()` copies completed server buffers back into the client
   shadows.

This deliberately converts implicit unified-memory access into explicit
network synchronization at commit and completion boundaries.

## Prerequisites

- An Apple Silicon Mac for the GPU server
- macOS and Xcode Command Line Tools
- Bazel 9.1.1, preferably through Bazelisk
- Network access for the first build

The repository pins Bazel in `.bazelversion`. The first build compiles gRPC and
protobuf from source and can take several minutes. Subsequent builds are much
faster.

The current client shim still uses Apple's Foundation runtime for types such as
`NS::String`, so a GPU-less macOS process or VM is the supported client
environment. Linux client support is not implemented yet.

## Build and run

Start the GPU server:

```sh
bazel run //server:server
```

In another terminal, run the vector-add client through the remote shim:

```sh
bazel run //src:adder
```

The client defaults to `localhost:50051`. To use another host or port:

```sh
METAL_API_REMOTER_SERVER=192.168.1.20:50051 bazel run //src:adder
```

Run the identical source directly against the local Metal GPU:

```sh
bazel run --//src:shim=false //src:adder
```

Run the existing 100-process concurrency smoke test:

```sh
scripts/test_concurrent_adders.sh
scripts/test_concurrent_adders.sh --local
```

Regenerate `compile_commands.json` after changing source files or dependencies:

```sh
bazel run //:refresh_compile_commands
```

## Current supported surface

- default device creation and `device->name()`
- command queues and one compute encoder per command buffer
- runtime Metal library and function creation
- compute pipeline creation and cached thread-limit queries
- shared buffers with `options == 0`
- one-dimensional `dispatchThreads`
- one commit and one blocking wait per command buffer
- explicit release RPCs for devices, queues, libraries, functions, pipelines,
  and buffers
- concurrent clients with thread-safe global handle tables and FIFO submission

## Known limitations

- This is source-compatible substitution for the supported subset, not binary
  interposition for arbitrary precompiled Metal applications.
- The client command-buffer and encoder proxies are deleted by their single
  `waitUntilCompleted()` call. Repeated waits and post-wait status queries are
  not implemented.
- Client buffer proxies must remain alive until wait completes. Native-style
  release immediately after commit requires proxy reference counting.
- Jobs currently re-resolve public server handles when the scheduler submits
  them. A release race fails the job safely, but native-compatible queued-job
  ownership still requires retaining resources at enqueue time.
- There is no session isolation. Handles are global and the prototype makes no
  security claim for malicious clients.
- The scheduler currently drains ready jobs in FIFO order. It does not yet use
  workload metadata, fairness, priorities, or an in-flight admission limit.
- Only compute workloads using the narrow surface above are supported. Render,
  blit, events, heaps, argument buffers, private storage, and cross-queue
  synchronization are outside the MVP.
- Buffer payloads use protobuf `bytes`; large workloads will need message-limit
  changes or chunked streaming.

## Repository layout

- `metal/`: client-side proxy classes and gRPC client
- `proto/`: shared wire contract
- `server/`: real Metal execution and FIFO submission
- `src/`: vector-add demonstration used for local and remote runs
- `scripts/`: concurrency smoke test
- `docs/`: design, roadmap, caveats, and engineering notes
- `third_party/metal-cpp/`: vendored Apple `metal-cpp` headers

## Documentation

- [Design](docs/DESIGN.md)
- [Roadmap](docs/TODO.md)
- [Known caveats](docs/caveats.md)
- [Engineering blog notes](docs/blog-notes.md)
- [Historical steering notes](docs/initial-steering-notes.md)

## License

Project code is available under the [MIT License](LICENSE). The vendored
`metal-cpp` headers retain Apple's license in `third_party/metal-cpp/LICENSE.txt`.
