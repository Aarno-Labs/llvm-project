//===- RefoldMapBuilder.h - Build clang-refold mapping ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header declares RefoldMapBuilder, a thin recorder around Clang’s
// Preprocessor that constructs the “refold map” consumed by clang-refold.
//
// The refold map is a deterministic description of how the *original*
// preprocessed token stream (A) was produced from source: it includes
// per-item token spans (macros, include directives, files), byte anchors for
// source sites (e.g. the line of an #include), optional coverage intervals
// in A (“pp_cover”), and a pp-token-to-source byte mapping (tokmap). The map
// is emitted as JSON and enables clang-refold to re-integrate edits made to a
// retransformed preprocessed stream (B) back into the original C/C++ sources.
//
// RefoldMapBuilder listens to:
//   - File entry/exit (#include nesting) to build a logical include tree.
//   - Macro define/undef and expansions to capture items and spans.
//   - Raw #pragma lines (opaque zero-token state items) with byte anchors
//     and include-owner provenance when available.
//   - Every printed preprocessor token to extend half-open token spans and
//     fill the primary token map.
//
// Invariants:
//   - Token spans per item are contiguous half-open intervals [Begin, End).
//   - Item “cover” is the minimal A interval covering all spans (may be
//     absent/empty and represented as [-1,-1) downstream).
//   - Path fields used as identity keys may be canonicalized internally. For
//     include edges, JSON `resolved_path` is retained as a legacy
//     spelling-preserving alias; new maps emit `opened_path` for physical
//     identity and `entered_file_spelling` for filename-observer proofs.
//
// This builder is intentionally serialization-agnostic except for the final
// `writeJSON()` pass.
//
// This header is private to clang/lib/Frontend/ (not installed).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_FRONTEND_REFOLDMAPBUILDER_H
#define LLVM_CLANG_FRONTEND_REFOLDMAPBUILDER_H

#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <cstdint>
#include <string>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

template <bool kWithCR = false>
static inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\f' || c == '\v' ||
         (kWithCR && c == '\r');
}

/// Half-open token range over the printed stream: [Begin, End). `Open == true`
/// while we are still extending the contiguous run for the owning item.
struct TokenSpan {
  uint64_t Begin = 0, End = 0;
  bool Open = false;
};

struct ArgTokenSpan {
  uint64_t Begin = 0, End = 0;
  std::optional<uint32_t> ArgIndex;
  bool Open = false;

  // Raw spelling location of the last expansion token folded into this span.
  // Two occurrences of the same single-token formal re-substitute the SAME
  // source token, so their fully resolved spelling locations are identical;
  // every other adjacency (a multi-token argument, or a nested macro-body token
  // that intrudes into the argument's expansion) resolves to distinct
  // locations.  See touchArgTokSpan().
  uint64_t LastSpellingLocRaw = 0;
  bool HasSpelling = false;

  // For token-internal provenance spans (currently paste_spans and wrapped
  // stringify_spans): byte range within the spelled output token
  // [ByteBegin, ByteEnd). When HasByteRange is false, ByteBegin/ByteEnd are
  // ignored.
  uint32_t ByteBegin = 0, ByteEnd = 0;
  bool HasByteRange = false;
};

struct MacroParam {
  std::string Name;
  bool Variadic = false;
};

enum MacroReplacementTokenKind {
  MRT_Literal,
  MRT_ParamRef,
};

/// One token in a macro definition replacement list, as understood by Clang at
/// definition time.
///
/// The consumer can use this as a producer-owned replay tape for simple macro
/// expansion proofs instead of reparsing the textual #define directive.  The
/// tape intentionally records token spellings, not source byte ranges: macro
/// definition editing is outside clang-refold's core refolding domain, while
/// deterministic replay only needs to know which replacement-list tokens are
/// fixed literals and which are formal-parameter references.
struct MacroReplacementToken {
  MacroReplacementTokenKind Kind = MRT_Literal;
  std::string Spelling;
  std::optional<uint32_t> ParamIndex;
};

enum MacroCalleeOriginKind {
  MCO_LiteralMacroName,
  MCO_CallerParam,
  MCO_Paste,
  MCO_Opaque
};

enum MacroCalleeOriginPartKind {
  MCOP_Literal,
  MCOP_CallerArgSlice,
};

