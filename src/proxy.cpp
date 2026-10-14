// Multithreaded HTTP reverse proxy with load balancing, rate limiting
// and IP-based access control.
//
// Request path:
//   accept() -> thread pool -> access control (403) -> rate limiter (429)
//            -> read request -> round-robin backend (failover, 502) -> relay response

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "access_control.h"
#include "config.h"
#include "load_balancer.h"
#include "rate_limiter.h"
#include "thread_pool.h"

constexpr int BUFFER_SIZE = 8192;
constexpr size_t MAX_HEADER_BYTES = 64 * 1024;  // reject oversized headers

std::mutex log_mutex;  // keeps log lines from different threads from interleaving

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << msg << std::endl;
}

// ---------- socket helpers ----------

int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, SOMAXCONN) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

// Without timeouts, a client that connects and never sends anything would hold
// a worker thread forever (a "Slowloris" attack). Timeouts free the thread.
void set_timeouts(int fd, int seconds) {
    timeval tv{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// getaddrinfo resolves hostnames too, e.g. "backend1" inside Docker Compose.
int connect_to_backend(const Backend& b, int timeout_sec) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(b.host.c_str(), std::to_string(b.port).c_str(), &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
        set_timeouts(fd, timeout_sec);
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) { close(fd); fd = -1; }
    }
    freeaddrinfo(res);
    return fd;
}

bool send_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}

void send_error(int fd, int code, const std::string& reason, const std::string& extra_headers = "") {
    std::string body = std::to_string(code) + " " + reason + "\n";
    std::string resp = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n" +
                       "Content-Type: text/plain\r\n" +
                       "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                       extra_headers + "Connection: close\r\n\r\n" + body;
    send_all(fd, resp.data(), resp.size());
}

// ---------- HTTP handling ----------

enum class ReadResult { Ok, Closed, TooLarge, BadRequest };

// Reads headers until "\r\n\r\n", then Content-Length bytes of body.
ReadResult read_request(int fd, std::string& request) {
    char buf[BUFFER_SIZE];
    size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return ReadResult::Closed;
        request.append(buf, n);
        if (request.size() > MAX_HEADER_BYTES) return ReadResult::TooLarge;
        header_end = request.find("\r\n\r\n");
    }

    size_t body_len = 0;
    std::string headers = request.substr(0, header_end);
    for (auto& c : headers) c = tolower(c);
    size_t pos = headers.find("\r\ncontent-length:");
    if (pos != std::string::npos) {
        try {
            body_len = std::stoul(headers.substr(pos + 17));
        } catch (...) {
            return ReadResult::BadRequest;
        }
    }

    size_t total = header_end + 4 + body_len;
    while (request.size() < total) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return ReadResult::Closed;
        request.append(buf, n);
    }
    return ReadResult::Ok;
}

// Rebuilds the request for the backend:
//  - drops hop-by-hop headers (Connection, Keep-Alive, Proxy-Connection), which
//    describe the client<->proxy link and must not be forwarded
//  - adds X-Forwarded-For so the backend knows the real client IP
//  - adds Connection: close so "backend closed the socket" = "response finished"
std::string rewrite_request(const std::string& request, const std::string& client_ip) {
    size_t header_end = request.find("\r\n\r\n");
    std::istringstream head(request.substr(0, header_end));
    std::string body = request.substr(header_end + 4);

    std::string out, line;
    std::getline(head, line);  // request line, e.g. "GET /path HTTP/1.1\r"
    out += line + "\n";

    std::string existing_xff;
    while (std::getline(head, line)) {
        std::string lower = line;
        for (auto& c : lower) c = tolower(c);
        if (lower.rfind("connection:", 0) == 0 || lower.rfind("keep-alive:", 0) == 0 ||
            lower.rfind("proxy-connection:", 0) == 0) {
            continue;
        }
        if (lower.rfind("x-forwarded-for:", 0) == 0) {
            existing_xff = trim(line.substr(16));
            continue;
        }
        out += line + "\n";
    }
    std::string xff = existing_xff.empty() ? client_ip : existing_xff + ", " + client_ip;
    out += "X-Forwarded-For: " + xff + "\r\n";
    out += "Connection: close\r\n\r\n";
    return out + body;
}

