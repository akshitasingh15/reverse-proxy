#pragma once
// IP-based access control with CIDR rules (e.g. 10.0.0.0/8).
// Policy: deny rules always win. If any allow rule exists, the proxy is
// default-deny: only clients matching an allow rule get through.
// This is the "never trust, always verify" idea behind zero trust:
// every request is checked, none is trusted because of where it came from.

#include <arpa/inet.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

class AccessControl {
public:
    void add_allow(const std::string& cidr) { allow_.push_back(parse(cidr)); }
    void add_deny(const std::string& cidr) { deny_.push_back(parse(cidr)); }

    bool is_allowed(const std::string& ip_str) const {
        in_addr addr{};
        if (inet_pton(AF_INET, ip_str.c_str(), &addr) != 1) return false;
        uint32_t ip = ntohl(addr.s_addr);

        for (const auto& r : deny_) if (r.matches(ip)) return false;
        if (allow_.empty()) return true;
        for (const auto& r : allow_) if (r.matches(ip)) return true;
        return false;
    }

private:
    struct Rule {
        uint32_t network;
        uint32_t mask;
        bool matches(uint32_t ip) const { return (ip & mask) == network; }
    };

    static Rule parse(const std::string& cidr) {
        size_t slash = cidr.find('/');
        std::string ip_part = cidr.substr(0, slash);
        int prefix = slash == std::string::npos ? 32 : std::stoi(cidr.substr(slash + 1));
        if (prefix < 0 || prefix > 32) throw std::invalid_argument("bad CIDR prefix: " + cidr);

        in_addr addr{};
        if (inet_pton(AF_INET, ip_part.c_str(), &addr) != 1) {
            throw std::invalid_argument("bad IP in rule: " + cidr);
        }
        // /0 must be handled separately: shifting a 32-bit value by 32 is undefined.
        uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
        return Rule{ntohl(addr.s_addr) & mask, mask};
    }

    std::vector<Rule> allow_;
    std::vector<Rule> deny_;
};