/// One segment of a producer-proven macro callee spelling.
///
/// For paste-derived callees, the consumer needs to know whether each substring
/// of the callee token came from fixed replacement-list text or from a root
/// invocation argument slice.  Recording that projection here turns Step-5
/// selector substitution into a mechanical proof over existing macros: the
/// consumer can rewrite the named root selector argument only when a unique
/// alternate pasted callee exactly explains the edited B expansion.
struct MacroCalleeOriginPart {
  MacroCalleeOriginPartKind Kind = MCOP_Literal;
  std::string Spelling;
  std::optional<uint64_t> RootMacroId;
  std::optional<uint32_t> RootParamIndex;
  uint32_t ByteBegin = 0;
  uint32_t ByteEnd = 0;
};

struct MacroCalleeOrigin {
  MacroCalleeOriginKind Kind = MCO_LiteralMacroName;
  std::vector<uint32_t> CallerParamIndices;
  std::string Spelling;
  SmallVector<MacroCalleeOriginPart, 4> Parts;
};

/// Structural forwarding metadata for a callee argument derived from a slice
/// of a caller formal argument, rather than by textual identifier reference.
/// This captures higher-order patterns such as:
///
///   #define H(z) (G z)
///
/// where the caller formal `z` is a parenthesized signature tuple whose
/// elements become the callee arguments of `G` after substitution.
struct InvArgTupleRef {
  uint32_t CallerParamIndex = 0;
  uint32_t CallerByteBegin = 0;
  uint32_t CallerByteEnd = 0;
};

struct PastePart {
  // ArgIndex >= 0 is a macro argument index; nullopt denotes a fixed literal
  // contribution from the macro replacement list.
  std::optional<uint32_t> ArgIndex = std::nullopt;

  // Byte range of this part inside the final pasted token spelling.
  uint32_t ByteBegin = 0;
  uint32_t ByteEnd = 0;

  // Exact spelling of this part as it contributes to the final pasted token.
  // For literal parts, this is the fixed delimiter/body text. For argument
  // parts, this is the substituted argument-token spelling.
  std::string Spelling;

  // Optional byte range inside the invocation argument spelling that produced
  // this argument part. These offsets are relative to that argument's raw text,
  // not absolute file offsets. They are intentionally absent for synthetic or
  // recursively resolved projection text where the producer cannot prove a
  // direct invocation-argument slice.
  std::optional<uint32_t> ArgByteBegin;
  std::optional<uint32_t> ArgByteEnd;
};

struct PasteToken {
  std::string Spelling;
  SmallVector<PastePart, 4> Parts;
};

struct HeaderDecl {
  std::string Kind;  // "function", "unknown", etc.
  std::string Name;  // e.g. "first"
  std::string File;  // header path for header_span.file
  uint64_t HeaderB = 0;
  uint64_t HeaderE = 0;
  uint64_t PPBegin = 0; // A-token index
  uint64_t PPEnd = 0;   // A-token index
};

/// Discriminates item category in the map: preprocessor directive, macro, or
/// file pseudo-item (for TU-level token spans).
enum ItemKind { IK_Directive, IK_Macro, IK_File };

/// A recorded unit in the map: include/macro/directive/file, with text,
/// anchors, and contiguous token spans over the printed stream.
struct Item {
  uint64_t ID = 0;
  ItemKind Kind = IK_File;
  std::string Subkind; // "#include", "#define", ...
  std::string Name;    // macro invocation or directive macro name
  // For #define directive items, records MacroInfo::isFunctionLike() directly
  // so consumers can distinguish object-like and function-like macro state
  // without reparsing raw directive text.  It is false for #undef and non-macro
  // directives.
  bool FunctionLikeDefinition = false;
  std::string Text;    // directive text
  // Exact bytes from the producer-selected source envelope for this macro
  // invocation. Ordinary source calls generally have NAME(...) spelling, but
  // generated function-like invocations can have noncanonical envelopes such
  // as the caller argument segment "ADD, (1, 2)".  Consumers that require a
  // canonical generated call must use NormalizedInvText when it is present.
  std::string InvText;
  std::string InvFile; // file containing the macro invocation

  bool IsBuiltinMacro =
      false;          // true for predefined/builtin macros (e.g. __FILE__)
  SourceLocation Loc; // primary location

  // Exact producer-observed location of the invoked macro name token. This is
  // retained only until finalization so callee-token provenance can be
  // recomputed against the sanitized immediate caller graph rather than the
  // provisional callback-time caller.
  SourceLocation CalleeLoc;
  std::vector<TokenSpan> Spans;
  std::vector<ArgTokenSpan> ArgSpans;       // tokens from any actual arguments
  std::vector<ArgTokenSpan> StringifySpans; // tokens produced by #param
  std::vector<ArgTokenSpan> PasteSpans; // tokens produced by ## involving param
  std::vector<TokenSpan> BodySpans;     // tokens from the macro body

