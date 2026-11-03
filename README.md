# Simple Shell with Job Control

A Unix-like interactive shell implemented in C++ supporting command execution, I/O redirection, piping, and full POSIX job control (foreground/background jobs, `Ctrl+Z`, `fg`, `bg`).

> **OS Mini Project** — B.Tech CSE, NSUT Delhi  
> Inspired by the Virginia Tech CS 3214 process group model

---

## Features

| Category | Capabilities |
|---|---|
| **Execution** | External commands via `fork` + `execvp`, built-in commands |
| **I/O Redirection** | `>` `>>` `<` (stdout, append, stdin) |
| **Piping** | Multi-stage pipelines: `cmd1 \| cmd2 \| cmd3` |
| **Background Jobs** | `command &` — runs without blocking the shell |
| **Job Control** | `jobs`, `fg %n`, `bg %n`, `Ctrl+Z` (SIGTSTP) |
| **Signals** | Shell ignores `SIGINT`/`SIGTSTP`; children get default handlers |
| **Terminal Control** | `tcsetpgrp` transfers terminal ownership to foreground job |
| **Built-ins** | `cd`, `exit`, `jobs`, `fg`, `bg` |

---

## Build & Run

**Requirements:** GCC/G++ (C++17), Linux/macOS

```bash
git clone https://github.com/agamya101/simple-shell.git
cd simple-shell
g++ -std=c++17 -o myshell shell.cpp
./myshell
```

---

## Usage

```
myshell> ls -la                          # run a command
myshell> cat file.txt | grep "error"     # pipeline
myshell> sort < input.txt > output.txt   # I/O redirection
myshell> sleep 10 &                      # background job
[1] 47312
myshell> jobs                            # list jobs
[1]+ Running    sleep 10
myshell> fg %1                           # bring to foreground
myshell> ^Z                              # suspend with Ctrl+Z
[1]+ Stopped    sleep 10
myshell> bg %1                           # resume in background
[1]+ Running    sleep 10
```

---

## Architecture

The shell follows a **REPL loop** (Read → Evaluate → Print → Loop):

```
┌─────────────────────────────────────────────────────────┐
│                     REPL Loop                           │
│  readline() → parse tokens → identify command type      │
│       │                                                  │
│       ├─ Built-in? → execute directly in shell process  │
│       └─ External? → fork() + execvp()                  │
│              │                                           │
│              ├─ Foreground: waitpid(WUNTRACED)           │
│              │   tcsetpgrp → child owns terminal         │
│              └─ Background: add to jobs map              │
└─────────────────────────────────────────────────────────┘
```

### Key System Calls

| Call | Purpose |
|---|---|
| `fork()` | Create child process; returns 0 to child, PID to parent |
| `execvp()` | Replace child image with target program (searches PATH) |
| `waitpid(WUNTRACED)` | Block parent; detect both exit and stop (SIGTSTP) |
| `pipe(fd)` | Kernel IPC buffer; `fd[0]` = read, `fd[1]` = write |
| `dup2()` | Rewire file descriptors for redirection |
| `setpgid(pid, pid)` | Give each job its own process group |
| `tcsetpgrp()` | Transfer terminal ownership to foreground job |
| `kill(-pgid, sig)` | Send signal to entire process group |

---

## Job Control Design

Job control follows the **Virginia Tech CS 3214 process group model**:

```cpp
struct Job {
    int id;
    pid_t pgid;
    string command;
    enum State { RUNNING, STOPPED, DONE } state;
};
map<int, Job> jobs;
```

**Shell initialization:**
```cpp
signal(SIGINT,  SIG_IGN);   // shell ignores Ctrl+C
signal(SIGTSTP, SIG_IGN);   // shell ignores Ctrl+Z
signal(SIGTTIN, SIG_IGN);
signal(SIGTTOU, SIG_IGN);
setpgid(getpid(), getpid()); // shell gets its own process group
tcsetpgrp(STDIN_FILENO, shellPgid);
```

**Child process setup (before exec):**
```cpp
setpgid(0, 0);              // new process group
signal(SIGINT,  SIG_DFL);   // restore default handlers
signal(SIGTSTP, SIG_DFL);
if (!background)
    tcsetpgrp(STDIN_FILENO, getpid()); // grab terminal
```

**Terminal handoff:**
```cpp
void giveTerm(pid_t pgid) { tcsetpgrp(STDIN_FILENO, pgid); }
void takeTerm()           { tcsetpgrp(STDIN_FILENO, shellPgid); }
```

**Background job reaping** (non-blocking, called each prompt):
```cpp
void reapJobs() {
    for (auto& [id, job] : jobs) {
        int status;
        pid_t r = waitpid(-job.pgid, &status, WNOHANG | WUNTRACED);
        if (r > 0) {
            if (WIFEXITED(status) || WIFSIGNALED(status))
                job.state = Job::DONE;
            else if (WIFSTOPPED(status))
                job.state = Job::STOPPED;
        }
    }
}
```

---

## Novelty & Research Contribution

Standard shell tutorials implement only basic `fork`/`exec`/`wait`. This project adds **full POSIX job control** — a feature most educational shells omit — by applying the process group model described in:

> R. Dampier, *"Writing a Unix Shell,"* Virginia Tech CS 3214, 2020.  
> Available: https://cs3214.cs.vt.edu/

**Key novelties over a basic shell:**
- Each job gets its own **process group** (`setpgid`), so signals like `SIGINT` target only the right job — not the shell or sibling jobs.
- **Terminal ownership** (`tcsetpgrp`) is explicitly transferred: the shell yields the terminal to a foreground job and reclaims it when the job stops or exits.
- **`Ctrl+Z` suspend + `fg`/`bg` resume** works correctly because SIGTSTP is delivered to the foreground process group, not the shell.
- **Background job reaping** with `WNOHANG | WUNTRACED` prevents zombie processes without blocking the shell prompt.

---

## File Structure

```
simple-shell/
├── shell.cpp        # complete shell implementation
└── README.md
```

---

## References

1. R. Dampier, "Writing a Unix Shell," Virginia Tech CS 3214, 2020. [Online]. Available: https://cs3214.cs.vt.edu/
2. W. Stallings, *Operating Systems: Internals and Design Principles*, 9th ed. Pearson, 2018.
3. M. Kerrisk, *The Linux Programming Interface*. No Starch Press, 2010.
4. The Open Group, *The Single UNIX Specification, Version 4 (POSIX.1-2017)*. IEEE/The Open Group, 2018. [Online]. Available: https://pubs.opengroup.org/onlinepubs/9699919799/

---

## Author

**Agamya Jain**  
B.Tech Computer Science & Engineering, NSUT Delhi (2025–2029)  
GitHub: [@agamya101](https://github.com/agamya101)
