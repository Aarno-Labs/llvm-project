//===--- RefoldModel.h ------------------------------------------*- C++ -*-===//
//
// This file defines the RefoldModel class — a structured in-memory
// representation of the "refold map" JSON emitted by the modified Clang
// preprocessor. The model captures the logical mapping between preprocessor
// constructs in the original source (A) and the corresponding constructs in the
// edited preprocessed stream (B).
//
// The RefoldModel serves as a stable, deterministic schema layer over the
// untyped JSON representation, providing strongly typed access to:
//   - Include items (#include, #include_next directives)
//   - Macro invocations (object- and function-like forms)
//   - Token-to-source span mappings and ownership relationships
//
// The model performs consistency validation during construction and exposes
// lightweight query methods for downstream consumers such as RefoldEngine.
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMODEL_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMODEL_H

// Must precede any `formatv` use below (e.g. the `std::optional<>` fields
// formatted from inline `ToString()` members): the custom format providers
// have to be visible before their first implicit instantiation under GCC.
#include "core/RefoldFormatProviders.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

enum class PPArgSpanKind { Standard, Stringify, Paste };

static inline StringRef toString(PPArgSpanKind kind) {
  switch (kind) {
  case PPArgSpanKind::Standard:
    return "Standard";
  case PPArgSpanKind::Stringify:
    return "Stringify";
  case PPArgSpanKind::Paste:
    return "Paste";
  }
  llvm_unreachable("Invalid PPArgSpanKind");
}

enum class MacroCalleeOriginKind {
  LiteralMacroName,
  CallerParam,
  Paste,
  Opaque
};

/// Producer-owned include lookup classification.
///
/// Search-chain kinds correspond to entries in pp_ctx.include_search_chain.
/// SourceRelative and AbsoluteOperand are per-edge lookup kinds and are never
/// global search-chain entries. Unknown is the fail-closed representation used
/// when the producer cannot represent Clang's lookup decision without guessing.
enum class IncludeLookupKind {
  SourceRelative,
  QuoteDir,
  UserI,
  System,
  IdirAfter,
  Framework,
  Builtin,
  AbsoluteOperand,
  Unknown
};

static inline StringRef toString(IncludeLookupKind kind) {
  switch (kind) {
  case IncludeLookupKind::SourceRelative:
    return "source_relative";
  case IncludeLookupKind::QuoteDir:
    return "quote_dir";
  case IncludeLookupKind::UserI:
    return "user_I";
  case IncludeLookupKind::System:
    return "system";
  case IncludeLookupKind::IdirAfter:
    return "idirafter";
  case IncludeLookupKind::Framework:
    return "framework";
  case IncludeLookupKind::Builtin:
    return "builtin";
  case IncludeLookupKind::AbsoluteOperand:
    return "absolute_operand";
  case IncludeLookupKind::Unknown:
    return "unknown";
  }
  llvm_unreachable("Invalid IncludeLookupKind");
}

static inline bool isSearchChainIncludeLookupKind(IncludeLookupKind kind) {
  switch (kind) {
  case IncludeLookupKind::QuoteDir:
  case IncludeLookupKind::UserI:
  case IncludeLookupKind::System:
  case IncludeLookupKind::IdirAfter:
  case IncludeLookupKind::Framework:
  case IncludeLookupKind::Builtin:
    return true;
  case IncludeLookupKind::SourceRelative:
  case IncludeLookupKind::AbsoluteOperand:
  case IncludeLookupKind::Unknown:
    return false;
  }
  llvm_unreachable("Invalid IncludeLookupKind");
}

static inline bool isPerEdgeIncludeLookupKind(IncludeLookupKind kind) {
  return kind == IncludeLookupKind::SourceRelative ||
         kind == IncludeLookupKind::AbsoluteOperand;
}

static inline StringRef toString(MacroCalleeOriginKind kind) {
  switch (kind) {
  case MacroCalleeOriginKind::LiteralMacroName:
    return "literal_macro_name";
  case MacroCalleeOriginKind::CallerParam:
    return "caller_param";
  case MacroCalleeOriginKind::Paste:
    return "paste";
  case MacroCalleeOriginKind::Opaque:
    return "opaque";
  }
  llvm_unreachable("Invalid MacroCalleeOriginKind");
}

/// \brief Strongly-typed view over the clang-refold map JSON produced by the
///        modified Clang preprocessor.
///
/// The **RefoldModel** mirrors the structure of `RefoldSchema.json` and is
/// intentionally *dumb*: it performs no inference or guessing. If any required
/// data is missing, the engine fails fast, ensuring that issues are caught at
/// the producer rather than silently tolerated.
///
/// ### Coordinate spaces & conventions
/// * **A-tokens:** indices into the original preprocessed token stream
///   (“pp domain”). Many ranges in this model are expressed as A-token
///   half-open intervals `[begin, end)` via `pp_cover`.
/// * **Source bytes:** file-local byte offsets `[b, e)` referring to concrete
///   files (TU or headers). All paths are absolute and canonicalized except
///   textual include targets, which retain their original source form.
/// * **Ranges:** all byte and token ranges are half-open `[b, e)`.
///
/// ### Top-level fields
/// * `version`: schema version string.
/// * `source`: canonical path to the main TU that refolding writes into.
/// * `tokens.count`: number of A-tokens (preprocessed stream length).
/// * `items`: heterogeneous list of provenance items; see **Items** below.
/// * `tokmap`: mapping from A-token or local PP slice back to original
///   source files and byte spans, used for TU edits and token→byte resolution.
/// * `slots`: deterministic insertion anchors (e.g., `file_begin`,
///   `after_last_include`, `arm_begin`/`arm_end`) for heuristic-free insertion
///   placement.
/// * `conds`: conditional groups discovered in source files, with one arm
///   marked `selected=true` for this preprocessing run.
///
/// ### Items
/// Each element in `items` has a stable `id`, a `kind`, optional `subkind`,
/// and a provenance coverage range.
/// * **DirectiveIncludeItem** (`kind="directive"`, `subkind="#include"` or
///   `"#include_next"`):
///   - Directive site (`site_path`, `site_b/e`)
///   - Textual target (`target`)
///   - Legacy resolved spelling (`resolved_path`)
///   - Optional opened file identity (`opened_path`)
///   - Optional entered filename spelling (`entered_file_spelling`)
///   - A-token spans contributed by the included file
///   - Optional `parent` for nested includes.
/// * **MacroItem** (`kind="macro"`): a single macro invocation site and
///   its expansion coverage.
///   Includes:
///   - `name`, invocation bytes (`inv_b/e`) and file
///   - A-token spans
///   - `owner_include_id` (the include that opened the callsite file)
/// * **DirectiveMacroItem** (`kind="directive"`, `subkind="#define"` or
///   `"#undef"`): the exact directive line, file/byte site, and optional
///   `pp_cover` if it contributes tokens to A.
/// * **DirectivePragmaItem** (`subkind="#pragma"`): pragma line and site
///   bytes, treated opaquely by the engine.
/// * **FileItem** (`kind="file"`): spans emitted while the TU was active,
///   used for TU-owned coverage and stable ordering.
///
/// ### Ownership & disambiguation
/// * `owner_include_id`: identifies which include instance produced the bytes,
///   allowing deterministic grouping and realization per include instance.
///   `null` means TU-owned.
/// * `parent` (for includes) and `parent_include_id` (for cond groups) form a
///   nesting tree for multi-level includes and conditionals.
///
/// ### Invariants relied on by the engine
/// * Every item contributing bytes to A has a minimal `pp_cover` over A-tokens.
/// * All file paths are canonical; include targets retain their original
///   spelling.
/// * IDs are unique and stable per map.
/// * `tokmap` entries reference real file byte ranges and (if present)
///   associate an A-token index `pp` for alignment.
///
/// ### Role in the refolding pipeline
/// The model enables the engine to:
/// 1. Attribute edited A-intervals from B to a deterministic provenance item
///    (include, macro, or TU).
/// 2. Realize include instances bottom-up across nesting and conditionals.
/// 3. Apply deterministic TU edits using `tokmap` and `slots`.
///
/// No data synthesis or heuristics occur here.
///
/// \author jeikenberry
class RefoldModel {
public:
  // ========================== Coordinate primitives ==========================
  /// Half-open producer-token interval in the original preprocessed stream A.
  struct PPSpan {
    /// Inclusive A-token index in the original preprocessed stream.
    uint64_t begin;
    /// Exclusive A-token index in the original preprocessed stream.
    uint64_t end;

