#!/bin/sh
# End-to-end regression suite for ted. Every scenario drives the real
# binary through a pty (tests/drive.py) and asserts byte-exact results.
# Run via `make test` (release build) or `make test-asan` (sanitizers).
set -u

cd "$(dirname "$0")/.." || exit 1
TED="$PWD/ted"
DRIVE="$PWD/tests/drive.py"
[ -x "$TED" ] || { echo "no ./ted binary — run make first"; exit 1; }

TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
LOGS="$TMP/logs"
FX="$TMP/fx"    # fixtures live apart from logs: short names keep the
mkdir -p "$LOGS" "$FX"  # tree pane at its 20-col floor (textcols = 74)
cd "$FX" || exit 1

pass=0; fail=0
ok()  { pass=$((pass + 1)); }
bad() { echo "FAIL: $1"; fail=$((fail + 1)); }

# run <name> <keys> [file args...] — drive ted; count a failure on bad exit
run() {
    _name=$1; _keys=$2; shift 2
    if ! python3 "$DRIVE" "$_keys" "$TED" "$@" > "$LOGS/$_name.log" 2>&1; then
        bad "$_name (editor exit code)"
        return 1
    fi
}

# check <name> <file> <expected> — byte-exact compare (\n escapes allowed).
# The printf|cmp pipe stays inside the function: a pipeline element runs in
# a subshell, so `printf | check` at a call site would lose the counters.
check() {
    if printf '%b' "$3" | cmp -s - "$2" 2>/dev/null; then ok
    else bad "$1 (content of $2)"; fi
}

# --- fixtures ---------------------------------------------------------------
printf 'hello world\n'                > c1.txt
printf 'abc\ndef\n'                   > c2.txt
printf 'one\ntwo\nthree\n'            > c3.txt
printf 'aa\nbb\ncc\n'                 > c4.txt
printf 'aaa\nbbb\nccc\n'              > c5.txt
printf 'hello\nworld\n'               > c6.txt
printf 'cat cat cat\ncat\n'           > p1.txt
printf 'foo bar_baz qux\nnext line\n' > words.txt
printf 'alpha\nbeta gamma\n'          > find.txt
printf 'x\n' > a.txt; printf 'y\n'    > b.txt
mkdir -p treedir projdir
printf '1\n' > treedir/aaa.txt; printf '2\n' > treedir/bbb.txt
printf 'one\ntwo\nthree\n'            > m1.txt
printf 'aaa\nbbb\nccc\n'              > m2.txt
printf 'one\ntwo\nthree\n'            > m3.txt
i=1; while [ $i -le 100 ]; do echo "line$i"; i=$((i + 1)); done > g1.txt
python3 -c "print('A'*74 + 'B'*74 + 'C'*52)" > long.txt

# --- scenarios ---------------------------------------------------------------
run plain '\x11' && ok                                    # Ctrl-Q quits
run quit-x '\x18' && ok                                   # Ctrl-X quits

run tree-open '\x1b[B\r\x11' treedir &&                   # Enter opens file
    { grep -aq '1/1' "$LOGS/tree-open.log" && ok || bad tree-open; }

run edit-save 'hi\r    indent\rx\x09y\x13\x11' new.txt && # autoindent+softtab
    check edit-save new.txt 'hi\n    indent\n    x   y\n'

run find '\x06beta\rZ\x13\x11' find.txt &&                # find lands cursor
    check find find.txt 'alpha\nZbeta gamma\n'

run buffers '\x0e\x0e\x10\x17\x11' a.txt b.txt && ok      # cycle + close

run paste '\x1b[200~if (x) {\r    y;\r}\x1b[201~\x13\x11' paste.txt &&
    check paste paste.txt 'if (x) {\n    y;\n}\n'  # verbatim paste

run undo 'hello world\x1a\x13\x11' u1.txt &&              # one-step undo
    check undo u1.txt ''

run words '\x1b[3;5~\x1b[1;5C\x08\x1b[F\x1b[3;5~\x1a\x13\x11' words.txt &&
    check words words.txt '  qux\nnext line\n'  # word ops + undo

run newfile 'ncreated.c\rint x;\x13\x11' projdir &&       # tree 'n' creates
    check newfile projdir/created.c 'int x;\n'

run newdir 'Nsub2\r\x11' projdir &&                       # tree 'N' mkdirs
    { [ -d projdir/sub2 ] && ok || bad newdir; }

run sel-replace '\x1b[1;6Cbye\x13\x11' c1.txt &&          # select word, type
    check sel-replace c1.txt 'bye world\n'

run copy-paste '\x0c\x03\x1b[B\x1b[F\x16\x13\x11' c2.txt && # Ctrl-L/C/V
    check copy-paste c2.txt 'abc\ndef\nabc\n\n'

run cut-undo '\x0b\x13\x1a\x13\x11' c3.txt &&             # cut line, undo
    check cut-undo c3.txt 'one\ntwo\nthree\n'

run selall-undo '\x01\x1b[3~\x13\x1a\x13\x11' c4.txt &&   # select all + del
    check selall-undo c4.txt 'aa\nbb\ncc\n'

run shift-sel '\x1b[1;2B\x1b[1;2B\x1b[1;2C\x7f\x13\x11' c5.txt &&
    check shift-sel c5.txt 'cc\n'               # shift selection

run drag-cut '\x1b[<0;6;1M\x1b[<32;9;2M\x1b[<0;9;2m\x0b\x13\x11' c6.txt &&
    check drag-cut c6.txt 'ld\n'                # mouse drag select

run undo-redo 'one two\r\x1b[200~pp\x1b[201~\x1a\x1a\x1a\x19\x19\x19\x13\x11' r3.txt &&
    check undo-redo r3.txt 'one two\npp\n'      # 3 groups roundtrip

run redo-clean 'a\x13\x1a\x19\x11' r4.txt &&              # dirty flag exact:
    check redo-clean r4.txt 'a\n'               # single Ctrl-Q exits

run goto '\x0750\rX\x13\x11' g1.txt &&                    # Ctrl-G line 50
    { [ "$(sed -n 50p g1.txt)" = "Xline50" ] && ok || bad goto; }

run replace '\x12cat\rdog\ryna\x13\x11' p1.txt &&         # y/n/a answers
    check replace p1.txt 'dog cat dog\ndog\n'

run move '\x1b[1;3B\x1b[1;3B\x1b[1;3A\x13\x11' m1.txt &&  # Alt+Down x2, Up x1
    check move m1.txt 'two\none\nthree\n'

run move-sel '\x1b[1;2B\x1b[1;2B\x1b[1;3B\x1b[1;3B\x13\x11' m2.txt &&
    check move-sel m2.txt 'ccc\naaa\nbbb\n'    # selected block: 2nd is no-op

run move-undo '\x1b[1;3B\x1a\x13\x11' m3.txt &&           # one-step undo
    check move-undo m3.txt 'one\ntwo\nthree\n'

run wrap '\x1b[F\x1b[AX\x13\x18' long.txt &&              # visual arrow moves
    { python3 -c "import sys; s = open('long.txt').read(); sys.exit(0 if s.index('X') == 126 else 1)" \
        && ok || bad "wrap (X not at index 126)"; }       # one screen width

# --- sanitizer findings (asan builds abort; this catches leak reports) -------
if grep -al -E 'AddressSanitizer|LeakSanitizer|runtime error' "$LOGS"/*.log \
        > /dev/null 2>&1; then
    bad "sanitizer findings in logs"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
