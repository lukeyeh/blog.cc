// Site by example: which request gets which response, for a small site built
// on disk by the fixture below.

#include "site/site.h"

#include <benchmark/benchmark.h>
#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/message.h"

namespace fs = std::filesystem;

using absl_testing::StatusIs;

// Builds this tree in a temporary directory:
//
//   www/                    the site root
//     index.html
//     style.css
//     blog/index.html
//     blog/.draft           hidden: name starts with a dot
//     .git/config           hidden: inside a dot directory
//     alias.css  -> style.css          link that stays inside the root
//     config.txt -> .git/config        link to a hidden file
//     leak.txt   -> ../secret.txt      link that leaves the root
//     linked/    -> ../elsewhere/      linked directory
//   secret.txt
//   elsewhere/page.html
class SiteTest : public testing::Test {
 protected:
  void SetUp() override {
    const fs::path tmp = fs::path(testing::TempDir()) / "site_test";
    fs::remove_all(tmp);
    root_ = tmp / "www";
    Write(root_ / "index.html", "<h1>home</h1>");
    Write(root_ / "style.css", "body{}");
    Write(root_ / "blog" / "index.html", "blog");
    Write(root_ / ".git" / "config", "hidden");
    Write(root_ / "blog" / ".draft", "hidden");
    Write(tmp / "secret.txt", "secret");
    Write(tmp / "elsewhere" / "page.html", "outside");
    fs::create_symlink(tmp / "secret.txt", root_ / "leak.txt");
    fs::create_symlink(root_ / ".git" / "config", root_ / "config.txt");
    fs::create_symlink(root_ / "style.css", root_ / "alias.css");
    fs::create_directory_symlink(tmp / "elsewhere", root_ / "linked");
  }

  static void Write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << contents;
  }

  Site Load() const {
    absl::StatusOr<Site> site = Site::Load(root_);
    ABSL_EXPECT_OK(site);
    return std::move(*site);
  }

  fs::path root_;
};

Response Get(const Site& site, const std::string& path) {
  return site.Handle(Request{
      .method = "GET",
      .path = path,
  });
}

// A directory's URL serves its index.html.
TEST_F(SiteTest, ServesIndexForRoot) {
  const Site site = Load();
  const Response response = Get(site, "/");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.content_type, "text/html; charset=utf-8");
  EXPECT_EQ(response.body.view(), "<h1>home</h1>");
}

// The Content-Type comes from the file extension.
TEST_F(SiteTest, ServesFileWithMimeType) {
  const Site site = Load();
  const Response response = Get(site, "/style.css");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.content_type, "text/css; charset=utf-8");
  EXPECT_EQ(response.body.view(), "body{}");
}

TEST_F(SiteTest, IndexIsServedUnderBothItsNames) {
  const Site site = Load();
  EXPECT_EQ(Get(site, "/blog/").body.view(), "blog");
  EXPECT_EQ(Get(site, "/blog/index.html").body.view(), "blog");
}

// Without the trailing slash, a browser would resolve the page's relative
// links against the parent directory, so it is sent to the right URL.
TEST_F(SiteTest, DirectoryWithoutSlashRedirects) {
  const Site site = Load();
  const Response response = Get(site, "/blog");
  EXPECT_EQ(response.status, 301);
  ASSERT_EQ(response.headers.size(), 1);
  EXPECT_EQ(response.headers[0].first, "Location");
  EXPECT_EQ(response.headers[0].second, "/blog/");
}

TEST_F(SiteTest, MissingFileIs404) {
  const Site site = Load();
  EXPECT_EQ(Get(site, "/nope.html").status, 404);
}

// Nothing outside the root can be reached, whether by ".." in the URL or by
// a link on disk.
TEST_F(SiteTest, CannotEscapeRoot) {
  const Site site = Load();
  EXPECT_EQ(Get(site, "/../secret.txt").status, 404);
  EXPECT_EQ(Get(site, "/blog/../../secret.txt").status, 404);
  EXPECT_EQ(Get(site, "/leak.txt").status, 404);
  EXPECT_EQ(Get(site, "/linked/page.html").status, 404);
  EXPECT_EQ(Get(site, "/linked").status, 404);
}

// Links are fine as long as they stay inside.
TEST_F(SiteTest, LinkWithinRootIsServed) {
  const Site site = Load();
  EXPECT_EQ(Get(site, "/alias.css").body.view(), "body{}");
}

// Dotfiles are never served, so pointing --root at a git checkout does not
// publish its .git directory. A link does not get round this.
TEST_F(SiteTest, DotfilesAreHidden) {
  const Site site = Load();
  EXPECT_EQ(Get(site, "/.git/config").status, 404);
  EXPECT_EQ(Get(site, "/.git").status, 404);
  EXPECT_EQ(Get(site, "/blog/.draft").status, 404);
  EXPECT_EQ(Get(site, "/config.txt").status, 404);
}

TEST_F(SiteTest, CountsFilesOnce) {
  // index.html, style.css, blog/index.html and alias.css.
  EXPECT_EQ(Load().file_count(), 4);
}

