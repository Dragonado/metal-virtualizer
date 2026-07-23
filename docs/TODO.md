# tnrc build plan

Ordered by build sequence, not by importance. Each milestone is a provable
checkpoint. The seams from the design discussion are grouped into the milestone
where they first have to work.

Legend: (create) object seam, (cmd) command seam, (coh) coherence seam,
(life) lifecycle seam, (query) query seam.

## M0 — Transport and build scaffolding

The current `build.sh` is one `clang++` call. It will not survive gRPC and
protobuf codegen. Stand up the plumbing before any Metal crosses the wire.

- [x] Move to a real build system (went with Bazel, not CMake) that runs protobuf/gRPC codegen; everything including the metal sample builds via `bazel build //...`
- [ ] Define the `.proto` service skeleton with one hello RPC
- [ ] Trivial client <-> server RPC round-trips, no Metal involved
- [ ] Decide the process model (client process <-> local daemon over gRPC loopback for MVP)

## M1 — Root interposition + handle plumbing (riskiest proof)

- [ ] Interpose `MTLCreateSystemDefaultDevice()` and return a proxy device
- [x] DECIDE the proxy strategy: header shim (DECIDED). ObjC-runtime interposition is a stretch goal only.
- [ ] Handle table on both sides: proxy handle <-> server object
- [ ] Server creates the ONE real device once, shared across all sessions
- [ ] Reject unknown or released remote handles with `NOT_FOUND`; never use
      `map[id]` for an untrusted RPC ID because it inserts a null entry.
- [ ] Prove it end to end: (query) `device->name()`, `device->supportsFamily()` forward over the wire and print correctly

## M2 — Object (create) seams

Each returns a proxy carrying a server-side handle. Failable ones must
round-trip the `NS::Error`.

- [ ] (create) `newCommandQueue`
- [ ] (create) `newLibrary(source, ...)` — ships the source string, compiles server-side
- [ ] (create) `newFunction(name)`
- [ ] (create) `newComputePipelineState(func, &err)` — piggyback `maxTotalThreadsPerThreadgroup` on the response so it never needs its own round-trip
- [ ] (create) `newBuffer(length, options)` — allocate server buffer AND client shadow; ship length+options only, not contents
- [ ] Convert server-side creation failures into a local `NS::Error` when the
      caller supplies `NS::Error**`; never leave the caller's error pointer
      uninitialized.

## M3 — Command seams + first single-tenant round trip

Command seams do NOT each cross the wire. They append to a local command list
that flushes at commit. This batching is the latency hider.

- [ ] (create) `queue->commandBuffer()` opens a local recording
- [ ] (create) `commandBuffer->computeCommandEncoder()` opens an encoding scope
- [ ] (cmd) `setComputePipelineState`
- [ ] (cmd) `setBuffer` — record which buffers are bound; this feeds commit's snapshot
- [ ] (cmd) `dispatchThreads`
- [ ] (cmd) `endEncoding`
- [ ] (coh) `commit()` — flush command list + snapshot bound input buffers, ship both
- [ ] (data) `buffer->contents()` — return the client shadow pointer (allocated at newBuffer)
- [ ] (coh) completion / `waitUntilCompleted()` — deliver output bytes back into shadows
- [ ] MILESTONE: the vector-add from `main.cpp` runs correctly over the wire and `verify()` passes

## M4 — Copy-at-commit shim correctness

- [ ] Snapshot-both-ways per bound buffer (correct-but-wasteful baseline)
- [ ] Confirm coherence contract: inputs at commit, outputs at completion, stale-before-completion matches local semantics
- [ ] Storage-mode routing: Shared (shadow + copy), Private (server-only + blit), Managed (explicit sync hooks)
- [ ] Document the unsupported pattern (concurrent mid-flight CPU/GPU access), do not police it
- [ ] (life) `retain` / `release` -> server-side GC: free buffers and PSOs when the client is done (mandatory with multiple tenants)

## M5 — Multi-tenant + scheduler

Two independent decisions live here. Do not conflate them.
- Admission (who may use the GPU at once): serial (one tenant until it
  finishes/idles, the Thunder model) vs concurrent (multiple live tenants).
- Policy (how committed work is ordered once several tenants have some): trivial
  FIFO vs fair-share / priority / preemption.