// ---------- the proxy ----------

struct Proxy {
    Config cfg;
    LoadBalancer lb;
    RateLimiter limiter;
    AccessControl acl;

    explicit Proxy(Config c)
        : cfg(std::move(c)), lb(cfg.backends), limiter(cfg.rate_capacity, cfg.rate_refill_per_sec) {
        for (const auto& r : cfg.allow) acl.add_allow(r);
        for (const auto& r : cfg.deny) acl.add_deny(r);
    }

    void handle_client(int client_fd, const std::string& client_ip) {
        set_timeouts(client_fd, cfg.timeout_sec);

        // 1. Access control first: blocked clients cost almost nothing.
        if (!acl.is_allowed(client_ip)) {
            send_error(client_fd, 403, "Forbidden");
            log_line("[deny ] " + client_ip + " 403 blocked by access policy");
            return;
        }
        // 2. Rate limit before reading the request or touching a backend.
        if (!limiter.allow(client_ip)) {
            send_error(client_fd, 429, "Too Many Requests", "Retry-After: 1\r\n");
            log_line("[limit] " + client_ip + " 429 rate limit exceeded");
            return;
        }

        std::string request;
        ReadResult r = read_request(client_fd, request);
        if (r == ReadResult::TooLarge) { send_error(client_fd, 431, "Request Header Fields Too Large"); return; }
        if (r == ReadResult::BadRequest) { send_error(client_fd, 400, "Bad Request"); return; }
        if (r != ReadResult::Ok) return;

        std::string request_line = request.substr(0, request.find("\r\n"));
        std::string forwarded = rewrite_request(request, client_ip);

        // 3. Round-robin, failing over to the next backend if one is down.
        size_t start = lb.next_start();
        for (size_t attempt = 0; attempt < lb.size(); ++attempt) {
            const Backend& b = lb.at(start + attempt);
            int backend_fd = connect_to_backend(b, cfg.timeout_sec);
            if (backend_fd < 0) {
                log_line("[warn ] backend " + b.host + ":" + std::to_string(b.port) + " unreachable, failing over");
                continue;
            }
            if (send_all(backend_fd, forwarded.data(), forwarded.size())) {
                char buf[BUFFER_SIZE];
                ssize_t n;
                while ((n = recv(backend_fd, buf, sizeof(buf), 0)) > 0) {
                    if (!send_all(client_fd, buf, n)) break;
                }
            }
            close(backend_fd);
            log_line("[ok   ] " + client_ip + " " + request_line + " -> " + b.host + ":" + std::to_string(b.port));
            return;
        }

        send_error(client_fd, 502, "Bad Gateway");
        log_line("[error] " + client_ip + " " + request_line + " -> 502 all backends down");
    }
};

int main(int argc, char* argv[]) {
    std::string config_path = argc > 1 ? argv[1] : "config/proxy.conf";
    signal(SIGPIPE, SIG_IGN);  // a client disconnecting mid-write must not kill the process

    Config cfg;
    try {
        cfg = load_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << std::endl;
        return 1;
    }

    Proxy proxy(cfg);
    ThreadPool pool(cfg.threads, cfg.max_queue);

    int listen_fd = create_listener(cfg.listen_port);
    if (listen_fd < 0) return 1;

    std::cout << "[proxy] listening on :" << cfg.listen_port << " with " << cfg.threads
              << " worker threads, " << cfg.backends.size() << " backends" << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
        if (client_fd < 0) { perror("accept"); continue; }

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string client_ip = ip_buf;

        bool queued = pool.submit([&proxy, client_fd, client_ip] {
            proxy.handle_client(client_fd, client_ip);
            close(client_fd);
        });
        if (!queued) {  // overloaded: shed load quickly instead of queueing forever
            send_error(client_fd, 503, "Service Unavailable");
            close(client_fd);
        }
    }
}
