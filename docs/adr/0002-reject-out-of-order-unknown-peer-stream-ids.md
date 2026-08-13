---
status: accepted
date: 2026-08-11
---

# Reject out-of-order unknown peer stream IDs

## Context and Problem Statement

kcpmux creates a peer-initiated stream when the first payload for an unknown stream ID arrives. Without a separate open handshake or retained history of every closed stream, the receiver must distinguish a genuinely new stream from delayed traffic that could recreate an old stream. Cross-stream packet reordering makes that distinction ambiguous when an unknown ID arrives below the latest accepted peer stream ID.

## Decision Drivers

- Delayed packets for a closed stream must not recreate that stream.
- Stream creation should not require a new control message or additional handshake round trip.
- Per-connection state for tracking past peer stream IDs should remain bounded and simple.
- The existing serial-number wraparound behavior and initiator/acceptor ID parity must remain unchanged.

## Considered Options

- Accept only unknown peer stream IDs above the current peer stream ID high-water mark.
- Accept older unknown IDs within a finite reordering window and retain tombstones.
- Add an explicit `STREAM_OPEN` control message.

## Decision Outcome

Chosen option: "accept only unknown peer stream IDs above the current peer stream ID high-water mark", because it prevents delayed traffic from recreating old streams while preserving first-payload stream creation and constant-size tracking state.

An existing stream remains valid after the high-water mark advances. The restriction applies only when a payload references an unknown stream ID. Serial-number comparison continues to account for 32-bit wraparound and stream ID parity.

### Positive Consequences

- A connection retains only the latest accepted peer stream ID rather than a history of open and closed IDs.
- Delayed payloads cannot recreate a stream whose ID is at or below the high-water mark.
- No additional stream-open packet, acknowledgment, or protocol state is required.

### Negative Consequences

- If first payloads from different streams arrive out of order, accepting the higher stream ID can permanently prevent creation of the earlier stream on that connection.
- The sender cannot recover the rejected earlier stream merely by retransmitting its first payload; it must use a newer stream ID.
- The protocol favors simple anti-recreation semantics over tolerance for cross-stream first-payload reordering.

## Pros and Cons of the Options

### Peer stream ID high-water mark

- Good: Uses constant state and preserves payload-driven stream creation.
- Good: Rejects delayed traffic for old unknown stream IDs.
- Bad: Can reject a genuinely new stream when its first payload is reordered behind a higher stream ID.

### Finite reordering window with tombstones

- Good: Can tolerate bounded cross-stream reordering while preventing known closed IDs from reopening.
- Bad: Requires window sizing, tombstone expiration rules, and additional per-connection state.
- Bad: Reordering beyond the configured window remains ambiguous.

### Explicit `STREAM_OPEN`

- Good: Makes stream lifecycle intent unambiguous and can support ordered state independent of payload delivery.
- Bad: Adds protocol messages, state transitions, and potentially a handshake round trip before data transfer.
- Bad: Expands compatibility and versioning requirements for peers.
