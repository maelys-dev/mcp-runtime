# Codex and Claude clients

Codex and Claude are MCP clients. They launch the native `maelys-mcp` host; they do not
connect directly to the private `maelys-provider/2` process protocol.

Build the host and every selected provider first. All executable paths should be
absolute so the same configuration works independently of the current directory.

## Codex

Current Codex clients support local stdio MCP servers and share MCP configuration from
`~/.codex/config.toml`. Register the host through the CLI:

```sh
codex mcp add yavena-hermes -- \
  /absolute/path/to/mcp-runtime/build/bin/maelys-mcp \
  --provider /absolute/path/to/yavena-hermes/apps/hermes-mcp-provider/dist/index.js

codex mcp get yavena-hermes
codex mcp list --json
```

Remove and recreate the entry when its executable paths change:

```sh
codex mcp remove yavena-hermes
```

Official reference: <https://learn.chatgpt.com/docs/extend/mcp>

## Claude Code

Claude Code also accepts a local stdio command. A user-scoped entry is available in
every project on the machine:

```sh
claude mcp add yavena-hermes --scope user -- \
  /absolute/path/to/mcp-runtime/build/bin/maelys-mcp \
  --provider /absolute/path/to/yavena-hermes/apps/hermes-mcp-provider/dist/index.js

claude mcp get yavena-hermes
claude mcp list
```

For a project-scoped configuration, use `--scope project`; Claude writes `.mcp.json`
and asks the user to approve the project server before use.

Official reference: <https://docs.anthropic.com/en/docs/claude-code/mcp>

## Policy

The host enables `read` and `preview` tools by default. Do not add
`--allow-effect apply`, `commit` or `execute` merely to make a client configuration
work. Those capabilities require an explicit, separately reviewed authorization
decision.
