#include "shell/HttpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace folio::shell {
namespace {

bool sendAll(int fd, const char* data, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        // MSG_NOSIGNAL: a browser closing the tab mid-frame must return
        // EPIPE, not kill the process with SIGPIPE.
        const ssize_t k = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(k);
    }
    return true;
}

std::string statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        default:  return "Error";
    }
}

}  // namespace

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(std::uint16_t port, int workerThreads) {
    (void)workerThreads;
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // 0.0.0.0, not loopback: the preview proxy reaches us from outside the
    // container's network namespace.
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[folio] bind :%u failed: %s\n", port, std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 32) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    port_ = port;
    running_ = true;
    acceptor_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (acceptor_.joinable()) acceptor_.join();
    for (auto& t : workers_) {
        if (t.joinable()) t.detach();  // streaming threads exit on write failure
    }
    workers_.clear();
}

void HttpServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) {
            if (!running_.load()) break;
            if (errno == EINTR) continue;
            break;
        }
        // One thread per connection. The client count here is 1-2 (a browser
        // tab), so a thread pool would be more machinery than it saves.
        workers_.emplace_back([this, fd] {
            handleConnection(fd);
            ::close(fd);
        });
        // Reap finished threads occasionally so the vector does not grow
        // without bound over a long session.
        if (workers_.size() > 64) {
            for (auto& t : workers_) {
                if (t.joinable()) t.detach();
            }
            workers_.clear();
        }
    }
}

void HttpServer::handleConnection(int fd) {
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    std::string buf;
    buf.reserve(2048);
    char chunk[2048];

    // Read until the end of headers.
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return;
        buf.append(chunk, static_cast<std::size_t>(n));
        headerEnd = buf.find("\r\n\r\n");
        if (buf.size() > 64 * 1024) return;  // header flood
    }

    // Request line.
    const std::size_t eol = buf.find("\r\n");
    std::string line = buf.substr(0, eol);
    HttpRequest req;
    {
        const std::size_t s1 = line.find(' ');
        const std::size_t s2 = line.find(' ', s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos) return;
        req.method = line.substr(0, s1);
        std::string target = line.substr(s1 + 1, s2 - s1 - 1);
        const std::size_t q = target.find('?');
        if (q == std::string::npos) {
            req.path = target;
        } else {
            req.path = target.substr(0, q);
            req.query = target.substr(q + 1);
        }
    }

    // Content-Length (case-insensitive scan of the header block).
    std::size_t contentLength = 0;
    {
        const std::string headers = buf.substr(0, headerEnd);
        std::string lower = headers;
        for (char& c : lower) c = static_cast<char>(::tolower(c));
        const std::size_t p = lower.find("content-length:");
        if (p != std::string::npos) {
            contentLength = static_cast<std::size_t>(
                std::strtoul(headers.c_str() + p + 15, nullptr, 10));
        }
    }
    if (contentLength > 4 * 1024 * 1024) return;

    std::string body = buf.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        body.append(chunk, static_cast<std::size_t>(n));
    }
    req.body = std::move(body);

    // Streaming routes take over the socket entirely.
    if (auto it = streams_.find(req.path); it != streams_.end()) {
        it->second(fd, running_);
        return;
    }

    HttpResponse res;
    if (auto it = routes_.find(req.path); it != routes_.end()) {
        it->second(req, res);
    } else {
        res.status = 404;
        res.body = "not found";
    }

    std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " +
                       statusText(res.status) + "\r\n";
    head += "Content-Type: " + res.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    // No X-Frame-Options / CSP frame-ancestors: the preview renders this
    // inside an iframe, and sending either would blank it.
    head += "Cache-Control: no-store\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    for (const auto& [k, v] : res.extraHeaders) head += k + ": " + v + "\r\n";
    head += "Connection: close\r\n\r\n";

    if (!sendAll(fd, head.data(), head.size())) return;
    sendAll(fd, res.body.data(), res.body.size());
}

}  // namespace folio::shell