    std::string ToString() const {
      return formatv("[{0},{1})", begin, end).str();
    }

    /// Return true when this span consumes at least one A token.
    bool IsValid() const { return end > begin; }
  };

  /// Return whether an argument span kind carries producer byte subranges in
  /// addition to its A-token interval.
  static constexpr bool HasByteRange(PPArgSpanKind kind) {
    switch (kind) {
    case PPArgSpanKind::Standard:
      return false;
    case PPArgSpanKind::Stringify:
    case PPArgSpanKind::Paste:
      return true;
    }
    llvm_unreachable("Invalid PPArgSpanKind");
  }

  /// Producer span for one macro actual/formal contribution.
  ///
  /// `begin`/`end` name the A-token interval.  Stringification and paste spans
  /// additionally carry byte offsets into the source-spelled argument and/or
  /// produced preprocessed bytes so replay services can prove byte-exact
  /// spelling recovery without guessing from token identity alone.
  struct PPArgSpan : public PPSpan {
    PPArgSpanKind kind = PPArgSpanKind::Standard;
    uint32_t argIdx;
    std::optional<uint32_t> byteBegin;
    std::optional<uint32_t> byteEnd;
    std::optional<uint64_t> ppByteBegin;
    std::optional<uint64_t> ppByteEnd;

    std::string ToString() const {
      return formatv("Arg {0}: [{1}, {2})", argIdx, begin, end).str();
    }

    /// Return true when the A-token span and any required byte subranges are
    /// internally consistent for this argument-span kind.
    bool IsValid() const;
  };

  /// Minimal half-open A-token cover computed from producer span fragments.
  struct PPCover {
    uint64_t begin = 0;
    uint64_t end = 0;

    /// Compute the smallest half-open A-token interval covering every valid
    /// ordinary, argument, and body span supplied by the producer.
    void Init(const std::vector<PPSpan> &spans,
              const std::vector<PPArgSpan> *argSpans = nullptr,
              const std::vector<PPSpan> *bodySpans = nullptr) noexcept {
      uint64_t mn = std::numeric_limits<uint64_t>::max();
      uint64_t mx = 0;
      bool found = false;

      // Unified processor for anything that acts like a PPSpan
      auto update = [&](const auto &range) {
        for (const auto &s : range) {
          if (s.IsValid()) {
            if (s.begin < mn)
              mn = s.begin;
            if (s.end > mx)
              mx = s.end;
            found = true;
          }
        }
      };

      update(spans);
      if (argSpans)
        update(*argSpans);
      if (bodySpans)
        update(*bodySpans);

      if (found) {
        begin = mn;
        end = mx;
      } else {
        begin = 0;
        end = 0;
      }
    }

    /// Return true when the cover consumes at least one A token.
    bool IsValid() const noexcept { return end > begin; }

    /// Return true when the cover contains the requested half-open A-token
    /// interval.
    bool Covers(uint64_t aStart, uint64_t aEnd) const noexcept {
      return IsValid() && begin <= aStart && aEnd <= end;
    }
  };

  /// Producer tokmap entry tying one A-token to a physical source byte range.
  struct TokMapEntry {
    /// Physical source file that contributed the mapped token spelling.
    std::string file;
    /// A-token index in the original preprocessed stream.
    uint64_t pp;
    /// Inclusive physical source-byte offset in `file`.
    uint64_t b;
    /// Exclusive physical source-byte offset in `file`.
    uint64_t e;
  };

  // ================================== Items ==================================

  /// Header declaration surface recorded for an included file.
  struct HeaderDecl {
    /// Producer classification for the declaration surface.
    StringRef kind;
    /// Declaration spelling/name recorded by the producer.
    StringRef name;
    /// Physical header file containing the declaration.
    StringRef file;
    /// Inclusive physical header source-byte offset for the declaration.
    uint64_t headerB;
    /// Exclusive physical header source-byte offset for the declaration.
    uint64_t headerE;
    /// Half-open A-token span contributed by this declaration surface.
    PPSpan span;
  };

  /// One producer-normalized effective include-search entry.
  ///
  /// Entries are stored in the exact zero-based order emitted under
  /// pp_ctx.include_search_chain. Include lookup provenance references these
  /// entries by index; consumers must not reconstruct this order from argv when
  /// the producer provided it.
  struct IncludeSearchEntry {
    /// Zero-based entry index in `pp_ctx.include_search_chain`.
    uint32_t index = 0;
    /// Producer-normalized include lookup bucket for this chain entry.
    IncludeLookupKind kind = IncludeLookupKind::Unknown;
    /// Directory spelling as supplied to Clang's include search machinery.
    StringRef spelling;
    /// Canonical physical directory path for identity comparison.
    StringRef path;
  };

