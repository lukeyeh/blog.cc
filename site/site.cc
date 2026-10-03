#include "site/site.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "http/message.h"

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kIndexFile = "index.html";

// Resolves `root` as described at Site::Load.
absl::StatusOr<fs::path> FindRoot(const fs::path& root) {
  fs::path resolved = root;
  // `bazel run` sets this to the directory the user ran it from. It is unset
  // when the binary is started directly, and then relative paths already
  // resolve against the right directory.
  if (const char* const launch_directory =
          std::getenv("BUILD_WORKING_DIRECTORY");
      launch_directory != nullptr && root.is_relative()) {
    resolved = fs::path(launch_directory) / root;
  }

  std::error_code ec;
  resolved = fs::canonical(resolved, ec);
  if (ec || !fs::is_directory(resolved, ec)) {
    return absl::NotFoundError(
        absl::StrCat("not a directory: ", root.native()));
  }
  return resolved;
}

bool IsHidden(const fs::path& name) {
  return absl::StartsWith(name.native(), ".");
}

// Whether `file`, a path under `root`, may be served: it must really live
// under `root` once symbolic links are followed, at a path with no hidden
// component.
bool IsServable(const fs::path& file, const fs::path& root) {
  std::error_code ec;
  const fs::path target = fs::canonical(file, ec);
  if (ec) return false;
  for (const fs::path& part : target.lexically_relative(root)) {
    // Catches ".." (outside root) as well as dotfiles such as .git.
    if (IsHidden(part)) return false;
  }
  return true;
}

absl::StatusOr<std::string> ReadFile(const fs::path& path) {
  const std::ifstream file(path, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();
  if (!file) {
    return absl::UnavailableError(absl::StrCat("cannot read ", path.native()));
  }
  return std::move(contents).str();
}

}  // namespace

absl::StatusOr<Site> Site::Load(const fs::path& root) {
  Site site;
  ABSL_ASSIGN_OR_RETURN(site.root_, FindRoot(root));

  std::error_code ec;
  for (fs::recursive_directory_iterator entry(site.root_, ec), end;
       !ec && entry != end; entry.increment(ec)) {
    const fs::path& path = entry->path();
    const std::string url =
        absl::StrCat("/", path.lexically_relative(site.root_).generic_string());

    if (IsHidden(path.filename())) {
      if (entry->is_directory(ec)) entry.disable_recursion_pending();
    } else if (entry->is_directory(ec)) {
      // The iterator does not descend into linked directories, so they have
      // nothing to redirect to.
      if (!entry->is_symlink(ec)) site.directories_.insert(url);
    } else if (entry->is_regular_file(ec) && IsServable(path, site.root_)) {
      ABSL_ASSIGN_OR_RETURN(std::string contents, ReadFile(path));
      const File file = {
          .content_type = MimeType(path.extension().native()),
          .contents = std::move(contents),
      };
      if (path.filename() == kIndexFile) {
        site.files_[url.substr(0, url.size() - kIndexFile.size())] = file;
      }
      site.files_[url] = file;
      ++site.file_count_;
    }
  }
  if (ec) {
    return absl::UnavailableError(
        absl::StrCat("cannot list ", site.root_.native(), ": ", ec.message()));
  }
  return site;
}

Response Site::Handle(const Request& request) const {
  if (request.method != "GET" && request.method != "HEAD") {
    Response response = StatusResponse(405);
    response.headers.emplace_back("Allow", "GET, HEAD");
    return response;
  }
  if (request.path == "/healthz") {
    return Response{
        .body = Body::Borrowed("ok\n"),
    };
  }
  if (const auto file = files_.find(request.path); file != files_.end()) {
    return Response{
        .content_type = file->second.content_type,
        .body = Body::Borrowed(file->second.contents),
    };
  }
  if (directories_.contains(request.path)) {
    Response response = StatusResponse(301);
    response.headers.emplace_back("Location", absl::StrCat(request.path, "/"));
    return response;
  }
  return StatusResponse(404);
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
