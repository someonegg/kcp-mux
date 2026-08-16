---
status: accepted
date: 2026-08-15
---

# Use vectored handoffs at send-path ownership boundaries

## Context and Problem Statement

Applications commonly hold a small application header and a large payload in separate memory regions. The contiguous stream API required callers to join them before KCP copied the data into retransmittable segments. On output, kcp-mux joined its protocol header and the contiguous KCP datagram before handing the packet to the transport. The send path should remove these two intermediate payload copies without attempting end-to-end zero-copy through KCP retransmission state or transport ownership.

## Decision Drivers

- Application payload bytes should be copied directly into KCP-owned send segments by the bundled KCP implementation.
- A KCP output datagram should pass directly to the final transport operation without an intermediate kcp-mux envelope copy.
- KCP must retain ownership of retransmittable data and may keep its contiguous output datagram.
- Vectored send must preserve the existing MSS-based message and receive behavior.
- Custom KCP implementations that only provide contiguous send must remain usable.

## Considered Options

- Join vectors into contiguous buffers inside kcp-mux and retain the existing KCP and transport interfaces.
- Expose vectors only at application-to-KCP and kcp-mux-to-transport ownership boundaries.
- Extend vectored output through KCP itself by exposing encoded KCP headers and segment payloads separately.

## Decision Outcome

Chosen option: "expose vectors only at application-to-KCP and kcp-mux-to-transport ownership boundaries", because these are the two avoidable copies while KCP segment ownership, ACK/retransmission behavior, and contiguous datagram assembly remain appropriate.

The bundled KCP implementation accepts a flattened iovec range and gather-copies it directly into final send segments. kcp-mux advances a cursor across the iovec and submits independent MSS-sized KCP messages, preserving existing receive boundaries. A custom KCP adapter may omit `sendv`; kcp-mux then gather-copies each MSS-sized message into a stack scratch buffer before invoking its scalar `send`.

KCP output remains a contiguous datagram. kcp-mux synchronously passes an 8-byte protocol header and the KCP datagram to the required transport `write_socketv` callback. Control packets use the same callback with one fragment.

### Positive Consequences

- The bundled path removes the application-side header/payload join before KCP ownership.
- Stream payload output removes the intermediate kcp-mux envelope buffer and full KCP datagram copy.
- Transports can use scatter/gather I/O or perform one direct copy into their final owned buffer.
- Wire format, KCP retransmission ownership, and stream receive behavior do not change.

### Negative Consequences

- Replacing the scalar transport callback with required `write_socketv` is a source and ABI break for engine integrations.
- The KCP abstraction gains an optional range-oriented vectored operation that custom adapters must implement to receive the input-side copy reduction.
- Custom adapters without `sendv` still incur an extra MSS-sized fallback copy.
- Callback users must honor synchronous fragment lifetimes and copy before returning when transport work is asynchronous.

## Pros and Cons of the Options

### Join vectors inside kcp-mux

- Good: Keeps all existing extension interfaces unchanged.
- Bad: Moves the application's temporary copy into the library and preserves both targeted payload copies.

### Vectored ownership-boundary handoffs

- Good: Removes both targeted copies without changing KCP reliability or wire behavior.
- Good: Supports native scatter/gather and final-buffer-copy transports.
- Bad: Requires a new stream API, a KCP extension, and a breaking transport callback change.

### Vectored output through KCP internals

- Good: Could also avoid copying KCP segment data into KCP's output buffer.
- Bad: Expands vector counts and couples transports to ACK aggregation, MTU packing, KCP encoding, and retransmission internals beyond the required optimization.
