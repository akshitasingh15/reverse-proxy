#pragma once
// Loads proxy settings from a simple "key = value" config file.
// Lines starting with # are comments. `backend`, `allow` and `deny` may repeat.

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "load_balancer.h"

struct Config {
    int listen_port = 8080;
    size_t threads = 8;
    size_t max_queue = 1024;
    int timeout_sec = 5;
    double rate_capacity = 20;      // burst size per client (0 = disabled)
    double rate_refill_per_sec = 10;  // sustained requests/sec per client
    std::vector<Backend> backends;
    std::vector<std::string> allow;
    std::vector<std::string> deny;
};

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

inline Config load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path);

    Config cfg;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error("config line " + std::to_string(line_no) + ": expected key = value");
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "listen_port") cfg.listen_port = std::stoi(val);
        else if (key == "threads") cfg.threads = std::stoul(val);
        else if (key == "max_queue") cfg.max_queue = std::stoul(val);
        else if (key == "timeout_sec") cfg.timeout_sec = std::stoi(val);
        else if (key == "rate_limit_capacity") cfg.rate_capacity = std::stod(val);
        else if (key == "rate_limit_refill_per_sec") cfg.rate_refill_per_sec = std::stod(val);
        else if (key == "allow") cfg.allow.push_back(val);
        else if (key == "deny") cfg.deny.push_back(val);
        else if (key == "backend") {
            size_t colon = val.rfind(':');
            if (colon == std::string::npos) throw std::runtime_error("backend must be host:port, got " + val);
            cfg.backends.push_back({val.substr(0, colon), std::stoi(val.substr(colon + 1))});
        } else {
            throw std::runtime_error("config line " + std::to_string(line_no) + ": unknown key '" + key + "'");
        }
    }
    if (cfg.backends.empty()) throw std::runtime_error("config needs at least one backend");
    if (cfg.threads == 0) throw std::runtime_error("threads must be > 0");
    return cfg;
}
