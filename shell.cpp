#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

using namespace std;

// ── History file path ──────────────────────────────────────────────────────
const string HISTORY_FILE = string(getenv("HOME")) + "/.myshell_history";

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

void changeDirectory(const vector<string>& args) {
    if (args.size() < 2) { cerr << "cd: missing argument\n"; return; }
    if (chdir(args[1].c_str()) != 0) perror("cd");
}

// ── Custom prompt: shows current directory ─────────────────────────────────
void printPrompt() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        string path(cwd);
        // Show only last folder name to keep it short
        size_t pos = path.rfind('/');
        string folder = (pos != string::npos) ? path.substr(pos + 1) : path;
        if (folder.empty()) folder = "/";
        cout << "\033[1;32mmyshell\033[0m:\033[1;34m" << folder << "\033[0m$ ";
    } else {
        cout << "myshell> ";
    }
    cout.flush();
}

// ── Save history to file ───────────────────────────────────────────────────
void saveHistory(const vector<string>& history) {
    ofstream f(HISTORY_FILE);
    for (auto& cmd : history) f << cmd << "\n";
}

// ── Load history from file ─────────────────────────────────────────────────
vector<string> loadHistory() {
    vector<string> history;
    ifstream f(HISTORY_FILE);
    string line;
    while (getline(f, line)) if (!line.empty()) history.push_back(line);
    return history;
}

void runExternal(vector<string>& args, bool background) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
    } else if (pid == 0) {
        // Child restores default Ctrl+C behaviour
        signal(SIGINT, SIG_DFL);
        auto c_args = toCArgs(args);
        execvp(c_args[0], c_args.data());
        perror("command not found");
        exit(1);
    } else {
        if (background) {
            cout << "[Process running in background, PID: " << pid << "]\n";
        } else {
            waitpid(pid, nullptr, 0);
        }
    }
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
        signal(SIGINT, SIG_DFL);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        auto c = toCArgs(leftArgs);
        execvp(c[0], c.data());
        perror("command not found"); exit(1);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        signal(SIGINT, SIG_DFL);
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
        signal(SIGINT, SIG_DFL);
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

int main() {
    // ── Signal handling: parent ignores Ctrl+C ─────────────────────────────
    signal(SIGINT, SIG_IGN);

    string input;
    vector<string> history = loadHistory(); // load previous session history

    while (true) {
        printPrompt();

        if (!getline(cin, input)) {
            cout << "\n";
            break;
        }
        if (input.empty()) continue;

        // ── !! repeats last command ────────────────────────────────────────
        if (input == "!!") {
            if (history.empty()) {
                cout << "No commands in history.\n";
                continue;
            }
            input = history.back();
            cout << input << "\n"; // show what's being run
        }

        history.push_back(input);

        bool background = false;
        if (!input.empty() && input.back() == '&') {
            background = true;
            input.pop_back();
        }

        string left, right;
        if (hasPipe(input, left, right)) { runPipe(left, right); continue; }

        vector<string> args = parseInput(input);
        if (args.empty()) continue;

        bool hasRedirect = false;
        for (auto& a : args) if (a == ">" || a == "<") { hasRedirect = true; break; }
        if (hasRedirect) { runRedirect(args); continue; }

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
        } else if (args[0] == "help") {
            cout << "Built-ins: cd, pwd, echo, history, help, exit\n";
            cout << "Features: pipes (|), redirection (> <), background (&), !! repeats last command\n";
        } else {
            runExternal(args, background);
        }
    }
    return 0;
}