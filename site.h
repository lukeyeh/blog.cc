// What this website serves: which response each request gets. Works purely in
// terms of Request and Response and never sees a connection.

#ifndef SITE_H_
#define SITE_H_

#include <filesystem>
#include <string_view>

#include "http.h"

// Routes a request: built-in endpoints first, then static files under root.
Response Handle(const Request& request, const std::filesystem::path& root);

// Serves the file that url_path names under root. Directories serve their
// index.html. Paths that resolve outside root (via ".." or symlinks) and
// paths with a dotfile component are reported as 404.
Response ServeFile(const std::filesystem::path& root, std::string_view url_path);

// Content-Type for a file extension, including the leading dot.
std::string_view MimeType(std::string_view extension);

#endif  // SITE_H_
