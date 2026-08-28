// Shell-style name matching, the subset python's fnmatch provides.
//
// The tool matches three different things with globs -- the SERVICE argument
// against the [match] patterns, a shorthand's pattern against the names in
// values.yaml, and the argument itself against the alias table -- so this had
// to come across from python along with everything else. It is written out by
// hand rather than translated into std::regex: the translation is where
// fnmatch's awkward corners live (an unclosed bracket is a literal, a `]`
// straight after `[` is a member, `!` negates only in first position), and
// getting them wrong changes which IOCs a command touches.
//
// Matching is case sensitive throughout, which is what fnmatchcase does and
// what fnmatch.filter does on any posix machine.
#pragma once

#include <string>
#include <vector>

bool fnmatch(const std::string &name, const std::string &pattern);

// Whether a pattern can select more than the one name it spells out. The tool
// leans on this in two places: a plain name is passed through to the file even
// when it is not there yet (so deploy can add it), and the cluster actions
// refuse anything that could pick out more than one IOC.
bool is_glob(const std::string &pattern);

std::vector<std::string> filter(const std::vector<std::string> &names,
                                const std::string &pattern);
