// Unit tests for the parts that used to come out of python's standard library.
//
//     make check
//
// tests/test_tool.py drives the built binary and is still the definition of
// what the tool must do. This is underneath it: globbing, path handling, the
// ini parser and the diff were free in python and are written out by hand here,
// so they are the pieces most worth pinning down on their own -- and the ones
// where a subtle mistake would quietly change which services a command picks
// rather than causing an obvious failure.
//
// Nothing here touches a deployment repo or the cluster. Diff checks use
// anonymous temporary files and the system diff executable.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "config.h"
#include "diff.h"
#include "glob.h"
#include "paths.h"
#include "support.h"
#include "values.h"

namespace {

int failures = 0;

void check(const std::string &name, bool ok, const std::string &detail = "") {
    std::cout << (ok ? "PASS  " : "FAIL  ") << name;
    if (!ok && !detail.empty()) std::cout << "  -- " << detail;
    std::cout << "\n";
    if (!ok) failures++;
}

void equal(const std::string &name, const std::string &got, const std::string &want) {
    check(name, got == want, "got [" + got + "] want [" + want + "]");
}

void matches(const std::string &name, const std::string &pattern, bool want) {
    check(format("glob: '%s' %s '%s'", name.c_str(), want ? "matches" : "does not match",
                 pattern.c_str()),
          fnmatch(name, pattern) == want);
}

// ------------------------------------------------------------------ globbing

void test_glob() {
    matches("fe15i-cs-ioc-01", "fe15i-cs-ioc-01", true);
    matches("fe15i-cs-ioc-01", "fe15*", true);
    matches("fe15i-cs-ioc-01", "fe16*", false);
    matches("fe15i-cs-ioc-01", "*-cs-*", true);
    matches("fe15i-cs-ioc-01", "*", true);
    matches("", "*", true);
    matches("", "", true);
    matches("a", "", false);
    matches("fe15i-cs-ioc-01", "fe1?i-cs-ioc-01", true);
    matches("fe15i-cs-ioc-01", "fe1??i-cs-ioc-01", false);

    // The shorthand from the shipped config, against real service names.
    matches("fe15i-cs-ioc-01", "fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]", true);
    matches("fe15i-mo-ioc-01", "fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]", false);
    matches("fe-epics-opis", "fe[0-9]*", false);
    matches("fe22i-py-ioc-01", "fe[0-9]*", true);
    matches("sr22c-va-ioc-01", "sr[0-2][0-9]c-va-ioc-[0-9][0-9]", true);
    matches("sr22c-pfwd", "sr[0-2][0-9]c-va-ioc-[0-9][0-9]", false);

    // Bracket expressions, including the corners fnmatch has.
    matches("b", "[abc]", true);
    matches("d", "[abc]", false);
    matches("d", "[!abc]", true);
    matches("a", "[!abc]", false);
    matches("-", "[a-]", true);        // a trailing - is a literal, not a range
    matches("-", "[-a]", true);        // and so is a leading one
    matches("b", "[a-]", false);
    matches("]", "[]]", true);         // ] straight after [ is a member
    matches("]", "[!]]", false);
    matches("[", "[15", false);        // no closing bracket: an ordinary [
    matches("[15", "[15", true);
    matches("^", "[^]", true);         // ^ does not negate; only ! does

    // Backtracking: the first * must give a character back for this to match.
    matches("aaa-b", "*a-b", true);
    matches("aaaaaaaaaaaaaaaaaaaaaaaaaaaaab", "*a*a*a*a*a*a*c", false);

    check("is_glob: a plain name is not one", !is_glob("sr22c-va-ioc-01"));
    check("is_glob: a star is", is_glob("fe15*"));
    check("is_glob: a class is", is_glob("fe1[59]-x"));
    check("is_glob: a question mark is", is_glob("fe1?i"));

    const std::vector<std::string> names = {"fe15i-cs-ioc-01", "fe15i-mo-ioc-01",
                                            "fe18i-cs-ioc-01", "fe-epics-opis"};
    equal("filter keeps file order", join(filter(names, "fe1*"), " "),
          "fe15i-cs-ioc-01 fe15i-mo-ioc-01 fe18i-cs-ioc-01");
    check("filter can select nothing", filter(names, "zz*").empty());
}

// --------------------------------------------------------------------- paths

void test_paths() {
    equal("normpath: repeated slashes", normpath("a//b"), "a/b");
    equal("normpath: a dot", normpath("a/./b"), "a/b");
    equal("normpath: a double dot", normpath("a/b/../c"), "a/c");
    equal("normpath: above the root", normpath("/../x"), "/x");
    equal("normpath: above a relative path", normpath("../x"), "../x");
    equal("normpath: nothing left", normpath(""), ".");
    equal("normpath: the root", normpath("/"), "/");
    equal("normpath: trailing slash", normpath("/a/b/"), "/a/b");
    equal("normpath: two leading slashes are kept", normpath("//a"), "//a");
    equal("normpath: three are not", normpath("///a"), "/a");

    equal("path_join: ordinary", path_join("/a", "b"), "/a/b");
    equal("path_join: absolute tail wins", path_join("/a", "/b"), "/b");
    equal("path_join: no doubled slash", path_join("/a/", "b"), "/a/b");
    equal("basename", basename("/a/b/c"), "c");
    equal("basename: no slash", basename("c"), "c");
    equal("dirname", dirname("/a/b/c"), "/a/b");
    equal("dirname: the root", dirname("/a"), "/");

    setenv("DEPLOYMENT_TOOL_TEST_VAR", "/somewhere", 1);
    unsetenv("DEPLOYMENT_TOOL_TEST_UNSET");
    equal("expandvars: $NAME", expandvars("$DEPLOYMENT_TOOL_TEST_VAR/va"),
          "/somewhere/va");
    equal("expandvars: ${NAME}", expandvars("${DEPLOYMENT_TOOL_TEST_VAR}/va"),
          "/somewhere/va");
    equal("expandvars: an unset name is left alone",
          expandvars("$DEPLOYMENT_TOOL_TEST_UNSET/va"), "$DEPLOYMENT_TOOL_TEST_UNSET/va");
    equal("expandvars: a bare $ is not a variable", expandvars("/a$/b"), "/a$/b");
    equal("expandvars: nothing to do", expandvars("/a/b"), "/a/b");

    setenv("HOME", "/home/someone", 1);
    equal("expanduser: ~/", expanduser("~/work"), "/home/someone/work");
    equal("expanduser: ~ alone", expanduser("~"), "/home/someone");
    equal("expanduser: not at the start", expanduser("/a/~/b"), "/a/~/b");
    equal("expanduser: an unknown user is left alone",
          expanduser("~nosuchuser12345/x"), "~nosuchuser12345/x");

    // What a configured [repos] path actually goes through.
    equal("expand: variables, then ~, then made absolute",
          expand("$DEPLOYMENT_TOOL_TEST_VAR/../elsewhere/va-deployment/"),
          "/elsewhere/va-deployment");
}

// -------------------------------------------------------------------- config

void test_config() {
    Config config;
    config.parse(
        "# a comment\n"
        "; and another\n"
        "\n"
        "[repos]\n"
        "fe = /a/fe-deployment\n"
        "VA: /a/va-deployment\n"
        "\n"
        "[match]\n"
        "fe = fe*\n"
        "va = sr*-va-ioc-*,\n"
        "     va-*\n"
        "\n"
        "[fe.aliases]\n"
        "cs = fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]\n"
        "\n"
        "[general]\n"
        "restart_script = /a/b/ioc-restart  # not a comment: part of the value\n",
        "<test>");

    check("config: sections are found", config.has_section("repos"));
    check("config: a missing section is not", !config.has_section("nope"));
    equal("config: a value", config.get("repos", "fe"), "/a/fe-deployment");
    equal("config: ':' separates too", config.get("repos", "va"), "/a/va-deployment");
    equal("config: names are folded to lower case", config.get("repos", "VA"),
          "/a/va-deployment");
    equal("config: a missing option falls back", config.get("repos", "id", "none"),
          "none");
    check("config: has_option", config.has_option("fe.aliases", "cs"));
    check("config: has_option on a missing section",
          !config.has_option("va.aliases", "cs"));

    equal("config: sections keep their order", join(config.section_names(), " "),
          "repos match fe.aliases general");
    const Options &match = config.section("match");
    check("config: options keep their order",
          match.size() == 2 && match[0].first == "fe" && match[1].first == "va");
    equal("config: a continuation line joins on",
          join(patterns(config.get("match", "va")), "|"), "sr*-va-ioc-*|va-*");
    equal("config: a # after a value is part of it",
          config.get("general", "restart_script"),
          "/a/b/ioc-restart  # not a comment: part of the value");

    equal("config: an absent section reads as empty",
          format("%zu", config.section("nope").size()), "0");

    // A % in a path or a glob is just a %: there is no interpolation.
    Config percent;
    percent.parse("[repos]\nva = /a/100%-va-deployment\n", "<test>");
    equal("config: % is literal", percent.get("repos", "va"), "/a/100%-va-deployment");
}

// ------------------------------------------------------- editing values.yaml

Lines document() {
    return split_lines(
        "project: accelerator\n"
        "services:\n"
        "  # a comment that is not an entry\n"
        "  va-epics-pvcs:\n"
        "  sr21c-va-ioc-01:\n"
        "    enabled: true\n"
        "    targetRevision: 2026_sd3\n"
        "    labels:\n"
        "      description:\n"
        "  sr22c-va-ioc-01:\n"
        "    enabled: false  # left off deliberately\n"
        "\n"
        "extraKey:\n"
        "  foo: bar\n");
}

void test_values() {
    const Lines lines = document();
    const std::pair<size_t, size_t> range = block(lines);
    check("values: the block starts after 'services:'", range.first == 2,
          format("%zu", range.first));
    check("values: the block ends before the next top level key", range.second == 11,
          format("%zu", range.second));

    size_t start, end;
    check("values: an entry is found",
          find_service(lines, "sr21c-va-ioc-01", &start, &end) && start == 4 && end == 9,
          format("%zu..%zu", start, end));
    check("values: an entry with no keys is found",
          find_service(lines, "va-epics-pvcs", &start, &end) && start == 3 && end == 4);
    check("values: a name that is not there is not found",
          !find_service(lines, "sr99c-va-ioc-01", &start, &end));
    check("values: an exact name cannot reach a nested key",
          !find_service(lines, "labels", &start, &end));

    const Options no_aliases;
    equal("select: a plain name passes through even if absent",
          join(select(lines, "sr99c-va-ioc-01", no_aliases), " "), "sr99c-va-ioc-01");
    equal("select: a glob picks entries in file order",
          join(select(lines, "sr2*", no_aliases), " "),
          "sr21c-va-ioc-01 sr22c-va-ioc-01");
    equal("select: comments and nested keys are not entries",
          join(select(lines, "*", no_aliases), " "),
          "va-epics-pvcs sr21c-va-ioc-01 sr22c-va-ioc-01");

    Options aliases;
    aliases.push_back(std::make_pair(std::string("all"), std::string("sr2*")));
    equal("select: a shorthand becomes its pattern",
          join(select(lines, "all", aliases), " "), "sr21c-va-ioc-01 sr22c-va-ioc-01");

    aliases.push_back({"one", "sr21c-va-ioc-01"});
    equal("select: shorthand may name one service",
          join(select(lines, "one", aliases), " "), "sr21c-va-ioc-01");

    Lines edited = document();
    set_key(edited, "sr21c-va-ioc-01", "enabled", "false");
    equal("set_key: an existing key is rewritten in place", edited[5],
          "    enabled: false\n");
    set_key(edited, "va-epics-pvcs", "enabled", "false");
    equal("set_key: a new key is added under the entry", edited[4],
          "    enabled: false\n");
    set_key(edited, "sr22c-va-ioc-01", "enabled", "true");
    equal("set_key: a trailing comment survives", edited[11],
          "    enabled: true  # left off deliberately\n");
    set_key(edited, "sr21c-va-ioc-01", "targetRevision", "main");
    equal("set_key: a key below others is found", edited[7],
          "    targetRevision: main\n");
    check("set_key: nothing else moved", edited.size() == document().size() + 1);
}

// ---------------------------------------------------------------------- diff

Lines numbered(size_t count) {
    Lines lines;
    for (size_t i = 0; i < count; i++) lines.push_back(format("l%zu\n", i));
    return lines;
}

void test_diff() {
    equal("diff: no change is no output", unified_diff(numbered(20), numbered(20), "f", "f"),
          "");

    Lines one = numbered(20);
    one[9] = "CHANGED\n";
    equal("diff: one changed line, three lines of context",
          unified_diff(numbered(20), one, "apps/values.yaml", "apps/values.yaml"),
          "--- apps/values.yaml\n"
          "+++ apps/values.yaml\n"
          "@@ -7,7 +7,7 @@\n"
          " l6\n l7\n l8\n-l9\n+CHANGED\n l10\n l11\n l12\n");

    Lines two = numbered(20);
    two[2] = "A\n";
    two[17] = "B\n";
    equal("diff: distant changes make two hunks",
          unified_diff(numbered(20), two, "f", "f"),
          "--- f\n+++ f\n"
          "@@ -1,6 +1,6 @@\n l0\n l1\n-l2\n+A\n l3\n l4\n l5\n"
          "@@ -15,6 +15,6 @@\n l14\n l15\n l16\n-l17\n+B\n l18\n l19\n");

    Lines near = numbered(20);
    near[8] = "A\n";
    near[12] = "B\n";
    equal("diff: changes close together share one hunk",
          unified_diff(numbered(20), near, "f", "f"),
          "--- f\n+++ f\n"
          "@@ -6,11 +6,11 @@\n l5\n l6\n l7\n-l8\n+A\n l9\n l10\n l11\n-l12\n+B\n"
          " l13\n l14\n l15\n");

    Lines appended = numbered(3);
    appended.push_back("new\n");
    equal("diff: an entry appended at the end",
          unified_diff(numbered(3), appended, "f", "f"),
          "--- f\n+++ f\n@@ -1,3 +1,4 @@\n l0\n l1\n l2\n+new\n");

    Lines removed = numbered(3);
    removed.erase(removed.begin() + 1);
    equal("diff: a line removed", unified_diff(numbered(3), removed, "f", "f"),
          "--- f\n+++ f\n@@ -1,3 +1,2 @@\n l0\n-l1\n l2\n");

    equal("diff: missing final newline is marked",
          unified_diff({"old"}, {"new\n"}, "a file; $literal", "a file; $literal"),
          "--- a file; $literal\n+++ a file; $literal\n"
          "@@ -1 +1 @@\n-old\n\\ No newline at end of file\n+new\n");

    equal("diff: from nothing", unified_diff(Lines(), numbered(2), "f", "f"),
          "--- f\n+++ f\n@@ -0,0 +1,2 @@\n+l0\n+l1\n");
}

}  // namespace

int main() {
    test_glob();
    test_paths();
    test_config();
    test_values();
    test_diff();

    std::cout << "\n";
    if (failures) {
        std::cout << failures << " FAILURE(S)" << std::endl;
        return 1;
    }
    std::cout << "all checks passed" << std::endl;
    return 0;
}
