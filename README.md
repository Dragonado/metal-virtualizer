# metal-virtualizer

Remote GPU virtualization for Apple Silicon. A GPU-less client runs a
metal-cpp program while the actual work executes on a server that owns the
real GPU, with a scheduler that gap-fills between tenants at
committed-command-buffer granularity.

Design: [docs/metal-gpu-remoting-steering-doc.md](docs/metal-gpu-remoting-steering-doc.md).
Build plan: [docs/TODO.md](docs/TODO.md).

## Layout

- `proto/` - gRPC wire contract
- `server/` - Hosted server with real GPU
- `src/` - client code
- `third_party/metal-cpp/` - vendored Apple metal-cpp headers
- `docs/` - steering doc, build plan, blog notes

## Build and run

Everything builds with Bazel:

```sh
bazel run //server:server    # Host the gRPC server with the GPU
```

In a new terminal

```sh
bazel run //src:adder     # Run client code (which is automatically hijacked by shim)
```

Editor IntelliSense (regenerates compile_commands.json):

```sh
bazel run //:refresh_compile_commands
```