  /// Producer-owned lookup provenance for one include edge.
  ///
  /// Search-chain hits carry a search_chain_index that selects an entry in
  /// pp_ctx.include_search_chain. New-schema JSON omits per-edge directory
  /// copies for those hits; the parser normalizes the optional in-memory
  /// directory spelling/path from the referenced chain entry after validating
  /// any legacy redundant copies. Source-relative and absolute-operand hits
  /// carry their selecting directory spelling/path directly. Unknown carries no
  /// directory or cursor data and therefore cannot satisfy replay proof.
  struct IncludeLookupProvenance {
    /// Producer-normalized lookup kind for this include edge.
    IncludeLookupKind kind = IncludeLookupKind::Unknown;
    /// Search-chain index when `kind` is an include-search entry kind.
    std::optional<uint32_t> searchChainIndex;
    /// Directory spelling selected by source-relative or absolute-operand
    /// lookup, or normalized from the referenced search-chain entry.
    std::optional<StringRef> directorySpelling;
    /// Canonical physical directory path selected by the include lookup.
    std::optional<StringRef> directoryPath;
  };

  /// Producer-owned #include_next resume provenance.
  ///
  /// The JSON schema intentionally stores only the non-redundant facts: the
  /// containing include id and the resume cursor.  The selected target's
  /// search-chain index lives on this include edge's lookup object, and the
  /// containing file's selected index lives on the referenced containing
  /// include edge.  During model finalization we copy those two indices into
  /// the optional derived fields below so replay proof code can consume a
  /// single self-contained obligation without re-performing id lookups on every
  /// test.
  ///
  /// For old maps, missing or unknown provenance remains represented by
  /// known=false with all cursor fields absent; consumers must keep the
  /// existing conservative #include_next fallback in that case.
  struct IncludeNextProvenance {
    /// True when the producer supplied structurally usable #include_next facts.
    bool known = false;
    /// Include edge that opened the file containing this #include_next
    /// directive.
    std::optional<uint64_t> containingFileIncludeId;
    /// Search-chain cursor where Clang resumed lookup for this #include_next.
    std::optional<uint32_t> resumeSearchChainIndex;

    /// Derived from the containing include edge's lookup.search_chain_index
    /// when known provenance is structurally valid. Not serialized.
    std::optional<uint32_t> containingFileSearchChainIndex;

    /// Derived from this include edge's lookup.search_chain_index when known
    /// provenance is structurally valid. Not serialized.
    std::optional<uint32_t> selectedSearchChainIndex;
  };

  /// Producer record for one include directive instance.
  struct IncludeItem {
    /// Stable producer id for this include edge.
    uint64_t id;
    /// Directive spelling class, either `#include` or `#include_next`.
    StringRef subkind;
    /// Full source directive line text, when serialized by the producer.
    /// Full source directive line text.
    StringRef text;
    /// Physical source file that contains the directive site.
    StringRef sitePath;
    /// Inclusive physical source-byte offset of the directive site.
    uint64_t siteB;
    /// Exclusive physical source-byte offset of the directive site.
    uint64_t siteE;
    /// As-written include target token, e.g. `"e.h"` or `<vector>`.
    StringRef target;

    // Legacy compatibility field.  Producer maps with normalized include
    // resolution split this overloaded value into openedPath for physical
    // identity and enteredFileSpelling for filename observers.
    std::optional<StringRef> resolvedPath;

    // New normalized include-resolution metadata. All fields are optional so
    // old maps remain loadable. When present, consumers should prefer them over
    // legacy resolved_path and argv-derived lookup reconstruction.
    std::optional<StringRef> openedPath;
    std::optional<StringRef> enteredFileSpelling;
    std::optional<StringRef> enteredFileName;
    std::optional<IncludeLookupProvenance> lookup;
    std::optional<IncludeNextProvenance> includeNext;

    /// True when the target was spelled with angle brackets.
    bool angled;
    /// Parent include edge id; nullopt means this include was reached from the
    /// TU.
    std::optional<uint64_t> parent;
    /// A-token spans contributed by the included file instance.
    std::vector<PPSpan> spans;
    /// Header declaration surfaces recorded inside this include instance.
    std::vector<HeaderDecl> decls;
    /// Minimal half-open A-token cover of `spans`.
    PPCover cover;

    IncludeItem(uint64_t id, StringRef subkind, StringRef text,
                StringRef sitePath, uint64_t siteB, uint64_t siteE,
                StringRef target, std::optional<StringRef> resolvedPath,
                std::optional<StringRef> openedPath,
                std::optional<StringRef> enteredFileSpelling,
                std::optional<StringRef> enteredFileName,
                std::optional<IncludeLookupProvenance> lookup,
                std::optional<IncludeNextProvenance> includeNext, bool angled,
                std::optional<uint64_t> parent, std::vector<PPSpan> spans,
                std::vector<HeaderDecl> decls) noexcept
        : id(id), subkind(subkind), text(text), sitePath(sitePath),
          siteB(siteB), siteE(siteE), target(target),
          resolvedPath(resolvedPath), openedPath(openedPath),
          enteredFileSpelling(enteredFileSpelling),
          enteredFileName(enteredFileName), lookup(std::move(lookup)),
          includeNext(std::move(includeNext)), angled(angled), parent(parent),
          spans(std::move(spans)), decls(std::move(decls)) {
      cover.Init(this->spans);
    }

    /// Return true when this include's producer cover contains the requested
    /// half-open A-token interval.
    bool Covers(uint64_t aStart, uint64_t aEnd) const {
      return cover.Covers(aStart, aEnd);
    }
  };

  /// Root invocation argument slice referenced by a nested macro argument.
  struct InvArgRef {
    /// Formal parameter index in the caller invocation.
    uint32_t callerParamIndex;
    /// Inclusive caller-argument-local byte offset for the referenced slice.
    uint32_t byteBegin;
    /// Exclusive caller-argument-local byte offset for the referenced slice.
    uint32_t byteEnd;
  };

  /// Root invocation tuple-element slice referenced by a nested macro argument.
  struct TupleArgRef {
    /// Formal parameter index in the caller invocation.
    uint32_t callerParamIndex;
    /// Inclusive caller-argument-local byte offset for the tuple slice.
    uint32_t callerByteBegin;
    /// Exclusive caller-argument-local byte offset for the tuple slice.
    uint32_t callerByteEnd;
  };

