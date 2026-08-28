#include "process.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "support.h"

namespace {

// execvp wants a null terminated array of mutable char pointers. The strings
// stay alive in the caller's vector for as long as this is used.
std::vector<char *> raw_argv(const std::vector<std::string> &argv) {
    std::vector<char *> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string &arg : argv) raw.push_back(const_cast<char *>(arg.c_str()));
    raw.push_back(nullptr);
    return raw;
}

// Our own output is buffered and the child's is not, so without this a git
// push can appear above the line saying which repo it is pushing to.
void flush_before_handing_over() { std::cout.flush(); }

}  // namespace

int run(const std::vector<std::string> &argv) {
    if (argv.empty()) fail("nothing to run");
    flush_before_handing_over();

    const pid_t child = fork();
    if (child < 0) fail(format("cannot start %s: %s", argv[0].c_str(), strerror(errno)));
    if (child == 0) {
        std::vector<char *> raw = raw_argv(argv);
        execvp(raw[0], raw.data());
        // Still here, so the exec failed. 127 is what a shell reports for a
        // command it could not run, and the message names the program.
        std::cerr << "error: cannot run " << argv[0] << ": " << strerror(errno)
                  << std::endl;
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) fail(format("waiting for %s failed: %s", argv[0].c_str(),
                                        strerror(errno)));
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

void run_replacing_self(const std::vector<std::string> &argv) {
    if (argv.empty()) fail("nothing to run");
    flush_before_handing_over();

    std::vector<char *> raw = raw_argv(argv);
    execvp(raw[0], raw.data());
    // Reached only when the script could not be started at all -- not
    // executable, or a shebang line naming an interpreter that is not there.
    fail(format("cannot run %s: %s", argv[0].c_str(), strerror(errno)));
}
