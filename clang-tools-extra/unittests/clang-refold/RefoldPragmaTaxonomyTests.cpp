//===-- RefoldPragmaTaxonomyTests.cpp --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The pragma classification is a pure function over a spelling, so its whole
// contract is testable here rather than through input shapes that would have to
// reach a fallback path first.
//
// The tests that matter most are the negative ones: an unrecognized spelling
// must classify as `Unknown`, and `Unknown` must be simultaneously the most
// state-changing and the most binding answer.  Every admission built on this
// taxonomy inherits that default, so a spelling silently drifting out of
// `Unknown` is how an unsound realization would be admitted.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldPragmaTaxonomy.h"

#include "clang/Basic/LangOptions.h"

#include "gtest/gtest.h"

using namespace clang;
using namespace clang::refold;

namespace {

LangOptions cLangOptions() {
  LangOptions lang;
  lang.C99 = true;
  return lang;
}

TEST(RefoldPragmaTaxonomy, RecognizesModeledSpellings) {
  struct Case {
    const char *text;
    PragmaStateEffect effect;
  };
  const Case cases[] = {
      {"#pragma once\n", PragmaStateEffect::IncludeOnce},
      {"  #  pragma   once  \n", PragmaStateEffect::IncludeOnce},
      {"#pragma once /* trailing */\n", PragmaStateEffect::IncludeOnce},
      {"#pragma message(\"hi\")\n", PragmaStateEffect::NoState},
      {"#pragma warning(\"hi\")\n", PragmaStateEffect::NoState},
      {"#pragma GCC poison FOO\n", PragmaStateEffect::PoisonIdentifiers},
      {"#pragma GCC poison FOO BAR\n", PragmaStateEffect::PoisonIdentifiers},
      {"#pragma GCC system_header\n", PragmaStateEffect::SystemHeader},
      {"#pragma GCC diagnostic push\n", PragmaStateEffect::DiagnosticState},
      {"#pragma clang diagnostic pop\n", PragmaStateEffect::DiagnosticState},
      {"#pragma push_macro(\"M\")\n", PragmaStateEffect::MacroStateStack},
      {"#pragma pop_macro(\"M\")\n", PragmaStateEffect::MacroStateStack},
  };

  for (const Case &c : cases) {
    const PragmaClassification classification = classifyPragmaDirective(c.text);
    EXPECT_EQ(classification.effect, c.effect) << c.text;
    // Everything the taxonomy models is non-binding.  A construct-binding
    // pragma is exactly one it does not model.
    EXPECT_EQ(classification.binding, PragmaConstructBinding::NonBinding)
        << c.text;
    EXPECT_TRUE(classification.IsClassified()) << c.text;
  }
}

TEST(RefoldPragmaTaxonomy, UnrecognizedSpellingsFailClosed) {
  // Binding vendor pragmas, near-misses, and malformed operands must all land
  // on the same conservative answer.
  const char *unclassified[] = {
      "#pragma omp parallel for\n",
      "#pragma clang loop unroll(enable)\n",
      "#pragma GCC ivdep\n",
      "#pragma GCC unroll 4\n",
      "#pragma pack(1)\n",
      "#pragma once_more\n",        // longer identifier, not `once`
      "#pragma onceextra\n",        // no separator
      "#pragma GCC poison\n",       // no identifier operand
      "#pragma GCC poison 12\n",    // operand is not an identifier
      "#pragma push_macro(M)\n",    // operand is not a string literal
      "#pragma push_macro(\"\")\n", // empty macro name
      "#pragma once /* unterminated\n",
      "#pragma\n",
      "not a pragma at all\n",
      "",
  };

  for (const char *text : unclassified) {
    const PragmaClassification classification = classifyPragmaDirective(text);
    EXPECT_EQ(classification.effect, PragmaStateEffect::Unknown) << text;
    EXPECT_EQ(classification.binding,
              PragmaConstructBinding::BindsFollowingConstruct)
        << text;
    EXPECT_FALSE(classification.IsClassified()) << text;
  }
}

TEST(RefoldPragmaTaxonomy, RecordsNamedIdentifiers) {
  const PragmaClassification poison =
      classifyPragmaDirective("#pragma GCC poison FOO BAR\n");
  ASSERT_EQ(poison.namedIdentifiers.size(), 2u);
  EXPECT_EQ(poison.namedIdentifiers[0], "FOO");
  EXPECT_EQ(poison.namedIdentifiers[1], "BAR");

  const PragmaClassification push =
      classifyPragmaDirective("#pragma push_macro(\"NAME\")\n");
  ASSERT_EQ(push.namedIdentifiers.size(), 1u);
  EXPECT_EQ(push.namedIdentifiers[0], "NAME");
}

TEST(RefoldPragmaTaxonomy, OperandOnceGrammarMatchesDirective) {
  // `_Pragma("once")` carries the operand without the introducer, so the two
  // entry points must agree on what counts as once-state.
  EXPECT_TRUE(pragmaOperandNamesOnce("once"));
  EXPECT_TRUE(pragmaOperandNamesOnce("  once  "));
  EXPECT_TRUE(pragmaOperandNamesOnce("once // trailing"));
  EXPECT_FALSE(pragmaOperandNamesOnce("once_more"));
  EXPECT_FALSE(pragmaOperandNamesOnce("onceextra"));
  EXPECT_FALSE(pragmaOperandNamesOnce("twice"));
  EXPECT_FALSE(pragmaOperandNamesOnce(""));
}

TEST(RefoldPragmaTaxonomy, PoisonIsObservedOnlyByThePoisonedIdentifier) {
  const LangOptions lang = cLangOptions();
  const PragmaClassification poison =
      classifyPragmaDirective("#pragma GCC poison FOO\n");

  EXPECT_FALSE(payloadObservesPragmaState(poison, "3", lang));
  EXPECT_FALSE(payloadObservesPragmaState(poison, "int x = 1;", lang));
  EXPECT_FALSE(payloadObservesPragmaState(poison, "\"FOO\"", lang))
      << "a poisoned name inside a string literal is not an identifier";
  EXPECT_TRUE(payloadObservesPragmaState(poison, "FOO", lang));
  EXPECT_TRUE(payloadObservesPragmaState(poison, "int y = FOO;", lang));
}

TEST(RefoldPragmaTaxonomy, DiagnosticOnlyEffectsAreNeverObserved) {
  const LangOptions lang = cLangOptions();
  for (const char *text :
       {"#pragma message(\"m\")\n", "#pragma GCC diagnostic push\n",
        "#pragma GCC system_header\n"}) {
    const PragmaClassification classification = classifyPragmaDirective(text);
    EXPECT_FALSE(
        payloadObservesPragmaState(classification, "int x = FOO;", lang))
        << text;
  }
}

TEST(RefoldPragmaTaxonomy, OnceIsObservedOnlyByDirectiveCapablePayload) {
  const LangOptions lang = cLangOptions();
  const PragmaClassification once = classifyPragmaDirective("#pragma once\n");

  EXPECT_FALSE(payloadObservesPragmaState(once, "int x = 1;", lang));
  EXPECT_FALSE(payloadObservesPragmaState(once, "\"# not a hash\"", lang));
  EXPECT_TRUE(payloadObservesPragmaState(once, "#include \"h.h\"", lang));
}

TEST(RefoldPragmaTaxonomy, MacroStateIsObservedByAnyIdentifier) {
  const LangOptions lang = cLangOptions();
  const PragmaClassification push =
      classifyPragmaDirective("#pragma push_macro(\"M\")\n");

  // Deliberately conservative: deciding that some other identifier is
  // unaffected needs macro-liveness facts this classification does not carry.
  EXPECT_FALSE(payloadObservesPragmaState(push, "1 + 2", lang));
  EXPECT_TRUE(payloadObservesPragmaState(push, "M", lang));
  EXPECT_TRUE(payloadObservesPragmaState(push, "unrelated", lang));
}

TEST(RefoldPragmaTaxonomy, UnknownObservesEverything) {
  const LangOptions lang = cLangOptions();
  const PragmaClassification unknown =
      classifyPragmaDirective("#pragma omp parallel for\n");

  EXPECT_TRUE(payloadObservesPragmaState(unknown, "", lang));
  EXPECT_TRUE(payloadObservesPragmaState(unknown, "1", lang));
}

} // namespace
