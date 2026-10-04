#include "OpenLibraryClient.h"

#include <Logging.h>
#include <StreamingJsonParser.h>
#include <esp_http_client.h>

#include <cstring>
#include <string>

#include "network/HttpDownloader.h"

static constexpr const char* TAG = "OLC";

// Number of HTTP retry attempts per source before giving up or falling back.
static constexpr int kMaxAttempts = 2;
// Back-off between retries (ms).  Doubles each attempt: 500 → 1000.
static constexpr int kRetryBaseMs = 500;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      const char hex[] = "0123456789ABCDEF";
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0xF];
    }
  }
  return out;
}

// Rewrite an http:// URL to https://.  Google Books returns http thumbs.
static std::string ensureHttps(const std::string& url) {
  if (url.size() > 7 && url.substr(0, 7) == "http://") {
    return "https://" + url.substr(7);
  }
  return url;
}

// Run `cb` with streaming data from `url`.  Retries up to kMaxAttempts times.
// Returns true if any attempt succeeds.
static bool fetchWithRetry(const std::string& url, const HttpDownloader::DataCallback& cb) {
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      const int backoffMs = kRetryBaseMs * attempt;
      LOG_INF(TAG, "Retry %d after %dms (url=%s)", attempt, backoffMs, url.c_str());
      delay(backoffMs);
    }
    if (HttpDownloader::fetchUrl(url, cb)) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// OpenLibrary JSON parser
// ---------------------------------------------------------------------------
// Parses: { "docs": [ { "title":"…", "author_name":["…"], "cover_i": 12345 } ] }
// ---------------------------------------------------------------------------

struct OLParseState {
  OpenLibraryResult* result = nullptr;
  int depth = 0;
  int docsDepth = -1;
  int firstDocDepth = -1;
  bool inDocs = false;
  bool inFirstDoc = false;
  bool firstDocDone = false;
  bool expectingDocsArray = false;
  bool expectingAuthorName = false;
  bool authorCaptured = false;
  // Raw cover_i integer from OL (> 0 means a cover exists).
  int coverId = 0;
  std::string lastKey;
};

static void olOnObjectStart(void* ctx) {
  auto* ps = static_cast<OLParseState*>(ctx);
  ps->depth++;
  if (ps->inDocs && !ps->firstDocDone && ps->firstDocDepth == -1) {
    ps->firstDocDepth = ps->depth;
    ps->inFirstDoc = true;
  }
}

static void olOnObjectEnd(void* ctx) {
  auto* ps = static_cast<OLParseState*>(ctx);
  if (ps->inFirstDoc && ps->depth == ps->firstDocDepth) {
    ps->inFirstDoc = false;
    ps->firstDocDone = true;
  }
  ps->depth--;
}

static void olOnArrayStart(void* ctx) {
  auto* ps = static_cast<OLParseState*>(ctx);
  ps->depth++;
  if (ps->expectingDocsArray) {
    ps->inDocs = true;
    ps->docsDepth = ps->depth;
    ps->expectingDocsArray = false;
  } else if (ps->inFirstDoc && ps->lastKey == "author_name") {
    ps->expectingAuthorName = true;
  }
}

static void olOnArrayEnd(void* ctx) {
  auto* ps = static_cast<OLParseState*>(ctx);
  if (ps->inDocs && ps->depth == ps->docsDepth) {
    ps->inDocs = false;
  }
  if (ps->expectingAuthorName) {
    ps->expectingAuthorName = false;
  }
  ps->depth--;
}

static void olOnKey(void* ctx, const char* key, size_t len) {
  auto* ps = static_cast<OLParseState*>(ctx);
  ps->lastKey = std::string(key, len);
  if (ps->depth == 1 && ps->lastKey == "docs") {
    ps->expectingDocsArray = true;
  }
}

static void olOnString(void* ctx, const char* value, size_t len) {
  auto* ps = static_cast<OLParseState*>(ctx);
  if (!ps->inFirstDoc) return;
  if (ps->lastKey == "title" && ps->result->title.empty()) {
    ps->result->title = std::string(value, len);
  } else if (ps->lastKey == "author_name" && !ps->authorCaptured) {
    ps->result->author = std::string(value, len);
    ps->authorCaptured = true;
  }
}

static void olOnNumber(void* ctx, const char* value, size_t len) {
  auto* ps = static_cast<OLParseState*>(ctx);
  if (!ps->inFirstDoc) return;
  if (ps->lastKey == "cover_i" && ps->coverId == 0) {
    ps->coverId = static_cast<int>(atol(std::string(value, len).c_str()));
  }
}

