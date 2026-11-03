#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

using namespace std;

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

void runExternal(vector<string>& args, bool background) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
    } else if (pid == 0) {
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
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        auto c = toCArgs(leftArgs);
        execvp(c[0], c.data());
        perror("command not found"); exit(1);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
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
    string input;
    vector<string> history;

    while (true) {
        cout << "myshell> ";
        cout.flush();
        if (!getline(cin, input)) break;
        if (input.empty()) continue;

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
            cout << "Features: pipes (|), redirection (> <), background (&)\n";
        } else {
            runExternal(args, background);
        }
    }
    return 0;
}
