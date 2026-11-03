#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <map>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

using namespace std;

// ── Job Control Data Structure ─────────────────────────────────────────────
struct Job {
    int id;           // job number shown to user [1], [2]...
    pid_t pgid;       // process group ID of this job
    string command;   // the command string
    enum State { RUNNING, STOPPED, DONE } state;
};

map<int, Job> jobs;   // job list: jobId -> Job
int nextJobId = 1;
pid_t shellPgid;      // shell's own process group

const string HISTORY_FILE = string(getenv("HOME")) + "/.myshell_history";

// ── Utility ────────────────────────────────────────────────────────────────
vector<string> parseInput(const string& input) {
    vector<string> args;
    istringstream iss(input);
    string token;
    while (iss >> token) args.push_back(token);
    return args;
}

vector<char*> toCArgs(vector<string>& args) {
    vector<char*> c_args;
    for (auto& s : args) c_args.push_back(s.data());
    c_args.push_back(nullptr);
    return c_args;
}

// ── Prompt ─────────────────────────────────────────────────────────────────
void printPrompt() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        string path(cwd);
        size_t pos = path.rfind('/');
        string folder = (pos != string::npos) ? path.substr(pos + 1) : path;
        if (folder.empty()) folder = "/";
        cout << "\033[1;32mmyshell\033[0m:\033[1;34m" << folder << "\033[0m$ ";
    } else {
        cout << "myshell> ";
    }
    cout.flush();
}

// ── History ────────────────────────────────────────────────────────────────
void saveHistory(const vector<string>& history) {
    ofstream f(HISTORY_FILE);
    for (auto& cmd : history) f << cmd << "\n";
}

vector<string> loadHistory() {
    vector<string> history;
    ifstream f(HISTORY_FILE);
    string line;
    while (getline(f, line)) if (!line.empty()) history.push_back(line);
    return history;
}

// ── Reap finished/stopped background jobs ──────────────────────────────────
void reapJobs() {
    for (auto it = jobs.begin(); it != jobs.end(); ) {
        int status;
        pid_t result = waitpid(-it->second.pgid, &status, WNOHANG | WUNTRACED);
        if (result > 0) {
            if (WIFSTOPPED(status)) {
                it->second.state = Job::STOPPED;
                cout << "\n[" << it->second.id << "] Stopped   " << it->second.command << "\n";
                ++it;
            } else {
                cout << "\n[" << it->second.id << "] Done      " << it->second.command << "\n";
                it = jobs.erase(it);
            }
        } else {
            ++it;
        }
    }
}

// ── Give terminal control to a process group ───────────────────────────────
void giveTerm(pid_t pgid) {
    tcsetpgrp(STDIN_FILENO, pgid);
}

void takeTerm() {
    tcsetpgrp(STDIN_FILENO, shellPgid);
}

// ── Run external command with job control ──────────────────────────────────
void runExternal(vector<string>& args, bool background, const string& cmdStr) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return;
    }

    if (pid == 0) {
        // Child: put itself in its own process group
        setpgid(0, 0);
        if (!background) giveTerm(getpid());

        // Restore default signal handlers in child
        signal(SIGINT,  SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        auto c_args = toCArgs(args);
        execvp(c_args[0], c_args.data());
        perror("command not found");
        exit(1);
    }

    // Parent: put child in its own process group too (race-free)
    setpgid(pid, pid);

    // Add to job list
    Job job;
    job.id      = nextJobId++;
    job.pgid    = pid;
    job.command = cmdStr;
    job.state   = Job::RUNNING;
    jobs[job.id] = job;

    if (background) {
        cout << "[" << job.id << "] " << pid << "\n";
    } else {
        // Foreground: give terminal to child, wait for it
        giveTerm(pid);
        int status;
        waitpid(pid, &status, WUNTRACED);
        takeTerm();

        if (WIFSTOPPED(status)) {
            jobs[job.id].state = Job::STOPPED;
            cout << "\n[" << job.id << "] Stopped   " << cmdStr << "\n";
        } else {
            jobs.erase(job.id);
            nextJobId = 1;
            for (auto& j : jobs) nextJobId = max(nextJobId, j.second.id + 1);
        }
    }
}

// ── Built-in: jobs ─────────────────────────────────────────────────────────
void listJobs() {
    if (jobs.empty()) { cout << "No jobs.\n"; return; }
    for (auto& [id, job] : jobs) {
        string state = (job.state == Job::RUNNING) ? "Running " : "Stopped ";
        cout << "[" << id << "] " << state << "  " << job.command << "\n";
    }
}

// ── Built-in: fg <jobid> ───────────────────────────────────────────────────
void bringFg(const vector<string>& args) {
    if (jobs.empty()) { cout << "No background jobs.\n"; return; }

    int id = jobs.begin()->first; // default: most recent
    if (args.size() >= 2) id = stoi(args[1]);

    if (jobs.find(id) == jobs.end()) {
        cout << "fg: job " << id << " not found\n"; return;
    }

    Job& job = jobs[id];
    cout << job.command << "\n";
    job.state = Job::RUNNING;

    // Resume if stopped, give terminal, wait
    kill(-job.pgid, SIGCONT);
    giveTerm(job.pgid);

    int status;
    waitpid(-job.pgid, &status, WUNTRACED);
    takeTerm();

    if (WIFSTOPPED(status)) {
        job.state = Job::STOPPED;
        cout << "\n[" << id << "] Stopped   " << job.command << "\n";
    } else {
        jobs.erase(id);
    }
}

