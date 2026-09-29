#pragma once

// FreeInk SDK — resumable HTTP GET over SecureHttpClient.
//
// A 2xx only means the headers arrived; the body can still be cut short by a
// transport drop, a server stall, or a TLS record OOM. fetchResumable() picks
// the transfer back up from the received byte count with a Range request on a
// fresh connection. A server that ignores the Range (200 instead of 206)
// restarts the body, so the sink is rewound before that body's first byte.
//
// Header-only, like SecureHttpClient.

#include <functional>
#include <string>

#include "SecureHttpClient.h"

namespace freeink {

struct FetchOptions {
  // Bytes the sink already holds from an earlier transfer; the first request
  // asks for the rest.
  size_t startOffset = 0;
  // Step followed https redirect targets down to http. For payloads that are
  // already content-encrypted, this skips a second TLS session and its record
  // buffer on low-heap boards; only the URL token loses transport security.
  bool redirectToHttp = false;
};

struct FetchSink {
  // Body bytes in order. Return false to stop the transfer.
  std::function<bool(const uint8_t* data, size_t len)> write;
  // The server ignored Range: drop everything held and restart from byte 0.
  // Unset, or returning false, stops the transfer.
  std::function<bool()> rewind;
  // After each write: (bytes held, whole-resource size or 0 when unknown).
  std::function<void(size_t bytes, size_t total)> progress;
};

struct FetchResult {
  int status = 0;         // last HTTP status; -1 on a bad URL or transport failure
  size_t bytes = 0;       // bytes the sink holds, including startOffset
  size_t total = 0;       // whole-resource size when the server reported it, else 0
  bool complete = false;  // the full body arrived
  bool stopped = false;   // the sink stopped the transfer (write/rewind returned false)
  bool aborted = false;   // shouldAbort fired
};

// GETs url into sink. configure runs on each attempt's client after begin():
// headers, auth, user agent, timeout, trust. Non-2xx bodies never reach the sink.
inline FetchResult fetchResumable(const std::string& startUrl, const FetchOptions& options,
                                  const std::function<void(SecureHttpClient&)>& configure, const FetchSink& sink,
                                  const SecureHttpClient::AbortCallback& shouldAbort = nullptr) {
  static constexpr int kMaxRedirects = 5;
  // Only consecutive zero-progress attempts count toward kMaxStalled;
  // kMaxAttempts is a backstop against a server that trickles forever.
  static constexpr int kMaxStalled = 3;
  static constexpr int kMaxAttempts = 20;
  FetchResult result;
  result.bytes = options.startOffset;
  std::string url = startUrl;
  int redirects = 0;
  int stalled = 0;
  for (int attempt = 0; attempt < kMaxAttempts && stalled < kMaxStalled; ++attempt) {
    SecureHttpClient http;
    if (!http.begin(url)) {
      result.status = -1;
      return result;
    }
    if (configure) configure(http);
    size_t attemptStart = result.bytes;
    const bool resuming = attemptStart > 0;
    if (resuming) http.addHeader("Range", "bytes=" + std::to_string(attemptStart) + "-");

    bool firstChunk = true;
    result.status = http.GET(
        [&](const uint8_t* data, size_t len) {
          const int status = http.getStatus();
          if (status < 200 || status >= 300) return true;  // error page or redirect body: drain
          if (firstChunk) {
            firstChunk = false;
            if (resuming && status == 200) {
              if (!sink.rewind || !sink.rewind()) {
                result.stopped = true;
                return false;
              }
              result.bytes = attemptStart = 0;
            }
            // A 206's Content-Length covers only the remainder.
            if (result.total == 0 && http.hasContentLength()) result.total = attemptStart + http.getContentLength();
          }
          if (!sink.write(data, len)) {
            result.stopped = true;
            return false;
          }
          result.bytes += len;
          if (sink.progress) sink.progress(result.bytes, result.total);
          return true;
        },
        shouldAbort);

    if (http.aborted()) {
      result.aborted = true;
      return result;
    }
    const int status = result.status;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      const std::string location = http.getHeader("location");
      if (redirects++ >= kMaxRedirects || location.empty() ||
          !SecureHttpClient::resolveUrl(url, location, url)) {
        return result;
      }
      if (options.redirectToHttp && url.rfind("https://", 0) == 0) url.replace(0, 8, "http://");
      --attempt;  // a hop is not a transfer attempt
      continue;
    }
    if (result.stopped || status < 200 || status >= 300) return result;  // resume cannot help
    if (http.responseComplete()) {
      result.complete = true;
      return result;
    }
    stalled = result.bytes > attemptStart ? 0 : stalled + 1;
  }
  return result;
}

}  // namespace freeink
