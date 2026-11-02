#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>

// Parse input string into a list of tokens
std::vector<std::string> parseInput(const std::string& input) {
    std::vector<std::string> args;
    std::istringstream iss(input);
    std::string token;
    while (iss >> token) args.push_back(token);
    return args;
}

// Convert vector<string> to char* array for execvp
std::vector<char*> toCArgs(std::vector<std::string>& args) {
    std::vector<char*> c_args;
    for (auto& s : args) c_args.push_back(s.data());
    c_args.push_back(nullptr);
    return c_args;
}

// Built-in: cd
void changeDirectory(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "cd: missing argument\n";
        return;
    }
    if (chdir(args[1].c_str()) != 0) {
        perror("cd");
    }
}

// Run external command using fork + exec (core OS concept)
void runExternal(std::vector<std::string>& args) {
    pid_t pid = fork(); // create child process
    if (pid < 0) {
        perror("fork failed");
    } else if (pid == 0) {
        // Inside child process
        auto c_args = toCArgs(args);
        execvp(c_args[0], c_args.data()); // replace child with command
        perror("command not found");
        exit(1);
    } else {
        // Inside parent process — wait for child to finish
        waitpid(pid, nullptr, 0);
    }
}

int main() {
    std::string input;
    std::vector<std::string> history;

    while (true) {
        std::cout << "myshell> ";
        std::cout.flush();

        if (!std::getline(std::cin, input)) break;
        if (input.empty()) continue;

        history.push_back(input);

        std::vector<std::string> args = parseInput(input);
        if (args.empty()) continue;

        if (args[0] == "exit") {
            std::cout << "Goodbye!\n";
            break;
        } else if (args[0] == "cd") {
            changeDirectory(args);
        } else if (args[0] == "pwd") {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd))) std::cout << cwd << "\n";
        } else if (args[0] == "echo") {
            for (size_t i = 1; i < args.size(); i++)
                std::cout << args[i] << (i + 1 < args.size() ? " " : "\n");
        } else if (args[0] == "history") {
            for (size_t i = 0; i < history.size(); i++)
                std::cout << i + 1 << "  " << history[i] << "\n";
        } else if (args[0] == "help") {
            std::cout << "Built-in commands: cd, pwd, echo, history, help, exit\n";
            std::cout << "Any other command runs as an external program.\n";
        } else {
            runExternal(args);
        }
    }

    return 0;
}