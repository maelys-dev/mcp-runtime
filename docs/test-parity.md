# Test parity with codexmanager

The extraction uses the generic protocol tests from `codexmanager` as a behavioral
inventory, not as a requirement to copy application-specific fixtures. The source
inventory contains 91 C test functions and 191 named cases across JSON-RPC, MCP,
stdio, CLI, and Maelys L0/L1 suites.

## Ported generic behavior

| Source area | Runtime coverage |
|---|---|
| JSON Lines framing | fragmentation, CRLF, multiple messages, empty/invalid input, duplicate keys, exact/oversized limits, partial EOF |
| Content-Length framing | byte fragmentation, multiple frames, case-insensitive header, extra headers, missing/empty/duplicate/non-numeric/negative/overflow lengths, header/body limits, partial EOF |
| JSON-RPC envelopes | integer and string ids, invalid id types, invalid version/method, notifications, error ownership and callback propagation |
| MCP lifecycle | modern metadata, unsupported versions, legacy initialization, duplicate initialization, initialized notification, pre-initialization rejection |
| MCP tools | list/call, unknown tools, input validation, output validation, effects, policy denial, audit, provider failure |
| MCP modules | empty core, explicit/idempotent activation, capability derivation, method isolation, Tools-before-provider invariant |
| Rich results | text, image, audio, embedded resources, mixed content, base64/MIME validation and structured output |
| MRTR | input-required/complete rounds, retry context, opaque state validation, capability enforcement and official elicitation scenario |
| Provider registry | mandatory effects, duplicate names within and across providers, capacity, exact schema retention |
| Process providers | absolute paths, direct execution, minimal environment, describe/call/shutdown, premature exit, bounded messages and timeout plumbing |
| stdio and CLI | end-to-end requests, parse errors, stdout isolation, `FD_CLOEXEC`, simulated third-party `printf()` contamination |
| Memory safety | native ASan/UBSan and pinned Linux Docker ASan/UBSan/LSan |
| Parser robustness | libFuzzer targets and seed corpora for JSON Lines, Content-Length, schema definition/value pairs, and rich content blocks |
| Polyglot providers | version 2 black-box lifecycle and explicit complete/input-required results for C, TypeScript and Python, including structured failures and stdout contamination |

The standalone runtime adds checks that the original integrated server did not make
at this boundary: unsupported JSON Schema keywords are rejected at registration,
input schemas must describe an object, duplicate `required` entries are rejected,
and invalid provider output is never returned as `structuredContent`.

## Deliberately not ported

The following cases stay in `codexmanager` or future providers because they exercise
business behavior rather than the generic MCP boundary:

- Maelys L0/L1 agent, workflow, prompt, session, and approval operations;
- HTTP endpoint and agent-process orchestration specific to codexmanager;
- registry fixtures whose tool names and schemas belong to codexmanager;
- the old runtime adapter's tolerance for arbitrary text before the first JSON frame.

The last exclusion is intentional: the standalone host prevents stdout contamination
at its source and the framing core rejects non-protocol input. Silently skipping noise
would hide a broken transport boundary.

## Release gate

Before a release, run:

```sh
make clean
make check
make check-all
make analyze
make test-asan-linux
```

`test-asan-linux` builds from a digest-pinned Ubuntu 24.04 image, runs the complete
suite under ASan/UBSan with leak detection enabled, and executes 2,000 smoke mutations
for each fuzzer. Longer campaigns can invoke the binaries under `build/fuzz/` with a
persistent corpus and time budget.
