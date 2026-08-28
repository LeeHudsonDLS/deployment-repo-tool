#include "config.h"

#include <cstdlib>

#include "paths.h"
#include "support.h"

namespace {

const char *CONFIG_NAME = "config.ini";

bool is_comment(const std::string &stripped) {
    return !stripped.empty() && (stripped[0] == '#' || stripped[0] == ';');
}

}  // namespace

void Config::parse(const std::string &text, const std::string &path) {
    // Points at the section being filled in, and at the option a following
    // indented line would continue.
    Options *current = nullptr;
    std::string current_name;
    std::string *continuing = nullptr;

    const std::vector<std::string> lines = split_lines(text);
    for (size_t index = 0; index < lines.size(); index++) {
        const std::string line = rstrip(lines[index]);
        const std::string trimmed = strip(line);
        const int number = static_cast<int>(index) + 1;

        if (trimmed.empty() || is_comment(trimmed)) continue;

        // Indented and following an option: more of that option's value. This
        // is how a long [match] list is wrapped over several lines.
        if (is_space(line[0])) {
            if (!continuing) {
                fail(format("%s: line %d: unexpected indentation", path.c_str(), number));
            }
            *continuing += "\n" + trimmed;
            continue;
        }

        if (trimmed[0] == '[') {
            const size_t close = trimmed.find(']');
            if (close == std::string::npos) {
                fail(format("%s: line %d: section heading with no ']'", path.c_str(),
                            number));
            }
            const std::string name = trimmed.substr(1, close - 1);
            if (has_section(name)) {
                fail(format("%s: line %d: [%s] appears twice", path.c_str(), number,
                            name.c_str()));
            }
            sections_.emplace_back(name, Options());
            current = &sections_.back().second;
            current_name = name;
            continuing = nullptr;
            continue;
        }

        if (!current) {
            fail(format("%s: line %d: '%s' comes before any [section]", path.c_str(),
                        number, trimmed.c_str()));
        }

        // The first '=' or ':' separates, so a value may contain either --
        // which a path or a glob very well might.
        const size_t split = trimmed.find_first_of("=:");
        if (split == std::string::npos) {
            fail(format("%s: line %d: '%s' is not 'key = value'", path.c_str(), number,
                        trimmed.c_str()));
        }
        const std::string key = lower(rstrip(trimmed.substr(0, split)));
        if (key.empty()) {
            fail(format("%s: line %d: no name before the separator", path.c_str(),
                        number));
        }
        for (const std::pair<std::string, std::string> &option : *current) {
            if (option.first == key) {
                fail(format("%s: line %d: '%s' appears twice in [%s]", path.c_str(),
                            number, key.c_str(), current_name.c_str()));
            }
        }
        current->emplace_back(key, strip(trimmed.substr(split + 1)));
        continuing = &current->back().second;
    }
}

bool Config::has_section(const std::string &name) const {
    for (const std::pair<std::string, Options> &entry : sections_) {
        if (entry.first == name) return true;
    }
    return false;
}

const std::string *Config::lookup(const std::string &section,
                                  const std::string &option) const {
    const std::string wanted = lower(option);
    for (const std::pair<std::string, Options> &entry : sections_) {
        if (entry.first != section) continue;
        for (const std::pair<std::string, std::string> &pair : entry.second) {
            if (pair.first == wanted) return &pair.second;
        }
    }
    return nullptr;
}

bool Config::has_option(const std::string &section, const std::string &option) const {
    return lookup(section, option) != nullptr;
}

std::string Config::get(const std::string &section, const std::string &option,
                        const std::string &fallback) const {
    const std::string *value = lookup(section, option);
    return value ? *value : fallback;
}

const Options &Config::section(const std::string &name) const {
    static const Options empty;
    for (const std::pair<std::string, Options> &entry : sections_) {
        if (entry.first == name) return entry.second;
    }
    return empty;
}

std::vector<std::string> Config::section_names() const {
    std::vector<std::string> names;
    names.reserve(sections_.size());
    for (const std::pair<std::string, Options> &entry : sections_) {
        names.push_back(entry.first);
    }
    return names;
}

LoadedConfig read_config(const std::string &given) {
    const char *from_environment = std::getenv("DEPLOYMENT_REPO_CONFIG");
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string home = (xdg && *xdg) ? std::string(xdg) : expanduser("~/.config");

    std::vector<std::string> candidates;
    if (!given.empty()) candidates.push_back(given);
    if (from_environment && *from_environment) candidates.push_back(from_environment);
    candidates.push_back(path_join(path_join(home, "deployment-repo-tool"), CONFIG_NAME));
    // Last resort: a config.ini next to the binary. The repo ships only
    // config.ini.example, so a fresh clone finds nothing here and cannot push
    // to anyone else's checkouts.
    const std::string here = own_path();
    if (!here.empty()) candidates.push_back(path_join(dirname(here), CONFIG_NAME));

    for (const std::string &candidate : candidates) {
        if (!is_file(candidate)) continue;
        LoadedConfig loaded;
        loaded.config.parse(read_file(candidate), candidate);
        loaded.path = candidate;
        return loaded;
    }

    // Only an explicitly named one is an error to be missing; the rest of the
    // chain is allowed to come up empty.
    if (!given.empty()) fail("no config file at " + given);
    return LoadedConfig();
}

std::vector<std::string> patterns(const std::string &value) { return split_list(value); }