static bool searchOpenLibrary(const std::string& title, OpenLibraryResult& out) {
  const std::string url =
      std::string("https://openlibrary.org/search.json?title=") + urlEncode(title) +
      "&fields=title,author_name,cover_i&limit=1";

  LOG_INF(TAG, "OL search: %s", url.c_str());

  OLParseState ps;
  ps.result = &out;

  JsonCallbacks cb{};
  cb.ctx = &ps;
  cb.onKey = olOnKey;
  cb.onString = olOnString;
  cb.onNumber = olOnNumber;
  cb.onObjectStart = olOnObjectStart;
  cb.onObjectEnd = olOnObjectEnd;
  cb.onArrayStart = olOnArrayStart;
  cb.onArrayEnd = olOnArrayEnd;

  StreamingJsonParser parser(cb);

  const bool ok = fetchWithRetry(url, [&](const uint8_t* data, size_t len) -> bool {
    parser.feed(reinterpret_cast<const char*>(data), len);
    return !parser.hasError();
  });

  if (!ok || parser.hasError()) {
    LOG_ERR(TAG, "OL fetch/parse failed");
    return false;
  }

  if (out.title.empty()) {
    LOG_INF(TAG, "OL: no results for '%s'", title.c_str());
    return false;
  }

  // Build cover URL for the largest available size.  downloadCoverJpeg() will
  // cascade to smaller sizes if the server returns an error for this one.
  if (ps.coverId > 0) {
    out.coverUrl = std::string("https://covers.openlibrary.org/b/id/") +
                   std::to_string(ps.coverId) + "-L.jpg";
  }

  out.found = true;
  LOG_INF(TAG, "OL found: '%s' by '%s' cover=%s",
          out.title.c_str(), out.author.c_str(),
          out.coverUrl.empty() ? "(none)" : out.coverUrl.c_str());
  return true;
}

// ---------------------------------------------------------------------------
// Google Books JSON parser
// ---------------------------------------------------------------------------
// Parses: { "items": [ { "volumeInfo": { "title":"…", "authors":["…"],
//           "imageLinks": { "thumbnail":"http://…" } } } ] }
// ---------------------------------------------------------------------------

struct GBParseState {
  OpenLibraryResult* result = nullptr;
  int depth = 0;
  // Track path: items[0] → volumeInfo → imageLinks
  bool inItems = false;
  bool inFirstItem = false;
  bool firstItemDone = false;
  bool inVolumeInfo = false;
  bool inImageLinks = false;
  bool inAuthors = false;
  bool expectingItems = false;
  bool authorCaptured = false;
  int itemsDepth = -1;
  int firstItemDepth = -1;
  int volumeInfoDepth = -1;
  int imageLinksDepth = -1;
  std::string lastKey;
};

static void gbOnObjectStart(void* ctx) {
  auto* ps = static_cast<GBParseState*>(ctx);
  ps->depth++;
  if (ps->inItems && !ps->firstItemDone && ps->firstItemDepth == -1) {
    ps->firstItemDepth = ps->depth;
    ps->inFirstItem = true;
  }
  if (ps->inFirstItem && ps->lastKey == "volumeInfo" && !ps->inVolumeInfo) {
    ps->inVolumeInfo = true;
    ps->volumeInfoDepth = ps->depth;
  }
  if (ps->inVolumeInfo && ps->lastKey == "imageLinks" && !ps->inImageLinks) {
    ps->inImageLinks = true;
    ps->imageLinksDepth = ps->depth;
  }
}

static void gbOnObjectEnd(void* ctx) {
  auto* ps = static_cast<GBParseState*>(ctx);
  if (ps->inImageLinks && ps->depth == ps->imageLinksDepth) {
    ps->inImageLinks = false;
  }
  if (ps->inVolumeInfo && ps->depth == ps->volumeInfoDepth) {
    ps->inVolumeInfo = false;
  }
  if (ps->inFirstItem && ps->depth == ps->firstItemDepth) {
    ps->inFirstItem = false;
    ps->firstItemDone = true;
  }
  ps->depth--;
}

static void gbOnArrayStart(void* ctx) {
  auto* ps = static_cast<GBParseState*>(ctx);
  ps->depth++;
  if (ps->expectingItems) {
    ps->inItems = true;
    ps->itemsDepth = ps->depth;
    ps->expectingItems = false;
  }
  if (ps->inVolumeInfo && ps->lastKey == "authors") {
    ps->inAuthors = true;
  }
}

static void gbOnArrayEnd(void* ctx) {
  auto* ps = static_cast<GBParseState*>(ctx);
  if (ps->inItems && ps->depth == ps->itemsDepth) ps->inItems = false;
  if (ps->inAuthors) ps->inAuthors = false;
  ps->depth--;
}

static void gbOnKey(void* ctx, const char* key, size_t len) {
  auto* ps = static_cast<GBParseState*>(ctx);
  ps->lastKey = std::string(key, len);
  if (ps->depth == 1 && ps->lastKey == "items") {
    ps->expectingItems = true;
  }
}

