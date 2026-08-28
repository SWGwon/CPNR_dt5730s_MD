#ifndef CONFIGPARSER_H
#define CONFIGPARSER_H

#include <iostream>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <map>
#include <set>
#include <stdexcept>

class ConfigParser {
private:
    std::map<std::string, std::map<std::string, std::string>> data_;

    // [핵심] 문자열 양끝의 스페이스, 탭, 캐리지리턴(\r) 및 복붙 시 딸려오는 NBSP 제거
    static std::string trim(const std::string& str) {
        std::string s = str;
        // UTF-8 Non-Breaking Space(C2 A0)를 일반 공백으로 치환하여 파괴
        size_t pos;
        while ((pos = s.find("\xC2\xA0")) != std::string::npos) {
            s.replace(pos, 2, " ");
        }
        
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, (last - first + 1));
    }

    void Parse(std::istream& file, const std::string& source_name) {
        std::string line, current_section;
        std::set<std::string> declared_sections;
        size_t line_number = 0;
        while (std::getline(file, line)) {
            ++line_number;
            line = trim(line);

            // 주석(#, ;)이나 빈 줄은 가볍게 무시
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line.front() == '[') {
                if (line.back() != ']') {
                    throw std::runtime_error("Malformed config section in " +
                                             source_name + " at line " +
                                             std::to_string(line_number));
                }
                current_section = trim(line.substr(1, line.size() - 2));
                if (current_section.empty()) {
                    throw std::runtime_error("Empty config section in " +
                                             source_name + " at line " +
                                             std::to_string(line_number));
                }
                if (!declared_sections.insert(current_section).second) {
                    throw std::runtime_error("Duplicate config section [" +
                                             current_section + "] in " +
                                             source_name + " at line " +
                                             std::to_string(line_number));
                }
            } else {
                size_t eq_pos = line.find('=');
                if (eq_pos == std::string::npos || current_section.empty()) {
                    throw std::runtime_error("Malformed config entry in " +
                                             source_name + " at line " +
                                             std::to_string(line_number));
                }

                std::string key = trim(line.substr(0, eq_pos));
                std::string val = trim(line.substr(eq_pos + 1));
                if (key.empty() || val.empty()) {
                    throw std::runtime_error("Empty config key or value in " +
                                             source_name + " at line " +
                                             std::to_string(line_number));
                }

                auto& section = data_[current_section];
                if (section.count(key)) {
                    throw std::runtime_error("Duplicate config key [" +
                                             current_section + "] " + key +
                                             " in " + source_name + " at line " +
                                             std::to_string(line_number));
                }
                section[key] = val;
            }
        }

        if (file.bad()) {
            throw std::runtime_error("Error while reading config: " + source_name);
        }
        if (data_.empty()) {
            throw std::runtime_error("Config contains no settings: " + source_name);
        }
    }

public:
    explicit ConfigParser(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file: " + filename);
        }
        Parse(file, filename);
    }

    static ConfigParser FromText(const std::string& contents,
                                 const std::string& source_name) {
        std::istringstream input(contents);
        ConfigParser parsed;
        parsed.Parse(input, source_name);
        return parsed;
    }

    int GetInt(const std::string& section, const std::string& key, int default_val) const {
        auto section_it = data_.find(section);
        if (section_it == data_.end()) return default_val;

        auto value_it = section_it->second.find(key);
        if (value_it == section_it->second.end()) return default_val;
        return ParseInt(section, key, value_it->second);
    }

    int GetRequiredInt(const std::string& section, const std::string& key,
                       int min_value = std::numeric_limits<int>::min(),
                       int max_value = std::numeric_limits<int>::max()) const {
        auto section_it = data_.find(section);
        if (section_it == data_.end()) {
            throw std::runtime_error("Missing required config section [" + section + "]");
        }

        auto value_it = section_it->second.find(key);
        if (value_it == section_it->second.end()) {
            throw std::runtime_error("Missing required config key [" + section + "] " + key);
        }

        int value = ParseInt(section, key, value_it->second);
        if (value < min_value || value > max_value) {
            throw std::runtime_error("Config value out of range [" + section + "] " + key +
                                     "=" + value_it->second + " (expected " +
                                     std::to_string(min_value) + ".." +
                                     std::to_string(max_value) + ")");
        }
        return value;
    }

    double GetDouble(const std::string& section, const std::string& key, double default_val) const {
        auto section_it = data_.find(section);
        if (section_it == data_.end()) return default_val;

        auto value_it = section_it->second.find(key);
        if (value_it == section_it->second.end()) return default_val;

        try {
            size_t consumed = 0;
            double value = std::stod(value_it->second, &consumed);
            if (consumed != value_it->second.size()) throw std::invalid_argument("trailing characters");
            return value;
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid floating-point config value [" + section +
                                     "] " + key + "=" + value_it->second);
        }
    }

    std::string GetString(const std::string& section, const std::string& key,
                          const std::string& default_val) const {
        auto section_it = data_.find(section);
        if (section_it == data_.end()) return default_val;

        auto value_it = section_it->second.find(key);
        if (value_it == section_it->second.end()) return default_val;
        return value_it->second;
    }

private:
    ConfigParser() = default;

    static int ParseInt(const std::string& section, const std::string& key,
                        const std::string& raw_value) {
        try {
            size_t consumed = 0;
            long long value = std::stoll(raw_value, &consumed, 10);
            if (consumed != raw_value.size() ||
                value < std::numeric_limits<int>::min() ||
                value > std::numeric_limits<int>::max()) {
                throw std::invalid_argument("not a complete integer");
            }
            return static_cast<int>(value);
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid integer config value [" + section + "] " +
                                     key + "=" + raw_value);
        }
    }
};

#endif