  /// Origin kind for one segment of a pasted-token spelling.
  enum class PastePartKind { Arg, Literal };

  /// Producer token kind for a macro definition replacement-list replay token.
  enum class MacroReplacementTokenKind { Literal, ParamRef };

  /// Origin kind for one segment of a generated macro-callee spelling.
  enum class CalleeOriginPartKind { Literal, CallerArgSlice };

  /// One contiguous contribution to a pasted token's final spelling.
  ///
  /// `kind` records whether the part came from an invocation argument or from
  /// fixed replacement-list text. `byteBegin`/`byteEnd` are half-open byte
  /// offsets within the final pasted-token spelling, and `spelling` is the
  /// exact byte slice occupying that range.
  ///
  /// For argument-derived parts, `argIndex` names the contributing formal. When
  /// present, `argByteBegin`/`argByteEnd` identify the exact byte slice within
  /// the original invocation argument that supplied this pasted part. The
  /// consumer can then splice replay-derived edits back into the argument
  /// without guessing via prefix/suffix matching.
  struct PastePart {
    PastePartKind kind = PastePartKind::Literal;
    std::optional<uint32_t> argIndex;
    uint32_t byteBegin = 0;
    uint32_t byteEnd = 0;
    StringRef spelling;
    std::optional<uint32_t> argByteBegin;
    std::optional<uint32_t> argByteEnd;
  };

  /// Exact witness for one token synthesized by `##` within a specific macro
  /// invocation.
  ///
  /// Array order is significant: it is the producer's deterministic replay
  /// order for repeated identical pasted spellings inside the same invocation.
  struct PasteToken {
    StringRef spelling;
    std::vector<PastePart> parts;
  };

  /// One formal parameter from a producer-recorded macro definition.
  struct MacroDefParam {
    /// Formal parameter spelling.
    StringRef name;
    /// True when this formal is the variadic parameter.
    bool variadic;

    MacroDefParam(StringRef name, bool variadic)
        : name(name), variadic(variadic) {}
  };

  /// Producer-owned replay token for a macro definition replacement list.
  ///
  /// This is the typed form of DirectiveMacroItem.replacement_tokens.  It lets
  /// later proof code replay simple macro definitions from producer evidence
  /// instead of reparsing #define text in the consumer.  `ParamRef` tokens
  /// carry a formal index into the directive's defParams vector; `Literal`
  /// tokens are fixed replacement-list spellings.
  struct MacroReplacementToken {
    MacroReplacementTokenKind kind = MacroReplacementTokenKind::Literal;
    StringRef spelling;
    std::optional<uint32_t> paramIndex;
  };

  /// One producer-proven segment of a macro callee token spelling.
  ///
  /// Literal parts are fixed replacement-list bytes.  CallerArgSlice parts name
  /// the root invocation and root argument slice that supplied the selector
  /// text.  This gives the consumer a direct selector-substitution witness for
  /// paste-derived callees without rediscovering that relationship from raw
  /// invocation text.
  struct CalleeOriginPart {
    CalleeOriginPartKind kind = CalleeOriginPartKind::Literal;
    StringRef spelling;
    std::optional<uint64_t> rootMacroId;
    std::optional<uint32_t> rootParamIndex;
    std::optional<uint32_t> byteBegin;
    std::optional<uint32_t> byteEnd;
  };

  /// Producer witness for the spelling source of a macro callee token.
  struct MacroCalleeOrigin {
    /// High-level callee-origin kind recorded by the producer.
    MacroCalleeOriginKind kind = MacroCalleeOriginKind::LiteralMacroName;
    /// Caller formal indices that contributed to the callee spelling.
    std::vector<uint32_t> callerParamIndices;
    /// Literal callee spelling when it is available directly.
    std::optional<StringRef> spelling;
    /// Ordered callee-origin segments for paste/generated callee cases.
    std::vector<CalleeOriginPart> parts;
  };

  /// Producer record for one macro invocation instance.
  struct MacroInvocation {
    /// Stable producer id for this physical macro invocation.
    uint64_t id;
    /// Producer macro item subtype, such as object-like or function-like.
    StringRef subkind;
    /// Macro name spelling observed at the call site.
    StringRef name;
    /// A-token spans produced by this macro invocation.
    std::vector<PPSpan> spans;
    /// Standard formal/actual A-token spans for each argument contribution.
    std::vector<PPArgSpan> argSpans;
    /// Stringification A-token and argument-byte spans.
    std::vector<PPArgSpan> stringifySpans;
    /// Token-paste A-token and argument/preprocessed-byte spans.
    std::vector<PPArgSpan> pasteSpans;
    /// Exact producer witnesses for tokens synthesized by `##`.
    std::vector<PasteToken> pasteTokens;
    /// Formal parameters from the macro definition used by this invocation.
    std::vector<MacroDefParam> defParams;
    /// Optional half-open byte range whose endpoints may be absent
    /// independently.
    using OptByteRange =
        std::pair<std::optional<uint64_t>, std::optional<uint64_t>>;
    /// Source-spelled invocation-argument byte ranges, indexed by formal.
    std::vector<OptByteRange> invArgRanges;
    /// Producer-normalized invocation text, when available.
    std::optional<StringRef> normalizedInvText;
    /// Byte ranges inside `normalizedInvText`, indexed by formal.
    std::vector<OptByteRange> normalizedInvArgTextRanges;
    /// A-token spans for definition-body tokens replayed by this invocation.
    std::vector<PPSpan> bodySpans;
    /// Source-spelled invocation text at the physical call site, when
    /// available.
    std::optional<StringRef> invText;
    /// Physical source file containing the invocation spelling.
    std::optional<StringRef> invFile;
    /// Half-open physical source-byte range for `invText` in `invFile`.
    std::optional<uint64_t> invB, invE;
    /// Half-open byte range in the original preprocessed A stream for
    /// `invText`.
    std::optional<uint64_t> invPPByteBegin, invPPByteEnd;
    /// Include instance that owns the invocation callsite, or nullopt for TU.
    std::optional<uint64_t> ownerIncludeId;
    /// Macro directive id whose definition was used for this invocation.
    std::optional<uint64_t> definitionDirectiveId;
    /// Parent macro invocation id for nested/generated invocations.
    std::optional<uint64_t> callerMacroId;
    /// Producer witness describing how the callee token spelling was formed.
    MacroCalleeOrigin calleeOrigin;
    /// Formal dependency graph: callee formal -> caller formal indices.
    std::vector<std::vector<uint32_t>> argDeps;
    /// Root invocation argument slices used by each formal.
    std::vector<std::vector<InvArgRef>> argRefs;
    /// Root invocation tuple-element slices used by each formal.
    std::vector<std::vector<TupleArgRef>> argTupleRefs;
    /// Minimal half-open A-token cover for spans, argument spans, and body
    /// spans.
    PPCover cover;

