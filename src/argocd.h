#pragma once

#include <string>
#include <vector>

// Validate before editing; parent may be namespace/name, children are service names.
void check_sync_names(const std::string &parent, const std::vector<std::string> &services);
void prepare_argocd(const std::string &parent);
void sync_apps(const std::string &parent, const std::vector<std::string> &services,
               bool dry_run);
