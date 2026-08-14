# Security policy

## Supported versions

Security fixes are provided for the latest published minor release. Pre-1.0 releases
may change their private provider protocol between minor versions; the public C ABI
follows [the documented compatibility policy](docs/abi-policy.md).

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub's private
vulnerability reporting form under **Security → Advisories → Report a vulnerability**.
Include the affected version, operating system, transport, a minimal reproducer and
the expected impact when available.

The project will acknowledge a report within seven days. Confirmed issues are fixed
on a private branch, tested with the sanitizer and fuzzing gates, then disclosed with
the corresponding release. No bounty program is currently offered.

## Security boundary

Providers are local child processes selected by the user. They are trusted to access
the files and services granted by the operating system, but their protocol output is
still parsed, bounded and validated. MCP clients are untrusted protocol peers. See
[the security model](docs/security-model.md) for the complete boundary.