  // Macro-body tokens can be *derived* from invocation arguments via
  // projections:
  //   - stringification (#X) produces a string literal token
  //   - token paste (X##Y) produces a synthesized token
  //
  // To make the consumer's macro policy deterministic, we precompute the
  // *exact* spelled tokens these projections would produce for this specific
  // invocation (using MacroArgs) and then match printed macro-body tokens
  // against those spellings during onToken().
  //
  // StringifySpell2ArgIndices remains producer-internal because the consumer
  // already receives the exact stringify spans. PasteTokens, however, are now
  // serialized so the consumer can carry a first-class witness for the exact
  // pasted spelling, the ordered decomposition into literal/argument-derived
  // parts, and the deterministic replay order for repeated identical spellings.
  StringMap<SmallVector<uint32_t, 2>> StringifySpell2ArgIndices;

  // Precomputed token-paste projections for this macro invocation, in expansion
  // order. Array order is the replay order used by the consumer when the same
  // pasted spelling occurs multiple times within one invocation.
  SmallVector<PasteToken, 4> PasteTokens;
  size_t PasteTokenCursor = 0;

  // Reverse index: pasted spelling -> indices into PasteTokens.
  StringMap<SmallVector<size_t, 2>> PasteSpell2TokenIndices;

  // Definition-time macro formal parameters.
  std::vector<MacroParam> DefParams;

  // Producer-owned replacement-list replay tape for #define directive items.
  // This is populated only for directive records whose Subkind is "#define".
  std::vector<MacroReplacementToken> ReplacementTokens;

  // Invocation-site byte ranges [begin,end) for each actual argument (index
  // matches formal parameter order).
  std::vector<std::pair<std::optional<uint64_t>, std::optional<uint64_t>>>
      InvArgRanges;

  // Exact unexpanded actual-argument spellings captured while MacroArgs is
  // available. This producer-internal evidence is retained until finalization
  // so tuple forwarding metadata can be derived only after the immediate caller
  // graph and callee origin have been finalized. A null entry means that the
  // producer could not recover that argument spelling exactly.
  std::vector<std::optional<std::string>> UnexpandedArgTexts;

  // Macro nesting DAG + deterministic dependency edges for nested expansions:
  //
  // If this invocation occurs while expanding another macro, CallerMacroId
  // identifies the immediately enclosing caller invocation (its Item::ID).
  // InvArgDeps records, for each invocation argument, which caller formal(s)
  // the raw argument text references (by index in the caller's DefParams).
  // Exact #define directive item used as the active definition for this
  // invocation, when the defining directive was recorded in this map.
  std::optional<uint64_t> DefinitionDirectiveId;

  // Exact producer-owned expansion-frame identities. These remain internal
  // to finalization and are not serialized.
  uint64_t ExpansionFrameId = 0;
  uint64_t ParentExpansionFrameId = 0;

  std::optional<uint64_t> CallerMacroId;
  MacroCalleeOrigin CalleeOrigin;
  std::vector<std::vector<uint32_t>> InvArgDeps;

  struct InvArgRef {
    uint32_t CallerParamIndex = 0;
    uint32_t ByteBegin = 0;
    uint32_t ByteEnd = 0;
  };

  // For each invocation argument, record the precise byte ranges in inv_text
  // that reference caller formals (indexed by CallerParamIndex). This is a
  // refinement of InvArgDeps that allows the consumer to invert nested macro
  // argument templates hop-by-hop without re-expanding. The consumer treats
  // these slices as proof material for generalized DAG lifting; producer-side
  // upward propagation remains only a best-effort optimization.
  std::vector<std::vector<InvArgRef>> InvArgRefs;

  // Structural slice forwarding for higher-order tuple signatures. For each
  // invocation argument, record the slice(s) of the caller formal text that
  // produce that callee argument. Byte ranges are relative to the *trimmed*
  // caller-argument text returned by getInvocationArgText-style consumers.
  std::vector<std::vector<InvArgTupleRef>> InvArgTupleRefs;

