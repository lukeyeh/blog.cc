#include "http/message.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

absl::StatusOr<Request> ParseRequest(std::string_view head) {
  // This runs for every request, so it works on views of `head` and avoids
  // allocating.
  const size_t line_end = head.find("\r\n");
  const std::string_view request_line = head.substr(0, line_end);
  const size_t method_end = request_line.find(' ');
  const size_t target_end = request_line.rfind(' ');
  if (method_end == 0 || method_end == std::string_view::npos ||
      method_end == target_end) {
    return absl::InvalidArgumentError("malformed request line");
  }
  const std::string_view target =
      request_line.substr(method_end + 1, target_end - method_end - 1);
  const std::string_view version = request_line.substr(target_end + 1);
  if (absl::StrContains(target, ' ')) {
    return absl::InvalidArgumentError("malformed request line");
  }
  if (!absl::StartsWith(version, "HTTP/1.")) {
    return absl::InvalidArgumentError("unsupported HTTP version");
  }
  if (!absl::StartsWith(target, "/")) {
    return absl::InvalidArgumentError("request target must start with '/'");
  }

  Request request;
  request.method = request_line.substr(0, method_end);
  // HTTP/1.1 connections persist unless a header says otherwise; HTTP/1.0
  // ones do not.
  request.keep_alive = version == "HTTP/1.1";

  const std::string_view path = target.substr(0, target.find('?'));
  if (path.size() < target.size()) {
    request.query = target.substr(path.size() + 1);
  }
  if (absl::StrContains(path, '%')) {
    std::optional<std::string> decoded = PercentDecode(path);
    if (!decoded) {
      return absl::InvalidArgumentError("malformed percent-encoding in path");
    }
    request.path = std::move(*decoded);
  } else {
    request.path = std::string(path);
  }

  if (line_end == std::string_view::npos) return request;
  for (const std::string_view line :
       absl::StrSplit(head.substr(line_end + 2), "\r\n")) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      return absl::InvalidArgumentError("malformed header line");
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value =
        absl::StripAsciiWhitespace(line.substr(colon + 1));
    if (absl::EqualsIgnoreCase(name, "Connection")) {
      if (absl::StrContainsIgnoreCase(value, "close")) {
        request.keep_alive = false;
      }
    } else if ((absl::EqualsIgnoreCase(name, "Content-Length") &&
                value != "0") ||
               absl::EqualsIgnoreCase(name, "Transfer-Encoding")) {
      // A body follows. It is never read, so whatever comes next on this
      // connection cannot be told apart from it.
      request.keep_alive = false;
    }
  }
  return request;
}

absl::StatusOr<ResponseHead> ParseResponseHead(std::string_view head) {
  // The status line is "<version> <status> <reason>".
  const size_t line_end = head.find("\r\n");
  const std::string_view status_line = head.substr(0, line_end);
  const size_t version_end = status_line.find(' ');
  if (!absl::StartsWith(status_line, "HTTP/1.") ||
      version_end == std::string_view::npos) {
    return absl::InvalidArgumentError("malformed status line");
  }
  ResponseHead result;
  if (!absl::SimpleAtoi(status_line.substr(version_end + 1, 3),
                        &result.status)) {
    return absl::InvalidArgumentError("malformed status code");
  }

  if (line_end == std::string_view::npos) return result;
  for (const std::string_view line :
       absl::StrSplit(head.substr(line_end + 2), "\r\n")) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      return absl::InvalidArgumentError("malformed header line");
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value =
        absl::StripAsciiWhitespace(line.substr(colon + 1));
    if (absl::EqualsIgnoreCase(name, "Content-Length")) {
      if (!absl::SimpleAtoi(value, &result.content_length)) {
        return absl::InvalidArgumentError("malformed Content-Length");
      }
    } else if (absl::EqualsIgnoreCase(name, "Transfer-Encoding")) {
      return absl::UnimplementedError(
          "responses without a Content-Length are not supported");
    }
  }
  return result;
}

void AppendResponseHead(const Response& response, bool keep_alive,
                        std::string* head) {
  absl::StrAppend(head, "HTTP/1.1 ", response.status, " ",
                  ReasonPhrase(response.status), "\r\n",
                  "Content-Type: ", response.content_type, "\r\n",
                  "Content-Length: ", response.body.view().size(), "\r\n");
  // Keeping the connection open is the default and needs no header.
  if (!keep_alive) absl::StrAppend(head, "Connection: close\r\n");
  for (const auto& [name, value] : response.headers) {
    absl::StrAppend(head, name, ": ", value, "\r\n");
  }
  absl::StrAppend(head, "\r\n");
}

std::optional<std::string> PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out += text[i];
      continue;
    }
    std::string byte;
    if (i + 2 >= text.size()) return std::nullopt;
    if (!absl::HexStringToBytes(text.substr(i + 1, 2), &byte)) {
      return std::nullopt;
    }
    if (byte[0] == '\0') return std::nullopt;
    out += byte;
    i += 2;
  }
  return out;
}

std::string_view ReasonPhrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default: return "Unknown";
  }
}

Response StatusResponse(int status) {
  return Response{
      .status = status,
      .body = absl::StrCat(ReasonPhrase(status), "\n"),
  };
}
