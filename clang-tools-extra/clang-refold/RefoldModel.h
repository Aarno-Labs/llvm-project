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
///   - Resolved file (`resolved_path`)
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
  struct PPSpan {
    uint64_t begin; // inclusive A-token index
    uint64_t end;   // exclusive A-token index

    std::string ToString() const {
      return formatv("[{0},{1})", begin, end).str();
    }

    bool IsValid() const { return begin >= 0 && end >= 0 && end > begin; }
  };

#if 0
  // Getter for the 'key' field
  static constexpr StringRef GetKey(PPArgSpanKind kind) {
    switch (kind) {
    case PPArgSpanKind::Standard:
      return "arg_spans";
    case PPArgSpanKind::Stringify:
      return "stringify_spans";
    case PPArgSpanKind::Paste:
      return "paste_spans";
    }
    llvm_unreachable("Invalid PPArgSpanKind");
  }
#endif

  static constexpr bool HasByteRange(PPArgSpanKind kind) {
    switch (kind) {
    case PPArgSpanKind::Standard:
    case PPArgSpanKind::Stringify:
      return false;
    case PPArgSpanKind::Paste:
      return true;
    }
    llvm_unreachable("Invalid PPArgSpanKind");
  }

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

    bool IsValid() const;
  };

  struct PPCover {
    uint64_t begin = 0;
    uint64_t end = 0;

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

    bool IsValid() const noexcept { return end > begin; }

    bool Covers(uint64_t aStart, uint64_t aEnd) const noexcept {
      return IsValid() && begin <= aStart && aEnd <= end;
    }
  };

  struct TokMapEntry {
    std::string file;
    uint64_t pp;
    uint64_t b;
    uint64_t e;
  };

  // ================================== Items ==================================

  struct HeaderDecl {
    StringRef kind;
    StringRef name;
    StringRef file;
    uint64_t headerB;
    uint64_t headerE;
    PPSpan span;
  };

  struct IncludeItem {
    uint64_t id;
    StringRef subkind;  // "#include" | "#include_next"
    StringRef text;     // directive line (optional)
    StringRef sitePath; // includer path
    uint64_t siteB;
    uint64_t siteE;
    StringRef target; // as-written (e.g. "\"e.h\"" or "<vector>")
    std::optional<StringRef> resolvedPath;
    bool angled;
    std::optional<uint64_t> parent; // parent include id
    std::vector<PPSpan> spans;
    std::vector<HeaderDecl> decls; // header decls referenced by this include
    PPCover cover;

    IncludeItem(uint64_t id, StringRef subkind, StringRef text,
                StringRef sitePath, uint64_t siteB, uint64_t siteE,
                StringRef target, std::optional<StringRef> resolvedPath,
                bool angled, std::optional<uint64_t> parent,
                std::vector<PPSpan> spans,
                std::vector<HeaderDecl> decls) noexcept
        : id(id), subkind(subkind), text(text), sitePath(sitePath),
          siteB(siteB), siteE(siteE), target(target),
          resolvedPath(resolvedPath), angled(angled), parent(parent),
          spans(std::move(spans)), decls(std::move(decls)) {
      cover.Init(this->spans);
    }

    bool Covers(uint64_t aStart, uint64_t aEnd) const {
      return cover.Covers(aStart, aEnd);
    }
  };

  struct MacroInvocation {
    uint64_t id;
    StringRef subkind; // "func" | "obj"
    StringRef name;
    std::vector<PPSpan> spans; // expansion coverage (A tokens)
    std::vector<PPArgSpan> argSpans;
    std::vector<PPArgSpan> stringifySpans;
    std::vector<PPArgSpan> pasteSpans;
    std::vector<PPSpan> bodySpans;
    std::optional<StringRef> invText;
    std::optional<StringRef> invFile;   // file containing invocation
    std::optional<uint64_t> invB, invE; // byte offsets within invFile
    std::optional<uint64_t> invPPByteBegin,
        invPPByteEnd; // A-stream byte envelope
    std::optional<uint64_t> ownerIncludeId;
    PPCover cover;

    MacroInvocation(uint64_t id, StringRef subkind, StringRef name,
                    std::optional<StringRef> invText,
                    std::optional<StringRef> invFile,
                    std::optional<uint64_t> invB, std::optional<uint64_t> invE,
                    std::optional<uint64_t> invPPByteBegin,
                    std::optional<uint64_t> invPPByteEnd,
                    std::optional<uint64_t> ownerIncludeId,
                    std::vector<PPSpan> spans, std::vector<PPArgSpan> argSpans,
                    std::vector<PPArgSpan> stringifySpans,
                    std::vector<PPArgSpan> pasteSpans,
                    std::vector<PPSpan> bodySpans) noexcept
        : id(id), subkind(subkind), name(name), spans(std::move(spans)),
          argSpans(std::move(argSpans)),
          stringifySpans(std::move(stringifySpans)),
          pasteSpans(std::move(pasteSpans)), bodySpans(std::move(bodySpans)),
          invText(invText), invFile(invFile), invB(invB), invE(invE),
          invPPByteBegin(invPPByteBegin), invPPByteEnd(invPPByteEnd),
          ownerIncludeId(ownerIncludeId) {
      cover.Init(this->spans, &this->argSpans, &this->bodySpans);
    }

    bool Covers(uint64_t aStart, uint64_t aEnd) const {
      return cover.Covers(aStart, aEnd);
    }
  };

  struct MacroDirective {
    uint64_t id;
    StringRef subkind; // "#define" | "#undef"
    StringRef text;
    StringRef sitePath;
    uint64_t siteB;
    uint64_t siteE;
    std::optional<uint64_t> ownerIncludeId;
    std::vector<PPSpan> spans;
  };

  struct PragmaDirective {
    uint64_t id;
    StringRef text;
    StringRef sitePath;
    uint64_t siteB;
    uint64_t siteE;
  };

  struct FileItem {
    uint64_t id;
    StringRef path;
    std::vector<PPSpan> spans;
  };

  // ================================== Slots ==================================

  struct Slot {
    uint64_t id;
    StringRef file;
    StringRef kind;              // enum per schema
    std::optional<uint64_t> ref; // include id or cond-arm id
    std::optional<uint64_t> pp;
    uint64_t b;
    uint64_t e;
    std::optional<uint64_t> ownerIncludeId;
  };

  // ================================ Segments =================================

  struct Segment {
    StringRef file;
    uint64_t b;
    uint64_t e;
    std::optional<uint64_t>
        ownerIncludeId; // current include ownership at [b,e)
    std::optional<uint64_t> ownerCondArmId; // current conditional arm at [b,e)

    std::string ToString() const {
      return formatv("Segment{file='{0}', b={1}, e={2}, ownerIncludeId={3} "
                     "ownerCondArgId={4}}",
                     file, b, e, ownerIncludeId, ownerCondArmId);
    }
  };

  // =============================== Conditionals ==============================
  struct CondArm {
    uint64_t id;
    uint64_t groupId; // owning CondGroup id
    StringRef kind;   // if/ifdef/ifndef/elif/else
    std::optional<StringRef> cond;
    uint64_t bodyB;
    uint64_t bodyE;
    std::optional<PPSpan> span; // optional A-token span for this arm
    bool selected;

    bool ContainsByte(uint64_t byteOffset) const {
      return bodyB <= byteOffset && byteOffset < bodyE;
    }
  };

  struct CondGroup {
    uint64_t id;
    StringRef file;
    std::optional<uint64_t> parentArmId;
    std::optional<uint64_t> parentIncludeId;
    uint64_t groupB;
    uint64_t groupE;
    std::vector<CondArm> arms;

    bool ContainsByte(uint64_t byteOffset) const {
      return groupB <= byteOffset && byteOffset < groupE;
    }
  };

  struct ArmRef {
    const CondGroup *group;
    const CondArm *arm;
  };

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
  static Expected<RefoldModel> FromJson(const json::Object &Root);

  // ============================== Basic getters ==============================

  StringRef GetVersion() const { return version_; }
  StringRef GetSourcePath() const { return sourcePath_; }
  StringRef GetPPCwd() const { return ppCwd_; }
  uint64_t GetTokensCountA() const { return tokensCountA_; }

  const DenseMap<uint64_t, TokMapEntry> &GetTokmapByPP() const {
    return tokmapByPP_;
  }
  ArrayRef<TokMapEntry> GetTokmap() const { return tokmap_; }

  ArrayRef<IncludeItem> GetIncludes() const { return includes_; }
  ArrayRef<MacroInvocation> GetMacroInvocations() const { return macroInvs_; }
  ArrayRef<MacroDirective> GetMacroDirectives() const { return macroDirs_; }
  ArrayRef<PragmaDirective> GetPragmas() const { return pragmas_; }
  ArrayRef<FileItem> GetFileItems() const { return fileItems_; }
  ArrayRef<Slot> GetSlots() const { return slots_; }
  ArrayRef<CondGroup> GetConds() const { return conds_; }

  const IncludeItem *GetIncludeById(uint64_t id) const {
    auto it = includeById_.find(id);
    return it == includeById_.end() ? nullptr : it->second;
  }

  const CondGroup *GetCondGroupById(uint64_t id) const {
    auto it = condGroupById_.find(id);
    return it == condGroupById_.end() ? nullptr : it->second;
  }

  std::optional<ArmRef> GetArmRefById(uint64_t armId) const {
    auto it = armById_.find(armId);
    if (it == armById_.end())
      return std::nullopt;
    return it->second;
  }

  // ========================= Helpers for the engine  =========================

  // --- Segments ---
  ArrayRef<Segment> GetSegmentsForFile(StringRef file) const {
    auto it = segmentsByFile_.find(file);
    if (it == segmentsByFile_.end())
      return ArrayRef<Segment>();
    return ArrayRef<Segment>(it->second);
  }

  // --- Include nesting ---
  uint32_t GetIncludeDepth(std::optional<uint64_t> includeId) const;
  std::optional<uint64_t> InnermostIncludeAtPP(uint64_t ppIndex) const;
  std::optional<uint64_t>
  LeastCommonAncestorInclude(std::optional<uint64_t> a,
                             std::optional<uint64_t> b) const;

  // --- Conditional nesting ---
  uint32_t GetCondGroupDepth(uint64_t groupId) const;
  uint32_t GetCondArmDepth(uint64_t armId) const {
    if (auto ref = GetArmRefById(armId))
      return GetCondGroupDepth(ref->group->id);
    return 0;
  }

  std::vector<const CondGroup *>
  GetCondGroups(StringRef file, std::optional<uint64_t> parentIncludeId) const;

  std::optional<ArmRef>
  FindArmRefForByte(StringRef file, std::optional<uint64_t> parentIncludeId,
                    uint64_t byteOffset) const;

  std::optional<const CondArm *>
  FindArmForByte(StringRef file, std::optional<uint64_t> parentIncludeId,
                 uint64_t byteOffset) const {
    if (auto armRef = FindArmRefForByte(file, parentIncludeId, byteOffset))
      return (*armRef).arm;
    return std::nullopt;
  }

  std::optional<ArmRef> FindArmRefAtPP(uint64_t ppIndex) const;
  std::optional<const CondArm *> FindArmForPP(uint64_t ppIndex) const {
    auto ref = FindArmRefAtPP(ppIndex);
    if (!ref)
      return std::nullopt;
    return ref->arm;
  }

  std::optional<uint64_t>
  FirstConditionalArmStartA(const CondGroup &group) const;
  std::optional<uint64_t> FirstConditionalArmStartA(uint64_t groupId) const {
    const CondGroup *group = GetCondGroupById(groupId);
    if (!group)
      return std::nullopt;
    return FirstConditionalArmStartA(*group);
  }

  // --- Slot queries ---
  std::vector<const Slot *>
  FindSlots(std::optional<StringRef> file, std::optional<StringRef> kind,
            std::optional<uint64_t> ref,
            std::optional<uint64_t> ownerIncludeId) const;

  std::optional<const Slot *> GetBeforeIncludeSlot(uint64_t includeId) const {
    auto slots =
        FindSlots(std::nullopt, "before_include", includeId, std::nullopt);
    if (slots.empty())
      return std::nullopt;
    return slots.front();
  }

  std::optional<const Slot *> GetAfterIncludeSlot(uint64_t includeId) const {
    auto slots =
        FindSlots(std::nullopt, "after_include", includeId, std::nullopt);
    if (slots.empty())
      return std::nullopt;
    return slots.front();
  }

  std::optional<const Slot *> GetArmBeginSlot(uint64_t armId) const;
  std::optional<const Slot *> GetArmEndSlot(uint64_t armId) const;

  // --- Tokmap queries ---
  std::optional<TokMapEntry> MapPP(uint64_t pp) const {
    auto it = tokmapByPP_.find(pp);
    if (it == tokmapByPP_.end())
      return std::nullopt;
    return it->second;
  }

  std::vector<TokMapEntry> MapSpan(const PPSpan &span) const;

private:
  RefoldModel() = default;

  const json::Object *root_;

  // =============================== Stored data ===============================

  StringRef version_;
  StringRef sourcePath_;
  StringRef ppCwd_;
  uint64_t tokensCountA_ = 0;

  /// Optional per-token byte offsets in the preprocessed output (A stream).
  std::optional<std::vector<uint64_t>> tokPPByteBeginA_;
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

  std::vector<Segment>
  BuildSegmentsForFile(StringRef file, ArrayRef<const Slot *> fileSlots) const;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMODEL_H
