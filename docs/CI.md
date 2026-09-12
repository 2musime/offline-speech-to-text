# Continuous integration

The workflow is `.github/workflows/ci.yml`. It exists to protect the two things
this application promises: that transcription happens locally, and that nothing
captured ever leaves the machine or the working directory.

## Running on a free account

No paid plan is needed.

- A **private** repository on the Free plan includes 2,000 Actions minutes per
  month. Linux runners bill at 1x, so a minute of runtime is a minute of budget.
- A **public** repository has unlimited minutes.
- GitHub hosts **no Fedora runner on any plan**. The Fedora jobs run the
  `fedora:41` container on the free `ubuntu-latest` runner, which is the normal
  way to build for a distribution GitHub does not host. It costs nothing extra.

Written to stay inside that budget:

- the compiler cache is restored between runs, so the vendored Whisper build is
  paid for once rather than on every push
- `concurrency` cancels a superseded run when a branch is pushed again
- the cheap job runs without a compiler and fails fast
- **no model is ever downloaded.** The models are hundreds of megabytes and the
  tests do not need them

A cold first run pays for the full Whisper build in each compiled job. Later
runs reuse the cache. Treat any minute figure here as an estimate: measure the
first few runs in the repository's Actions tab rather than trusting a guess.

## Jobs

| Job | Runner | Purpose |
|---|---|---|
| `guarantees` | `ubuntu-latest` | nothing private is tracked; formatting |
| `build-and-test` | `fedora:41` container | warnings as errors, full test suite, no-network proof |
| `sanitizers` | `fedora:41` container | AddressSanitizer and UndefinedBehaviorSanitizer |

`guarantees` needs no compiler, so the mistakes that matter most, a recording or
a model committed by accident, are caught in seconds.

## What the gates actually check

### The application stays offline

`tests/check_guarantees.sh` is the mission gate, and it runs locally too:

- no networking header is included in this project's sources
- no network call appears in them
- no telemetry string or embedded endpoint
- neither built binary imports a network syscall
- neither built binary uses a TLS or HTTP symbol
- a full transcription completes inside a network namespace with no interfaces

The symbol checks exclude mangled C++ names. Without that, Qt's
`QObject::connect` matches a search for `connect` and the check reports a
socket that does not exist.

The check is at symbol level rather than library level on purpose. `libcrypto`
arrives transitively through Qt6Core for hashing; it is not a networking library
and not something this project chose. What matters is whether this code calls
into TLS or HTTP, and it must not.

### Nothing private is committed

- no `.wav`, `.mp3`, `.flac`, `.ogg` or `.m4a`
- no model weights outside `third_party/`
- no transcript
- no build output or CMake cache
- nothing over 2 MB outside `third_party/`, which is almost always a model or a
  recording that slipped in

### The code compiles clean

`-Wall -Wextra -Wpedantic -Werror`, applied to this project's targets only.
Vendored Whisper produces warnings that are not this project's to fix, so it is
deliberately not held to the same bar.

Enable it locally with `-DAUDIO_TO_TEXT_WARNINGS_AS_ERRORS=ON`.

### Tests and sanitizers

`ctest` runs all three suites; see [TESTING.md](TESTING.md). The CLI suite skips
itself when no model is present, which is the case on a runner.

The sanitizer job runs the unit and interface suites under ASan and UBSan with
`abort_on_error=1`, so a report fails the job instead of being buried in the log.
Leak detection is on for the unit suite and off for the interface suite, because
Qt's own allocations at exit are not this project's leaks.

## Formatting

`tests/check_style.sh` enforces what can be checked deterministically: spaces
not tabs, no trailing whitespace, LF endings, a final newline, and a 120 column
limit.

It deliberately does **not** run `clang-format`. A `.clang-format` file is
provided for editors, but adopting it as a gate would reformat the whole tree,
and that belongs in its own commit rather than buried inside a CI change. Rules
like continuation-line alignment need a real parser; a half-rule that flags
correctly aligned code is worse than no rule.

## Protecting main

`main` is the production branch. Every change reaches it through a pull request,
and the workflow runs on every pull request targeting it.

If repository rulesets or branch protection are available for private
repositories on your plan, require `guarantees`, `build-and-test` and
`sanitizers` to pass before merging into `main`. If they are not, the checks
still run on every pull request and the result is visible before you merge;
the enforcement is then yours rather than the platform's.

## Running the gates locally

Everything CI does can be run before pushing:

```bash
./tests/check_guarantees.sh
./tests/check_style.sh

cmake -S . -B build -DAUDIO_TO_TEXT_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## The repository was not clonable

Adding CI exposed a pre-existing fault. `third_party/whisper.cpp` was recorded
as a submodule gitlink, but no `.gitmodules` existed, so git knew which commit
was wanted and not where to fetch it. A fresh clone produced an empty directory
and `git submodule update --init` failed with:

```text
fatal: No url found for submodule path 'third_party/whisper.cpp' in .gitmodules
```

CMake then failed at `add_subdirectory`. This affected anyone cloning the
repository, not only CI. `.gitmodules` now registers the upstream URL, and a
fresh clone checks out the pinned commit `1da4dc8`.

Clone with:

```bash
git clone --recurse-submodules git@github.com:2musime/offline-speech-to-text.git
```

or, in an existing checkout:

```bash
git submodule update --init --recursive
```
