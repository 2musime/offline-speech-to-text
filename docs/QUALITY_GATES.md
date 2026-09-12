# Quality gates

Every gate runs locally, in one command:

```bash
tests/run_gates.sh          # everything
tests/run_gates.sh --quick  # skip the sanitizer build
```

It exits non-zero and names what failed.

## Why these do not run on a hosted service

GitHub Actions is not available on this repository's plan. A workflow was
written and then removed: every run failed at startup, zero workflows were
registered, and the only result was a red mark on each pull request that told
nobody anything.

Gates that cannot run are worse than no gates, because they look like coverage.
These run on any machine that can build the project, which is the same machine
doing the work.

## What runs

| Gate | Script | Checks |
|---|---|---|
| Guarantees | `tests/check_guarantees.sh` | nothing private committed; no networking in source or binaries |
| Formatting | `tests/check_style.sh` | tabs, trailing whitespace, line endings, final newline, 120 columns |
| Warnings | build with `-DAUDIO_TO_TEXT_WARNINGS_AS_ERRORS=ON` | `-Wall -Wextra -Wpedantic -Werror` on this project's targets |
| Tests | `ctest` | unit, interface and command-line suites |
| Sanitizers | instrumented build | AddressSanitizer and UndefinedBehaviorSanitizer |

## Enforcing them automatically

A hook refuses a push when the quick gates fail:

```bash
git config core.hooksPath .githooks
```

Bypass once with `git push --no-verify`. This is local to your clone; it is not
enforced for anyone else, which is the honest limit of a hook.

## The guarantees gate

This is the one that protects what the application is for. It verifies:

- no networking header, network call, telemetry string or embedded endpoint in
  this project's sources
- neither built binary imports a network syscall
- neither built binary uses a TLS or HTTP symbol
- a full transcription completes inside a network namespace with no interfaces
- no audio, model weights, transcripts or build output are committed
- nothing over 2 MB is tracked outside `third_party`

Two details that matter when reading its output:

The symbol checks exclude mangled C++ names. Without that, Qt's
`QObject::connect` matches a search for `connect` and the check reports a socket
that does not exist.

The check is at symbol level, not library level. `libcrypto` arrives
transitively through Qt6Core for hashing; it is not a networking library and not
something this project chose. What matters is whether this code calls into TLS
or HTTP, and it must not.

`third_party` is excluded from the committed-content rules, because Whisper
ships its own sample audio and test fixtures. Those are not a user's recording
committed by accident, which is what the rules exist to catch.

## Formatting

`tests/check_style.sh` enforces only what can be checked deterministically. It
deliberately does not run `clang-format`. A `.clang-format` file is provided for
editors, but adopting it as a gate would reformat the whole tree, which belongs
in its own commit. Rules like continuation-line alignment need a real parser; a
half-rule that flags correctly aligned code is worse than no rule.

## If hosted CI becomes available

Two routes, neither required:

- **Make the repository public.** Public repositories get Actions minutes at no
  cost. This project has no secrets: no credentials, no endpoints, and the
  models are downloaded from upstream rather than committed.
- **A paid plan**, if the repository must stay private.

Either way the work is small, because every gate is already a script. A workflow
would install `gcc-c++ cmake make qt6-qtbase-devel` on a Fedora container and
call `tests/run_gates.sh`. Nothing else would need to change.

## Before a release

`tests/run_gates.sh` covers steps 2 and 3 of the release checklist. The clean
installation test in [RELEASE.md](RELEASE.md) is separate and cannot be skipped:
it is the only check that catches a package which builds correctly and does not
run on a machine that never built it.