  // Canonicalized invocation text for higher-order function-like invocations
  // whose raw source envelope is not a normal NAME(...) form. When present,
  // this string is synthesized deterministically from the callee name and
  // actual callee arguments, and NormalizedInvArgTextRanges are byte ranges
  // within this string. This metadata is committed atomically with tuple
  // provenance and is the only producer-owned NAME(...)-shape representation
  // for generated calls.
  std::optional<std::string> NormalizedInvText;
  std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>>
      NormalizedInvArgTextRanges;
  std::vector<HeaderDecl> Decls;

  // main-file byte range of the macro invocation (if applicable)
  std::optional<uint64_t> InvBegin;
  std::optional<uint64_t> InvEnd;

  // --- New: include-site anchors and structure ---
  std::optional<uint64_t>
      SiteBegin; // byte offset of '#' in the directive's file
  std::optional<uint64_t>
      SiteEnd;                 // one-past-end of the directive line (incl. EOL)
  std::string SitePath;        // file path that contains the directive

  // --- Pragma-operator provenance (Subkind == "#pragma") ---
  // True when the directive was spelled with the `_Pragma("...")` operator
  // rather than a `#pragma` directive line.  Lets the consumer fold a pragma
  // content edit back into the operator form instead of a raw `#pragma`.
  bool ViaPragmaOperator = false;
  // For a `_Pragma` operator, the exact source byte range of the operator
  // expression `_Pragma ( "..." )` (not the whole physical line).  Present only
  // when ViaPragmaOperator is true and the range was recoverable; lets the
  // consumer edit a mid-line `_Pragma` in place instead of rejecting a site
  // that overlaps ordinary tokens.
  std::optional<uint64_t> PragmaOperatorBegin;
  std::optional<uint64_t> PragmaOperatorEnd;
  std::string TargetAsWritten; // as-written header token ("e.h" or <vector>)
  // Historical JSON name `resolved_path`.  With the current producer default
  // EmitAbsPaths == false, this stores the producer-observed entered-file
  // spelling for the include edge: the spelling Clang exposes through
  // `__FILE__` inside the included file.  If absolute-path emission is ever
  // enabled, split physical identity and entered-file spelling into distinct
  // map fields instead of overloading this one.
  std::string ResolvedPath;

  // New include-resolution metadata.  These fields split the legacy
  // resolved_path spelling into explicit physical identity, filename-observer
  // spelling, and HeaderSearch lookup provenance.
  std::string OpenedPath;
  std::string EnteredFileSpelling;
  std::string EnteredFileName;

  // File entry opened by this include, retained so the controlling macro can be
  // resolved at serialization time.  A header's include guard is only known to
  // HeaderSearch once the file has actually been lexed, which has not happened
  // yet when the InclusionDirective callback fires for a first inclusion.
  OptionalFileEntryRef OpenedFileEntry;

  // Name of the macro whose `#ifndef` guards this header's entire contents, when
  // HeaderSearch recognized one.
  //
  // A consumer that inlines this header's *tokens* rather than its source loses
  // every directive the header contained, including this guard.  The header's
  // content is then present while its controlling macro stays undefined, so any
  // later path back to the same physical file re-enters it.  Recording the name
  // lets the consumer restore that state explicitly instead of failing closed.
  std::string ControllingMacro;
  std::string LookupKind;
  std::optional<unsigned> LookupSearchChainIndex;
  std::string LookupDirectorySpelling;
  std::string LookupDirectoryPath;
  bool IncludeNextProvenanceKnown = false;
  std::optional<uint64_t> IncludeNextContainingFileIncludeId;
  std::optional<unsigned> IncludeNextResumeSearchChainIndex;

  bool IsAngled = false;       // <...> vs "..."
  std::optional<uint64_t>
      Parent; // parent include item id, or nullopt if top-level
  std::optional<uint64_t> OwnerIncludeId; // include item id that opened the
                                          // file containing this item
};

// Small utility to append/extend a half-open token span list.
inline void touchTokSpan(std::vector<TokenSpan> &V, uint64_t TokIdx) {
  if (V.empty() || V.back().End != TokIdx) V.push_back({TokIdx, TokIdx+1});
  else V.back().End++;
}

