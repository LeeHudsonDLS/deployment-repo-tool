#include "helpers.h"

#include "support.h"
#include "embedded_helpers.h"

std::vector<std::string> embedded_helper(const std::string &name,
                                         const std::vector<std::string> &args) {
    const char *script = nullptr;
    if (name == "ioc-restart") script = helper_1;
    else if (name == "ioc-exec") script = helper_2;
    else if (name == "ioc-argocd") script = helper_3;
    else fail("unknown embedded helper: " + name);
    std::vector<std::string> command = {"bash", "-c", script, name};
    command.insert(command.end(), args.begin(), args.end());
    return command;
}
