//===--- RefoldLog.cpp - Logging option definitions -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Definitions for the logging surface declared in RefoldLog.h.
//
// The engine logs from nearly every translation unit, so `LogLevelOpt` must be
// defined by the engine itself rather than by whichever program links it.
// Defining it in a program's `main` leaves the library with an undefined
// reference that only a `main` can satisfy -- invisible while the tool is the
// sole consumer, and a link error for anything else.
//
// The option category lives here for the same reason and because `LogLevelOpt`
// names it at construction: keeping both in one translation unit makes their
// initialization order a language guarantee instead of a link-order accident.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLog.h"

// Declared at global scope in RefoldLog.h, matching the logging macros' use
// sites, so the definitions live at global scope too.
cl::OptionCategory RefoldCategory("clang-refold options");

cl::opt<LogLevel> LogLevelOpt(
    "log-level", cl::desc("Set log level"),
    cl::values(clEnumValN(LogLevel::Trace, "trace", "Trace"),
               clEnumValN(LogLevel::Debug, "debug", "Debug"),
               clEnumValN(LogLevel::Info, "info", "Info  (default)"),
               clEnumValN(LogLevel::Warn, "warn", "Warn"),
               clEnumValN(LogLevel::Error, "error", "Error"),
               clEnumValN(LogLevel::Fatal, "fatal", "Fatal")),
    cl::init(LogLevel::Info), cl::cat(RefoldCategory));
