//===--- RefoldPerf.h ------------------------------------------*- C++ -*-===//
//
// Environment-gated performance instrumentation for clang-refold.
//
// This header is intentionally side-effect free unless CLANG_REFOLD_PERF_TRACE
// is set to a truthy value.  The probes are diagnostic only: they do not feed
// proof selection, candidate ordering, validation outcomes, or emitted text.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PERF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PERF_H

#include "RefoldLog.h"

#include "llvm/ADT/StringRef.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace clang {
namespace refold {
namespace perf {

inline bool enabled() {
#if 0
  static const bool isEnabled = []() {
    const char *value = std::getenv("CLANG_REFOLD_PERF_TRACE");
    if (!value || !*value)
      return false;
    llvm::StringRef text(value);
    return text != "0" && !text.equals_insensitive("false") &&
           !text.equals_insensitive("off") &&
           !text.equals_insensitive("no");
  }();
  return isEnabled;
#endif
  return true;
}

inline uint64_t nowNs() {
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
}

inline uint64_t nsToUs(uint64_t ns) { return ns / 1000; }

class ScopedTimer {
public:
  explicit ScopedTimer(llvm::StringRef label)
      : label_(label.str()), active_(enabled()), startNs_(active_ ? nowNs() : 0) {}

  ScopedTimer(const ScopedTimer &) = delete;
  ScopedTimer &operator=(const ScopedTimer &) = delete;

  ~ScopedTimer() {
    if (!active_)
      return;
    const uint64_t elapsedNs = nowNs() - startNs_;
    REFOLD_LOG_INFO("perf", "{0}: {1} us", label_, nsToUs(elapsedNs));
  }

private:
  std::string label_;
  bool active_ = false;
  uint64_t startNs_ = 0;
};

template <typename... Args>
inline void log(llvm::StringRef fmt, Args &&...args) {
  if (!enabled())
    return;
  REFOLD_LOG_INFO("perf", fmt, std::forward<Args>(args)...);
}

} // namespace perf
} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PERF_H
