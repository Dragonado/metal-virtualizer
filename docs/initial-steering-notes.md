# Initial steering notes: Metal GPU remoting on Apple Silicon

> Process archive: this document captured the initial mentoring instructions
> and research direction. It is not the project design specification. Start
> with [DESIGN.md](DESIGN.md) for the current human-facing architecture.

**Project codename suggestion:** `metalstorm` (placeholder, rename freely)
**Audience of this doc:** an expert Claude agent who will mentor the builder.
**The builder:** a strong systems engineer (ex-Google, GT MS CS) who wants to *learn by building*, not be handed a finished implementation.

---

## 0. Instructions to the implementing agent (read first)

You are acting as a **mentor, not an implementer**. The builder learns by writing the logic themselves. Honor this contract strictly:

- **Never hand-write the entire logic in code.** Do not produce full working modules, full classes, or end-to-end implementations.
- **Small snippets are fine and encouraged** when they unblock understanding: a function signature, a 3–5 line illustration of an interposition pattern, a struct layout for a wire message, a single tricky API call. Illustrate the *shape*, then stop and let the builder fill the body.
- **Lead with hints, questions, and structure.** When the builder is stuck, give the next concept or the next seam to attack, not the answer. Prefer "here's the seam and why; you write the body" over writing the body.
- **Push for rigor.** This builder catches hand-waving and wants the theoretical underpinnings, not just "what works." If you state a performance or correctness claim, justify it or mark it as an assumption to verify.
- **Be honest about scope and difficulty.** Do not oversell. If something is genuinely hard or a dead end, say so plainly.
- **Always read the current code before commenting on it.** The builder edits files between conversations and mid-conversation. Never review, critique, or reason from a remembered or summarized version of a file; re-read it first. Stale reviews have wasted sessions before.
- **Writing register:** plain, technical, grad-student voice. Avoid em dashes and AI-boilerplate phrasing in any prose deliverables.

### Editor tooling: compile_commands.json goes stale

clangd IntelliSense is driven by a generated `compile_commands.json` snapshot,
not a live view of the build. Whenever a `.cpp`/`.h` file is added, moved,
renamed, or gains new deps, squiggles in the editor are the database being
stale, not (usually) a real error. The fix is always:

1. `bazel run //:refresh_compile_commands` (target list lives in the root
   `BUILD.bazel`; add new binaries there so their TUs get entries)
2. Restart clangd: Cmd+Shift+P, "clangd: Restart language server"

Diagnose before debugging: if the editor shows include errors for code that
`bazel build` compiles fine, it is this, every time. This loop has recurred on
every restructure (src/metal split, network shim, server/ move).

The point of the project is the builder's understanding of GPU virtualization, API interposition, deferred-execution scheduling, and the unified-memory consequences of remoting. Optimize every interaction for that.

---

## 1. What Thunder Compute (tnr) is

Thunder Compute (CLI: `tnr`) is a YC-backed (S24) GPU cloud whose core product is **network-attached GPU virtualization**: a CPU-only machine is made to behave as if a GPU is physically attached, while the real GPU sits elsewhere and is shared across users. Their pitch is that GPUs sit idle most of their lifecycle, and that pooling + sharing reclaims that idle time for large cost savings.

Two claims worth holding as *company claims* (not independently benchmarked facts), to be cited as such:
- Roughly 4–5x better utilization / cost by sharing instead of dedicating cards.
- A 20–50% performance overhead versus directly attached hardware for many workloads (early tests were far worse and improved fast).

## 2. What layer tnr attacks, and how their system works

**The layer: user-space API-library interposition.** tnr replaces the standard GPU client library (CUDA, i.e. `libcuda`/`libcudart`) with a network-aware drop-in. The application still calls `cudaMalloc`, still sets `device="cuda"`, still runs unmodified. Underneath, those calls are translated into network messages and shipped over TCP to a host that owns the physical GPU. This works cleanly for CUDA because CUDA exposes a **stable, documented C ABI in a user-space library**, which is trivial to interpose (e.g. `LD_PRELOAD`). The academic ancestor of this idea is **rCUDA** (remote CUDA); read it for the canonical mental model.