// The site is a snapshot taken when it was loaded. This is what makes it
// fast, and why the server must be restarted after editing the site.
TEST_F(SiteTest, ChangesAfterLoadingAreNotSeen) {
  const Site site = Load();
  Write(root_ / "style.css", "changed");
  Write(root_ / "new.txt", "new");
  EXPECT_EQ(Get(site, "/style.css").body.view(), "body{}");
  EXPECT_EQ(Get(site, "/new.txt").status, 404);
}

// A built-in endpoint for checking that the server is up; not a file.
TEST_F(SiteTest, Healthz) {
  const Site site = Load();
  const Response response = Get(site, "/healthz");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body.view(), "ok\n");
}

// The site is read-only.
TEST_F(SiteTest, OtherMethodsAre405) {
  const Site site = Load();
  const Response response = site.Handle(Request{
      .method = "POST",
      .path = "/",
  });
  EXPECT_EQ(response.status, 405);
}

TEST_F(SiteTest, LoadReportsAbsoluteRoot) {
  absl::StatusOr<Site> site = Site::Load(root_ / "blog" / "..");
  ABSL_ASSERT_OK(site);
  EXPECT_EQ(site->root(), fs::canonical(root_));
}

// The root has to be an existing directory.
TEST_F(SiteTest, LoadRejectsMissingDirectoryAndFile) {
  EXPECT_THAT(Site::Load(root_ / "nope"),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Site::Load(root_ / "index.html"),
              StatusIs(absl::StatusCode::kNotFound));
}

// `bazel run` starts programs in a private directory and reports where the
// user really was in this variable; "www" should mean the user's www.
TEST_F(SiteTest, LoadResolvesRelativePathAgainstBazelLaunchDirectory) {
  setenv("BUILD_WORKING_DIRECTORY", root_.parent_path().c_str(), 1);
  absl::StatusOr<Site> site = Site::Load("www");
  unsetenv("BUILD_WORKING_DIRECTORY");
  ABSL_ASSERT_OK(site);
  EXPECT_EQ(site->root(), fs::canonical(root_));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What deciding on a response costs, and what loading a site costs.
//
//   bazel run -c opt //site:site_test -- --benchmark_filter=all
//
// Handle runs for every request, so it is kept to a lookup in memory. Load
// runs once at startup and reads every file, which is the trade the site
// makes: slow start, fast requests.

namespace {

// Builds a site of `pages` small pages, plus an index and a subdirectory, in
// a temporary directory, and returns its path.
fs::path BuildSite(int pages) {
  const fs::path root = fs::temp_directory_path() / "site_benchmark" /
                        absl::StrCat(pages, "_pages");
  fs::remove_all(root);
  fs::create_directories(root / "blog");
  std::ofstream(root / "index.html") << "<h1>home</h1>";
  std::ofstream(root / "blog" / "index.html") << "<h1>blog</h1>";
  for (int i = 0; i < pages; ++i) {
    std::ofstream(root / absl::StrCat("page", i, ".html"))
        << std::string(2000, 'x');
  }
  return root;
}

// A site of 1000 pages, loaded once and shared by the Handle benchmarks.
const Site& LoadedSite() {
  static const Site* const site = new Site(*Site::Load(BuildSite(1000)));
  return *site;
}

// Measures Handle for a GET (or other `method`) of `path`.
void MeasureHandle(benchmark::State& state, std::string_view method,
                   const std::string& path) {
  const Site& site = LoadedSite();
  const Request request = {
      .method = method,
      .path = path,
  };
  for (auto _ : state) benchmark::DoNotOptimize(site.Handle(request));
}

// The common case: a page that exists.
void BM_HandleFile(benchmark::State& state) {
  MeasureHandle(state, "GET", "/page500.html");
}
BENCHMARK(BM_HandleFile);

// A directory's index page, under the directory's own URL.
void BM_HandleIndex(benchmark::State& state) {
  MeasureHandle(state, "GET", "/");
}
BENCHMARK(BM_HandleIndex);

// The built-in endpoint, checked before any file lookup.
void BM_HandleHealthz(benchmark::State& state) {
  MeasureHandle(state, "GET", "/healthz");
}
BENCHMARK(BM_HandleHealthz);

// A page that does not exist. Costs more than one that does, because the
// error response's body is built for the occasion.
void BM_HandleMissing(benchmark::State& state) {
  MeasureHandle(state, "GET", "/no-such-page.html");
}
BENCHMARK(BM_HandleMissing);

// A directory without its trailing slash, answered with a redirect.
void BM_HandleRedirect(benchmark::State& state) {
  MeasureHandle(state, "GET", "/blog");
}
BENCHMARK(BM_HandleRedirect);

// A method the site does not support.
void BM_HandleWrongMethod(benchmark::State& state) {
  MeasureHandle(state, "POST", "/");
}
BENCHMARK(BM_HandleWrongMethod);

// Reading a whole site into memory, for sites of several sizes. Time per
// item is time per file.
void BM_Load(benchmark::State& state) {
  const fs::path root = BuildSite(static_cast<int>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(Site::Load(root));
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Load)->Arg(10)->Arg(100)->Arg(1000);

}  // namespace