// Small utility to append/extend a half-open token span list (args only).
//
// When \p SpellingLocRaw is provided it is the raw fully-resolved spelling
// location of this token.  Index-adjacent expansion tokens of the same argument
// are merged into one span unless the new token resolves to the SAME spelling
// location as the previous one.  That happens only when the exact same source
// token is substituted again, i.e. a repeated occurrence of a single-token
// formal (e.g. `#define D2(x) x x`).  Merging those would fold both occurrences
// into one arg span, and the consumer would then reconstruct the argument as
// the concatenation of every occurrence (`D2(9 9)` instead of `D2(9)`).  A
// genuine multi-token argument, and any nested macro-body token that intrudes
// into the argument's expansion, resolve to distinct locations and still merge.
// Callers that do not pass a location keep the original merge behavior.
inline void touchArgTokSpan(std::vector<ArgTokenSpan> &V, uint64_t TokIdx,
                            uint32_t ArgIndex,
                            std::optional<uint64_t> SpellingLocRaw = std::nullopt) {
  if (!V.empty() && V.back().Open && V.back().End == TokIdx &&
      V.back().ArgIndex == ArgIndex) {
    const bool repeatedOccurrence =
        SpellingLocRaw && V.back().HasSpelling &&
        *SpellingLocRaw == V.back().LastSpellingLocRaw;
    if (!repeatedOccurrence) {
      V.back().End = TokIdx + 1;
      if (SpellingLocRaw) {
        V.back().LastSpellingLocRaw = *SpellingLocRaw;
        V.back().HasSpelling = true;
      }
      return;
    }
    // A repeated occurrence of the same formal: close the current span so the
    // occurrence below starts a fresh one.
    V.back().Open = false;
  }

  if (!V.empty() && V.back().Open)
    V.back().Open = false;

  ArgTokenSpan S;
  S.Begin = TokIdx;
  S.End = TokIdx + 1;
  S.ArgIndex = ArgIndex;
  S.Open = true;
  if (SpellingLocRaw) {
    S.LastSpellingLocRaw = *SpellingLocRaw;
    S.HasSpelling = true;
  }
  V.push_back(S);
}

inline void appendExactArgTokSpan(std::vector<ArgTokenSpan> &V, uint64_t TokIdx,
                                  uint32_t ArgIndex, uint32_t ByteBegin,
                                  uint32_t ByteEnd) {
  for (const ArgTokenSpan &E : V) {
    if (E.Begin != TokIdx || E.End != TokIdx + 1)
      continue;
    if (E.ArgIndex != ArgIndex || !E.HasByteRange)
      continue;
    if (E.ByteBegin == ByteBegin && E.ByteEnd == ByteEnd)
      return;
  }

  if (!V.empty() && V.back().Open)
    V.back().Open = false;

  ArgTokenSpan S;
  S.Begin = TokIdx;
  S.End = TokIdx + 1;
  S.ArgIndex = ArgIndex;
  S.Open = false;
  S.HasByteRange = true;
  S.ByteBegin = ByteBegin;
  S.ByteEnd = ByteEnd;
  V.push_back(S);
}

/// Mapping from a printed PP token to its source file byte range.
struct TokMapEntry {
  std::string File; // path of the source file containing [SrcBegin,SrcEnd)
  uint64_t PPIndex = 0;
  uint64_t SrcBegin = 0;
  uint64_t SrcEnd = 0;
};

/// One arm of a conditional group (#if/#elif/#else), with kind, condition text,
/// and body byte range (exclusive of directive lines).
struct CondArm {
  std::string Kind;  // "if","ifdef","ifndef","elif","else"
  std::string Cond; // optional (if/elif expr, or macro for ifdef/ifndef)
  uint64_t BodyB = 0, BodyE = 0;
  /// True when this arm's condition actually evaluated `__has_include` /
  /// `__has_include_next` during the real preprocessing run.  Because it is
  /// captured from Clang's evaluation (via the HasInclude callback) rather than
  /// scanned from `Cond`, it catches macro-hidden and token-pasted operators a
  /// textual scan would miss.  These operators perform header lookup relative to
  /// the file containing the directive, so relocating the header out of its
  /// original directory can flip the selected branch.
  bool CondUsesHasInclude = false;
};

/// A conditional group (#if..#endif) within a file, holding arms in order and
/// the byte range of the whole group.
struct CondGroup {
  std::string File;
  uint64_t GroupB = 0, GroupE = 0; // [#if .. #endif] as bytes
  std::vector<CondArm> Arms;
};

/// Producer-proven active source line-control event.
///
/// Clang has already evaluated any macro operands and conditional activity by
/// the time this event is recorded.  The consumer therefore does not need to
/// parse or re-evaluate arbitrary `#line` expressions to recover the logical
/// state established by this directive.
struct LineControlEvent {
  uint64_t ID = 0;
  std::string PhysicalFile;
  std::optional<uint64_t> SiteBegin;
  std::optional<uint64_t> SiteEnd;
  bool Active = true;
  bool ProducerProven = true;
  uint64_t LogicalLineAfter = 0;
  std::string LogicalFileAfter;
  std::optional<uint64_t> OwnerIncludeId;
  std::string Text;
};

