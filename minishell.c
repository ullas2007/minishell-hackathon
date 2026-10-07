/*
 * MiniShell — a tiny Unix-like command interpreter in C
 *
 * Features:
 *   - Builtins: cd, pwd, help, exit (with I/O redirection support)
 *   - External commands via fork() + execvp() + waitpid()
 *   - I/O redirection:  cmd > file     (write/truncate)
 *                       cmd >> file    (append)
 *                       cmd < file     (read stdin from file)
 *   - Pipes:            cmd1 | cmd2 | cmd3
 *   - Background:       cmd &
 *   - Double-quote support:  echo "hello world"
 *   - Ctrl+C never kills the shell itself
 *
 * Compile:  gcc -o minishell minishell.c -Wall -Wextra
 * Run:      ./minishell
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_LINE 2048
#define MAX_ARGS 128
#define MAX_CMDS 16

typedef struct {
    char *argv[MAX_ARGS];  /* NULL-terminated argument list */
    int   argc;
    char *infile;          /* file after <            */
    char *outfile;         /* file after > or >>      */
    int   append;          /* 1 if >>, 0 if >         */
} command_t;

/* ---------------- builtins (run in the parent process) ---------------- */

static int is_builtin(const char *cmd)
{
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "pwd") == 0 ||
           strcmp(cmd, "exit") == 0 || strcmp(cmd, "help") == 0;
}

static void run_builtin(command_t *c)
{
    if (strcmp(c->argv[0], "cd") == 0) {
        const char *dir = c->argv[1] ? c->argv[1] : getenv("HOME");
        if (!dir) dir = "/";
        if (chdir(dir) != 0)
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
    } else if (strcmp(c->argv[0], "pwd") == 0) {
        char buf[1024];
        if (getcwd(buf, sizeof buf))
            printf("%s\n", buf);
        else
            perror("pwd");
    } else if (strcmp(c->argv[0], "help") == 0) {
        printf("MiniShell — builtins: cd, pwd, help, exit\n");
        printf("Operators:  |  >  >>  <  &\n");
        printf("Examples:   ls -l | grep .c\n");
        printf("            echo hello > out.txt\n");
        printf("            echo \"hello world\"\n");
        printf("            sort < unsorted.txt\n");
        printf("            sleep 5 &\n");
    } else if (strcmp(c->argv[0], "exit") == 0) {
        printf("bye!\n");
        exit(0);
    }
}

/* ---------------- parsing ---------------- */

/*
 * Put spaces around | < > & so they become separate tokens.
 * '>>' is kept as one token (checked before the single-char cases).
 * Text inside double quotes is copied untouched, so "a|b" stays one word.
 */
