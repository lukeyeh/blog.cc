#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "http.h"

namespace {

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr int kTimeoutSeconds = 10;

absl::Status ErrnoStatus(std::string_view what) {
  return absl::UnavailableError(absl::StrCat(what, ": ", strerror(errno)));
}

void SendAll(int fd, std::string_view data) {
  while (!data.empty()) {
    // MSG_NOSIGNAL: a client that hung up should not kill us with SIGPIPE.
    ssize_t n = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (n <= 0) return;
    data.remove_prefix(n);
  }
}

void HandleConnection(int fd, const Handler& handler) {
  // Bounds how long a slow or silent client can hold this thread.
  timeval timeout{.tv_sec = kTimeoutSeconds, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  std::string buffer;
  char chunk[4096];
  size_t head_end;
  while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
    if (buffer.size() > kMaxHeaderBytes) {
      SendAll(fd, SerializeResponse(StatusResponse(431)));
      close(fd);
      return;
    }
    ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      close(fd);
      return;
    }
    buffer.append(chunk, n);
  }

  absl::StatusOr<Request> request =
      ParseRequest(std::string_view(buffer).substr(0, head_end));
  if (request.ok()) {
    Response response = handler(*request);
    LOG(INFO) << request->method << " " << request->path << " "
              << response.status;
    SendAll(fd, SerializeResponse(response, request->method != "HEAD"));
  } else {
    LOG(INFO) << "bad request: " << request.status().message();
    SendAll(fd, SerializeResponse(StatusResponse(400)));
  }
  close(fd);
}

}  // namespace

absl::Status Serve(const std::string& address, uint16_t port,
                   const Handler& handler) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("not an IPv4 address: ", address));
  }

  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return ErrnoStatus("socket");
  int enable = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      listen(listener, SOMAXCONN) < 0) {
    absl::Status status = ErrnoStatus(absl::StrCat(address, ":", port));
    close(listener);
    return status;
  }
  LOG(INFO) << "listening on http://" << address << ":" << port;

  for (;;) {
    int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) {
      if (errno != EINTR) PLOG(WARNING) << "accept";
      continue;
    }
    std::thread(HandleConnection, fd, handler).detach();
  }
}
