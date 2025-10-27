//===--- RefoldLog.h --------------------------------------------*- C++ -*-===//
//
// Lightweight logging utilities for the refolding tools.
//
// Overview
// --------
// Provides a tiny, LLVM-friendly logging facade with explicit log levels and
// printf-free formatting via llvm::formatv. The interface is header-only for
// call sites (definitions typically live alongside the library) and aims to be
// simple, deterministic, and easy to stub in unit tests.
//
// Features
// --------
//  • Log levels: trace, debug, info, warn, error, fatal (fatal terminates).
//  • Format strings use llvm::formatv-style placeholders: {0}, {1}, ...
//  • Destination: llvm::raw_ostream (errs() by default); pluggable sinks.
//  • Category tag (e.g., "lexer", "json") for structured filtering.
//  • Global runtime level gate; cheap level checks to avoid formatting costs.
//  • Optional compile-time stripping of verbose levels (TRACE/DEBUG) via
//  macros. • Thread-friendly: emits one complete line per call; synchronization
//  is
//    delegated to the chosen raw_ostream implementation.
//
// Usage
// -----
//   // Emit messages:
//   trace("lexer", "token={0} off={1}", tok, off);
//   debug("json",  "validated {0} entries", N);
//   info ("plan",  "includes={0} macros={1}", Incs, Macros);
//   warn ("io",    "non-UTF8 byte at {0}", pos);
//   error("map",   "missing field '{0}'", "tokmap");
//   fatal("abort", "unrecoverable error in phase {0}", phase); // terminates
//
// Policy
// ------
//  • Logging must never throw; formatting failures are treated as best-effort.
//  • Fatal logs end the process deterministically after emitting the message.
//  • Library users can override the output stream and termination hook
//    (e.g., to funnel logs into test buffers).
//
// Public API (summary)
// --------------------
//   enum class LogLevel { trace, debug, info, warn, error, fatal };
//
//   // Global configuration
//   const char* logLevelToString(LogLevel level);
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
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
template <typename T> struct format_provider<std::optional<T>> {
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

static inline StringRef logLevelToString(LogLevel level) {
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

inline llvm::raw_ostream::Colors levelColor(LogLevel level) {
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
      static_cast<unsigned>(LogLevelOpt.getValue()))
    return;

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
      os << llvm::formatv(fmt.c_str(), std::forward<Args>(args)...) << '\n';
    }
  };

  // Colored prefix based on level (fatal is bold “bright red”)
  if (level == LogLevel::trace || !useColor) {
    os << formatv("|refold::{0}|  ", fmt_align(tag, AlignStyle::Left, 21, '.'));
    printMsg(os, msg, std::forward<Args>(args)...);
  } else {
    const bool bold = (level == LogLevel::fatal);
    llvm::WithColor _(os, levelColor(level), bold);
    os << formatv("|refold::{0}|  ",
                  fmt_align(tag, llvm::AlignStyle::Left, 21, '.'));
    printMsg(os, msg, std::forward<Args>(args)...);
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
