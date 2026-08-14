# KCPMUX Wire Protocol

This document describes the current implementation, mainly [src/kcpmux_protocol.c](../src/kcpmux_protocol.c). The protocol layer wraps connection control, stream control, and KCP output packets into lower-layer datagrams. Actual socket I/O, timing, and peer address handling are provided by upper-layer callbacks and `kcpmux_engine`.

## Overview

KCPMUX multiplexes multiple streams over one logical connection identified by a peer address. Each stream is driven by an independent KCP state machine. Every lower-layer datagram is a KCPMUX message:

```text
+---------+------------------+-------------------+
| type(1) | generation_id(3) | message body(...) |
+---------+------------------+-------------------+
```

General rules:

- Multi-byte integers use network byte order, meaning big-endian.
- `generation_id` is a nonzero 24-bit connection generation. Except for `CONN_CONNECT`, packets are accepted only when both peer address and generation match.
- `stream_id` is a nonzero 32-bit unsigned integer; initiators use odd ids and acceptors use even ids.
- The current protocol version is `KCPMUX_VERSION = 3`.
- The stack buffer limit for one protocol message is `KCPMUX_PROTO_MSG_MAX_LEN = 1500`.
- Protocol extension data is limited to `KCPMUX_PROTO_EXT_MAX_LEN = 512`.
- The receiver validates minimum length and declared extension length, but does not require the datagram length to exactly match the message length. Extra trailing bytes are ignored, except for `STREAM_PAYLOAD`, where all bytes after the header are passed into KCP.

## Message Types

| Type | Value | Direction | Purpose |
| --- | --- | --- | --- |
| `CONN_CONNECT` | `0x81` | initiator to acceptor | Start connection handshake |
| `CONN_CONNECT_ACK` | `0x82` | acceptor to initiator | Return handshake result |
| `CONN_KEEPALIVE` | `0x83` | both directions | Keepalive |
| `CONN_CLOSE` | `0x84` | both directions | Request connection close |
| `CONN_CLOSE_ACK` | `0x85` | both directions | Acknowledge connection close |
| `STREAM_CLOSE` | `0x93` | both directions | Request stream close |
| `STREAM_CLOSE_ACK` | `0x94` | both directions | Acknowledge stream close |
| `STREAM_PAYLOAD` | `0xa1` | both directions | Carry a KCP segment for one stream |

## Connection Handshake

### `CONN_CONNECT`

```text
0                   1                   4                   5                   7
+-------------------+-------------------------------+-------------------+-------------------+
| type = 0x81       | generation_id(u24)            | version           | ext_len(u16)      |
+-------------------+-------------------------------+-------------------+-------------------+
| ext bytes ...                                             |
+-----------------------------------------------------------+
```

The initiator calls `kcpmux_conn_connect`, creates a connection, enters `CONNECTING`, and sends `CONN_CONNECT`. `ext` comes from the caller-provided `proto_ext`; values longer than 512 bytes are truncated before being stored.

On receive, the acceptor:

1. Checks that `version` equals `KCPMUX_VERSION`.
2. Looks up an existing connection by peer address and compares its generation.
3. If the generation matches, treats the packet as a retransmission and resends a successful ACK.
4. If the generation differs, creates the replacement before closing the old connection with `KCPMUX_CLOSE_REASON_REPLACED`.
5. Stores the peer extension data and calls `engine->callbacks.conn_connect_notify`. Only `KCPMUX_ACK_RESULT_OK` accepts the connection; any other result rejects and automatically releases it.

Note: if the version does not match and no connection exists for the peer address, the current implementation only returns an error. It does not create a temporary connection to send a version-reject ACK.

### `CONN_CONNECT_ACK`

```text
0                   1                   4                   5                   6                   8
+-------------------+-------------------------------+-------------------+-------------------+-------------------+
| type = 0x82       | generation_id(u24)            | version           | result            | ext_len(u16)      |
+-------------------+-------------------------------+-------------------+-------------------+-------------------+
| ext bytes ...                                                                 |
+-------------------------------------------------------------------------------+
```

`result` values:

| Result | Value | Meaning |
| --- | --- | --- |
| `KCPMUX_ACK_RESULT_OK` | `0x00` | Accepted |
| `KCPMUX_ACK_RESULT_ERROR` | `0x01` | Generic rejection |
| `KCPMUX_ACK_RESULT_VERSION` | `0x02` | Version mismatch |
| `KCPMUX_ACK_RESULT_CUSTOM` and above | `0x10..0xff` | Upper-layer custom rejection code |

The initiator accepts this message only while the connection is `CONNECTING`. If `result == OK`, it stores the acceptor's extension data and enters `CONNECTED`; otherwise it enters `ERROR` and closes with `VERSION` or `REJECTED`.

The receiver validates both the connection generation and the protocol version. A version mismatch terminates the connection with `KCPMUX_CLOSE_REASON_VERSION`.

### Handshake Retransmission

While a connection is `CONNECTING`, its absolute control deadline is registered with the engine timer heap. If no ACK was received and `retry_count < connect_retries`, the due callback retransmits `CONN_CONNECT`; after the retry limit is reached, the connection enters `ERROR` and closes with `TIMEOUT`.

