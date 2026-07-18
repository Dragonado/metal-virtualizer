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
