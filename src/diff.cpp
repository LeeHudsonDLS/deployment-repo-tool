#include "diff.h"

#include <cstdio>
#include <fcntl.h>
#include <memory>

#include "process.h"
#include "support.h"

namespace {
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;

File temporary_file(const std::vector<std::string> &lines) {
    // tmpfile unlinks the file immediately, including on failure or interruption.
    File file(std::tmpfile(), &std::fclose);
    if (!file) fail("cannot create temporary file for diff");
    for (const auto &line : lines) {
        if (std::fwrite(line.data(), 1, line.size(), file.get()) != line.size()) {
            fail("cannot write temporary file for diff");
        }
    }
    if (std::fflush(file.get()) != 0) fail("cannot flush temporary file for diff");
    // diff opens these inherited descriptors through /proc; no shell or named
    // temporary paths are needed. This tool already requires Linux /proc.
    if (fcntl(fileno(file.get()), F_SETFD, 0) < 0) fail("cannot pass file to diff");
    return file;
}
}

std::string unified_diff(const std::vector<std::string> &before,
                         const std::vector<std::string> &after,
                         const std::string &from_file, const std::string &to_file) {
    if (before == after) return {};
    auto old_file = temporary_file(before);
    auto new_file = temporary_file(after);
    auto output = temporary_file({});
    const int status = run({"diff", "-u", "--label", from_file, "--label", to_file,
                            "--", "/proc/self/fd/" + std::to_string(fileno(old_file.get())),
                            "/proc/self/fd/" + std::to_string(fileno(new_file.get()))},
                           fileno(output.get()));
    // diff uses 1 for differences, and 2 (or an exec failure) for an error.
    if (status != 0 && status != 1) fail("system diff failed; values file has not been written");
    if (std::fseek(output.get(), 0, SEEK_SET) != 0) fail("cannot rewind diff output");
    std::string result;
    char buffer[4096];
    size_t count;
    while ((count = std::fread(buffer, 1, sizeof(buffer), output.get())) != 0) {
        result.append(buffer, count);
    }
    if (std::ferror(output.get())) fail("cannot read diff output");
    return result;
}
