# kcp-mux

`kcp-mux` is a small C library for multiplexed stream transport over KCP.
It lets one peer connection carry multiple independent streams, while the
caller keeps control of the underlying packet I/O, timers, and event loop.

## Features

- connection handshake with optional protocol extension data
- multiple independent streams over one peer connection
- stream read/write notifications and close notifications
- keepalive, idle timeout, and retry configuration
- per-engine, per-connection, and per-stream statistics
- pluggable KCP operations, with a bundled default KCP implementation

## Build

Requirements:

- CMake 3.10+
- C99 compiler
- C++17 compiler when building tests

Build the library:

```sh
cmake -S . -B build
cmake --build build
```

Build the demo programs:

```sh
cmake -S . -B build -DKCPMUX_BUILD_DEMOS=ON
cmake --build build --target demo_server demo_client
```

## Install

```sh
cmake --install build --prefix /usr/local
```

The install exports a CMake package:

```cmake
find_package(kcpmux CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE kcp-mux::kcpmux)
```

## Demo

Start the UDP echo server:

```sh
./build/demo/demo_server --port 8443
```

Run a multi-stream client from another terminal:

```sh
./build/demo/demo_client --host 127.0.0.1 --port 8443 \
    --streams 3 --message hello
```

The client exits after all streams receive their echo responses:

```text
OK streams=3 bytes=66
```

See [demo/README.md](demo/README.md) for the full demo usage.

## Protocol

See [docs/protocol.md](docs/protocol.md) for the current wire format and
connection/stream state behavior.

## Architecture Decision Records

- [Use an indexed binary min-heap for kcpmux deadlines](docs/adr/0001-use-indexed-min-heap-for-kcpmux-deadlines.md)
- [Reject out-of-order unknown peer stream IDs](docs/adr/0002-reject-out-of-order-unknown-peer-stream-ids.md)

## License

MIT. See [LICENSE](LICENSE).