    MacroInvocation(uint64_t id, StringRef subkind, StringRef name,
                    std::optional<StringRef> invText,
                    std::optional<StringRef> normalizedInvText,
                    std::optional<StringRef> invFile,
                    std::optional<uint64_t> invB, std::optional<uint64_t> invE,
                    std::optional<uint64_t> invPPByteBegin,
                    std::optional<uint64_t> invPPByteEnd,
                    std::optional<uint64_t> ownerIncludeId,
                    std::optional<uint64_t> definitionDirectiveId,
                    std::vector<MacroDefParam> defParams,
                    std::vector<OptByteRange> invArgRanges,
                    std::vector<OptByteRange> normalizedInvArgTextRanges,
                    std::vector<PPSpan> spans, std::vector<PPArgSpan> argSpans,
                    std::vector<PPArgSpan> stringifySpans,
                    std::vector<PPArgSpan> pasteSpans,
                    std::vector<PasteToken> pasteTokens,
                    std::vector<PPSpan> bodySpans,
                    std::optional<uint64_t> callerMacroId,
                    MacroCalleeOrigin calleeOrigin,
                    std::vector<std::vector<uint32_t>> argDeps,
                    std::vector<std::vector<InvArgRef>> argRefs,
                    std::vector<std::vector<TupleArgRef>> argTupleRefs) noexcept
        : id(id), subkind(subkind), name(name), spans(std::move(spans)),
          argSpans(std::move(argSpans)),
          stringifySpans(std::move(stringifySpans)),
          pasteSpans(std::move(pasteSpans)),
          pasteTokens(std::move(pasteTokens)), defParams(std::move(defParams)),
          invArgRanges(std::move(invArgRanges)),
          normalizedInvText(normalizedInvText),
          normalizedInvArgTextRanges(std::move(normalizedInvArgTextRanges)),
          bodySpans(std::move(bodySpans)), invText(invText), invFile(invFile),
          invB(invB), invE(invE), invPPByteBegin(invPPByteBegin),
          invPPByteEnd(invPPByteEnd), ownerIncludeId(ownerIncludeId),
          definitionDirectiveId(definitionDirectiveId),
          callerMacroId(callerMacroId), calleeOrigin(std::move(calleeOrigin)),
          argDeps(std::move(argDeps)), argRefs(std::move(argRefs)),
          argTupleRefs(std::move(argTupleRefs)) {
      cover.Init(this->spans, &this->argSpans, &this->bodySpans);
    }

    /// Return true when the macro invocation's producer cover contains the
    /// requested half-open A-token interval.
    ///
    /// Empty insertion gaps at the invocation boundary are deliberately
    /// excluded so boundary edits are not forced into macro ownership when a
    /// neighboring TU/include proof can own them instead.
    bool Covers(uint64_t aStart, uint64_t aEnd) const {
      if (!cover.IsValid())
        return false;

      // For insertions (empty ranges), exclude the macro's boundary. This keeps
      // boundary edits outside the macro invocation and avoids forcing
      // expansion when the change can be represented at the call site.
      if (aStart == aEnd)
        return cover.begin < aStart && aStart < cover.end;

      return cover.begin <= aStart && aEnd <= cover.end;
    }
  };

  /// Producer record for one macro-state directive.
  struct MacroDirective {
    /// Stable producer id for this directive.
    uint64_t id;
    /// Directive spelling class, either `#define` or `#undef`.
    StringRef subkind;
    /// Producer-recorded macro-state key for this #define/#undef directive.
    /// Keeping the name in the model lets replay proofs resolve macro state
    /// without reparsing raw directive text in the consumer.
    StringRef name;
    StringRef text;
    /// True iff this #define was function-like in Clang's MacroInfo.
    /// This is producer-owned macro-state shape data; consumers use it instead
    /// of reparsing directive text to distinguish NAME from NAME(...).
    bool functionLike = false;
    /// Physical source file containing the directive site.
    StringRef sitePath;
    /// Inclusive physical source-byte offset of the directive site.
    uint64_t siteB;
    /// Exclusive physical source-byte offset of the directive site.
    uint64_t siteE;
    /// Include instance that owns the directive site, or nullopt for TU.
    std::optional<uint64_t> ownerIncludeId;
    /// A-token spans contributed by this directive, when any.
    std::vector<PPSpan> spans;
    /// Formal parameters for a function-like #define.
    std::vector<MacroDefParam> defParams;
    /// Producer replacement-list replay tokens for a #define.
    std::vector<MacroReplacementToken> replacementTokens;
  };

  /// Producer record for one pragma directive line.
  struct PragmaDirective {
    /// Stable producer id for this pragma directive.
    uint64_t id;
    /// Producer-recorded pragma text.
    ///
    /// Ordinary directives carry the full source `#pragma` line.  For an
    /// operator-spelled pragma this can be the replayed directive spelling;
    /// `operatorB/operatorE`, not this field, identify the exact physical
    /// `_Pragma("...")` expression.
    StringRef text;
    /// Physical source file containing the directive site.
    StringRef sitePath;
    /// Inclusive physical source-byte offset of the directive site.
    uint64_t siteB;
    /// Exclusive physical source-byte offset of the directive site.
    uint64_t siteE;
    /// Include instance that owned this pragma directive, when the producer
    /// recorded it.  Older maps omit the field, so consumers must either infer
    /// the owner from slot/segment facts or reject repeated-header ambiguity
    /// instead of binding the pragma by physical path alone.
    std::optional<uint64_t> ownerIncludeId;
    /// True when the pragma was spelled as `_Pragma("...")` rather than as a
    /// preprocessing directive line.
    bool viaPragmaOperator = false;
    /// Inclusive byte offset of the exact `_Pragma("...")` expression.
    std::optional<uint64_t> operatorB;
    /// Exclusive byte offset of the exact `_Pragma("...")` expression.
    std::optional<uint64_t> operatorE;
  };

