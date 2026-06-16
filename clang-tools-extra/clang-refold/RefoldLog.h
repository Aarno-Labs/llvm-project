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
//   enum class LogLevel { trace, debug, info, warn, error, fatal };
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

#include "DiffAlgorithms.h"
#include "StringUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
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
    llvm_unreachable("fatal logging unexpectedly disabled");                  \
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

namespace llvm {
// General provider for `std::optional<>`
template <typename T> struct format_provider<std::optional<T>, void> {
  static void format(const std::optional<T> &opt, raw_ostream &os,
                     StringRef style) {
    if (!opt) {
      // Default "none" text; adjust to taste ("" for empty, etc.)
      os << "(none)";
      return;
    }
    // Delegate formatting of the contained value, honoring any :Style
    format_provider<T>::format(*opt, os, style);
  }
};

using namespace clang::refold::diffutils;

// Detector for class/structs with a `ToString()` member function.
template <typename T, typename = void>
struct has_to_string : std::false_type {};

template <typename T>
struct has_to_string<T, std::void_t<decltype(std::declval<T>().ToString())>>
    : std::is_same<decltype(std::declval<T>().ToString()), std::string> {};

// General `ToString()` provider, excluding Hunk (we special case this)
template <typename T>
struct format_provider<
    T, std::enable_if_t<has_to_string<T>::value && !std::is_same_v<T, Hunk>>> {
  static void format(const T &val, raw_ostream &os, StringRef style) {
    os << val.ToString();
  }
};

// Detector for enums that have a 'toString' function available via ADL
template <typename T, typename = void>
struct has_enum_to_string : std::false_type {};

template <typename T>
struct has_enum_to_string<T, std::void_t<decltype(toString(std::declval<T>()))>>
    : std::is_enum<T> {};

// The General Enum Provider
template <typename T>
struct format_provider<T, std::enable_if_t<has_enum_to_string<T>::value>> {
  static void format(const T &val, llvm::raw_ostream &os, StringRef style) {
    // This calls the toString(T) function found via Argument Dependent Lookup
    os << toString(val);
  }
};

// This handles cl::opt wrapper specifically
template <typename T>
struct format_provider<
    llvm::cl::opt<T>,
    std::enable_if_t<!llvm::support::detail::use_string_formatter<
        llvm::cl::opt<T>>::value>> {
  static void format(const llvm::cl::opt<T> &val, llvm::raw_ostream &os,
                     StringRef style) {
    // We delegate to the provider for the underlying type T
    // Note: We use val.getValue() because operator* doesn't exist
    format_provider<T>::format(val.getValue(), os, style);
  }
};
} // namespace llvm

using namespace llvm;

enum class LogLevel : unsigned {
  fatal = 0,
  error = 1,
  warn = 2,
  info = 3,
  debug = 4,
  trace = 5
};

static inline StringRef toString(LogLevel level) {
  switch (level) {
  case LogLevel::trace:
    return "trace";
  case LogLevel::debug:
    return "debug";
  case LogLevel::info:
    return "info";
  case LogLevel::warn:
    return "warn";
  case LogLevel::error:
    return "error";
  case LogLevel::fatal:
    return "fatal";
  }
  llvm_unreachable("Invalid log level");
}

extern cl::opt<LogLevel> LogLevelOpt;

inline raw_ostream::Colors levelColor(LogLevel level) {
  using color = raw_ostream::Colors;
  switch (level) {
  case LogLevel::fatal:
    return color::RED; // bold handled separately
  case LogLevel::error:
    return color::RED;
  case LogLevel::warn:
    return color::YELLOW;
  case LogLevel::info:
    return color::CYAN;
  case LogLevel::debug:
    return color::GREEN;
  case LogLevel::trace:
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
  if (level == LogLevel::trace || !useColor) {
    os << formatv("[{0,-5}][{1}]  ", level,
                  fmt_align(tag, AlignStyle::Left, 21, '.'));
    printMsg(os, msg, std::forward<Args>(args)...);
  } else {
    const bool bold = (level == LogLevel::fatal);
    WithColor _(os, levelColor(level), bold);
    os << formatv("[{0,-5}][{1}]  ", level,
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
    line.reserve(k + 16);  // Add an extra cushion of space.

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

inline bool inFatalMode() { return inLogLevel(LogLevel::fatal); }

inline bool inErrorMode() { return inLogLevel(LogLevel::error); }

inline bool inWarnMode() { return inLogLevel(LogLevel::warn); }

inline bool inInfoMode() { return inLogLevel(LogLevel::info); }

inline bool inDebugMode() { return inLogLevel(LogLevel::debug); }

inline bool inTraceMode() { return inLogLevel(LogLevel::trace); }

template <typename... Args>
[[noreturn]]
static inline void fatal(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::fatal, tag, msg, std::forward<Args>(args)...);
  std::abort();
}

template <typename... Args>
static inline void error(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::error, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void warn(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::warn, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void info(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::info, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void debug(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::debug, tag, msg, std::forward<Args>(args)...);
}

template <typename... Args>
static inline void trace(StringRef tag, StringRef msg, Args &&...args) {
  logMsg(LogLevel::trace, tag, msg, std::forward<Args>(args)...);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLDLOG_H
