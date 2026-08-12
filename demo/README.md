# kcpmux demo

This directory contains a small UDP client/server demo for `kcp-mux`.

The demo shows the minimum integration path:

- create a `kcpmux_engine_t`
- replace the one-shot engine timer from `set_timer` and dispatch it when due
- feed UDP packets into `kcpmux_engine_input`
- send UDP packets from the `write_socket` callback
- defer read/write API calls out of notification callbacks into the event loop
- open multiple streams on one connection
- echo data back on each stream

## Build

```sh
cmake -S . -B build -DKCPMUX_BUILD_DEMOS=ON
cmake --build build --target demo_server demo_client
```

## Run

Start the server:

```sh
./build/demo/demo_server --port 8443
```

Run the client from another terminal:

```sh
./build/demo/demo_client --host 127.0.0.1 --port 8443 \
    --streams 3 --message hello
```

Expected client output ends with:

```text
OK streams=3 bytes=66
```

## Options

Server:

```text
--host ADDR
--port PORT
--quiet
```

Client:

```text
--host ADDR
--port PORT
--streams N
--message TEXT
--timeout-ms MS
--quiet
```