**How the runtime behaves (from their own engineering writeup):**
- GPU-over-TCP: the VM talks to the GPU over plain TCP. Calls become network messages.
- **Sole-tenancy with session-level handoff:** when your process runs, it owns the whole card (full VRAM/compute). When the process finishes or idles out, the orchestrator hands the card to someone else. This is the key nuance: their *documented* sharing granularity is the **session**, not sub-job interleaving.
- Latency hiding: initial connection is ~10–20 ms; they optimize so network latency does not dominate, because most ML jobs are compute-heavy relative to data movement.

**Around the mechanism (the "production substrate"):** pooling cards across nodes, hardware-level tenant isolation, on-the-wire encryption, billing per second, hot-swapping GPU tiers. None of this is interception; it is the system wrapped around interception.

## 3. What the builder is building

The same architecture, on a **different API and platform**: **Metal on Apple Silicon, via `metal-cpp`**, with a **finer scheduler** than tnr's documented model.

Restated precisely (this is the locked scope):
- **Same layer as tnr.** User-space API-library interposition. Interpose Metal.framework's bootstrap symbol(s); a stock `metal-cpp` program compiled against real Metal runs **unmodified on a GPU-less client**. This genuinely *is* the transparency property, not a lesser cousin of it.
- **Same protocol shape.** The GPU API decomposes into **create / record / execute**, so remoting is interception at those seams.
- **A sharper scheduler.** Schedule at **committed-command-buffer granularity** and do **gap-filling** between concurrent tenants, which is finer than tnr's documented session-level sole-tenancy handoff.
- **MVP scope, honest about it.** Own matmul workload against own client interface. Transparency holds only over the **narrow, self-chosen API subset** the builder proxies. Not transparent interception of arbitrary third-party apps. No production substrate.

### The honest delta from tnr (name these explicitly, do not gloss)

It is **not** a "different layer." Same layer, same transparency mechanism. The distance to tnr is:
1. **Coverage breadth.** tnr covers enough of CUDA that *unmodified PyTorch* (which they did not write) runs. The builder's transparency holds only for programs that stay inside the proxied subset. Step outside it (heaps, argument buffers, events, render/blit encoders, indirect command buffers, or even a different device-acquisition entry point) and it breaks.
2. **Fidelity within the layer.** Correct async/stream ordering, allocator semantics, and especially latency-hiding so overhead stays at 20–50% instead of 100x.
3. **Production substrate.** Node pooling, hardware isolation, encryption, billing. Not interception at all.

The defensible one-line claim for the writeup: *"Same layer, same transparency mechanism, narrower self-chosen API surface, plus a scheduler that gap-fills at committed-command-buffer granularity rather than handing off at session level."* That is sharper and more honest than "I reimplemented tnr."

## 4. Why Apple makes this different from CUDA (the one deep technical wrinkle)

This is the most intellectually interesting part of the project and the agent should make sure the builder internalizes it.

**Unified memory breaks the explicit-copy assumption.** With CUDA you move data with explicit `cudaMemcpy` calls, which are discrete API calls you can intercept and ship. With Metal **shared-storage** buffers, the app calls `buffer->contents()`, gets a raw `void*`, and writes into mapped memory with **zero further API calls**. There is nothing on the wire that says "the app just filled this buffer."

**Consequence: you must reconstruct explicit-copy semantics.** The proxy's `contents()` returns a **client-side shadow buffer** you own. Synchronization happens only at two points you already intercept:
- **`commit()`** = snapshot every input buffer bound during encoding, ship it with the recorded command stream. (Inputs out.)
- **completion handler** = blit results back into the client's shadow pointers. (Outputs back.)

This is the **copy-at-commit shim**. Framing for the writeup: you are turning implicit shared-memory access into explicit copy-at-commit, because the network forces a boundary the hardware did not have.

## 5. The interception design: the factory cascade

`metal-cpp` is **header-only**: `MTL::Device`, `MTL::Buffer`, etc. are thin inline C++ wrappers that compile down to `objc_msgSend` baked into the app binary. There is **no metal-cpp dylib to interpose**, so you cannot cut "at the metal-cpp layer." Two real seams exist:

