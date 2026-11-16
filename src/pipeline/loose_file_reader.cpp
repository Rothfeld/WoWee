#include "pipeline/loose_file_reader.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>

namespace wowee {
namespace pipeline {

std::vector<uint8_t> LooseFileReader::readFile(const std::string& filesystemPath) {
    if (!fileExists(filesystemPath)) {
        LOG_WARNING("Asset path is not a regular file: ", filesystemPath);
        return {};
    }
    std::ifstream file(filesystemPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    auto size = file.tellg();
    if (size <= 0) {
        return {};
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (!file.good()) {
        LOG_WARNING("Incomplete read of: ", filesystemPath);
        data.resize(static_cast<size_t>(file.gcount()));
    }

    return data;
}

bool LooseFileReader::fileExists(const std::string& filesystemPath) {
    std::error_code ec;
    return std::filesystem::is_regular_file(filesystemPath, ec);
}
} // namespace pipeline
} // namespace wowee
