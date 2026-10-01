#include "OpenLibraryClient.h"

#include <Logging.h>
#include <StreamingJsonParser.h>

#include <cstring>
#include <string>

#include "network/HttpDownloader.h"

static constexpr const char* TAG = "OLC";

// ---------------------------------------------------------------------------
// URL encoding
// ---------------------------------------------------------------------------

static std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
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

// ---------------------------------------------------------------------------
// JSON parser state machine
// ---------------------------------------------------------------------------
// We look for the shape:
//   { "docs": [ { "title": "...", "author_name": ["..."], "cover_i": 12345 } ] }
// ---------------------------------------------------------------------------

struct ParseState {
  OpenLibraryResult* result = nullptr;
  int depth = 0;         // overall nesting depth
  int docsDepth = -1;    // depth at which "docs" array starts
  int firstDocDepth = -1;  // depth of the first docs[0] object
  bool inDocs = false;
  bool inFirstDoc = false;
  bool firstDocDone = false;
  bool expectingDocsArray = false;
  bool expectingAuthorName = false;
  bool authorCaptured = false;
  std::string lastKey;
};

static void onObjectStart(void* ctx) {
  auto* ps = static_cast<ParseState*>(ctx);
  ps->depth++;
  if (ps->inDocs && !ps->firstDocDone && ps->firstDocDepth == -1) {
    ps->firstDocDepth = ps->depth;
    ps->inFirstDoc = true;
  }
}

static void onObjectEnd(void* ctx) {
  auto* ps = static_cast<ParseState*>(ctx);
  if (ps->inFirstDoc && ps->depth == ps->firstDocDepth) {
    ps->inFirstDoc = false;
    ps->firstDocDone = true;
  }
  ps->depth--;
}

static void onArrayStart(void* ctx) {
  auto* ps = static_cast<ParseState*>(ctx);
  ps->depth++;
  if (ps->expectingDocsArray) {
    ps->inDocs = true;
    ps->docsDepth = ps->depth;
    ps->expectingDocsArray = false;
  } else if (ps->inFirstDoc && ps->lastKey == "author_name") {
    ps->expectingAuthorName = true;
    ps->depth++;  // account for the array itself being one more level
    // (onArrayStart already incremented depth above — increment once more so
    // the array's depth is tracked separately from its elements)
    ps->depth--;  // undo the extra — just mark the flag
  }
}

static void onArrayEnd(void* ctx) {
  auto* ps = static_cast<ParseState*>(ctx);
  if (ps->inDocs && ps->depth == ps->docsDepth) {
    ps->inDocs = false;
  }
  if (ps->expectingAuthorName) {
    ps->expectingAuthorName = false;
  }
  ps->depth--;
}

static void onKey(void* ctx, const char* key, size_t len) {
  auto* ps = static_cast<ParseState*>(ctx);
  ps->lastKey = std::string(key, len);
  if (ps->depth == 1 && ps->lastKey == "docs") {
    ps->expectingDocsArray = true;
  }
}

static void onString(void* ctx, const char* value, size_t len) {
  auto* ps = static_cast<ParseState*>(ctx);
  if (!ps->inFirstDoc) return;

  if (ps->lastKey == "title" && ps->result->title.empty()) {
    ps->result->title = std::string(value, len);
  } else if (ps->lastKey == "author_name" && !ps->authorCaptured) {
    ps->result->author = std::string(value, len);
    ps->authorCaptured = true;
  }
}

static void onNumber(void* ctx, const char* value, size_t len) {
  auto* ps = static_cast<ParseState*>(ctx);
  if (!ps->inFirstDoc) return;
  if (ps->lastKey == "cover_i" && ps->result->coverId == 0) {
    ps->result->coverId = static_cast<int>(atol(std::string(value, len).c_str()));
  }
}

// ---------------------------------------------------------------------------

bool OpenLibraryClient::search(const std::string& title, OpenLibraryResult& out) {
  out = OpenLibraryResult{};

  const std::string url =
      std::string("https://openlibrary.org/search.json?title=") + urlEncode(title) +
      "&fields=title,author_name,cover_i&limit=1";

  LOG_INF(TAG, "Searching: %s", url.c_str());

  std::string response;
  if (!HttpDownloader::fetchUrl(url, response)) {
    LOG_ERR(TAG, "HTTP fetch failed");
    return false;
  }

  if (response.empty()) {
    LOG_ERR(TAG, "Empty response");
    return false;
  }

  ParseState ps;
  ps.result = &out;

  JsonCallbacks cb{};
  cb.ctx = &ps;
  cb.onKey = onKey;
  cb.onString = onString;
  cb.onNumber = onNumber;
  cb.onObjectStart = onObjectStart;
  cb.onObjectEnd = onObjectEnd;
  cb.onArrayStart = onArrayStart;
  cb.onArrayEnd = onArrayEnd;

  StreamingJsonParser parser(cb);
  parser.feed(response.c_str(), response.size());

  if (parser.hasError()) {
    LOG_ERR(TAG, "JSON parse error");
    return false;
  }

  if (out.title.empty()) {
    LOG_INF(TAG, "No results found for '%s'", title.c_str());
    return false;
  }

  out.found = true;
  LOG_INF(TAG, "Found: '%s' by '%s' cover_i=%d", out.title.c_str(), out.author.c_str(), out.coverId);
  return true;
}

bool OpenLibraryClient::downloadCoverJpeg(int coverId, const std::string& destPath) {
  if (coverId <= 0) return false;

  const std::string url = std::string("https://covers.openlibrary.org/b/id/") +
                          std::to_string(coverId) + "-L.jpg";

  LOG_INF(TAG, "Downloading cover: %s", url.c_str());

  const auto err = HttpDownloader::downloadToFile(url, destPath);
  if (err != HttpDownloader::DownloadError::OK) {
    LOG_ERR(TAG, "Cover download failed (err=%d)", static_cast<int>(err));
    return false;
  }

  return true;
}
