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
