//===- PPMacroPrinting.cpp - Frontend-internal macro printing ---*- C++ -*-===//
//
// This file declares a small helper for printing macro definitions in a form
// accepted by the preprocessor (#define …) — shared by PPO and refold map.
//
//===----------------------------------------------------------------------===//
#include "PPMacroPrinting.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

namespace clang {

void PrintMacroDefinition(const IdentifierInfo &II, const MacroInfo &MI,
                          Preprocessor &PP, raw_ostream *OS) {
  *OS << "#define " << II.getName();

  if (MI.isFunctionLike()) {
    *OS << '(';
    if (!MI.param_empty()) {
      MacroInfo::param_iterator AI = MI.param_begin(), E = MI.param_end();
      for (; AI + 1 != E; ++AI) {
        *OS << (*AI)->getName();
        *OS << ',';
      }

      // Last argument.
      if ((*AI)->getName() == "__VA_ARGS__")
        *OS << "...";
      else
        *OS << (*AI)->getName();
    }

    if (MI.isGNUVarargs())
      *OS << "..."; // #define foo(x...)

    *OS << ')';
  }

  // GCC always emits a space, even if the macro body is empty.  However, do not
  // want to emit two spaces if the first token has a leading space.
  if (MI.tokens_empty() || !MI.tokens_begin()->hasLeadingSpace())
    *OS << ' ';

  SmallString<128> SpellingBuffer;
  for (const auto &T : MI.tokens()) {
    if (T.hasLeadingSpace())
      *OS << ' ';

    *OS << PP.getSpelling(T, SpellingBuffer);
  }
}

} // namespace clang
