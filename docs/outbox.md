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
writer removes a batch or reports a terminal failure. Responses and notifications use
separate FIFO queues. The default response burst is eight; a waiting notification is
then forced before response processing resumes.

A notification may provide a coalescence key such as `resource:<uri>`. A replacement
keeps only the newest JSON message and moves its node to the queue tail. Thus the event
order `A, B, A` is observed as `B, A`, which reflects the newest invalidation order.
Responses are never coalesced.

Subscription acknowledgements are wire-level JSON-RPC notifications, but are queued
in the response-priority lane. MCP requires acknowledgement to be the first message of
the listen stream; this classification prevents a later ordinary or graceful-close
response from overtaking it. Resource and list-change notifications use keyed
coalescence keys that include both the subscription id and event subject.

The callback must obey its transport's own shutdown contract and must not call back
into the same outbox. The built-in stdio transport gives each complete message a
monotonic write deadline (five seconds by default), reports a terminal I/O error to the
outbox, and wakes the service loop so stdin cannot keep the failed runtime alive. Use
`maelys_mcp_runtime_serve_stdio_with_options` or `--stdio-write-timeout-ms` to choose a
different deadline. Custom callbacks must provide equivalent bounded or cancellable
I/O when their transport can stall indefinitely.
