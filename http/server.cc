#include "http/server.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "http/connection.h"
#include "net/event_loop.h"
#include "net/tcp.h"

namespace {

// Serves every client that `listener` hands to the calling thread.
Task<> ServeClients(const TcpListener& listener, const Handler& handler) {
  for (;;) {
    Spawn(ServeConnection(co_await listener.Accept(), handler));
  }
}

}  // namespace

absl::StatusOr<HttpServer> HttpServer::Listen(const std::string& address,
                                              uint16_t port) {
  ABSL_ASSIGN_OR_RETURN(TcpListener listener,
                        TcpListener::Listen(address, port));
  return HttpServer(std::make_unique<TcpListener>(std::move(listener)));
}

HttpServer::HttpServer(std::unique_ptr<TcpListener> listener)
    : listener_(std::move(listener)) {}
HttpServer::HttpServer(HttpServer&&) = default;
HttpServer& HttpServer::operator=(HttpServer&&) = default;
HttpServer::~HttpServer() = default;

uint16_t HttpServer::port() const { return listener_->port(); }

absl::Status HttpServer::Run(const Handler& handler) {
  // One event loop per core, all accepting from the same listener. A
  // connection stays on the thread that accepted it, so the threads share
  // nothing but the listener and the handler. Everything that can fail is
  // done before the first thread starts.
  std::vector<EventLoop> loops;
  loops.reserve(std::thread::hardware_concurrency());
  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i) {
    ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());
    loops.push_back(std::move(loop));
  }

  LOG(INFO) << "serving on " << loops.size() << " threads using "
            << loops.front().io_backend();

  std::vector<std::jthread> threads;
  threads.reserve(loops.size());
  for (EventLoop& loop : loops) {
    threads.emplace_back([&] { loop.Run(ServeClients(*listener_, handler)); });
  }
  // Joining the threads, which never finish, is what keeps Run from
  // returning.
  threads.clear();
  return absl::InternalError("server threads exited");
}