Default values:

| Parameter | Default |
| --- | --- |
| `ctrl_timeout_ms` | `400ms` |
| `connect_retries` | `2` |

## Connection Keepalive And Close

### `CONN_KEEPALIVE`

```text
0                   1                   4                   8                   12
+-------------------+-------------------------------+-------------------+-------------------+
| type = 0x83       | generation_id(u24)            | time(u32)         | seq(u32)          |
+-------------------+-------------------------------+-------------------+-------------------+
```

After a connection enters `CONNECTED`, it sends `CONN_KEEPALIVE` when the elapsed time since the last keepalive send reaches `keepalive_interval_ms`. `keepalive_seq` is incremented before sending.

The receiver requires the message to be at least 12 bytes and updates `last_recv_ts`. The current implementation does not use `time` or `seq` for RTT, replies, or deduplication.

Default values:

| Parameter | Default |
| --- | --- |
| `keepalive_interval_ms` | `10000ms` |
| `keepalive_timeout_ms` | `30000ms` |

If a `CONNECTED` connection has not received any packet for `keepalive_timeout_ms`, it enters `ERROR` and closes with `TIMEOUT`.
`ctrl_timeout_ms` must be nonzero. A zero `keepalive_timeout_ms` uses the
default `30000ms`; a zero keepalive interval still disables keepalive sends.

### `CONN_CLOSE`

```text
+-------------------+-------------------------------+-------------------+
| type = 0x84       | generation_id(u24)            | reason            |
+-------------------+-------------------------------+-------------------+
```

On receive:

1. Update `last_recv_ts`.
2. Send `CONN_CLOSE_ACK` with the same `reason`.
3. Call `kcpmux_conn_close_internal`, closing the connection and cascading close to all streams.

When `kcpmux_conn_close` is called locally, the connection enters `CLOSING`, records `close_reason = NORMAL`, and sends `CONN_CLOSE`.

### `CONN_CLOSE_ACK`

```text
+-------------------+-------------------------------+-------------------+
| type = 0x85       | generation_id(u24)            | reason            |
+-------------------+-------------------------------+-------------------+
```

This message is accepted only while the connection is `CLOSING`. On receive, the packet `reason` is ignored and the connection closes with the locally recorded `conn->close_reason`.

Close retransmission uses the connection `ctrl_timeout_ms` and `close_retries`. The default `close_retries` is `1`; after the retry limit is reached, the local connection closes directly.

### Idle Timeout

The connection also tracks `last_payload_ts`. If `idle_timeout_ms > 0` and no `STREAM_PAYLOAD` has been received for that duration, the connection sends `CONN_CLOSE(IDLE)` and then closes locally immediately without waiting for an ACK. The default `idle_timeout_ms` is `60000ms`.

## Stream Multiplexing

### Stream Id Allocation

Stream ids are 32-bit numbers:

- The side that initiated the connection uses odd ids: `1, 3, 5, ...`.
- The side that accepted the connection uses even ids: `2, 4, 6, ...`.
- The initial id is randomly chosen with the correct parity, then incremented by 2.
- Allocation wraps naturally at 32 bits, skips zero and active ids, and preserves local parity.

When receiving `STREAM_PAYLOAD` for an unknown `stream_id`, the protocol first validates peer parity. It then applies serial-number ordering against the latest accepted peer stream id: an older, duplicate, or half-ring id is rejected so delayed packets cannot recreate a closed stream. Existing streams remain valid after the high-water mark advances.

### No Explicit Stream Open Message

The current protocol has no `STREAM_OPEN` or `STREAM_CREATE` control message. A remote stream is created by the first `STREAM_PAYLOAD`:

1. Receive an unknown `stream_id` with valid peer parity.
2. If the connection has no `stream_create_notify`, return `KCPMUX_ERR_NOT_FOUND` and do not create the stream.
3. Create a passive stream and insert it into the connection stream map.
4. Call `stream_create_notify`; return `0` to accept or nonzero to reject.
5. On acceptance, feed the current payload into KCP. On rejection, send `STREAM_CLOSE(REJECTED)` and automatically release the stream after its close lifecycle.

This means the peer does not know about a locally created stream until that stream sends payload. Accordingly, if a locally initiated stream has never sent payload, closing it closes it locally without sending `STREAM_CLOSE`.

## Stream Data

### `STREAM_PAYLOAD`

```text
0                   1
+-------------------+-------------------------------+-------------------+
| type = 0xa1       | generation_id(u24)            | stream_id(u32)    |
+-------------------+-------------------------------+-------------------+
| kcp_data bytes ...                                |
+---------------------------------------------------+
```

`kcp_data` is the raw segment emitted by KCP. KCP `conv` is set to `stream_id`, so every stream has its own independent KCP instance and reliable transport state.

Send path:

1. The upper layer calls `kcpmux_stream_send`.
2. Data is split by `kcp_mss` and passed into KCP.
3. The KCP output callback adds the 8-byte KCPMUX header to each KCP segment: `type(1) + generation_id(3) + stream_id(4)`.
4. The packet is sent to the lower layer through the `write_socket` callback.