- **Wrong seam (do not):** globally interposing `objc_msgSend`. Hand-written register-sensitive assembly, every message funnels through it. Classic trap.
- **Right seam:** Metal is a **factory cascade**. Intercept only object *creation*; every object is vended by another. Interpose the C bootstrap symbol `MTLCreateSystemDefaultDevice()` (a real exported C symbol, `dyld`-interposable), return a **proxy device**, and have every factory method return *your* proxy wrapping the real-or-remote object.

### The ~15 methods across ~6 proxy types, grouped by role on the wire

This set is **minimal and sufficient** for remoting a compute job. Group by wire behavior, not by class:

**Create (factory → returns a remote handle).** `newCommandQueue`, `newLibrary`, `newComputePipelineState`, `newBuffer`. Each allocates server-side and returns an opaque ID the proxy holds. `newLibrary` (compiles MSL) and `newComputePipelineState` can fail server-side, so they must be **synchronous round-trips with a status reply**. `newBuffer`/`newCommandQueue` can be optimistic/lazy.

**Record (buffered locally, sends nothing).** `commandBuffer`, `computeCommandEncoder`, `setComputePipelineState`, `setBuffer`, `dispatchThreadgroups`, `endEncoding`. None execute. They append to a local script ("bind pipeline 3, bind buffer 7 at index 0, dispatch this grid"). Metal's deferred-encoding model means **the whole job is a serializable script before anything runs**. This is the group that secretly hands you your scheduler's work unit.

**Execute / sync (the wire boundary).** `commit`, `waitUntilCompleted`, `addCompletedHandler`. `commit` flushes: snapshot inputs, serialize the script, ship. The other two are how you learn it finished (blocking vs async).

**Data plane.** `newBuffer` + `contents()`. `contents()` returns a pointer into the client-side shadow buffer. No wire traffic on app read/write. Sync only at commit (inputs) and completion (outputs). This is the copy-at-commit shim expressed in two methods.

### What these 15 deliberately do NOT cover (the MVP boundary)
- **Single stream only.** No `MTLEvent`/fence, no cross-queue ordering. Serialize independent tenants at the scheduler, not inside Metal.
- **Compute only.** No render or blit encoders. `contents()`-snapshot-at-commit replaces blit-copy.
- **No heaps / argument buffers / indirect command buffers.** A matmul never touches them. The first thing a real PyTorch-MPS workload would force you to add is argument buffers.

### metal-cpp gotchas the agent should flag early
- **Refcounting is manual** (`NS::Object` retain/release, `NS::AutoreleasePool`). Proxies must mirror ownership or you leak / over-release across the boundary.
- **Entry-point leakage.** You interposed `MTLCreateSystemDefaultDevice`, but a program that acquires its device via `MTL::CopyAllDevices()` or an `MTKView` never hits your symbol and silently gets a real-or-null device, bypassing the proxy. Cover the acquisition paths your workload actually uses, and document the ones you don't.

## 6. End-to-end architecture (the demo)

Goal: **two tenants, one physical GPU, prove that gap-filling beats naive serialization.**

```
[ Client A (GPU-less) ]            [ Client B (GPU-less) ]
   metal-cpp program                 metal-cpp program
   interposed Metal calls            interposed Metal calls
          |  (committed cmd buffers + input snapshots over TCP)
          v                                   v
       ============  SERVER (owns the one real Metal GPU)  ============
       | per-tenant queues of committed command buffers              |
       | SCHEDULER: gap-fills at committed-cmd-buffer granularity     |
       |   - commit            => "tenant has work ready" event       |
       |   - addCompletedHandler => "GPU slot just freed" event       |
       |   - never waitUntilCompleted on the submit path (async only) |
       | dispatch -> real MTLCommandQueue -> GPU                       |
       | on completion: blit outputs back to the right client shadow  |
       ===============================================================
```

### Critical correctness note on the "2 VMs" idea

The builder wants each user to sit in a VM that, from the user's POV, has a Metal GPU attached. That framing is fine for **isolation and "feels like its own GPU,"** but watch one trap:

> **Do not give the VMs real paravirtualized Metal GPUs.** If each VM has its own (paravirtual) GPU, the Metal calls execute locally and **never route through your remoting layer**, so there is nothing to schedule and nothing to prove. For the demo to be meaningful, the client environment (VM *or* simply a separate process/container) must be **GPU-less**, so its Metal calls get interposed and shipped to the single server that owns the one real GPU.

