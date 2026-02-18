#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

class AlpineCoreConfig
{
public:
    // Configurable fields
    bool vsync = false;

    std::vector<std::string> orphaned_lines;
    bool load(const std::string& filename = "rf2patch_system.ini");
    void save(const std::string& filename = "rf2patch_system.ini") const;

private:
    static bool string_to_bool(const std::string& str);
    static std::string bool_to_string(bool val);
};
