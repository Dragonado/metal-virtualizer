# metal-virtualizer

Remote GPU virtualization for Apple Silicon. A GPU-less client runs a
metal-cpp program while the actual work executes on a server that owns the
real GPU, with a scheduler that gap-fills between tenants at
committed-command-buffer granularity.

Design: [docs/metal-gpu-remoting-steering-doc.md](docs/metal-gpu-remoting-steering-doc.md).
Build plan: [docs/TODO.md](docs/TODO.md).

## Layout

- `proto/` - gRPC wire contract
- `src/` - server (owns the real GPU) and client
- `metal/` - local Metal sample workload (vector add)
- `third_party/metal-cpp/` - vendored Apple metal-cpp headers
- `docs/` - steering doc, build plan, notes

## Build and run

Everything builds with Bazel:

```sh
bazel build //...

bazel run //metal:adder    # vector add on the local GPU
bazel run //src:server     # gRPC server
bazel run //src:client     # gRPC client (expects a running server)
```

Editor IntelliSense (regenerates compile_commands.json):

```sh
bazel run //:refresh_compile_commands
```
