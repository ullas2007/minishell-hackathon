#!/bin/bash
# MiniShell automated test script.
# Run from anywhere:  ./tests/run_tests.sh   (repo layout)
#                  or ./run_tests.sh         (flat folder)
# If created on Windows, fix line endings first:  dos2unix run_tests.sh
# Each test feeds commands to ./minishell and checks REAL output,
# never incidental text like the banner.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -x "$SCRIPT_DIR/minishell" ]; then
    SHELL_BIN="$SCRIPT_DIR/minishell"
elif [ -x "$SCRIPT_DIR/../minishell" ]; then
    SHELL_BIN="$SCRIPT_DIR/../minishell"
else
    echo "build first: gcc -Wall -Wextra -o minishell minishell.c"
    exit 1
fi

PASS=0; FAIL=0
TDIR=$(mktemp -d /tmp/mstest.XXXXXX); trap 'rm -rf "$TDIR"' EXIT
cd "$TDIR" || exit 1

ok()   { echo "PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

# run: feed $2 to the shell, pass if $3 matches output
run() {
    out=$(printf '%b' "$2" | "$SHELL_BIN" 2>&1)
    if echo "$out" | grep -q "$3"; then ok "$1"; else bad "$1"; fi
}

run "builtin pwd"               'pwd\nexit\n'                          '\$ /tmp/'
run "external command"          'echo hello\nexit\n'                   '\$ hello$'
run "output redirect >"         'echo one > t1.txt\ncat t1.txt\nexit\n' '\$ one$'
run "input redirect <"         'echo data > t1.txt\ncat < t1.txt\nexit\n' '\$ data$'
run "pipe"                      'echo hello | tr a-z A-Z\nexit\n'      '\$ HELLO$'
run "multi-stage pipe"          'echo x | cat | tr a-z A-Z\nexit\n'     '\$ X$'
run "double quotes"             'echo "hello world"\nexit\n'           '\$ hello world$'
run "quoted pipe stays literal" 'echo "a|b"\nexit\n'                 '\$ a|b$'
run "quoted append stays literal" 'echo "x>>y"\nexit\n'             '\$ x>>y$'
run "quote keeps spacing"       'echo "a  b"\nexit\n'                '\$ a  b$'
run "builtin redirect"          'pwd > t1.txt\ncat t1.txt\nexit\n'    '\$ /tmp/'
run "background job"            'sleep 1 &\nexit\n'                   'started 1 job'
run "trailing pipe = syntax error" 'echo hi |\nexit\n'              "missing command after '|'"
run "unknown command"           'nosuchcmd_xyz\nexit\n'              'command not found'
run "cd persists"               'cd /tmp\npwd\nexit\n'               '\$ /tmp$'
run "missing input file"        'cat < nofile_xyz\nexit\n'           'No such file or directory'
run "empty line does not crash" '\n\nexit\n'                         'bye!'

# append: verify FILE CONTENTS, not terminal text (bye! contains 'b')
printf 'echo a > t2.txt\necho b >> t2.txt\nexit\n' | "$SHELL_BIN" >/dev/null 2>&1
if [ "$(cat t2.txt)" = "$(printf 'a\nb')" ]; then ok "append redirect >> (file contents)"; else bad "append redirect >> (file contents)"; fi

# pipe count: grep -c prints just the number after the prompt
printf 'echo x > minishell_probe.txt\nexit\n' | "$SHELL_BIN" >/dev/null 2>&1
out=$(printf 'ls | grep -c minishell\nexit\n' | "$SHELL_BIN" 2>&1)
if echo "$out" | grep -q '\$ 1$'; then ok "pipe + count"; else bad "pipe + count"; fi

echo "----------------------"
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
