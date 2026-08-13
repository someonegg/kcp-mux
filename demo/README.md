# kcpmux demo

This directory contains a small UDP client/server demo for `kcp-mux`.

The demo shows the minimum integration path:

- create a `kcpmux_engine_t`
- replace the one-shot engine timer from `set_timer` and dispatch it when due
- feed UDP packets into `kcpmux_engine_input`
- send UDP packets from the `write_socket` callback
- defer read/write API calls out of notification callbacks into the event loop
- open multiple streams on one connection
- send and echo multiple message-boundary-preserving payloads on each stream
- batch stream operations with `flush=0` and `kcpmux_stream_finish_batch()`

## Build

```sh
cmake -S . -B build -DKCPMUX_BUILD_DEMOS=ON
cmake --build build --target demo_server demo_client
```

## Run

Start the server:

```sh
./build/demo/demo_server --port 8443 --batch-threshold 4
```

Run the client from another terminal:

```sh
./build/demo/demo_client --host 127.0.0.1 --port 8443 \
    --streams 3 --messages 10 --batch-threshold 4 --message hello
```

Expected client output ends with:

```text
OK streams=3 messages=10 bytes=873
```

## Options

Server:

```text
--host ADDR
--port PORT
--batch-threshold N (default: 4; 1 disables batching)
--quiet
```

Client:

```text
--host ADDR
--port PORT
--streams N
--messages N (default: 10, maximum: 128)
--batch-threshold N (default: 4; 1 disables batching)
--message TEXT
--timeout-ms MS
--quiet
```

Each payload includes its logical stream number, message number, and the text supplied by
`--message`. Because every payload is smaller than the default KCP MSS, each `send` is
delivered as one distinct KCP message and the client validates every echo by message boundary
and sequence number.

Both sides call `kcpmux_stream_send(..., flush=0)` for each message. Reaching
`batch_threshold` schedules an update automatically. After sending a group on a known stream,
`kcpmux_stream_finish_batch()` immediately submits any tail below the threshold. After feeding
a group of UDP packets to the engine, `kcpmux_engine_finish_batch()` submits tails for every
affected stream, including streams that did not become readable. Both functions are safe no-ops
when there is no pending work.
