# Blog Notes

Running notes and insights worth writing up later.

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
