#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

struct TocFile {
    std::string addonName;
    std::string basePath;

    std::unordered_map<std::string, std::string> directives;
    std::vector<std::string> files;

    [[nodiscard]] std::string getTitle() const;
    [[nodiscard]] bool isLoadOnDemand() const;
    [[nodiscard]] std::vector<std::string> getSavedVariables() const;
    [[nodiscard]] std::vector<std::string> getSavedVariablesPerCharacter() const;
    /// Addons that must load first; without them this one does not load.
    [[nodiscard]] std::vector<std::string> getRequiredDeps() const;
    /// Addons loaded first when present and enabled.
    [[nodiscard]] std::vector<std::string> getOptionalDeps() const;
};

std::optional<TocFile> parseTocFile(const std::string& tocPath);

} // namespace wowee::addons