// ── Built-in: bg <jobid> ───────────────────────────────────────────────────
void sendBg(const vector<string>& args) {
    if (jobs.empty()) { cout << "No jobs.\n"; return; }

    int id = jobs.begin()->first;
    if (args.size() >= 2) id = stoi(args[1]);

    if (jobs.find(id) == jobs.end()) {
        cout << "bg: job " << id << " not found\n"; return;
    }

    Job& job = jobs[id];
    job.state = Job::RUNNING;
    kill(-job.pgid, SIGCONT); // resume in background
    cout << "[" << id << "] " << job.command << " &\n";
}

void changeDirectory(const vector<string>& args) {
    if (args.size() < 2) { cerr << "cd: missing argument\n"; return; }
    if (chdir(args[1].c_str()) != 0) perror("cd");
}

bool hasPipe(const string& input, string& left, string& right) {
    size_t pos = input.find('|');
    if (pos == string::npos) return false;
    left = input.substr(0, pos);
    right = input.substr(pos + 1);
    return true;
}

void runPipe(const string& left, const string& right) {
    int pipefd[2];
    pipe(pipefd);
    auto leftArgs  = parseInput(left);
    auto rightArgs = parseInput(right);

    pid_t pid1 = fork();
    if (pid1 == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        auto c = toCArgs(leftArgs);
        execvp(c[0], c.data());
        perror("command not found"); exit(1);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[1]); close(pipefd[0]);
        auto c = toCArgs(rightArgs);
        execvp(c[0], c.data());
        perror("command not found"); exit(1);
    }

    close(pipefd[0]); close(pipefd[1]);
    waitpid(pid1, nullptr, 0);
    waitpid(pid2, nullptr, 0);
}

void runRedirect(vector<string>& args) {
    int infd = -1, outfd = -1;
    vector<string> cleanArgs;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == ">" && i + 1 < args.size())
            outfd = open(args[++i].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        else if (args[i] == "<" && i + 1 < args.size())
            infd = open(args[++i].c_str(), O_RDONLY);
        else cleanArgs.push_back(args[i]);
    }
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        if (outfd != -1) { dup2(outfd, STDOUT_FILENO); close(outfd); }
        if (infd  != -1) { dup2(infd,  STDIN_FILENO);  close(infd);  }
        auto c = toCArgs(cleanArgs);
        execvp(c[0], c.data());
        perror("command not found"); exit(1);
    } else {
        if (outfd != -1) close(outfd);
        if (infd  != -1) close(infd);
        waitpid(pid, nullptr, 0);
    }
}

// ── Main ───────────────────────────────────────────────────────────────────
int main() {
    // Shell ignores these — children get them via Ctrl+C / Ctrl+Z
    signal(SIGINT,  SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    // Put shell in its own process group and take terminal
    shellPgid = getpid();
    setpgid(shellPgid, shellPgid);
    tcsetpgrp(STDIN_FILENO, shellPgid);

    string input;
    vector<string> history = loadHistory();

    while (true) {
        reapJobs(); // check if any background jobs finished
        printPrompt();

        if (!getline(cin, input)) { cout << "\n"; break; }
        if (input.empty()) continue;

        // !! repeat last command
        if (input == "!!") {
            if (history.empty()) { cout << "No commands in history.\n"; continue; }
            input = history.back();
            cout << input << "\n";
        }

        history.push_back(input);

        // Background check
        bool background = false;
        if (!input.empty() && input.back() == '&') {
            background = true;
            input.pop_back();
        }

        string cmdStr = input;
        string left, right;
        if (hasPipe(input, left, right)) { runPipe(left, right); continue; }

        vector<string> args = parseInput(input);
        if (args.empty()) continue;

        bool hasRedirect = false;
        for (auto& a : args) if (a == ">" || a == "<") { hasRedirect = true; break; }
        if (hasRedirect) { runRedirect(args); continue; }

        // Built-ins
        if (args[0] == "exit") {
            saveHistory(history);
            cout << "Goodbye!\n"; break;
        } else if (args[0] == "cd") {
            changeDirectory(args);
        } else if (args[0] == "pwd") {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd))) cout << cwd << "\n";
        } else if (args[0] == "echo") {
            for (size_t i = 1; i < args.size(); i++)
                cout << args[i] << (i + 1 < args.size() ? " " : "\n");
        } else if (args[0] == "history") {
            for (size_t i = 0; i < history.size(); i++)
                cout << i + 1 << "  " << history[i] << "\n";
        } else if (args[0] == "jobs") {
            listJobs();
        } else if (args[0] == "fg") {
            bringFg(args);
        } else if (args[0] == "bg") {
            sendBg(args);
        } else if (args[0] == "help") {
            cout << "Built-ins: cd, pwd, echo, history, jobs, fg, bg, help, exit\n";
            cout << "Features: pipes (|), redirection (> <), background (&), !!, Ctrl+Z, Ctrl+C\n";
        } else {
            runExternal(args, background, cmdStr);
        }
    }
    return 0;
}