  /// Producer record for one physical file's emitted A-token contribution.
  struct FileItem {
    /// Stable producer id for the file record.
    uint64_t id;
    /// Canonical physical file path.
    StringRef path;
    /// A-token spans emitted while this file was the active source file.
    std::vector<PPSpan> spans;
  };

  // ================================== Slots ==================================

  /// Producer-recorded deterministic insertion/replacement anchor.
  struct Slot {
    /// Stable producer id for this slot.
    uint64_t id;
    /// Physical source file containing the slot.
    StringRef file;
    /// Slot kind string from the schema.
    StringRef kind;
    /// Referenced include id or conditional-arm id, when the slot is scoped.
    std::optional<uint64_t> ref;
    /// A-token or PP-gap coordinate associated with this slot, when any.
    std::optional<uint64_t> pp;
    /// Inclusive physical source-byte offset of the slot.
    uint64_t b;
    /// Exclusive physical source-byte offset of the slot.
    uint64_t e;
    /// Include instance that owns the slot, or nullopt for TU.
    std::optional<uint64_t> ownerIncludeId;
  };

  // ================================ Segments =================================

  /// Physical source-byte ownership segment.
  struct Segment {
    /// Physical source file covered by this segment.
    StringRef file;
    /// Inclusive physical source-byte offset.
    uint64_t b;
    /// Exclusive physical source-byte offset.
    uint64_t e;
    /// Include owner for the half-open physical source-byte range [b,e), if
    /// any.
    std::optional<uint64_t> ownerIncludeId;
    /// Conditional-arm owner for the half-open physical source-byte range
    /// [b,e), if any.
    std::optional<uint64_t> ownerCondArmId;

    std::string ToString() const {
      return formatv("Segment{file='{0}', b={1}, e={2}, ownerIncludeId={3} "
                     "ownerCondArgId={4}}",
                     file, b, e, ownerIncludeId, ownerCondArmId);
    }
  };

  // =============================== Conditionals ==============================

  /// One arm of a producer-recorded conditional group.
  struct CondArm {
    /// Stable producer id for this arm.
    uint64_t id;
    /// Owning conditional group id.
    uint64_t groupId;
    /// Arm kind string, such as `if`, `ifdef`, `elif`, or `else`.
    StringRef kind;
    /// Source condition spelling when the arm has one.
    std::optional<StringRef> cond;
    /// Inclusive physical source-byte offset of the arm body.
    uint64_t bodyB;
    /// Exclusive physical source-byte offset of the arm body.
    uint64_t bodyE;
    /// Optional A-token span emitted by the selected arm.
    std::optional<PPSpan> span;
    /// True when this arm was selected by the producer preprocessing run.
    bool selected;

    bool ContainsByte(uint64_t byteOffset) const {
      return bodyB <= byteOffset && byteOffset < bodyE;
    }
  };

  /// Producer-recorded conditional directive group in one physical file.
  struct CondGroup {
    /// Stable producer id for this conditional group.
    uint64_t id;
    /// Physical source file containing the group.
    StringRef file;
    /// Parent conditional arm id when this group is nested in another arm.
    std::optional<uint64_t> parentArmId;
    /// Parent include instance id when this group is header-owned.
    std::optional<uint64_t> parentIncludeId;
    /// Inclusive physical source-byte offset of the whole conditional group.
    uint64_t groupB;
    /// Exclusive physical source-byte offset of the whole conditional group.
    uint64_t groupE;
    /// Conditional arms in producer source order.
    std::vector<CondArm> arms;

    /// Return true when \p byteOffset lies inside the physical conditional
    /// group byte extent.
    bool ContainsByte(uint64_t byteOffset) const {
      return groupB <= byteOffset && byteOffset < groupE;
    }
  };

  /// Borrowed pointer pair identifying one conditional arm and its group.
  struct ArmRef {
    /// Owning conditional group; points into RefoldModel storage.
    const CondGroup *group;
    /// Conditional arm; points into RefoldModel storage.
    const CondArm *arm;
  };

  /// Producer-proven active source line-control event.
  ///
  /// These records are emitted only after Clang has evaluated directive
  /// operands and conditional activity.  They are therefore model evidence for
  /// arbitrary source `#line` forms, including macro-expanded operands, without
  /// requiring the consumer to re-evaluate preprocessor expressions.
  struct LineControlEvent {
    /// Stable producer id for the event.
    uint64_t id = 0;
    /// Physical source file containing the line-control directive.
    StringRef physicalFile;
    /// Inclusive physical source-byte offset of the directive site, when known.
    std::optional<uint64_t> siteB;
    /// Exclusive physical source-byte offset of the directive site, when known.
    std::optional<uint64_t> siteE;
    /// True when this event was active in the producer preprocessing run.
    bool active = false;
    /// True when Clang supplied semantic line-control evidence for this event.
    bool producerProven = false;
    /// Logical line immediately after the directive takes effect.
    uint64_t logicalLineAfter = 0;
    /// Logical file spelling immediately after the directive takes effect.
    StringRef logicalFileAfter;
    /// Include instance that owns the directive site, or nullopt for TU.
    std::optional<uint64_t> ownerIncludeId;
    /// Full source directive text.
    StringRef text;
  };

  /// \brief Producer-recorded preprocessing context recovered from the
  /// refold-map `pp_ctx` field.
  ///
  /// `cwd` is the working directory the producer ran `cpp` from.  `argv` is
  /// the preprocessor command line (program + flags).  `lang` is the
  /// producer-asserted language mode token (e.g. "c", "c++").  Driver code that
  /// needs to re-invoke the preprocessor on a candidate source consumes this
  /// carrier; the same fields are also stored as individual model members and
  /// available through `GetPPCwd()` / `GetPPLang()` / `GetPPArgv()` once the
  /// full model has been constructed.
  struct PreprocessContext {
    /// Producer preprocessing working directory.
    std::string cwd;
    /// Producer preprocessor command line, including program and flags.
    std::vector<std::string> argv;
    /// Producer language token used to configure raw lexing.
    std::string lang;
  };

  /// \brief Extract the `pp_ctx` object from a refold-map JSON root.
  ///
  /// This is exposed as a free-standing static so the driver can obtain the
  /// preprocessing context before constructing the full model (e.g. to
  /// re-invoke `cpp` during `--check` mode without paying for full model
  /// parsing).  `FromJson` performs the same extraction internally and stores
  /// the resulting fields on the model.
  static Expected<PreprocessContext>
  ParsePreprocessContext(const json::Object &root);