Recommendation: start with **two plain GPU-less client processes** on the host, not VMs. It removes Apple's VM licensing limits (2 VMs/host, macOS-guest constraints) and isolates exactly the variable you care about (the scheduler). Promote to real VMs later only if you specifically want to demonstrate VM-level isolation. The scheduling result is identical either way.

## 7. Transport: gRPC over HTTP/2 (chosen)

**Decision: use gRPC + protobuf for the MVP.** Rationale: the value of this project is the interception and the scheduler, not a hand-rolled wire codec. gRPC gives serialization, message framing, request/response correlation, and connection multiplexing for free, so the transport stops being something the builder thinks about. The cost is framework overhead the builder is explicitly *not* trying to optimize away (this demo proves a scheduling win, not an overhead number).

**Framing point for the agent (so "automatic encoding" does not mislead the builder):** gRPC sits *on top of* TCP (gRPC -> HTTP/2 -> TCP). Raw TCP gives only a reliable ordered byte stream with **no message boundaries**, so going raw forces you to build framing (length-prefix), serialization, request/reply correlation, and multiplexing by hand. That is real work with nothing to do with GPU virtualization. gRPC is the well-tested version of the protocol you would inevitably start writing on top of TCP anyway. tnr itself runs closer to raw "GPU over TCP" precisely because it fights for the 20-50% overhead number; the builder does not, so gRPC is the right call here. Record it as a known tradeoff in the writeup and move on.

**Where protobuf is genuinely automatic:** the control plane. The recorded script (bind pipeline, bind buffer at index, dispatch dims), the create-calls and their status replies, tenant registration. Small structured messages with obvious schemas; `protoc` generates the C++ structs, encode, and decode. This is most of the method surface, the easy 90%.

**Where "bytes just works" hides a cliff:** buffer payloads. protobuf will carry input snapshots in a `bytes` field, but it copies into the message, and gRPC has a default ~4 MB inbound message cap. Real matmul inputs blow past that. So: raise the max message size on both channel ends, and for large buffers prefer a **client-streaming** RPC that chunks the buffer across frames rather than one giant message.

**Where the gRPC method shape matters for the scheduler (the important part):** the async submit path. Gap-filling depends on `commit` being non-blocking and completion arriving as an *event*, not a return value. Map that onto stream shapes:
- `Commit` must **not** be a unary blocking call that returns the result. A synchronous round-trip here serializes tenants and defeats the entire demo.
- Completion is a **server-streaming** RPC the tenant opens once; the server pushes "buffer N finished" when the GPU slot frees. This mirrors `addCompletedHandler` exactly.
- Keep synchronous round-trips only where they belong: `newLibrary` / `newComputePipelineState`, which can genuinely fail server-side and whose result you need before proceeding.

Notice the design pressure: gRPC did not just hand over encoding, it *nudged* the architecture toward separating submit from completion, which is exactly the separation the gap-filler needs.

First-cut service surface (shape only, builder fills the bodies):

```proto
service GpuRemote {
  rpc RegisterTenant(TenantHello) returns (TenantId);
  rpc CompilePipeline(PipelineSource) returns (PipelineStatus); // sync: can fail
  rpc Commit(stream CommandBufferChunk) returns (CommitAck);    // client-stream: chunk big inputs
  rpc StreamCompletions(TenantId) returns (stream Completion);  // server-stream: slot-freed event
}
```

Four RPCs cover register, create-with-failure, async submit with chunked payloads, and the completion event the scheduler runs on. The `.proto` for the **record group** (how one encoded command buffer is represented: pipeline/buffer references by handle, dispatch dims, bindings) is the schema everything else hangs off, so it is the right first real modeling decision (milestone 2).

## 8. The workload: a trivial program with *naturally embedded* idleness

The demo only works if each tenant's program leaves the GPU idle in a way the scheduler can reclaim. The agent must make sure the builder puts the idle in the **right place**.

- **Reclaimable idle lives between command buffers, on the CPU side.** Dispatch a matmul (GPU busy), then do host-side work / read back / wait before the next dispatch. During that CPU gap the GPU is idle. That gap is what tenant B fills.
- **Non-reclaimable idle:** a stall *inside* a kernel. Metal does not preempt mid-kernel, so you cannot reclaim that. Do not put the idle there.

