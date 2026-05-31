#pragma once 

#include <string>
#include <unordered_map>
#include <cstdint>

class Config {
public:
    bool loadFromFile(const std::string& path);
    std::string getString(const std::string& key, const std::string& defaultValue = "") const;
    int getInt(const std::string& key, int defaultValue = 0) const;
    uint64_t getUInt64(const std::string& key, uint64_t defaultValue = 0) const;

private:
    std::unordered_map<std::string, std::string> values_;
};