// What this website serves: which response each request gets. Works purely in
// terms of Request and Response and never sees a connection.

#ifndef SITE_SITE_H_
#define SITE_SITE_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "http/message.h"

// A website: a directory of static files plus a few built-in endpoints.
//
// The files are read into memory once, when the site is loaded, so answering
// a request never touches the disk. The price is that changes to the
// directory are not seen until the program is restarted.
//
// Thread-safe once loaded.
class Site {
 public:
  // Loads every file under `root`, a path as the user typed it, for example
  // on the command line. A relative `root` is taken relative to the directory
  // the user started the program from; this holds under `bazel run` too, even
  // though Bazel starts programs in a different directory.
  //
  // Not everything under `root` is served. Files and directories whose names
  // start with a dot are left out, as are symbolic links that lead outside
  // `root` and directories reached through symbolic links.
  //
  // Fails with NotFound if `root` is not a directory.
  static absl::StatusOr<Site> Load(const std::filesystem::path& root);

  // Returns the response to `request`. The response may refer to memory owned
  // by this site, so it must be sent before the site is destroyed.
  //
  // A file is served at its path relative to the root, and a directory's
  // index.html at the directory's path with a trailing slash.
  Response Handle(const Request& request) const;

  // The absolute path of the directory the site was loaded from.
  const std::filesystem::path& root() const { return root_; }

  // How many files were loaded.
  size_t file_count() const { return file_count_; }

 private:
  struct File {
    std::string_view content_type;
    std::string contents;
  };

  Site() = default;

  std::filesystem::path root_;
  size_t file_count_ = 0;

  // The servable files by URL path, e.g. "/style.css". An index.html also
  // appears under its directory's URL, e.g. "/blog/".
  absl::flat_hash_map<std::string, File> files_;

  // URL paths of directories, without trailing slash, e.g. "/blog". A request
  // for one is redirected to the form with the slash, because relative links
  // in the index page would otherwise resolve against the parent directory.
  absl::flat_hash_set<std::string> directories_;
};

// Content-Type for a file extension, including the leading dot.
std::string_view MimeType(std::string_view extension);

#endif  // SITE_SITE_H_
