#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "server.h"
#include "site.h"

ABSL_FLAG(std::string, address, "127.0.0.1", "IPv4 address to listen on");
ABSL_FLAG(uint16_t, port, 8080, "Port to listen on");
ABSL_FLAG(std::string, root, "www", "Directory of static files to serve");

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  std::filesystem::path root = absl::GetFlag(FLAGS_root);
  // `bazel run` starts the binary in its runfiles directory; resolve a
  // relative --root against the directory the user ran bazel from instead.
  if (const char* cwd = std::getenv("BUILD_WORKING_DIRECTORY");
      cwd != nullptr && root.is_relative()) {
    root = std::filesystem::path(cwd) / root;
  }
  if (!std::filesystem::is_directory(root)) {
    LOG(ERROR) << "--root is not a directory: " << root;
    return 1;
  }
  LOG(INFO) << "serving files from " << root;

  absl::Status status =
      Serve(absl::GetFlag(FLAGS_address), absl::GetFlag(FLAGS_port),
            [root](const Request& request) { return Handle(request, root); });
  LOG(ERROR) << status;
  return 1;
}
