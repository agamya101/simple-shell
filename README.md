# Simple Shell

A Unix shell implemented in C++ as an OS mini project. Built from scratch using POSIX system calls.

## Features

| Feature | Example | OS Concept |
|---|---|---|
| External commands | `ls -la`, `date`, `whoami` | `fork()` + `execvp()` |
| Pipes | `ls \| grep cpp` | Inter-process communication |
| Output redirection | `echo hello > out.txt` | File descriptors, `dup2()` |
| Input redirection | `cat < input.txt` | File descriptors, `dup2()` |
| Background processes | `sleep 10 &` | Process management |
| Command history | `history` | Built-in |
| Directory navigation | `cd`, `pwd` | Built-in |

## Compile and Run

```bash
g++ -o myshell shell.cpp
./myshell
```

## Built-in Commands

| Command | Description |
|---|---|
| `cd <dir>` | Change directory |
| `pwd` | Print working directory |
| `echo <text>` | Print text |
| `history` | Show command history |
| `help` | List supported commands |
| `exit` | Exit the shell |

## OS Concepts Demonstrated

- **Process creation** — `fork()` creates a child process for every external command
- **Program execution** — `execvp()` replaces the child process with the requested program
- **Inter-process communication** — `pipe()` connects stdout of one process to stdin of another
- **I/O redirection** — `dup2()` rewires file descriptors for `>` and `<`
- **Process synchronisation** — `waitpid()` makes the parent wait for child to finish
- **Background execution** — skipping `waitpid()` lets the process run independently

## Platform

Requires Linux or macOS. On Windows, use WSL.
