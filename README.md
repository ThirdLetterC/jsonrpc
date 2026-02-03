# JSON-RPC Server (C23 + libuv)

Minimal JSON-RPC 2.0 server skeleton written in strict C23 with a libuv transport layer and built with Zig. The code separates protocol handling from the event loop via a small transport interface and exposes application callbacks for `on_open`, `on_request`, `on_notification`, and `on_close`.

## Status and Limitations

- Implements JSON-RPC 2.0 request/response handling with batch support.
- Framing is newline-delimited JSON (one complete JSON-RPC message per line).
- Requests larger than 64 KiB are rejected; inbound buffer is capped at 128 KiB.
- Suitable as a starting point for experimenting with libuv and C23 patterns, not for production use.
- No TLS, authentication, or HTTP transport; connections are plain TCP.

## Prerequisites

- Zig (for the build system) and a C toolchain that supports `-std=c23`.
- libuv headers and libraries (e.g., `sudo apt install libuv1-dev` or `brew install libuv`).
- Optional tools: `just` for task shortcuts, `clang-format` for formatting, and `valgrind` for leak checks.

## Building

- Default debug build: `just build` or `zig build`.
- Release-style builds: `zig build -Doptimize=ReleaseSafe` (or `ReleaseFast`/`ReleaseSmall`).
- Address/UB/leak sanitizers (Debug only): `zig build -Dsanitizers=true`.
- Benchmark tool (`bench_rps`) is installed to `zig-out/bin/bench_rps` by `zig build`.

## Running

- Start the server on the default port 8080: `just run` or `zig build run`.
- Choose a port: `zig build run -- 9090` or `just run p=9090`.
- Shutdown signals: SIGINT/SIGTERM trigger a graceful loop stop.
- The server emits a console log on new connections and processes JSON-RPC requests.

## Testing (lightweight)

- Use `nc localhost 8080` and send one JSON-RPC message per line.
- Example request:
  `{"jsonrpc":"2.0","id":1,"method":"ping"}` -> `{"jsonrpc":"2.0","id":1,"result":"pong"}`
- Example add:
  `{"jsonrpc":"2.0","id":2,"method":"add","params":[1,2,3]}` -> `{"jsonrpc":"2.0","id":2,"result":6}`
- Notifications omit `id` and receive no response.

## Benchmarking

- Build (if not already): `zig build`
- Example run:
  `./zig-out/bin/bench_rps --host 127.0.0.1 --port 8080 --connections 50 --duration 5 --timeout 5 --method ping`

## Project Layout

- `src/main.c` — wires CLI args, signal handling, and application callbacks.
- `src/server.c` — libuv server setup, connection lifecycle, and transport glue.
- `src/jsonrpc.c` / `src/jsonrpc.h` — JSON-RPC protocol handling and callback surfaces.
- `build.zig` — Zig build graph, compiler flags (`-std=c23 -Wall -Wextra -Wpedantic -Werror`), and sanitizer toggles.
- `justfile` — helper tasks for build, run, deps, format, and leak checks.

## Valgrind Memory Profiling

To build and run the server with Valgrind's Massif tool for memory profiling, use:

```bash
zig build -Dvalgrind

valgrind --tool=massif --stacks=yes ./zig-out/bin/jsonrpc_server
ms_print massif.out.<pid>
```
