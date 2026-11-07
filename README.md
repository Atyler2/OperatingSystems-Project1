# Project 1: A Simple Unix Shell

Operating Systems, Project 1

## Build and Run

```bash
gcc -o shell shell.c
./shell
```

## 1. Overview

This project is a command line shell written in C. The project completes a number of different tasks, including prompting the user with the current working directory, running built-in commands, launching external programs in child processes, supporting background jobs, handling Ctrl-C, killing long-running foreground jobs after 10 seconds, and supporting I/O redirection and pipes.

## 2. Design Choices

**Task 1: prompt, tokenizer, built-ins.** `print_prompt()` shows the `getcwd()` path followed by `> `, `tokenize()` splits the line with `strtok` and expands `$VAR` through `getenv()`, and `run_builtin()` handles `cd`, `pwd`, `echo`, `exit`, `env`, and `setenv` inside the shell process so that `cd` and `setenv` actually change the shell.

**Task 2: external programs.** The shell calls `fork()`, the child calls `execvp()` (printing `execvp() failed` and `An error occurred.` if it fails), and the parent waits with `waitpid()` before showing the next prompt.

**Task 3: background jobs.** A trailing `&` is removed so the parent skips `waitpid`, and finished background children are reaped with a non-blocking `waitpid(-1, NULL, WNOHANG)` at the top of the main loop.

**Task 4: Ctrl-C.** A `sigaction()` handler for `SIGINT` only prints a newline (without `SA_RESTART`, so `fgets` returns and the prompt reprints), while foreground children reset `SIGINT` to `SIG_DFL` so Ctrl-C kills them but not the shell.

**Task 5: 10-second timeout.** Foreground child PIDs are saved in `fg_pids` and `alarm(10)` is set, so the `SIGALRM` handler can `SIGKILL` them and flag a timeout message, while `alarm(0)` cancels the timer if the job finishes early.

**Task 6: redirection and pipes.** `parse_commands()` splits the line into `Cmd` structs on `|`, `<`, and `>`, and `run_pipeline()` forks one child per command, wiring the stages together with `pipe()` and `dup2()` and applying file redirects through `apply_redirects()`.

## 3. Code Documentation

| Item | Purpose |
| --- | --- |
| `MAX_COMMAND_LINE_LEN`, `MAX_COMMAND_LINE_ARGS`, `MAX_PIPE_CMDS`, `TIMEOUT_SECONDS` | Limits for line length (1024), arguments (128), pipeline stages (16), and the foreground time limit (10 s). |
| `Cmd` | One command: argument vector plus optional input and output file names. |
| `fg_pids`, `fg_count`, `timed_out` | State shared between the main loop and the `SIGALRM` handler. |
| `sigint_handler()` | Prints a newline on Ctrl-C so the shell returns to a clean prompt. |
| `sigalrm_handler()` | Marks a timeout and kills all running foreground children. |
| `print_prompt()` | Prints `<cwd>> ` using `getcwd()`. |
| `tokenize()` | Splits a line into tokens and expands `$VAR`. |
| `is_builtin()` / `run_builtin()` | Identify and execute `cd`, `pwd`, `echo`, `exit`, `env`, `setenv`. |
| `parse_commands()` | Turns tokens into an array of `Cmd` structs, handling `\|`, `<`, and `>`. |
| `apply_redirects()` | Opens the redirect files and rewires stdin and stdout with `dup2()`. |
| `run_pipeline()` | Creates pipes, forks the children, and handles waiting, background mode, and the timeout. |
| `main()` | Installs signal handlers, reaps finished background jobs, reads and parses a line, and dispatches it. |

## 4. Testing and Limitations

Known limitations: operators must be separated by spaces (`cat a > b`, not `cat a>b`); quotes are not interpreted; only `>` is supported, not `>>`; `&` is recognized only as the last token; and background jobs are not subject to the 10-second limit.
