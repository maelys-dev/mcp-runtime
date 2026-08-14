# Asynchronous outbox

`maelys_mcp_outbox_t` is the transport-independent output boundary. Any worker, timer,
or module can enqueue a complete JSON-RPC message; exactly one writer thread invokes
the configured callback.

## Ownership and locking

- Enqueue success steals one Jansson reference; failure leaves it with the caller.
- The writer decrements every stolen reference after its callback, including errors.
- The queue mutex protects only links, counters and state.
- No callback, serialization write, or other blocking I/O runs while the mutex is held.
- Shutdown stops producers first, then either drains or discards pending messages.

## Capacity and scheduling

Both the number of messages and their estimated compact serialized bytes are bounded.
Producers wait on a condition variable when capacity is exhausted and wake when the
writer removes a batch. Responses and notifications use separate FIFO queues. The
default response burst is eight; a waiting notification is then forced before response
processing resumes.

A notification may provide a coalescence key such as `resource:<uri>`. A replacement
keeps only the newest JSON message and moves its node to the queue tail. Thus the event
order `A, B, A` is observed as `B, A`, which reflects the newest invalidation order.
Responses are never coalesced.

The callback must obey its transport's own shutdown contract. In particular, it must
not call back into the same outbox and should use bounded or cancellable I/O when the
underlying transport can stall indefinitely.