DECISION: concurrent admission + trivial FIFO policy. Only concurrent admission
produces an instantaneous utilization uplift; serial admission just packs
tenants over wall-clock time and cannot move the 50%->95% number. The
interleaving mechanism is nearly free once session isolation exists: everything
funnels through one server and one real queue, so gap-fill falls out of "one
submission point, multiple producers." FIFO is enough for the proof. Fancy
policy is deferred.

- [ ] Two independent client sessions against one server/GPU, admitted CONCURRENTLY
- [ ] Session isolation: separate handle tables, separate command streams, per-session copy-at-commit
- [ ] Do NOT hold a global lock that serializes tenants across GPU submit (that would silently make it serial)
- [ ] FIFO submission of committed command buffers across tenants (this IS the gap-fill mechanism, not a separate component)
- [ ] Synchronize shared server handle tables and ID allocation before allowing
      concurrent RPC handlers to mutate them.
- [ ] Start with one mutex for `counter_` and all handle maps. Keep its critical
      sections to ID/map bookkeeping only; do not hold it while waiting for the
      GPU. Consider per-map locks only after measuring contention, with a fixed
      lock order and safe object lifetimes after lookup.
- [ ] Scheduler job lifetime: at enqueue, retain the real command queue,
      pipeline state, and bound buffers in `PendingJob`. A client `release()`
      removes its public handle, but the real object stays alive until every
      queued or running job using it has completed.
- [ ] Scheduler completion copyback: store output bytes with the completed job
      instead of looking buffers up through global maps in
      `WaitUntilCompleted()`. Copy results only into client shadow buffers that
      are still live; a client that released its last buffer handle cannot
      observe its output.
- [ ] DEFERRED (stretch): fair-share / priority policy. Sub-kernel preemption is not exposed by the Metal API, do not attempt.

### VM test setup (optional)

- [ ] Run the GPU server on the host, bound to `0.0.0.0:50051`.
- [ ] Give each VM a network interface with a route to the host.
- [ ] Configure each client to connect to the host's VM-reachable IP address and
      port, not `localhost`. Each VM has its own loopback interface, so
      `localhost:50051` inside a guest refers to that guest, not the host.
- [ ] CAVEAT: verify the guest is genuinely GPU-less or that all Metal device
      acquisition is forced through the shim. A VM that exposes a local virtual
      GPU/Metal path can bypass the remoting layer and invalidate the test.

## M6 — The proof (measurement)

Metric is pinned to the concurrent-admission model: INSTANTANEOUS GPU
utilization sampled while two tenants run at the same time. (Serial admission
would force a different metric, "fraction of wall-clock time the GPU is busy,"
which is not the claim being made.)

