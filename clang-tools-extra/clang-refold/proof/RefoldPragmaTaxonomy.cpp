//===--- RefoldPragmaTaxonomy.cpp - Pragma classification -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldPragmaTaxonomy.h"

#include "line-control/SourceLineDirectiveHelpers.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/StringSwitch.h"

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Consume `word` as a complete identifier, rejecting a longer identifier that
/// merely starts with it.  `once_more` must not be read as `once`.
bool consumeKeyword(StringRef text, size_t &pos, StringRef word) {
  if (!text.substr(pos).starts_with(word))
    return false;
  const size_t next = pos + word.size();
  if (next < text.size() && stringutils::isIdentPart(text[next]))
    return false;
  pos = next;
  return true;
}

/// Consume one identifier, returning its spelling.
bool consumeIdentifierWord(StringRef text, size_t &pos, StringRef &out) {
  size_t cursor = pos;
  if (cursor >= text.size() || !stringutils::isIdentStart(text[cursor]))
    return false;
  const size_t begin = cursor;
  while (cursor < text.size() && stringutils::isIdentPart(text[cursor]))
    ++cursor;
  out = text.slice(begin, cursor);
  pos = cursor;
  return true;
}

/// Return whether everything from `pos` on is directive trivia.
bool restIsTrivia(StringRef text, size_t pos) {
  return isWsOrCompleteCommentTrivia(text.drop_front(pos));
}

/// Collect the identifier list a `GCC poison` directive names.
///
/// Returns false if any operand is not an identifier, because the directive is
/// then not the spelling this classification models.
bool consumeIdentifierList(StringRef text, size_t pos,
                           SmallVectorImpl<StringRef> &out) {
  while (true) {
    stringutils::skipNonNewlineWs(text, pos);
    if (restIsTrivia(text, pos))
      return !out.empty();

    StringRef identifier;
    if (!consumeIdentifierWord(text, pos, identifier))
      return false;
    out.push_back(identifier);
  }
}

/// Recover the macro name operand of `push_macro`/`pop_macro`.
///
/// The operand is a parenthesized string literal, `push_macro("NAME")`.  Only
/// that exact shape is recognized; anything else leaves the classification
/// without a name and is treated as unclassified by the caller.
bool consumeParenthesizedStringOperand(StringRef text, size_t pos,
                                       StringRef &out) {
  stringutils::skipNonNewlineWs(text, pos);
  if (pos >= text.size() || text[pos] != '(')
    return false;
  ++pos;
  stringutils::skipNonNewlineWs(text, pos);
  if (pos >= text.size() || text[pos] != '"')
    return false;
  ++pos;
  const size_t begin = pos;
  while (pos < text.size() && text[pos] != '"') {
    // A macro name cannot contain an escape, so a backslash means this is not
    // the modeled spelling.
    if (text[pos] == '\\')
      return false;
    ++pos;
  }
  if (pos >= text.size())
    return false;
  out = text.slice(begin, pos);
  ++pos;

  stringutils::skipNonNewlineWs(text, pos);
  if (pos >= text.size() || text[pos] != ')')
    return false;
  ++pos;
  return !out.empty() && restIsTrivia(text, pos);
}

/// Classify the `GCC`/`clang` namespaced spellings.
PragmaClassification classifyNamespacedPragma(StringRef namespaceName,
                                              StringRef text, size_t pos) {
  PragmaClassification result;

  StringRef action;
  stringutils::skipNonNewlineWs(text, pos);
  if (!consumeIdentifierWord(text, pos, action))
    return result;

  // `clang diagnostic` and `GCC diagnostic` share one state model.
  if (action == "diagnostic") {
    result.effect = PragmaStateEffect::DiagnosticState;
    result.binding = PragmaConstructBinding::NonBinding;
    return result;
  }

  if (namespaceName != "GCC")
    return result;

  if (action == "poison") {
    if (!consumeIdentifierList(text, pos, result.namedIdentifiers)) {
      result.namedIdentifiers.clear();
      return result;
    }
    result.effect = PragmaStateEffect::PoisonIdentifiers;
    result.binding = PragmaConstructBinding::NonBinding;
    return result;
  }

  if (action == "system_header" && restIsTrivia(text, pos)) {
    result.effect = PragmaStateEffect::SystemHeader;
    result.binding = PragmaConstructBinding::NonBinding;
    return result;
  }

  // `GCC ivdep`, `GCC unroll` and the rest attach to the following construct.
  return result;
}

} // namespace