/// Builds a deterministic “refold map” by observing the Clang preprocessor.
///
/// This class hooks into `clang::Preprocessor` callbacks (include directives,
/// macro definitions and expansions, pragmas, file enter/exit, and token
/// emission) and records enough structure to later refold edits in a modified
/// preprocessed stream back into the original source files.
///
/// Responsibilities:
///  - Assign stable IDs to items (includes, macros, files).
///  - Record per-item token spans over the printed PP token stream as
///    contiguous half-open intervals [Begin, End), extending them incrementally
///    as tokens are seen.
///  - Capture byte-accurate source anchors for directive sites (e.g. the full
///    line containing an `#include`) and, for macro invocations, the exact
///    callsite range in the owning file.
///  - Maintain the include nesting (owner/parent) to disambiguate repeated
///    includes and enable deterministic placement of edits.
///  - Emit the assembled structure to JSON (`writeJSON()`).
///
/// Paths:
///  - If the main file can be resolved, the TU path and any file paths are
///    written as canonical absolute paths. Otherwise “pseudo” paths are
///    preserved as-is.
///
/// Lifetimes & usage:
///  - Construct once per TU with a `Preprocessor&`.
///  - Feed PP events as preprocessing proceeds.
///  - Call `writeJSON()` at the end to serialize the accumulated map.
///
/// All recording is deterministic; there is no heuristic guessing here—the
/// builder emits exactly what the preprocessor did.
class RefoldMapBuilder {
  Preprocessor &PP;
  SourceManager &SM;
  const LangOptions &Lang;
  bool IgnoreComments = true;
  uint64_t TokIndex = 0;
  std::vector<uint64_t> TokPPByteBegin;
  std::vector<uint64_t> TokPPByteEnd;

  std::vector<Item> Items;
  llvm::StringMap<size_t> MacroKey2Item;
  llvm::DenseMap<const MacroInfo *, uint64_t> MacroInfo2DefinitionDirectiveId;
  /// Global index of paste-produced token spellings -> macro invocation items
  /// that can produce that spelling. Used for paste-through-stringify
  /// projection when a pasted token is later embedded inside a string literal
  /// emitted by '#'.
  llvm::StringMap<llvm::SmallVector<size_t, 2>> PasteSpell2MacroItems;
  llvm::StringMap<size_t> IncludeKey2Item;
  std::vector<std::optional<size_t>> IncludeStack; // item indices (include
                                                   // items), nullopt for none
  std::optional<size_t> CurrentFileItem;

  std::vector<TokMapEntry> TokMap;
  std::vector<LineControlEvent> LineControlEvents;

  /// (FileID, file-local byte offset) of each site where `__has_include` /
  /// `__has_include_next` was evaluated during preprocessing.  Consumed while
  /// emitting conditional arms to set CondArm::CondUsesHasInclude.
  std::vector<std::pair<clang::FileID, uint64_t>> HasIncludeSites;

  std::string OutPath;
  std::string
      Cwd; // Captured working directory (for resolving relative spellings)

  std::string TUSourcePath;  // TU path spelling (for JSON 'source')
  // Keep legacy `resolved_path` spelling-preserving by default.  New include
  // maps always carry the split fields: `opened_path` for physical identity and
  // `entered_file_spelling` for filename-observer proof.
  bool EmitAbsPaths = false;
  bool EnableByteSpans;      // If true, then serialize the per-token byte spans

  /// Map resolved absolute include directories -> unique original include-dir
  /// spelling. Ambiguous entries are marked with a private sentinel and
  /// ignored rather than guessed.
  llvm::StringMap<std::string> IncludeDirAbs2Spelling;
  /// Map resolved absolute include directories -> unique producer-known search
  /// class from HeaderSearchOptions. Ambiguous entries are marked with a
  /// private sentinel and ignored rather than guessed; HeaderSearch public
  /// range checks still classify quote/user dirs.
  llvm::StringMap<std::string> IncludeDirAbs2Kind;
  /// Map resolved absolute file paths -> chosen spelling (TU/header).
  llvm::StringMap<std::string> FileAbs2Spelling;

