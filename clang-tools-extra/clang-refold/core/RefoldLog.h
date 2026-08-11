//===--- RefoldLog.h --------------------------------------------*- C++ -*-===//
//
// Lightweight logging utilities for the refolding tools.
//
// Overview
// --------
// Provides a tiny, LLVM-friendly logging facade with explicit log levels and
// printf-free formatting via llvm::formatv. The logging helpers are implemented
// inline for use from the refolder without introducing a separate logging
// library.
//
// Features
// --------
//  • Log levels: trace, debug, info, warn, error, fatal (fatal terminates).
//  • Format strings use llvm::formatv-style placeholders: {0}, {1}, ...
//  • Destination: llvm::outs(), with colors when the stream supports them.
//  • Category tag (e.g., "lexer", "json") for structured filtering.
//  • Global runtime level gate; cheap level checks to avoid formatting costs.
//  • One complete line is emitted per call; any synchronization is delegated to
//    the underlying raw_ostream implementation.
//
// Usage
// -----
//   // Emit messages:
//   REFOLD_LOG_TRACE("lexer", "token={0} off={1}", tok, off);
//   REFOLD_LOG_DEBUG("json",  "validated {0} entries", N);
//   REFOLD_LOG_INFO ("plan",  "includes={0} macros={1}", Incs, Macros);
//   REFOLD_LOG_WARN ("io",    "non-UTF8 byte at {0}", pos);
//   REFOLD_LOG_ERROR("map",   "missing field '{0}'", "tokmap");
//   REFOLD_LOG_FATAL("abort", "unrecoverable error in stage {0}", stage);
//
// Policy
// ------
//  • Logging must never throw; formatting failures are treated as best-effort.
//  • Fatal logs end the process deterministically after emitting the message.
//  • Fatal logs abort the process after printing; non-fatal logs return to the
//    caller after emission.
//  • Theorem-audit logs must use the ADL `toString()` names for proof classes,
//    accepted paths, obligations, and terminal-fallback reasons.  Logs should
//    not introduce parallel names for the strict-domain vocabulary.
//
// Public API (summary)
// --------------------
//   enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };
//
//   // Convenience wrappers:
//   void trace(llvm::StringRef Cat, llvm::StringRef Fmt, auto&&... Args);
//   void debug(llvm::StringRef Cat, llvm::StringRef Fmt, auto&&... Args);
//   void info (llvm::StringRef Cat, llvm::StringRef Fmt, auto&&... Args);
//   void warn (llvm::StringRef Cat, llvm::StringRef Fmt, auto&&... Args);
//   void error(llvm::StringRef Cat, llvm::StringRef Fmt, auto&&... Args);
//   [[noreturn]] void fatal(llvm::StringRef Cat, llvm::StringRef Fmt,
//                           auto&&... Args);
//
// Notes
// -----
//  • Keep logging out of hot loops or guard with level checks if expensive.
//  • Prefer structured, consistent categories to aid grep/filters.
//  • This header is designed for use within LLVM/Clang-style projects and
//    assumes llvm::Support is available.
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLDLOG_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLDLOG_H

#include "core/RefoldFormatProviders.h"
#include "source/DiffAlgorithms.h"
#include "util/StringUtils.h"

#include <chrono>
#include <cstdlib>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

// Guarded logging macros. These macros avoid evaluating formatting arguments
// when the requested log level is disabled. Keep any expensive diagnostic
// preparation outside these macros under an explicit in*Mode() guard.
#ifndef REFOLD_LOG_FATAL
#define REFOLD_LOG_FATAL(...)                                                  \
  do {                                                                         \
    if (::clang::refold::inFatalMode())                                        \
      ::clang::refold::fatal(__VA_ARGS__);                                     \
    llvm_unreachable("fatal logging unexpectedly disabled");                   \
  } while (false)
#endif

#ifndef REFOLD_LOG_ERROR
#define REFOLD_LOG_ERROR(...)                                                  \
  do {                                                                         \
    if (::clang::refold::inErrorMode())                                        \
      ::clang::refold::error(__VA_ARGS__);                                     \
  } while (false)
#endif

#ifndef REFOLD_LOG_WARN
#define REFOLD_LOG_WARN(...)                                                   \
  do {                                                                         \
    if (::clang::refold::inWarnMode())                                         \
      ::clang::refold::warn(__VA_ARGS__);                                      \
  } while (false)
#endif

#ifndef REFOLD_LOG_INFO
#define REFOLD_LOG_INFO(...)                                                   \
  do {                                                                         \
    if (::clang::refold::inInfoMode())                                         \
      ::clang::refold::info(__VA_ARGS__);                                      \
  } while (false)
#endif

#ifndef REFOLD_LOG_DEBUG
#define REFOLD_LOG_DEBUG(...)                                                  \
  do {                                                                         \
    if (::clang::refold::inDebugMode())                                        \
      ::clang::refold::debug(__VA_ARGS__);                                     \
  } while (false)