PragmaClassification classifyPragmaDirective(StringRef directiveText) {
  PragmaClassification result;

  StringRef text = directiveText;
  size_t pos = 0;

  stringutils::skipNonNewlineWs(text, pos);
  if (pos >= text.size() || text[pos] != '#')
    return result;
  ++pos;

  stringutils::skipNonNewlineWs(text, pos);
  if (!consumeKeyword(text, pos, "pragma"))
    return result;

  stringutils::skipNonNewlineWs(text, pos);
  StringRef head;
  if (!consumeIdentifierWord(text, pos, head))
    return result;

  if (head == "once") {
    if (!restIsTrivia(text, pos))
      return result;
    result.effect = PragmaStateEffect::IncludeOnce;
    result.binding = PragmaConstructBinding::NonBinding;
    return result;
  }

  // Diagnostic-only spellings.  The operand is not validated because no operand
  // spelling changes the effect: these produce a message and nothing else.
  if (head == "message" || head == "warning" || head == "error") {
    result.effect = PragmaStateEffect::NoState;
    result.binding = PragmaConstructBinding::NonBinding;
    return result;
  }

  // Editor-only region markers.  Clang registers `PragmaRegionHandler` for both
  // spellings and its handler body is empty -- the directive is recognized,
  // consumed, and does nothing -- so it changes no state a payload could
  // observe and binds to nothing that follows it.  The operand is free text a
  // folding editor displays, so it is not validated for the same reason the
  // diagnostic spellings' operands are not.
  if (head == "region" || head == "endregion") {
    result.effect = PragmaStateEffect::NoState;
    result.binding = PragmaConstructBinding::NonBinding;
    return result;
  }

  if (head == "push_macro" || head == "pop_macro") {
    StringRef macroName;
    if (!consumeParenthesizedStringOperand(text, pos, macroName))
      return result;
    result.effect = PragmaStateEffect::MacroStateStack;
    result.binding = PragmaConstructBinding::NonBinding;
    result.namedIdentifiers.push_back(macroName);
    return result;
  }

  if (head == "GCC" || head == "clang")
    return classifyNamespacedPragma(head, text, pos);

  return result;
}

bool pragmaOperandNamesOnce(StringRef operandText) {
  size_t pos = 0;
  stringutils::skipNonNewlineWs(operandText, pos);
  if (!consumeKeyword(operandText, pos, "once"))
    return false;
  stringutils::skipNonNewlineWs(operandText, pos);
  return restIsTrivia(operandText, pos);
}

bool payloadObservesPragmaState(const PragmaClassification &classification,
                                StringRef payload, const LangOptions &lang,
                                MacroStateObservationAnswer macroState) {
  switch (classification.effect) {
  case PragmaStateEffect::Unknown:
    return true;

  case PragmaStateEffect::NoState:
  case PragmaStateEffect::DiagnosticState:
  case PragmaStateEffect::SystemHeader:
    // These change which diagnostics fire, never which tokens are produced, so
    // no payload placement can observe them.
    return false;

  case PragmaStateEffect::MacroStateStack:
    // A caller holding the producer's macro records answers this one; only
    // without such a proof does the any-identifier scan below decide it.
    switch (macroState) {
    case MacroStateObservationAnswer::Unobserved:
      return false;
    case MacroStateObservationAnswer::Observed:
      return true;
    case MacroStateObservationAnswer::Unproven:
      break;
    }
    break;

  case PragmaStateEffect::IncludeOnce:
  case PragmaStateEffect::PoisonIdentifiers:
    break;
  }

  SmallVector<RefoldLexBoundaryToken, 32> tokens;
  refoldLexBoundaryTokens(payload, lang, tokens);

  if (classification.effect == PragmaStateEffect::IncludeOnce) {
    // Once-state is observable only by inclusion.  A payload that cannot
    // introduce a directive cannot include anything.  The raw lexer decides
    // what a `#` is, so a hash inside a literal or comment is not counted.
    for (const RefoldLexBoundaryToken &token : tokens)
      if (token.kind == tok::hash || token.kind == tok::hashhash)
        return true;
    return false;
  }

  for (const RefoldLexBoundaryToken &token : tokens) {
    if (token.kind != tok::raw_identifier)
      continue;

    // A saved or restored macro definition can change how any identifier
    // expands.  Reaching here means no caller proved otherwise, so treat every
    // identifier as observing.
    if (classification.effect == PragmaStateEffect::MacroStateStack)
      return true;

    for (StringRef poisoned : classification.namedIdentifiers)
      if (token.spelling == poisoned)
        return true;
  }
  return false;
}

StringRef toString(PragmaStateEffect effect) {
  switch (effect) {
  case PragmaStateEffect::Unknown:
    return "Unknown";
  case PragmaStateEffect::NoState:
    return "NoState";
  case PragmaStateEffect::IncludeOnce:
    return "IncludeOnce";
  case PragmaStateEffect::PoisonIdentifiers:
    return "PoisonIdentifiers";
  case PragmaStateEffect::MacroStateStack:
    return "MacroStateStack";
  case PragmaStateEffect::DiagnosticState:
    return "DiagnosticState";
  case PragmaStateEffect::SystemHeader:
    return "SystemHeader";
  }
  llvm_unreachable("invalid pragma state effect");
}

StringRef toString(PragmaConstructBinding binding) {
  switch (binding) {
  case PragmaConstructBinding::BindsFollowingConstruct:
    return "BindsFollowingConstruct";
  case PragmaConstructBinding::NonBinding:
    return "NonBinding";
  }
  llvm_unreachable("invalid pragma construct binding");
}

} // namespace refold
} // namespace clang
