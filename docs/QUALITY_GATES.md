# Quality gates

Every gate runs locally, in one command:

```bash
tests/run_gates.sh          # everything
tests/run_gates.sh --quick  # skip the sanitizer build
```

It exits non-zero and names what failed.

## Running them on Windows

Under **Git Bash**, which ships with Git for Windows and is therefore already on
any machine that can clone this repository with its submodule:

```bash
tests/run_gates.sh --quick
```

A PowerShell port was considered and rejected. It would mean two
implementations of every gate, and the two would diverge the first time one was
edited in a hurry -- the same reason the Windows support lives in `main` rather
than on a branch of its own. One set of scripts, with the platform differences
named inside them, is the smaller thing to keep correct.

Run from a **Developer Command Prompt** started with Git Bash, or the binary
checks cannot find `dumpbin` and report themselves as skipped.

`tests/platform.sh` holds everything the scripts need to know about the machine:
the executable suffix, the processor count, whether the generator picks its
configuration at build time, and where the binaries landed. Setting
`AUDIO_TO_TEXT_PLATFORM` pins the answer, which is how the Windows branches get
exercised from a Linux machine.

### What does not run there, and why

| Gate | Windows | Why |
|---|---|---|
| Nothing private is tracked | runs | `git ls-files`, nothing platform-specific |
| No networking in source | runs | text search |
| Binaries import no networking | runs, differently | `dumpbin -imports` reads the PE import table; `nm` and `ldd` read ELF |
| Transcription with no network | **skipped** | Windows has no network namespace |
| Formatting | runs | see `.gitattributes` below |
| Warnings as errors | runs | `/W4 /permissive- /WX` |
| Unit and interface tests | run | |
| Command-line suite | **skipped** | `cli_tests.sh` is not registered by CMake on Windows |
| Sanitizers | **skipped** | MSVC has AddressSanitizer but no UndefinedBehaviorSanitizer |

A skipped check is counted and named. The summary reads

```text
8 check(s), 0 failure(s), 3 skipped
```

and `run_gates.sh` finishes with `All gates that run on windows passed` followed
by the list of what did not, never a bare `All gates passed`. This is the whole
point of the exercise: a gate that quietly does nothing is worse than one that
is absent, because it reads as coverage. The Windows run is a weaker guarantee
than the Linux run, and it says so.

The binary check is not merely ported but arguably stronger on Windows. Every
socket call in every wrapper reaches `ws2_32.dll`, and the import table cannot
be satisfied without it, so naming the library catches more than naming a
symbol would.

### Line endings

`.gitattributes` pins the working tree to LF on every platform. Without it, a
clone on Windows with `core.autocrlf` set checks every source file out with
CRLF, the formatting gate fails on files nobody touched, and the first commit
from that machine rewrites whole files. Batch and PowerShell scripts are the
exception and are checked out with CRLF.

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
| Platform facts | `tests/platform.sh` | sourced by the others; not a gate itself |
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
