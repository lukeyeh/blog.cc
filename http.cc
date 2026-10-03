#include "http.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

absl::StatusOr<Request> ParseRequest(std::string_view head) {
  std::vector<std::string_view> lines = absl::StrSplit(head, "\r\n");
  std::vector<std::string_view> parts = absl::StrSplit(lines[0], ' ');
  if (parts.size() != 3) {
    return absl::InvalidArgumentError("malformed request line");
  }
  if (!absl::StartsWith(parts[2], "HTTP/1.")) {
    return absl::InvalidArgumentError("unsupported HTTP version");
  }

  Request request;
  request.method = std::string(parts[0]);

  std::string_view target = parts[1];
  if (!absl::StartsWith(target, "/")) {
    return absl::InvalidArgumentError("request target must start with '/'");
  }
  std::string_view path = target.substr(0, target.find('?'));
  if (path.size() < target.size()) {
    request.query = std::string(target.substr(path.size() + 1));
  }
  std::optional<std::string> decoded = PercentDecode(path);
  if (!decoded) {
    return absl::InvalidArgumentError("malformed percent-encoding in path");
  }
  request.path = std::move(*decoded);

  for (size_t i = 1; i < lines.size(); ++i) {
    size_t colon = lines[i].find(':');
    if (colon == std::string_view::npos) {
      return absl::InvalidArgumentError("malformed header line");
    }
    request.headers[absl::AsciiStrToLower(lines[i].substr(0, colon))] =
        std::string(absl::StripAsciiWhitespace(lines[i].substr(colon + 1)));
  }
  return request;
}

std::string SerializeResponse(const Response& response, bool include_body) {
  std::string out =
      absl::StrCat("HTTP/1.1 ", response.status, " ",
                   ReasonPhrase(response.status), "\r\n",
                   "Content-Type: ", response.content_type, "\r\n",
                   "Content-Length: ", response.body.size(), "\r\n",
                   "Connection: close\r\n");
  for (const auto& [name, value] : response.headers) {
    absl::StrAppend(&out, name, ": ", value, "\r\n");
  }
  out += "\r\n";
  if (include_body) out += response.body;
  return out;
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
  return Response{.status = status,
                  .body = absl::StrCat(ReasonPhrase(status), "\n")};
}
