# MiniShell — Problem #11: Custom Shell

A mini Unix-like command interpreter in C, built for the REVA University **Abhinava** hackathon (7 Oct 2026, Portfolio Building).

## Approach

The shell is a read → parse → execute loop. The parser splits a line into pipeline stages and records redirections. The executor calls `fork()` once per stage, wires stdin/stdout with `pipe()` and `dup2()`, and replaces each child with the program using `execvp()`. The parent uses `waitpid()` to wait for foreground jobs and to reap background ones.

## Tools

C (GCC), Linux/WSL, POSIX system calls (`fork`, `execvp`, `waitpid`, `pipe`, `dup2`, `open`, `chdir`, `signal`), Bash for the test script.

## Features

| Feature | Support |
|---|---|
| Builtins | `cd`, `pwd`, `help`, `exit` |
| External commands | `fork()` + `execvp()` + `waitpid()` |
| Redirection | `cmd > file`, `cmd >> file`, `cmd < file` |
| Pipes | `cmd1 \| cmd2 \| cmd3` (up to 16 stages) |
| Background | `cmd &` (jobs reaped, no zombies) |
| Quotes | `echo "hello world"` (operators inside quotes stay literal) |
| Signals | Ctrl+C never kills the shell; children reset to default |
| Error handling | Graceful syntax errors — no crashes on bad input |

## Build & run

```sh
gcc -Wall -Wextra -o minishell minishell.c
./minishell
```

## Run the tests

```sh
bash tests/run_tests.sh
```

## Demo walkthrough (for evaluation)

```
help
pwd
echo Hello MiniShell
ls -l | grep .c
echo hello | tr a-z A-Z
echo "b" > unsorted.txt
sort < unsorted.txt
echo test > out.txt
echo more >> out.txt
cat out.txt
echo "hello world"
echo "a|b"
sleep 3 &
echo hi |
notacmd
exit
```

## Known limits

- `&` works only at the end of a line (`sleep 1 & echo hi` passes `&` as an argument).
- Ctrl+C also stops background jobs (they share the terminal's process group; fixing needs `setpgid()`).
- Only double quotes are supported — no `\"` escapes, no `$VAR` expansion.
- A quoted token that is exactly `"|"` is still treated as a pipe operator.

## Design notes

- Builtins run in the parent process so `cd` persists; they are rejected in pipelines.
- Builtins honor I/O redirection (`pwd > out.txt` works) via save/restore of stdout.
- `>>` append is tokenized as a single operator (not two `>`).
- Tokens point into a `static` parse buffer — no dangling pointers after parse returns.
- A `syntax error: missing command after '|'` guard rejects trailing pipes.
- Background jobs are reaped with `waitpid(..., WNOHANG)` each prompt, so no zombies.
- The shell ignores `SIGINT`; each child resets it to default before `execvp`.