#endif

#ifndef REFOLD_LOG_TRACE
#define REFOLD_LOG_TRACE(...)                                                  \
  do {                                                                         \
    if (::clang::refold::inTraceMode())                                        \
      ::clang::refold::trace(__VA_ARGS__);                                     \
  } while (false)
#endif

// NOTE: The custom `llvm::format_provider<>` specializations used here (for
// `std::optional<>`, `ToString()`-bearing types, ADL-`toString()` enums, and
// `cl::opt<>`) now live in "core/RefoldFormatProviders.h", included above.
// They were moved out of this header so they are always declared before the
// first `formatv` instantiation in lower-level headers (GCC rejects a
// specialization seen after an implicit instantiation; Clang tolerated it).

using namespace llvm;

enum class LogLevel : unsigned {
  Fatal = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4,
  Trace = 5
};

static inline StringRef toString(LogLevel level) {
  switch (level) {
  case LogLevel::Trace:
    return "trace";
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  case LogLevel::Fatal:
    return "fatal";
  }
  llvm_unreachable("Invalid log level");
}

extern cl::opt<LogLevel> LogLevelOpt;

/// Seconds elapsed since this run emitted its first log line.
///
/// Logs are read to find where wall time goes, so the useful quantity is
/// elapsed time rather than the wall clock: the gap between two timestamps is
/// the cost of the work between them. A steady clock is used so the reading
/// cannot jump if the system clock is adjusted mid-run.
///
/// This must not be a local static inside the logging template. Each template
/// instantiation would then own a separate origin, so every call site would
/// restart from zero and the timings would be meaningless. One inline function
/// gives the whole program a single origin.
inline double elapsedLogSeconds() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point runStart = clock::now();
  return std::chrono::duration<double>(clock::now() - runStart).count();
}

inline raw_ostream::Colors levelColor(LogLevel level) {
  using color = raw_ostream::Colors;
  switch (level) {
  case LogLevel::Fatal:
    return color::RED; // bold handled separately
  case LogLevel::Error:
    return color::RED;
  case LogLevel::Warn:
    return color::YELLOW;
  case LogLevel::Info:
    return color::CYAN;
  case LogLevel::Debug:
    return color::GREEN;
  case LogLevel::Trace:
    return color::SAVEDCOLOR; // default color
  }
  llvm_unreachable("Invalid LogLevel");
}

template <typename... Args>
void logMsg(LogLevel level, StringRef tag, StringRef msg, Args &&...args) {
  if (static_cast<unsigned>(level) >
      static_cast<unsigned>(LogLevelOpt.getValue())) {
    return;
  }

  auto &os = outs();
  const double elapsedSeconds = elapsedLogSeconds();
  const bool useColor = os.has_colors(); // false if redirected

  auto printMsg = [](raw_ostream &os, StringRef msg, auto &&...args) {
    if constexpr (sizeof...(args) == 0) {
      // No formatting args: print message as-is
      os << msg << '\n';
    } else {
      // Format with args using LLVM's {0}, {1}, ... syntax
      // Copy Msg so the underlying char* lives through formatting.
      std::string fmt = msg.str();
      os << formatv(fmt.c_str(), std::forward<Args>(args)...) << '\n';
    }
  };

  // Colored prefix based on level (fatal is bold “bright red”)
  if (level == LogLevel::Trace || !useColor) {
    os << format("[%9.3f]", elapsedSeconds)
       << formatv("[{0,-5}][{1}]  ", level,
                  fmt_align(tag, AlignStyle::Left, 21, '.'));
    printMsg(os, msg, std::forward<Args>(args)...);
  } else {
    const bool bold = (level == LogLevel::Fatal);
    WithColor _(os, levelColor(level), bold);
    os << format("[%9.3f]", elapsedSeconds)
       << formatv("[{0,-5}][{1}]  ", level,
                  fmt_align(tag, AlignStyle::Left, 21, '.'));
    printMsg(os, msg, std::forward<Args>(args)...);
  }
}