  /// Compute the byte interval for a directive’s *entire line*.
  ///
  /// Given the location of the `#` token, returns a half-open byte range
  /// [begin, end) covering from the `#` through the terminating newline (or
  /// end-of-file if the line is unterminated). Used to anchor directive sites
  /// in the source file.
  ///
  /// \returns (begin, end) in the directive’s source file (or nullopt if not
  ///          valid)
  std::optional<std::pair<uint64_t, uint64_t>>
  computeDirectiveLine(SourceLocation HashLoc);

  /// Canonical absolute path for a file entry (when possible).
  ///
  /// Resolves the given `FileEntryRef` to a canonical absolute path suitable
  /// for emission into the JSON map. Pseudo paths (e.g. virtual buffers) are
  /// preserved as-is.
  ///
  /// \returns absolute/canonical path string or the original pseudo path.
  static std::string absolutePathFor(const clang::FileEntryRef &FER);

  /// Path for a source location; absolute if requested.
  ///
  /// Looks up the file corresponding to `L` and returns either a canonical
  /// absolute path (when `WantAbs` is true and resolution succeeds) or the
  /// best-effort path as managed by the `SourceManager`.
  ///
  /// \returns a path string appropriate for emission.
  std::string filePathForLocAbs(clang::SourceManager &SM,
                                clang::SourceLocation L, bool WantAbs);

  // Maps a macro-expanded token’s spelling location back to the invocation-site
  // argument index (0..N-1). Returns -1 if unknown / not in invocation file.
  std::optional<uint32_t>
  argIndexForSpellingLoc(const Item &MI, SourceLocation Loc, SourceManager &SM,
                         const LangOptions &Lang, bool EmitAbsPaths);

  void addHeaderDecl(Item &Inc, StringRef Kind, StringRef Name,
                     StringRef HeaderFile, uint64_t HeaderB, uint64_t HeaderE,
                     uint64_t PPBegin, uint64_t PPEnd) {
    HeaderDecl D;
    D.Kind = Kind.str();
    D.Name = Name.str();
    D.File = HeaderFile.str();
    D.HeaderB = HeaderB;
    D.HeaderE = HeaderE;
    D.PPBegin = PPBegin;
    D.PPEnd = PPEnd;
    Inc.Decls.push_back(std::move(D));
  }

  void addHeaderDecl(Item &Inc, StringRef Name, StringRef HeaderFile,
                     uint64_t HeaderB, uint64_t HeaderE, uint64_t PPBegin,
                     uint64_t PPEnd) {
    addHeaderDecl(Inc, "unknown", Name, HeaderFile, HeaderB, HeaderE, PPBegin,
                  PPEnd);
  }

public:
  /// Construct a builder bound to a preprocessor and an output path.
  ///
  /// This builder records *spelled* paths (verbatim spellings) rather than
  /// canonicalized paths:
  ///   - the TU path spelling is taken from the command line (argv) and written
  ///     to JSON `source`,
  ///   - include file spellings are derived from the include search directory
  ///     spelling (e.g. `-I ./headers`) combined with Clang’s provided
  ///     `RelativePath` (yielding e.g. `./headers/bob.h`).
  ///
  /// When a spelling is relative, it is interpreted relative to `pp_ctx.cwd`.
  ///
  /// \param PP      Preprocessor to observe (tokens, directives, nesting).
  /// \param OutPath Destination file path for JSON output; when empty,
  ///                the builder is effectively disabled (no-ops).
  /// \param EnableByteSpans If true, then include the per-token byte spans.
  RefoldMapBuilder(Preprocessor &PP, StringRef OutPath, bool EnableByteSpans);

  /// Extend (or open) the current contiguous token span for an item.
  ///
  /// Ensures spans for the given item are maintained as half-open intervals
  /// [Begin, End) over the *printed* PP-token index space. If the last span is
  /// currently open, this advances its End to `CurTokIndex + 1`; otherwise, a
  /// new open span starting at `CurTokIndex` is created.
  ///
  /// \param ItemIdx  Index into the internal `Items` vector; nullopt is
  ///                 ignored.
  /// \param CurTokIndex Zero-based index of the just-emitted printed token.
  void touchSpanForItem(std::optional<size_t> ItemIdx, uint64_t CurTokIndex) {
    if (!ItemIdx)
      return;
    auto &V = Items[*ItemIdx].Spans;
    if (!V.empty() && V.back().Open) {
      V.back().End = CurTokIndex + 1; // extend to one-past-current
    } else {
      TokenSpan S;
      S.Begin = CurTokIndex;
      S.End = CurTokIndex + 1;
      S.Open = true;
      V.push_back(S);
    }
  }