static void space_operators(const char *in, char *out)
{
    int inq = 0;
    while (*in) {
        if (*in == '"') {
            inq = !inq;
            *out++ = *in++;
        } else if (inq) {
            *out++ = *in++;                    /* copy quoted text untouched */
        } else if (in[0] == '>' && in[1] == '>') {
            memcpy(out, " >> ", 4);
            out += 4;
            in += 2;
        } else if (*in == '|' || *in == '<' || *in == '>' || *in == '&') {
            *out++ = ' ';
            *out++ = *in++;
            *out++ = ' ';
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

/*
 * Split spaced[] into tokens, honoring double quotes:
 *   echo "hello world"  ->  argv = {"echo", "hello world"}
 * Operators inside quotes (e.g. "a|b") stay literal text.
 */
static int tokenize(char *s, char **toks, int max)
{
    int n = 0;
    char *p = s;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            p++;                    /* skip opening quote */
            toks[n++] = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
                *p++ = '\0';        /* terminate, skip closing quote */
            /* unterminated quote: rest of line becomes the token */
        } else {
            toks[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n')
                p++;
            if (*p)
                *p++ = '\0';
        }
    }
    return n;
}

/*
 * Parse one input line.
 * Returns number of commands (pipeline stages), -1 on syntax error.
 * Sets *bg_out = 1 if the line ends with &.
 *
 * NOTE: tokens point into a static buffer, so they stay valid
 * after this function returns (no dangling pointers).
 */
static int parse_line(char *line, command_t *cmds, int *bg_out)
{
    static char spaced[MAX_LINE * 3 + 64];
    char *toks[MAX_ARGS * 2];
    int ntok, t, i;

    for (i = 0; i < MAX_CMDS; i++) {
        cmds[i].argc = 0;
        cmds[i].infile = cmds[i].outfile = NULL;
        cmds[i].append = 0;
        cmds[i].argv[0] = NULL;
    }

    space_operators(line, spaced);

    /* background? (must be the last token) */
    *bg_out = 0;
    {
        char *end = spaced + strlen(spaced);
        while (end > spaced && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\n'))
            end--;
        if (end > spaced && *(end - 1) == '&') {
            *bg_out = 1;
            *(end - 1) = '\0';
        }
    }

    ntok = tokenize(spaced, toks, (int)(sizeof toks / sizeof toks[0]));

    i = 0; /* current command index */
    for (t = 0; t < ntok; t++) {
        char *tok = toks[t];
        command_t *c = &cmds[i];
        if (strcmp(tok, "|") == 0) {
            if (c->argc == 0) {
                fprintf(stderr, "syntax error near '|'\n");
                return -1;
            }
            c->argv[c->argc] = NULL;
            if (++i >= MAX_CMDS) {
                fprintf(stderr, "too many piped commands\n");
                return -1;
            }
        } else if (strcmp(tok, "<") == 0) {
            if (++t >= ntok) { fprintf(stderr, "syntax error: expected file after '<'\n"); return -1; }
            c->infile = toks[t];
        } else if (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            int app = strcmp(tok, ">>") == 0;
            if (++t >= ntok) { fprintf(stderr, "syntax error: expected file after '>'\n"); return -1; }
            c->outfile = toks[t];
            c->append = app;
        } else {
            if (c->argc >= MAX_ARGS - 1) {
                fprintf(stderr, "too many arguments\n");
                return -1;
            }
            c->argv[c->argc++] = tok;
        }
    }
    cmds[i].argv[cmds[i].argc] = NULL;

    if (cmds[0].argc == 0)
        return 0; /* empty line */
    if (cmds[i].argc == 0) { /* trailing pipe: "cmd |" */
        fprintf(stderr, "syntax error: missing command after '|'\n");
        return -1;
    }
    return i + 1;
}

/* ---------------- execution ---------------- */

/* Apply <, >, >> redirections inside the child process. */
static void apply_redirection(const command_t *c)
{
    int fd;
    if (c->infile) {
        fd = open(c->infile, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", c->infile, strerror(errno)); _exit(1); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        fd = open(c->outfile, flags, 0644);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", c->outfile, strerror(errno)); _exit(1); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

/*
 * Parent-safe redirection for builtins: saves the old fds so the
 * caller can restore them. Returns 0 on success, -1 on failure
 * (never _exit()s, unlike the child version above).
 */
static int apply_redirection_parent(const command_t *c, int *save_in, int *save_out)
{
    *save_in = *save_out = -1;
    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", c->infile, strerror(errno)); return -1; }
        *save_in = dup(STDIN_FILENO);
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", c->outfile, strerror(errno));
            if (*save_in != -1) { dup2(*save_in, STDIN_FILENO); close(*save_in); *save_in = -1; }
            return -1;
        }
        *save_out = dup(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    return 0;
}

static void restore_redirection(int save_in, int save_out)
{
    fflush(stdout);
    if (save_in != -1)  { dup2(save_in, STDIN_FILENO);   close(save_in); }
    if (save_out != -1) { dup2(save_out, STDOUT_FILENO); close(save_out); }
}

/*
 * Run a pipeline of ncmds commands.
 * Classic fork/exec pattern:
 *   parent forks once per stage; each child wires its stdin/stdout
 *   with dup2() and then execvp() replaces it with the program.
 */
static void run_pipeline(command_t *cmds, int ncmds, int background)
{
    int prev_read = -1;          /* read end of previous pipe */
    pid_t pids[MAX_CMDS];
    int i;

    for (i = 0; i < ncmds; i++) {
        int pipefd[2] = { -1, -1 };

        if (i < ncmds - 1) {                 /* not the last stage: need a pipe */
            if (pipe(pipefd) < 0) { perror("pipe"); return; }
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return; }

        if (pid == 0) {                      /* ---- child ---- */
            signal(SIGINT, SIG_DFL);         /* child dies on Ctrl+C */
            if (prev_read != -1) {            /* stdin <- previous pipe */
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (pipefd[1] != -1) {           /* stdout -> next pipe */
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
                close(pipefd[0]);
            }
            apply_redirection(&cmds[i]);
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "%s: command not found\n", cmds[i].argv[0]);
            _exit(127);
        }

        /* ---- parent ---- */
        pids[i] = pid;
        if (prev_read != -1)
            close(prev_read);
        if (pipefd[1] != -1)
            close(pipefd[1]);
        prev_read = pipefd[0];
    }
    if (prev_read != -1)
        close(prev_read);

    if (background) {
        printf("[bg] started %d job(s), pids:", ncmds);
        for (i = 0; i < ncmds; i++)
            printf(" %d", pids[i]);
        printf("\n");
    } else {
        int status;
        for (i = 0; i < ncmds; i++)
            waitpid(pids[i], &status, 0);
    }
}

/* Reap finished background jobs so they don't become zombies. */
static void reap_background(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        printf("\n[bg] job %d finished\n", pid);
}

/* ---------------- main loop ---------------- */

int main(void)
{
    char line[MAX_LINE];
    command_t cmds[MAX_CMDS];

    signal(SIGINT, SIG_IGN);   /* Ctrl+C never kills the shell itself */

    printf("MiniShell v1.0 — type 'help' for help, 'exit' to quit\n");
    while (1) {
        int bg, n;

        reap_background();
        printf("minishell$ ");
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) {   /* Ctrl-D */
            printf("\nbye!\n");
            break;
        }

        n = parse_line(line, cmds, &bg);
        if (n <= 0)
            continue;                             /* empty line or syntax error */

        /* A lone builtin (no pipe) runs in the parent so cd persists. */
        if (n == 1 && is_builtin(cmds[0].argv[0])) {
            int save_in, save_out;
            if (apply_redirection_parent(&cmds[0], &save_in, &save_out) == 0) {
                run_builtin(&cmds[0]);
                restore_redirection(save_in, save_out);
            }
        } else {
            if (n > 1) {
                /* sanity: builtins can't be piped in this tiny shell */
                int k, bad = 0;
                for (k = 0; k < n; k++)
                    if (is_builtin(cmds[k].argv[0])) bad = 1;
                if (bad) {
                    fprintf(stderr, "builtins can't be used in a pipeline\n");
                    continue;
                }
            }
            run_pipeline(cmds, n, bg);
        }
    }
    return 0;
}
