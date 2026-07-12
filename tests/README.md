# ted test suite

End-to-end regression tests. Every scenario drives the **real editor
binary** through a pseudo-terminal — a fake terminal the OS provides, so
ted believes a human is typing — then asserts **byte-exact** results on
the files it saved.

These are development tools only: ted itself stays zero-dependency C.
The tests need `python3` and a POSIX `sh` (present on any Linux/WSL/macOS).

## Run

    make test        # 22 scenarios against the release build (~15 s)
    make test-asan   # same suite on the ASan/UBSan build — the memory audit
    make clean ted   # restore the normal binary after test-asan

Any failure prints the scenario name and the suite exits non-zero.

## How it works

- **`drive.py`** forks ted onto a pty fixed at 30×100 (so screen
  coordinates are deterministic), waits for raw mode, feeds a scripted
  keystroke string in small drained chunks (one big write can deadlock
  the pty), and exits with ted's exit code — or 99 if ted hangs.
  Keys are python escapes: `\x13` = Ctrl-S, `\x1b[B` = arrow-down,
  `\x1b[<0;6;1M` = mouse click at column 6, row 1.
- **`run.sh`** builds fixture files in a `mktemp -d` scratch directory
  (nothing touches the repo), runs each scenario, and compares saved
  files against expected bytes with `cmp`. It also greps every session
  log for sanitizer reports, which is what makes `make test-asan` a
  leak/overflow audit.

## Adding a test

Three lines in `run.sh`: a fixture, a `run` call with the keystroke
script, and an `expect` with the exact bytes the file must contain:

    printf 'ab\n' > mine.txt
    run mine 'X\x13\x11' mine.txt &&
        printf 'Xab\n' | expect mine mine.txt

Prefer byte-exact assertions over "did it exit 0" — exact contents are
what catch indent, undo-grouping, and word-boundary regressions.

One layout caveat: the tree pane sizes itself to its widest entry, which
changes the editor's column count. Fixtures use short names so the pane
stays at its 20-column floor, keeping text at 74 columns — the wrap test's
cursor math depends on that. Keep fixture filenames short.