Shape of the tenant program (illustrative, builder writes the body):

```
loop N times:
    dispatch matmul          # GPU busy ~Tbusy
    read result back to host
    host work / sleep        # GPU IDLE ~Tidle   <-- the reclaimable gap
```

Tune so each tenant is ~50% GPU-idle. One tenant ≈ X. Two tenants naive (serial, `waitUntilCompleted`) ≈ 2X. With gap-filling, the two idle halves overlap and you approach ~X plus overhead.

## 9. How to prove correctness (measurement, not vibes)

- **Two conditions, same workload:** (a) naive scheduler (`waitUntilCompleted`, one finishes before the next starts) vs (b) gap-filling scheduler.
- **Primary metric: GPU utilization, not just wall clock.** Use Metal's command-buffer `GPUStartTime` / `GPUEndTime` to compute busy time. The headline is utilization going from ~50% to ~95%+, with wall clock dropping as a consequence. Utilization is more convincing than wall clock alone.
- **State the ceiling explicitly (rigor).** If each tenant is 50% idle, two tenants can pack toward ~1X of pure GPU-busy time, so wall clock approaches X + overhead, **not below**. You cannot exceed 100% busy. Past the point where idle is filled, adding tenants re-serializes. Naming this ceiling is the kind of precision that makes the result land.
- **Report overhead honestly:** RPC + copy-at-commit cost, throughput under contention, latency tails.

## 10. Suggested build order (milestones)

1. **Single-tenant remoting, end to end.** Interpose `MTLCreateSystemDefaultDevice`, stand up the proxy device whose factory methods return wrappers. Run one matmul on the server, results back. (Keystone: once the cascade returns your objects, everything hangs off it.)
2. **Wire format for a committed command buffer.** Define the `.proto` for the record group (see Section 7): pipeline ref + bindings + dispatch dims + input snapshots. Make capability "serializable work unit" concrete.
3. **Copy-at-commit shim.** Shadow buffers, snapshot-at-commit, blit-back-on-completion. This is the Apple-specific core; do not skip the reasoning.
4. **Two tenants, one shared queue, round-robin.** Async submission via `addCompletedHandler`, never block the submit path.
5. **Memory accounting + admission control.** Unified memory is the thing actually oversubscribed; per-tenant quota, reject/evict over budget.
6. **Gap-filling scheduler + instrumentation.** Round-robin -> weighted fair queueing -> lottery, in increasing order of interesting. Measure utilization and wall clock across conditions. **This is the writeup payload.**
7. *(Optional stretch)* **One real, unwritten metal-cpp workload running unmodified.** The moment a third-party program works untouched, you cross from "remote GPU RPC" into "transparent GPU virtualization," the actual tnr threshold. Expect it to push you past 15 methods (argument buffers first).

## 11. Reading list to point the builder at
- **rCUDA** papers: the canonical remote-GPU-over-network model; this project is rCUDA-for-Metal conceptually.
- **NVIDIA MPS** docs: the closest "software GPU sharing" prior art; seeing what it gives you that Apple does not sharpens the design.
- Apple **Metal** programming guide (command buffers, encoders, shared vs private storage) and **`metal-cpp`** sample headers (refcounting, autorelease).
- Thunder Compute's own GPU-over-TCP writeup: read it for the session-level sole-tenancy model you are deliberately going finer than. Treat its numbers as company claims.

## 12. One-paragraph summary for the agent

The builder is reconstructing Thunder Compute's architecture on Apple Silicon: user-space interposition of Metal's create/record/execute factory cascade (about 15 methods across about 6 proxy types) so a GPU-less client runs a `metal-cpp` program unmodified against a remote GPU, with a copy-at-commit shim to handle unified memory (the key divergence from CUDA remoting). The differentiator is a scheduler that gap-fills at committed-command-buffer granularity, finer than tnr's documented session-level handoff. Scope is an MVP: own matmul workload with deliberately embedded CPU-side idle, two GPU-less tenants on one real GPU, proving that gap-filling drives utilization from ~50% to ~95% and wall clock below naive 2X. The builder wants to learn, so mentor with hints and small snippets and never hand over the full logic.