/// Formats a vector into a column-aligned grid and sends each row to \p logger.
///
/// This is analogous to a “hexdump” style display, except it prints each
/// element’s string form rather than bytes in hex.
///
/// \param array The input elements.
/// \param k The maximum total character width per output line.
/// \param sameWidth If true, all columns use the global maximum element width.
///                  If false, columns are sized per-column (vertically aligned)
///                  subject to \p k.
/// \param logger Callback that receives each formatted line (without trailing
/// spaces).
template <typename T>
void logFormattedArray(ArrayRef<T> array, size_t k, bool sameWidth,
                       function_ref<void(StringRef)> logger) {
  using namespace clang::refold;

  const size_t length = array.size();
  if (length == 0) {
    logger("<empty>");
    return;
  }

  // 1) Stringify and compute global max element width.
  std::vector<std::string> stringified;
  stringified.reserve(length);

  size_t globalMaxWidth = 0;
  for (const T &elem : array) {
    std::string s = stringutils::stringifyElement(elem);
    globalMaxWidth = std::max(globalMaxWidth, s.size());
    stringified.emplace_back(std::move(s));
  }

  auto numDecimalDigits = [](size_t v) -> size_t {
    size_t digits = 1;
    while (v >= 10) {
      v /= 10;
      ++digits;
    }
    return digits;
  };

  // 2) Index prefix: "%0Dd-%0Dd: " where D = digits(len-1).
  const size_t indexDigits = numDecimalDigits(length - 1);
  const size_t indexPrefixWidth = (indexDigits * 2) + 3; // "DDD-DDD: "
  const size_t gap = 2;

  // Guard: if even a single element cannot fit, bail.
  if (indexPrefixWidth + globalMaxWidth > k) {
    std::string msg = formatv("<element exceeds max width (k={0})>", k).str();
    logger(StringRef(msg));
    return;
  }

  // 3) Determine column structure and widths.
  size_t columnsPerRow;
  std::vector<size_t> colWidths;

  const size_t avail = k - indexPrefixWidth; // safe due to guard above

  if (sameWidth) {
    const size_t denom = globalMaxWidth + gap;
    columnsPerRow = std::max<size_t>(1, avail / denom);
    colWidths.assign(columnsPerRow, globalMaxWidth);
  } else {
    const size_t minColStride = 1 + gap;
    const size_t maxPossibleCols = std::max<size_t>(1, avail / minColStride);

    // Start at 1 column and grow while still fitting within k.
    columnsPerRow = 1;
    colWidths.assign(1, globalMaxWidth);

    for (size_t testCols = 2; testCols <= maxPossibleCols; ++testCols) {
      size_t totalWidth = indexPrefixWidth;
      std::vector<size_t> testWidths(testCols, 0);

      bool fits = true;
      for (size_t c = 0; c < testCols; ++c) {
        size_t maxW = 0;
        for (size_t idx = c; idx < length; idx += testCols)
          maxW = std::max(maxW, stringified[idx].size());

        testWidths[c] = maxW;

        const size_t add = maxW + gap;
        if (add > k - totalWidth) { // safe: totalWidth <= k guaranteed below
          fits = false;
          break;
        }
        totalWidth += add;
      }

      if (!fits)
        break;

      columnsPerRow = testCols;
      colWidths = std::move(testWidths);
    }
  }

  // 4) Emit rows.
  for (size_t i = 0; i < length; i += columnsPerRow) {
    const size_t rowEnd = std::min(i + columnsPerRow - 1, length - 1);

    std::string line;
    line.reserve(k + 16); // Add an extra cushion of space.

    line += stringutils::zpadUnsigned(i, indexDigits);
    line += "-";
    line += stringutils::zpadUnsigned(rowEnd, indexDigits);
    line += ": ";

    for (size_t j = 0; j < columnsPerRow && (i + j) < length; ++j) {
      const std::string &cell = stringified[i + j];
      const size_t fieldWidth = colWidths[j] + gap;

      line += cell;
      if (cell.size() < fieldWidth)
        line.append(fieldWidth - cell.size(), ' ');
    }

    logger(StringRef(line).rtrim());
  }
}

inline bool inLogLevel(LogLevel level) {
  return static_cast<unsigned>(LogLevelOpt.getValue()) >=
         static_cast<unsigned>(level);
}

namespace clang {
namespace refold {

inline bool inFatalMode() { return inLogLevel(LogLevel::Fatal); }

inline bool inErrorMode() { return inLogLevel(LogLevel::Error); }

inline bool inWarnMode() { return inLogLevel(LogLevel::Warn); }

inline bool inInfoMode() { return inLogLevel(LogLevel::Info); }

inline bool inDebugMode() { return inLogLevel(LogLevel::Debug); }

inline bool inTraceMode() { return inLogLevel(LogLevel::Trace); }

/// Report an unrecoverable condition and terminate with a nonzero status.
///
/// This exits rather than aborting.  A fatal here is a refusal -- an unreadable
/// input, a missing producer artifact, a proof the tool declines to make -- not
/// a violated internal invariant, and aborting turned those refusals into core
/// dumps with an LLVM stack trace that read as compiler crashes.  The
/// diagnostic is already emitted above, so the exit status is the whole
/// remaining signal.
template <typename... Args>
[[noreturn]]
static inline void fatal(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::Fatal, tag, msg, std::forward<Args>(args)...);
  std::exit(1);
}

template <typename... Args>
static inline void error(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::Error, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void warn(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::Warn, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void info(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::Info, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void debug(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::Debug, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void trace(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::Trace, tag, msg, std::forward<Args>(args)...);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLDLOG_H
