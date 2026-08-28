// The ini file: where the deployment repos are, which service names belong to
// which, and the shorthand each repo defines.
//
// This is the subset of python's configparser the tool used, with its
// behaviour kept where the config depends on it:
//
//   * sections and the options inside them keep the order they were written
//     in, because [match] is tried top to bottom and the first repo whose
//     patterns match wins;
//   * option names are folded to lower case, which is what configparser did to
//     them, so a shorthand written `CS =` is still `cs`;
//   * values are taken literally -- no % interpolation -- because a glob or a
//     path is allowed to contain a % and configparser's substitution would
//     have choked on it;
//   * a duplicated section or option is an error rather than one silently
//     winning, since two [repos] blocks would otherwise half work.
//
// Comments (`#` or `;` on their own line) and indented continuation lines are
// supported. Inline comments are not, again matching configparser: a `#` after
// a value is part of the value.
#pragma once

#include <string>
#include <utility>
#include <vector>

using Options = std::vector<std::pair<std::string, std::string>>;

class Config {
  public:
    // Any syntax error stops the tool, naming the file and the line.
    void parse(const std::string &text, const std::string &path);

    bool has_section(const std::string &name) const;
    bool has_option(const std::string &section, const std::string &option) const;
    std::string get(const std::string &section, const std::string &option,
                    const std::string &fallback = std::string()) const;

    // Empty when the section is not there, so callers can read a missing
    // config the same way they read an empty one. With no config file at all
    // the tool still works on whatever repo you are standing in.
    const Options &section(const std::string &name) const;

    // In file order, which is what the --help epilog is built from.
    std::vector<std::string> section_names() const;

  private:
    const std::string *lookup(const std::string &section, const std::string &option) const;

    std::vector<std::pair<std::string, Options>> sections_;
};

struct LoadedConfig {
    Config config;
    std::string path;  // empty when no config file was found
};

// The first of these that exists: the --config argument, $DEPLOYMENT_REPO_CONFIG,
// $XDG_CONFIG_HOME/deployment-repo-tool/config.ini (~/.config when unset), then
// config.ini beside the tool. A config file that is simply not there is not an
// error -- but one named with --config that is missing is.
LoadedConfig read_config(const std::string &given);

// A comma or space separated list of globs, as [match] and the alias sections
// are written.
std::vector<std::string> patterns(const std::string &value);
