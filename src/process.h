// Running the two other programs this tool needs: git, and the helper script
// for a cluster action.
//
// Both leave stdin, stdout and stderr exactly as they are. git has to be able
// to prompt for credentials and show its push progress, and `exec` hands a
// terminal to a shell running inside a pod -- neither survives having its
// output captured.
#pragma once

#include <string>
#include <vector>

// Waits for the command and returns its exit status. A command killed by a
// signal comes back as 128 + the signal number, the convention every shell
// uses, so a status of 0 always means it really did work.
int run(const std::vector<std::string> &argv);

// Replaces this process with the command: nothing is left to run afterwards,
// so its exit status is the tool's without anything having to pass it on, and
// there is no wrapper process between the terminal and the remote shell to
// swallow a Ctrl-C. Only returns by failing.
[[noreturn]] void run_replacing_self(const std::vector<std::string> &argv);