If the input exceeds `kcp_mss`, one `send` creates multiple KCP messages, so it
does not correspond to one `recv`.

KCP update batching is controlled per stream by `batch_threshold`. Values `0`
and `1` preserve immediate scheduling. Values greater than `1` count successful
non-flushing sends and payload inputs together; reaching the threshold schedules
an immediate KCP update. Call `kcpmux_stream_finish_batch()` after a batch on a
known stream, or `kcpmux_engine_finish_batch()` after an input batch whose UDP
packets may belong to multiple streams. The engine-level operation visits only
streams with pending work. A nonzero `flush` still updates KCP synchronously and
clears the accumulated count.

Receive path:

1. The protocol layer parses `stream_id`.
2. It finds or passively creates the stream.
3. It strips the 8-byte KCPMUX header and feeds the remaining bytes into that stream's KCP instance.
4. If KCP changes from unreadable to readable, `stream_read_notify` is triggered.

Packet size limits:

- Total `STREAM_PAYLOAD` length cannot exceed 1500.
- The KCPMUX header is 8 bytes, so one wrapped KCP segment can carry at most 1492 bytes.
- The default `kcp_mss` is `1200`; KCP's own header overhead is defined as 24 bytes.
- `kcp_mss` must be `1..1468` and is fixed when the stream is created.
- `send_pause_threshold` must be nonzero, and `send_resume_threshold` must not
  exceed it.

## Stream Close

### `STREAM_CLOSE`

```text
0                   1                   4                   8                   9
+-------------------+-------------------------------+-------------------+-------------------+
| type = 0x93       | generation_id(u24)            | stream_id(u32)    | reason            |
+-------------------+-------------------------------+-------------------+-------------------+
```

The receiver must find an existing stream, and `stream_id` must not be `0`. It sends `STREAM_CLOSE_ACK` and then closes the stream. If the stream is not found, the receiver returns `KCPMUX_ERR_NOT_FOUND`.

When `kcpmux_stream_close` is called locally, the stream enters `CLOSING`, records `close_reason = NORMAL`, and sends `STREAM_CLOSE`. The exception is a locally initiated stream that has never sent upper-layer data: it does not send a close packet and closes locally.

### `STREAM_CLOSE_ACK`

```text
0                   1                   4                   8                   9
+-------------------+-------------------------------+-------------------+-------------------+
| type = 0x94       | generation_id(u24)            | stream_id(u32)    | reason            |
+-------------------+-------------------------------+-------------------+-------------------+
```

This message is accepted only while the stream is `CLOSING`. On receive, the packet `reason` is ignored and the stream closes with the locally recorded `stream->close_reason`.

Stream close retransmission uses the stream `ctrl_timeout_ms` and `close_retries`. Default values:

| Parameter | Default |
| --- | --- |
| `ctrl_timeout_ms` | `600ms` |
| `close_retries` | `1` |

## Errors And Close Reasons

### Common Input Errors

| Case | Return |
| --- | --- |
| Null parameter or zero length | `-KCPMUX_ERR_INVALID_PARAM` |
| Unknown message type | `-KCPMUX_ERR_INVALID_FORMAT` |
| Truncated message | `-KCPMUX_ERR_INVALID_FORMAT` |
| Extension length exceeds 512 | `-KCPMUX_ERR_INVALID_FORMAT` |
| Connection or stream not found | `-KCPMUX_ERR_NOT_FOUND` |
| State mismatch | `KCPMUX_ERR_STATE` |
| KCP input failure | `KCPMUX_ERR_KCPRET(ret)` |

Note: some state errors return the already-negative constant `KCPMUX_ERR_STATE = -200`, while format errors usually return `-KCPMUX_ERR_INVALID_FORMAT`.

### Close Reasons

| Reason | Value | Meaning |
| --- | --- | --- |
| `NORMAL` | `0x00` | Normal close |
| `ERROR` | `0x01` | Error close |
| `VERSION` | `0x02` | Version mismatch |
| `REJECTED` | `0x03` | Rejected by upper layer |
| `IDLE` | `0x04` | Idle close |
| `TIMEOUT` | `0x05` | Timeout close |
| `REPLACED` | `0x06` | Superseded by a new connection generation |

## Implementation Notes

- Connection lookup starts from the lower-layer peer address; the 24-bit generation then isolates stale packets from replaced connections.
- Connection-level receive statistics are updated only after an existing connection is found. An unknown peer's initial `CONN_CONNECT` is not counted in that connection's stats before the connection is created.
- Passive connection creation depends on `conn_connect_notify`. If that callback is NULL, the current implementation creates a connection but defaults to `KCPMUX_ACK_RESULT_ERROR`, then sends a reject ACK and frees the connection.
- `KEEPALIVE` is a one-way heartbeat packet; the peer is not required to reply with the same sequence number.
- `STREAM_CLOSE` does not auto-create unknown streams. Only `STREAM_PAYLOAD` can trigger passive stream creation.
- The `reason` carried by control ACK packets is mainly echoed back. Close completion uses the locally recorded close reason.
- `idle_timeout_ms` is based on received payload time, not on arbitrary control packets or locally sent data.
