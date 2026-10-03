#include <cstdint>
#include <string>

#include "absl/base/log_severity.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "http/message.h"
#include "http/server.h"
#include "site/site.h"

ABSL_FLAG(std::string, address, "127.0.0.1", "IPv4 address to listen on");
ABSL_FLAG(uint16_t, port, 8080, "Port to listen on");
ABSL_FLAG(std::string, root, "www", "Directory of static files to serve");

// Runs the website until it fails; never returns OK.
absl::Status Run() {
  ABSL_ASSIGN_OR_RETURN(const Site site, Site::Load(absl::GetFlag(FLAGS_root)),
                        _.SetPrepend() << "--root: ");
  LOG(INFO) << "serving " << site.file_count() << " files from " << site.root();

  ABSL_ASSIGN_OR_RETURN(HttpServer server,
                        HttpServer::Listen(absl::GetFlag(FLAGS_address),
                                           absl::GetFlag(FLAGS_port)));
  LOG(INFO) << "listening on http://" << absl::GetFlag(FLAGS_address) << ":"
            << server.port();
  return server.Run(
      [&site](const Request& request) { return site.Handle(request); });
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  LOG(ERROR) << Run().message();
  return 1;
}
