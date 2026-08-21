# Passive outbox

`maelys_mcp_outbox_t` is a transport-independent passive bounded queue. Producers
enqueue complete JSON-RPC messages; a transport extracts them with
`maelys_mcp_outbox_next`. Creating an outbox never creates a thread and the outbox
never invokes a callback or performs I/O.

## Ownership and waits

- Enqueue success steals one Jansson reference; failure leaves it with the caller.
- `next` transfers one owned reference to the consumer.
- Admission and extraction use monotonic deadlines. A full queue returns
  `MAELYS_MCP_ERR_TIMEOUT`; an empty closed queue returns
  `MAELYS_MCP_ERR_CLOSED`.
- Closing stops admission and wakes all producers and consumers. A non-discarding
  close preserves admitted messages for the transport to drain.
- `maelys_mcp_outbox_wait_drained` is bounded. Destruction is refused until the queue
  is both closed and empty, preventing a successful API result from being silently
  discarded.
- Oversized messages, allocation failures, closed admission, and admission timeouts
  increment the observable `rejected` statistic; no rejected message is reported as
  delivered.
- The queue mutex protects links, counters and state only. No serialization, provider
  callback, transport I/O or thread join runs under it.

## The readiness descriptor

A thread inside `maelys_mcp_outbox_next` is waiting on a condition variable, and a
thread waiting on a condition variable is not watching a socket. A transport whose
connection can disappear while a provider is still running therefore needs a
descriptor it can poll beside its own, which is what
`maelys_mcp_channel_enable_wait_fd` and `maelys_mcp_channel_wait_fd`
(`src/internal/internal.h`, internal for now) provide.

- **Lazy.** No pipe exists until a transport asks for one, so a stdio channel
  allocates no extra descriptors and pays no extra write. Enabling is idempotent, and
  the descriptor a caller already holds stays the one it gets.
- **Level-triggered, one byte per transition.** The descriptor is readable exactly
  when `next` would answer immediately — a queued message, or a closed outbox whose
  `MAELYS_MCP_ERR_CLOSED` has still to be collected. The byte is written on the
  transition into that state and consumed on the transition out of it, under the same
  mutex that guards the queue, so a burst of enqueues costs one `write(2)` rather than
  one per message and the descriptor can never disagree with `queued_messages`.
- **Poll it, never read it.** The only correct response to readability is
  `maelys_mcp_channel_next`. Readability can be spurious, which `next` answers with
  `MAELYS_MCP_ERR_TIMEOUT`; it is never spuriously absent, which is the failure that
  would wedge a poller.
- **Failure is the caller's to report.** If the pipe cannot be created the transport
  refuses the request. Falling back to short timed waits would reintroduce the polling
  design this descriptor exists to avoid, and would do it invisibly.

## Capacity and scheduling

Message count and compact serialized bytes are both bounded. Responses and
notifications use separate FIFO queues. Responses are preferred, but after the
configured response burst a waiting notification is selected to prevent starvation.

A notification may provide a coalescence key such as a subscription id plus resource
URI. Replacing `A` in the sequence `A, B, A` moves the newest `A` to the tail, so the
observable order becomes `B, A`. Responses are never coalesced.

Subscription acknowledgements are wire-level notifications but use the response lane.
This preserves the required acknowledgement-before-event causal boundary. Ordinary
resource and catalog notifications use keyed coalescence local to one channel.

## Transport responsibility

The transport owns output pumping and its execution model. Stdio uses one writer
thread for its single durable channel and gives each descriptor write a monotonic
deadline. A future transport may use an event loop or shared writer pool. In every
case, transport shutdown must close the channel to wake a consumer waiting on an empty
queue before joining or releasing that consumer.
