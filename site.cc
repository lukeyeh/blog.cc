#include "site.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "http.h"

namespace fs = std::filesystem;

Response Handle(const Request& request, const fs::path& root) {
  if (request.method != "GET" && request.method != "HEAD") {
    Response response = StatusResponse(405);
    response.headers.emplace_back("Allow", "GET, HEAD");
    return response;
  }
  if (request.path == "/healthz") {
    return Response{.body = "ok\n"};
  }
  return ServeFile(root, request.path);
}

Response ServeFile(const fs::path& root, std::string_view url_path) {
  std::error_code ec;
  fs::path base = fs::canonical(root, ec);
  if (ec) return StatusResponse(500);

  fs::path candidate = base / fs::path(url_path).relative_path();
  if (fs::is_directory(candidate, ec)) {
    // Without the trailing slash, relative links in the page would resolve
    // against the parent directory.
    if (!absl::EndsWith(url_path, "/")) {
      Response response = StatusResponse(301);
      response.headers.emplace_back("Location", absl::StrCat(url_path, "/"));
      return response;
    }
    candidate /= "index.html";
  }

  // canonical() resolves ".." and symlinks, and fails if the file is missing.
  fs::path target = fs::canonical(candidate, ec);
  if (ec || !fs::is_regular_file(target, ec)) return StatusResponse(404);

  fs::path relative = target.lexically_relative(base);
  for (const fs::path& part : relative) {
    // Catches ".." (outside root) as well as dotfiles such as .git.
    if (absl::StartsWith(part.native(), ".")) return StatusResponse(404);
  }

  std::ifstream file(target, std::ios::binary);
  if (!file) return StatusResponse(404);
  std::ostringstream contents;
  contents << file.rdbuf();
  return Response{
      .content_type = std::string(MimeType(target.extension().native())),
      .body = std::move(contents).str()};
}

std::string_view MimeType(std::string_view extension) {
  if (extension == ".html") return "text/html; charset=utf-8";
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".js") return "text/javascript; charset=utf-8";
  if (extension == ".json") return "application/json";
  if (extension == ".txt") return "text/plain; charset=utf-8";
  if (extension == ".xml") return "application/xml";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".gif") return "image/gif";
  if (extension == ".webp") return "image/webp";
  if (extension == ".ico") return "image/x-icon";
  if (extension == ".woff2") return "font/woff2";
  if (extension == ".pdf") return "application/pdf";
  return "application/octet-stream";
}