  /// \brief Extract the top-level `source` path from a refold-map JSON root.
  ///
  /// Returns an error when the field is missing or empty.  The full model
  /// also exposes this value via `GetSourcePath()` once `FromJson` succeeds.
  static Expected<std::string> ParseSourcePath(const json::Object &root);

  /// \brief Constructs a RefoldModel instance from a parsed JSON object.
  ///
  /// This factory method deserializes a validated `clang-refold` map JSON
  /// (produced by the modified Clang preprocessor) into a strongly typed
  /// `RefoldModel`. The model mirrors the structure of `RefoldSchema.json`
  /// and provides direct access to tokens, items, and tokmap entries used
  /// during the refolding process.
  ///
  /// \param Root The top-level JSON object parsed from the refold map file.
  /// \return An `Expected<RefoldModel>` containing the constructed model on
  ///         success, or an error if required fields are missing or invalid.
  ///
  /// \see RefoldSchema.h
  static Expected<RefoldModel> FromJson(const json::Object &root);

  /// Return an independent read-only consumer copy of this model.
  ///
  /// The model's public records are value types, but several private lookup
  /// tables retain pointers into the owning vectors.  A compiler-generated
  /// copy would preserve those pointers and therefore make the copy borrow
  /// the original model's storage.  Isolated proof simulations require a
  /// genuinely independent model, so this helper copies only the stored
  /// producer records and then rebuilds every derived index against the
  /// cloned vectors.
  RefoldModel CloneForReadOnlyConsumer() const;

  // ============================== Basic getters ==============================

  /// Return the producer map version string.
  StringRef GetVersion() const { return version_; }
  /// Return the original physical translation-unit source path.
  StringRef GetSourcePath() const { return sourcePath_; }
  /// Return the producer preprocessing working directory.
  StringRef GetPPCwd() const { return ppCwd_; }
  /// Return the producer language token used to configure the raw lexer for
  /// source-byte slices in this model.
  StringRef GetPPLang() const { return ppLang_; }
  /// Return the producer preprocessor argv captured in the map.
  ArrayRef<std::string> GetPPArgv() const { return ppArgv_; }
  /// Return the producer include-search chain in recorded lookup order.
  ArrayRef<IncludeSearchEntry> GetIncludeSearchChain() const {
    return includeSearchChain_;
  }
  /// Return the number of producer A tokens recorded in the map.
  uint64_t GetTokensCountA() const { return tokensCountA_; }

  /// Return tokmap entries keyed by preprocessed A-token index.
  const DenseMap<uint64_t, TokMapEntry> &GetTokmapByPP() const {
    return tokmapByPP_;
  }
  /// Return the ordered producer A-token to physical source-byte map entries.
  ArrayRef<TokMapEntry> GetTokmap() const { return tokmap_; }

  /// Return producer include instances.
  ArrayRef<IncludeItem> GetIncludes() const { return includes_; }
  /// Return producer macro invocation instances.
  ArrayRef<MacroInvocation> GetMacroInvocations() const { return macroInvs_; }
  /// Return producer macro state directives.
  ArrayRef<MacroDirective> GetMacroDirectives() const { return macroDirs_; }
  /// Return producer pragma directives.
  ArrayRef<PragmaDirective> GetPragmas() const { return pragmas_; }
  /// Return producer file records.
  ArrayRef<FileItem> GetFileItems() const { return fileItems_; }
  /// Return source insertion/replacement slots recorded by the producer.
  ArrayRef<Slot> GetSlots() const { return slots_; }
  /// Return producer conditional groups.
  ArrayRef<CondGroup> GetConds() const { return conds_; }
  /// Return producer-proven active line-control events.
  ArrayRef<LineControlEvent> GetLineControls() const { return lineControls_; }

  /// Look up a producer include instance by stable ID.
  const IncludeItem *GetIncludeById(uint64_t id) const {
    auto it = includeById_.find(id);
    return it == includeById_.end() ? nullptr : it->second;
  }

  /// Look up a producer conditional group by stable ID.
  const CondGroup *GetCondGroupById(uint64_t id) const {
    auto it = condGroupById_.find(id);
    return it == condGroupById_.end() ? nullptr : it->second;
  }

  /// Look up a selected or inactive conditional arm by stable arm ID.
  std::optional<ArmRef> GetArmRefById(uint64_t armId) const {
    auto it = armById_.find(armId);
    if (it == armById_.end())
      return std::nullopt;
    return it->second;
  }

  // ========================= Helpers for the engine  =========================

  // --- Segments ---
  /// Return physical source ownership segments for \p file, or an empty range
  /// when the map contains no segment table for that file.
  ArrayRef<Segment> GetSegmentsForFile(StringRef file) const {
    auto it = segmentsByFile_.find(file);
    if (it == segmentsByFile_.end())
      return ArrayRef<Segment>();
    return ArrayRef<Segment>(it->second);
  }

  // --- Include nesting ---
  /// Return include nesting depth for an optional include owner.
  uint32_t GetIncludeDepth(std::optional<uint64_t> includeId) const;
  /// Return the innermost include owner that produced A-token \p ppIndex.
  std::optional<uint64_t> InnermostIncludeAtPP(uint64_t ppIndex) const;
  /// Return the least common include ancestor of two optional include owners.
  std::optional<uint64_t>
  LeastCommonAncestorInclude(std::optional<uint64_t> a,
                             std::optional<uint64_t> b) const;

  // --- Conditional nesting ---
  /// Return nesting depth for a producer conditional group.
  uint32_t GetCondGroupDepth(uint64_t groupId) const;
  /// Return nesting depth for a producer conditional arm.
  uint32_t GetCondArmDepth(uint64_t armId) const {
    if (auto ref = GetArmRefById(armId))
      return GetCondGroupDepth(ref->group->id);
    return 0;
  }

  /// Return conditional groups physically owned by \p file and optional
  /// parent include.
  std::vector<const CondGroup *>
  GetCondGroups(StringRef file, std::optional<uint64_t> parentIncludeId) const;

  /// Find the innermost conditional arm containing a physical source byte.
  std::optional<ArmRef>
  FindArmRefForByte(StringRef file, std::optional<uint64_t> parentIncludeId,
                    uint64_t byteOffset) const;

  /// Convenience wrapper returning only the conditional arm pointer.
  std::optional<const CondArm *>
  FindArmForByte(StringRef file, std::optional<uint64_t> parentIncludeId,
                 uint64_t byteOffset) const {
    if (auto armRef = FindArmRefForByte(file, parentIncludeId, byteOffset))
      return (*armRef).arm;
    return std::nullopt;
  }