static void gbOnString(void* ctx, const char* value, size_t len) {
  auto* ps = static_cast<GBParseState*>(ctx);
  if (!ps->inFirstItem || !ps->inVolumeInfo) return;

  if (ps->inImageLinks && ps->lastKey == "thumbnail" && ps->result->coverUrl.empty()) {
    // Google returns http; we need https.
    ps->result->coverUrl = ensureHttps(std::string(value, len));
    // Request a larger zoom level for better quality.
    // thumbnail URLs end in &zoom=1; bump to zoom=6 (medium-large).
    auto& cu = ps->result->coverUrl;
    const std::string zoomTag = "&zoom=1";
    const auto pos = cu.rfind(zoomTag);
    if (pos != std::string::npos) cu.replace(pos, zoomTag.size(), "&zoom=6");
  } else if (!ps->inImageLinks && ps->lastKey == "title" && ps->result->title.empty()) {
    ps->result->title = std::string(value, len);
  } else if (ps->inAuthors && !ps->authorCaptured) {
    ps->result->author = std::string(value, len);
    ps->authorCaptured = true;
  }
}

static bool searchGoogleBooks(const std::string& title, OpenLibraryResult& out) {
  const std::string url =
      std::string("https://www.googleapis.com/books/v1/volumes?q=intitle:") +
      urlEncode(title) +
      "&maxResults=1&fields=items(volumeInfo/title,volumeInfo/authors,volumeInfo/imageLinks/thumbnail)";

  LOG_INF(TAG, "GB search: %s", url.c_str());

  GBParseState ps;
  ps.result = &out;

  JsonCallbacks cb{};
  cb.ctx = &ps;
  cb.onKey = gbOnKey;
  cb.onString = gbOnString;
  cb.onObjectStart = gbOnObjectStart;
  cb.onObjectEnd = gbOnObjectEnd;
  cb.onArrayStart = gbOnArrayStart;
  cb.onArrayEnd = gbOnArrayEnd;

  StreamingJsonParser parser(cb);

  const bool ok = fetchWithRetry(url, [&](const uint8_t* data, size_t len) -> bool {
    parser.feed(reinterpret_cast<const char*>(data), len);
    return !parser.hasError();
  });

  if (!ok || parser.hasError()) {
    LOG_ERR(TAG, "GB fetch/parse failed");
    return false;
  }

  if (out.title.empty()) {
    LOG_INF(TAG, "GB: no results for '%s'", title.c_str());
    return false;
  }

  out.found = true;
  LOG_INF(TAG, "GB found: '%s' by '%s' cover=%s",
          out.title.c_str(), out.author.c_str(),
          out.coverUrl.empty() ? "(none)" : out.coverUrl.c_str());
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool OpenLibraryClient::search(const std::string& title, OpenLibraryResult& out) {
  out = OpenLibraryResult{};

  // Primary: OpenLibrary.
  if (searchOpenLibrary(title, out)) {
    return true;
  }

  // Fallback: Google Books.  Reset partial state from any failed OL attempt.
  out = OpenLibraryResult{};
  if (searchGoogleBooks(title, out)) {
    return true;
  }

  LOG_ERR(TAG, "Both sources failed for '%s'", title.c_str());
  return false;
}

bool OpenLibraryClient::downloadCoverJpeg(const std::string& coverUrl,
                                          const std::string& destPath) {
  if (coverUrl.empty()) return false;

  // For OpenLibrary covers we cascade through sizes (L → M → S → XS) if the
  // preferred size returns an error.  Other hosts are tried once.
  const bool isOpenLibraryCover =
      coverUrl.find("covers.openlibrary.org") != std::string::npos;

  static const char* const kOLSizes[] = {"-L.jpg", "-M.jpg", "-S.jpg", nullptr};

  auto tryDownload = [&](const std::string& url) -> bool {
    LOG_INF(TAG, "Downloading cover: %s", url.c_str());
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
      if (attempt > 0) delay(kRetryBaseMs * attempt);
      const auto err = HttpDownloader::downloadToFile(url, destPath);
      if (err == HttpDownloader::DownloadError::OK) return true;
      LOG_ERR(TAG, "Cover download err=%d (attempt %d)", static_cast<int>(err), attempt + 1);
    }
    return false;
  };

  if (!isOpenLibraryCover) {
    return tryDownload(coverUrl);
  }

  // Strip whatever size suffix is in the URL and try each size in order.
  std::string baseUrl = coverUrl;
  for (const char* const* sz = kOLSizes; *sz; ++sz) {
    if (baseUrl.size() >= 6 &&
        (baseUrl.substr(baseUrl.size() - 6) == "-L.jpg" ||
         baseUrl.substr(baseUrl.size() - 6) == "-M.jpg" ||
         baseUrl.substr(baseUrl.size() - 6) == "-S.jpg")) {
      baseUrl = baseUrl.substr(0, baseUrl.size() - 6);
    }
  }

  for (const char* const* sz = kOLSizes; *sz; ++sz) {
    const std::string sizedUrl = baseUrl + *sz;
    if (tryDownload(sizedUrl)) return true;
    LOG_INF(TAG, "OL size %s failed, trying next", *sz);
  }

  LOG_ERR(TAG, "All OL cover sizes failed");
  return false;
}
