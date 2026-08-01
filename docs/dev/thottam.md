# Using Kaappi thottam with Chaaya

Chaaya does **not** implement a package manager in C. For ecosystem packaging,
use Kaappi's existing **thottam** implementation from the sibling repo
`../kaappi/`.

## Why this exists

- Thottam already handles `kaappi.pkg`, dependency resolution, lockfiles, and
  native `build:` hooks.
- Chaaya only needs installed library trees and `--lib-path` wiring.
- Re-implementing thottam in Chaaya is out of scope.

## Basic workflow

1. Build thottam from the sibling repo:

```bash
cd ../kaappi
zig build
```

2. Install libraries with Kaappi/thottam (from that repo or your package root).
3. Run Chaaya with the installed lib directory:

```bash
cd ../chaaya
./build/chaaya --lib-path /path/to/installed/lib your-script.scm
```

## Smoke helper

Use [`scripts/thottam-smoke.sh`](../../scripts/thottam-smoke.sh) to check that:

- a Kaappi thottam executable is discoverable (or `kaappi thottam` is callable),
- a Chaaya binary exists,
- and the wiring prerequisites are present.

The smoke script exits successfully when prerequisites are missing by printing a
`SKIP:` reason (so CI/local runs can stay non-flaky).