  /// Find the selected conditional arm that produced A-token `ppIndex`.
  std::optional<ArmRef> FindArmRefAtPP(uint64_t ppIndex) const;
  std::optional<const CondArm *> FindArmForPP(uint64_t ppIndex) const {
    auto ref = FindArmRefAtPP(ppIndex);
    if (!ref)
      return std::nullopt;
    return ref->arm;
  }

  /// Return the first A-token index emitted by the selected arm of `group`.
  std::optional<uint64_t>
  FirstConditionalArmStartA(const CondGroup &group) const;
  std::optional<uint64_t> FirstConditionalArmStartA(uint64_t groupId) const {
    const CondGroup *group = GetCondGroupById(groupId);
    if (!group)
      return std::nullopt;
    return FirstConditionalArmStartA(*group);
  }

  // --- Slot queries ---
  /// Return all slots matching the supplied exact-match filters.
  ///
  /// Each engaged optional is a required equality predicate; each disengaged
  /// optional is a wildcard. Results are returned in deterministic source order
  /// (byte range, optional PP index, then slot id), matching the implementation
  /// in RefoldModel.cpp.
  std::vector<const Slot *>
  FindSlots(std::optional<StringRef> file, std::optional<StringRef> kind,
            std::optional<uint64_t> ref,
            std::optional<uint64_t> ownerIncludeId) const;

  /// Return the deterministic slot immediately before `includeId`, when
  /// present.
  std::optional<const Slot *> GetBeforeIncludeSlot(uint64_t includeId) const {
    auto slots =
        FindSlots(std::nullopt, "before_include", includeId, std::nullopt);
    if (slots.empty())
      return std::nullopt;
    return slots.front();
  }

  /// Return the deterministic slot immediately after `includeId`, when present.
  std::optional<const Slot *> GetAfterIncludeSlot(uint64_t includeId) const {
    auto slots =
        FindSlots(std::nullopt, "after_include", includeId, std::nullopt);
    if (slots.empty())
      return std::nullopt;
    return slots.front();
  }

  /// Return the deterministic slot at the beginning of a conditional arm.
  std::optional<const Slot *> GetArmBeginSlot(uint64_t armId) const;
  /// Return the deterministic slot at the end of a conditional arm.
  std::optional<const Slot *> GetArmEndSlot(uint64_t armId) const;

  // --- Tokmap queries ---
  /// Map one A-token index to its physical source-byte tokmap entry.
  std::optional<TokMapEntry> MapPP(uint64_t pp) const {
    auto it = tokmapByPP_.find(pp);
    if (it == tokmapByPP_.end())
      return std::nullopt;
    return it->second;
  }

  /// Map a half-open A-token span to the producer tokmap entries it contains.
  ///
  /// Invalid spans return an empty vector. Missing PP indices are skipped
  /// rather than synthesized, because not every preprocessed token is
  /// guaranteed to have a concrete source spelling in tokmap.
  std::vector<TokMapEntry> MapSpan(const PPSpan &span) const;

private:
  RefoldModel() = default;

  const json::Object *root_;

  // =============================== Stored data ===============================

  StringRef version_;
  StringRef sourcePath_;
  StringRef ppCwd_;
  StringRef ppLang_;
  std::vector<std::string> ppArgv_;
  std::vector<IncludeSearchEntry> includeSearchChain_;
  uint64_t tokensCountA_ = 0;

  /// Optional inclusive byte offsets for each A token in the original
  /// preprocessed output stream.
  std::optional<std::vector<uint64_t>> tokPPByteBeginA_;
  /// Optional exclusive byte offsets for each A token in the original
  /// preprocessed output stream.
  std::optional<std::vector<uint64_t>> tokPPByteEndA_;

  DenseMap<uint64_t, TokMapEntry> tokmapByPP_; // key = pp
  std::vector<TokMapEntry> tokmap_;

  std::vector<IncludeItem> includes_;
  std::vector<MacroInvocation> macroInvs_;
  std::vector<MacroDirective> macroDirs_;
  std::vector<PragmaDirective> pragmas_;
  std::vector<FileItem> fileItems_;
  std::vector<Slot> slots_;
  std::vector<CondGroup> conds_;
  std::vector<LineControlEvent> lineControls_;

  // ============================= Derived indices =============================

  DenseMap<uint64_t, const IncludeItem *> includeById_;
  DenseMap<uint64_t, const CondGroup *> condGroupById_;
  DenseMap<uint64_t, ArmRef> armById_;

  // file -> groups (all owners)
  StringMap<std::vector<const CondGroup *>> condsByFile_;

  // file -> ownerIncludeId -> groups
  StringMap<DenseMap<uint64_t, std::vector<const CondGroup *>>>
      condsByFileByOwner_;

  // file -> segments derived from slots
  StringMap<std::vector<Segment>> segmentsByFile_;

  // caches (computed on demand)
  mutable DenseMap<uint64_t, uint32_t> includeDepthCache_;
  mutable DenseMap<uint64_t, uint32_t> condGroupDepthCache_;

  // Internal helper to finalize indices and perform deterministic ordering.
  void BuildIndicesAndSort();

  /// Populate derived include_next cursor fields after include ids and lookup
  /// metadata have been indexed.  This is a model-finalization step, not JSON
  /// parsing: the schema deliberately stores non-redundant producer facts while
  /// the consumer keeps derived indices ready for include-next replay proofs.
  void CompleteIncludeNextDerivedProvenance();

  /// Remove invalid caller_macro_id edges so upward caller walks remain
  /// acyclic and terminate deterministically.
  void SanitizeMacroCallerGraph();

  /// Drop only those proof artifacts whose local producer contract is
  /// impossible to satisfy, while preserving independently useful invocation
  /// metadata so the consumer continues to fail closed rather than over-expand.
  void SanitizeMacroProofArtifacts();

  std::vector<Segment>
  BuildSegmentsForFile(StringRef file, ArrayRef<const Slot *> fileSlots) const;
};

// Parse an array of PPSpan objects from JSON. This is a small public wrapper
// over RefoldModel's internal span parsing logic, used by the driver for
// ancillary checks (e.g., --check + --no-lines ignore masks).
llvm::Expected<std::vector<RefoldModel::PPSpan>>
parsePPSpans(const llvm::json::Value &val, llvm::StringRef ctx);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMODEL_H
