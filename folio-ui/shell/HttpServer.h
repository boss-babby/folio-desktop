// folio-ui/shell/HttpServer.h
//
// A small threaded HTTP/1.1 server, enough to carry an MJPEG frame stream
// and an input channel.
//
// WHY NOT A LIBRARY
// -----------------
// Pulling in a web framework for this would add a dependency with its own
// licence to audit, for roughly 200 lines of socket code that never faces
// the public internet. It binds to the loopback/sandbox interface, serves
// three fixed routes and has no parser surface beyond the request line and
// Content-Length. Anything more would be a liability, not a feature.
//
// MJPEG (multipart/x-mixed-replace) is the transport because it needs zero
// client-side JavaScript to display - the browser decodes it in an <img> -
// and degrades gracefully. The real product does not stream anything; this
// is how a headless build makes itself visible.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace folio::shell {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};

class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
    /// Takes ownership of the socket for as long as it returns true. Used by
    /// the never-ending MJPEG response.
    using StreamHandler = std::function<void(int socketFd, std::atomic<bool>& running)>;

    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void route(const std::string& path, Handler h) { routes_[path] = std::move(h); }
    void streamRoute(const std::string& path, StreamHandler h) {
        streams_[path] = std::move(h);
    }

    /// Binds 0.0.0.0:port. Returns false if the port is taken.
    bool start(std::uint16_t port, int workerThreads = 4);
    void stop();
    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void acceptLoop();
    void handleConnection(int fd);

    std::map<std::string, Handler> routes_;
    std::map<std::string, StreamHandler> streams_;
    std::vector<std::thread> workers_;
    std::thread acceptor_;
    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
};

}  // namespace folio::shell