- [ ] Instrument GPU utilization (instantaneous, sampled during the concurrent run)
- [ ] Two GPU-less tenants driving one GPU concurrently: measure uplift, target ~50% -> ~95%
- [ ] CAVEAT (Risk #5): gap-fill only helps if tenants leave gaps. Use bursty,
      sub-saturating workloads. Two tenants that each already saturate the GPU
      alone gain nothing and the number will not move. The demo workloads must
      be designed for this, it is not automatic.
- [ ] Write up results in blog-notes.md

## Post-MVP hardening

- [ ] Handle failed gRPC statuses before using response data.
- [ ] Validate malformed RPC data server-side, including parallel repeated-field
      lengths and packed buffer-data sizes.
- [ ] Clean up completed server command buffers.

## Optimizations (after the skeleton works)

- [ ] Pipelining: encode command buffer N+1 while the GPU runs N
- [ ] Dirty-page tracking: mprotect the shadow, ship only dirty pages
- [ ] Copy-direction from kernel arg qualifiers (`device const` vs `device`) to drop wasted transfers
- [ ] Eager-fetch derived properties at create time (generalize the maxTotalThreadsPerThreadgroup trick)

## Risks and unknowns (biggest first)

1. **Proxy object strategy for metal-cpp.** metal-cpp methods are inline and
   compile to `objc_msgSend` on the underlying pointer, so you cannot just
   subclass `MTL::Device` and override. Three paths:
   - (a) intercept at the ObjC runtime (NSProxy-style `forwardInvocation` on the
     underlying protocol objects). Most transparent, deepest.
   - (b) ship your own shim headers so the client compiles `MTL::Device` etc. as
     YOUR proxy classes. Far more tractable, but requires recompiling the client
     against your headers, not transparent to prebuilt binaries.
   - (c) DYLD_INSERT_LIBRARIES to hook the C symbol, but the returned pointer
     still has to respond to MTLDevice selectors.
   DECISION MADE: header shim (b) for the MVP, because the client is always
   self-authored so the recompile cost is zero. ObjC-runtime interposition (a)
   is a stretch goal reserved for running unmodified third-party binaries. This
   removes the largest source of schedule variance.
2. gRPC C++ build/toolchain friction on macOS.
3. Correct serialization of command args (setBuffer offsets, sizes, handle ids).
4. Server-side object lifetime and GC under multiple tenants.
5. Whether the scheduler actually raises utilization is empirical. Gap-fill only
   helps if tenants leave gaps; a tenant that saturates the GPU alone gains
   nothing.

## Honest time estimate

Focused engineering effort (not calendar), wide error bars:

- M0: 2-4 days
- M1: 3-7 days (HIGH variance, driven entirely by Risk #1)
- M2: 3-5 days
- M3: 4-7 days (first real round trip, serialization debugging)
- M4: 4-7 days (coherence edge cases)
- M5: 5-10 days (concurrency + scheduler)
- M6: 2-4 days

Total: roughly 23-44 focused days, i.e. 4-9 weeks full-time-equivalent.
As a part-time learning project (~10-15 hrs/week) that is closer to 2-5 months
calendar. First big milestone (M0-M3, a correct single-tenant round trip) is
~2-4 weeks FTE, or 6-8 weeks part-time.

The single biggest determinant of the total is the M1 proxy decision. The
learning-as-you-go nature widens every estimate; treat these as planning
anchors, not commitments.

## Concepts to revisit (study notes)

### What objc_msgSend is (revisit until solid)

- [ ] Re-read this until it clicks; it is the whole reason the CUDA transparency
      trick does not port to Metal.

Essence: in C and C++, a method call is resolved to a specific function address
at compile or link time. In Objective-C, a call is a message dispatched at
runtime by name, and every message goes through one universal C function,
objc_msgSend, which looks up the real implementation and jumps to it.

What a call becomes. `[device newBufferWithLength:100 options:0]` compiles to
roughly `objc_msgSend(device, <selector "newBufferWithLength:options:">, 100, 0)`.
metal-cpp's `device->newBuffer(100, 0)` inlines to the same thing (see
NSObject.hpp line 214, where sendMessage calls objc_msgSend). The method name is
the second argument, a selector (SEL), essentially an interned string. It is
DATA passed in a register, not a symbol. The only symbol at the call site is
objc_msgSend itself.

Runtime lookup. Every ObjC object's first word is an isa pointer to its Class.
Each Class has a table mapping selector -> IMP (a plain function pointer) plus a
link to its superclass. objc_msgSend reads receiver->isa, looks up the selector
in the class method cache (fast path) or method list, walks the superclass chain
if needed, and tail-calls the IMP. If the selector is nowhere, it enters the
forwarding machinery (forwardInvocation:).

Three kinds of call:
- C function `cuMemAlloc(...)`: the symbol cuMemAlloc is at the call site,
  resolved by the dynamic linker, interposable by shadowing the symbol. This is
  why Thunder works.
- C++ virtual method: indirect jump through the object's vtable, not a symbol.
- ObjC message: objc_msgSend plus a selector as data, resolved by the runtime.
  The method is NOT a symbol, so the linker has nothing per-method to shadow.

Why this defeats LD_PRELOAD/DYLD interposition for Metal: newBufferWithLength:
is never a symbol in the binary, it is a string handed to objc_msgSend. The only
interposable symbol is objc_msgSend, and that is the funnel for every message to
every object in the process, so intercepting there is a performance catastrophe
and fragile.

The right layer (if you ever want transparency): intercept in the ObjC runtime,
not the linker. Either method swizzling (method_exchangeImplementations swaps the
IMP for one class+selector) or an NSProxy that implements nothing so every
message falls through to forwardInvocation:, which hands you a reified call
(selector + boxed args). This forwarding machinery was designed for remoting
(old NeXT/Apple Distributed Objects), which is why the ObjC path is intended, not
a hack, just deep.

Why the header shim sidesteps all of it: the client compiles against my C++
MTL::Device, whose newBuffer body is my code calling gRPC. No objc_msgSend, no
real Metal object, nothing to intercept. Substitution happens at compile time,
so the whole dispatch mechanism is out of the picture.