  /// \returns true iff an output path was provided and the builder is active.
  bool enabled() const { return !OutPath.empty(); }

  /// Callback for `#include`/`#include_next`.
  ///
  /// Records:
  ///  - The directive site’s byte range (including trailing newline).
  ///  - The as-written target token (e.g. `"e.h"` or `<vector>`).
  ///  - Whether it was angled or quoted.
  ///  - The resolved filesystem path when available.
  ///  - Parent include relationships (include stack).
  ///
  /// Also prepares a fresh include item (with no spans yet); spans are
  /// associated as subsequent tokens are printed while this include is active.
  void onIncludeDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File, StringRef SearchPath,
                          StringRef RelativePath,
                          const IncludeLookupProvenance &LookupProvenance);

  /// Callback for `#define`.
  ///
  /// Creates a directive item for the definition, records the macro’s name,
  /// exact text, and opens/extends its token spans as tokens are printed.
  void onMacroDefined(const Token &MacroNameTok, const MacroDirective *MD);

  /// Callback for `#undef`.
  ///
  /// Records an `#undef` directive item with exact text and site anchors.
  /// (Token spans for `#undef` are typically empty.)
  void onMacroUndefined(const Token &MacroNameTok, const MacroDefinition &MD,
                        const MacroDirective *Undef);

  /// Callback for macro expansion at a call site.
  ///
  /// Records a macro *item* with:
  ///  - Invocation text and byte range in its owning file,
  ///  - Owner include id (to disambiguate repeated includes),
  ///  - Token spans contributed by the expansion while it is active.
  void onMacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                      SourceRange Range, const MacroArgs *Args,
                      uint64_t ExpansionFrameId,
                      uint64_t ParentExpansionFrameId);

  /// Callback for an active source `#line` / GNU line-marker directive.
  ///
  /// The preprocessor has already evaluated conditional activity and any macro
  /// operands before this callback is reached, so the recorded logical state is
  /// producer-proven.  The source-site range is best-effort: it is present when
  /// the physical directive line can be located deterministically.
  void onLineControlDirective(SourceLocation Loc);

  /// Callback for a raw `#pragma` line.
  ///
  /// Pragmas are recorded as directive items with exact source text and site
  /// byte anchors. They do not contribute to A-token spans.
  void onPragma(SourceLocation HashLoc, StringRef FullText);

  /// Callback for an evaluation of `__has_include` / `__has_include_next`.
  ///
  /// Records the (FileID, file-local offset) of the operator so conditional-arm
  /// emission can mark the arm whose directive line contains it as
  /// context-sensitive.  Captured from Clang's evaluation, so it observes the
  /// operator even when it is produced by macro expansion or token pasting.
  void onHasInclude(SourceLocation Loc);

  /// Entering a file (either TU or an included header).
  ///
  /// Pushes a new include context on the include stack and creates/updates
  /// the current file item for span attribution.
  void onEnterFile(SourceLocation IncludeLoc, SourceLocation EnterLoc);

  /// Entering a file (either TU or an included header).
  ///
  /// Pushes a new include context on the include stack and creates/updates
  /// the current file item for span attribution.
  void onExitFile() {
    if (!enabled())
      return;
    if (!IncludeStack.empty())
      IncludeStack.pop_back();
  }

  /// Observe a single *printed* preprocessor token.
  ///
  /// Advances token index and attributes the token to the currently active
  /// items (file/include/macro) by extending their open spans.
  void onToken(const Token &Tok, uint64_t PPByteBegin, uint64_t PPByteEnd);

  /// End-of-stream notification.
  ///
  /// Closes any open spans so all recorded item spans become half-open and
  /// well-formed prior to serialization.
  void onEndOfStream() {
    if (!enabled())
      return;
    for (auto &It : Items)
      for (auto &S : It.Spans)
        S.Open = false;
  }

  void finalizeIncludeDecls();

  /// Serialize the accumulated map as JSON to `OutPath`.
  ///
  /// Emits:
  ///  - Version, TU path, and token count,
  ///  - Items (macros, directives, files) with spans and anchors,
  ///  - Primary PP-token map (`TokMap`),
  ///  - Conditional groups, if collected.
  ///
  /// This function performs no additional inference; it serializes exactly
  /// what has been recorded so far.
  void writeJSON();
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_FRONTEND_REFOLDMAPBUILDER_H
