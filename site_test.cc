#include "site.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "http.h"

namespace fs = std::filesystem;

class SiteTest : public testing::Test {
 protected:
  void SetUp() override {
    fs::path tmp = fs::path(testing::TempDir()) / "site_test";
    fs::remove_all(tmp);
    root_ = tmp / "www";
    Write(root_ / "index.html", "<h1>home</h1>");
    Write(root_ / "style.css", "body{}");
    Write(root_ / "blog" / "index.html", "blog");
    Write(root_ / ".git" / "config", "hidden");
    Write(tmp / "secret.txt", "secret");
    fs::create_symlink(tmp / "secret.txt", root_ / "leak.txt");
  }

  static void Write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << contents;
  }

  Response Get(const std::string& path) {
    return Handle(Request{.method = "GET", .path = path}, root_);
  }

  fs::path root_;
};

TEST_F(SiteTest, ServesIndexForRoot) {
  Response response = Get("/");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.content_type, "text/html; charset=utf-8");
  EXPECT_EQ(response.body, "<h1>home</h1>");
}

TEST_F(SiteTest, ServesFileWithMimeType) {
  Response response = Get("/style.css");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.content_type, "text/css; charset=utf-8");
  EXPECT_EQ(response.body, "body{}");
}

TEST_F(SiteTest, DirectoryWithSlashServesIndex) {
  EXPECT_EQ(Get("/blog/").body, "blog");
}

TEST_F(SiteTest, DirectoryWithoutSlashRedirects) {
  Response response = Get("/blog");
  EXPECT_EQ(response.status, 301);
  ASSERT_EQ(response.headers.size(), 1);
  EXPECT_EQ(response.headers[0].first, "Location");
  EXPECT_EQ(response.headers[0].second, "/blog/");
}

TEST_F(SiteTest, MissingFileIs404) {
  EXPECT_EQ(Get("/nope.html").status, 404);
}

TEST_F(SiteTest, CannotEscapeRoot) {
  EXPECT_EQ(Get("/../secret.txt").status, 404);
  EXPECT_EQ(Get("/blog/../../secret.txt").status, 404);
  EXPECT_EQ(Get("/leak.txt").status, 404);
}

TEST_F(SiteTest, DotfilesAreHidden) {
  EXPECT_EQ(Get("/.git/config").status, 404);
}

TEST_F(SiteTest, Healthz) {
  Response response = Get("/healthz");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, "ok\n");
}

TEST_F(SiteTest, OtherMethodsAre405) {
  Response response = Handle(Request{.method = "POST", .path = "/"}, root_);
  EXPECT_EQ(response.status, 405);
}
