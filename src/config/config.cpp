#include "aegisflow/config/config.hpp"
#include "aegisflow/log/logger.hpp"

#include <algorithm>
#include <string>
#include <fstream>
#include <iostream>
#include <exception>
#include <cstdint>

namespace aegisflow::config {

namespace {
    // 去除字符串首尾空格
    std::string trim(const std::string& s) {
        auto begin = std::find_if_not(s.begin(), s.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char ch) {
            return std::isspace(ch);
        }).base();

        if (begin >= end) {
            return "";
        }

        return std::string(begin, end);
    }

    // 判断行是否是注释或空行
    bool isCommentOrEmptyLine(const std::string& line) {
        std::string t = trim(line);
        return t.empty() || t[0] == '#';
    }
} //namespace

bool Config::loadFromFile(const std::string& path)
{
    std::ifstream file(path);
    
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << path << std::endl;
        return false;
    }

    values_.clear();

    std::string line;
    size_t line_no = 0;

    while (std::getline(file, line)) {
        ++ line_no;

        if (isCommentOrEmptyLine(line)) {
            continue;
        }

        auto pos = line.find('=');

        if (pos == std::string::npos) {
            std::cerr << "Invalid config line " << line_no << ": " << line << std::endl;
            continue;
        }

        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));

        if (key.empty()) {
            std::cerr << "Empty key in config line " << line_no << ": " << line << std::endl;
            continue;
        }

        values_[key] = value;
    }

    return true;
}

std::string Config::getString(const std::string& key, const std::string& defaultValue) const {
    auto it = values_.find(key);
    return it != values_.end() ? it->second : defaultValue;
}

int Config::getInt(const std::string& key, const int defaultValue) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return defaultValue;
    }

    try {
        size_t idx = 0;
        int value = std::stoi(it->second, &idx);
        if (idx != it->second.size()) {
            return defaultValue;
        }
        return value;
    } catch (const std::exception&) {
        return defaultValue;
    }
}

uint64_t Config::getUInt64(const std::string& key, const uint64_t defaultValue) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return defaultValue;
    }

    try {
        size_t idx = 0;
        uint64_t value = std::stoull(it->second, &idx);
        if (idx != it->second.size()) {
            return defaultValue;
        }
        return value;
    } catch (const std::exception&) {
        return defaultValue;
    }
}

} //namespace aegisflow::config