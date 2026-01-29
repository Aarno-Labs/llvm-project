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
  llvm_unreachable("Invalid owner kind");
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
    int begin; // inclusive A-token index
    int end;   // exclusive A-token index

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
    PPArgSpanKind kind;
    int argIdx;
    int byteBegin = -1, byteEnd = -1;
    std::optional<int> ppByteBegin;
    std::optional<int> ppByteEnd;

    std::string ToString() const {
      return formatv("Arg {0}: [{1}, {2})", argIdx, begin, end).str();
    }

    bool IsValid() const;
  };

  struct PPCover {
    int begin; // inclusive
    int end;   // exclusive

    void Init(std::optional<int> coverBeginOpt, std::optional<int> coverEndOpt,
              const std::vector<PPSpan> &spans,
              const std::vector<PPArgSpan> *argSpans = nullptr,
              const std::vector<PPSpan> *bodySpans = nullptr) noexcept {
      int cb = -1, ce = -1;
      if (coverBeginOpt && coverEndOpt) {
        cb = *coverBeginOpt;
        ce = *coverEndOpt;
      }

      // If pp_cover is not present, approximate coverage from any recorded PP
      // spans. This must include arg/body spans as well as the outer 'spans' so
      // that edits to nested macro expansions inside a macro body can be
      // attributed to the enclosing macro invocation.
      if (cb < 0 || ce < 0) {
        int mn = std::numeric_limits<int>::max();
        int mx = std::numeric_limits<int>::min();

        for (const auto &s : spans) {
          if (!s.IsValid())
            continue;
          if (s.begin < mn)
            mn = s.begin;
          if (s.end > mx)
            mx = s.end;
        }

        if (argSpans) {
          for (const auto &s : *argSpans) {
            if (!s.IsValid())
              continue;
            if (s.begin < mn)
              mn = s.begin;
            if (s.end > mx)
              mx = s.end;
          }
        }

        if (bodySpans) {
          for (const auto &s : *bodySpans) {
            if (!s.IsValid())
              continue;
            if (s.begin < mn)
              mn = s.begin;
            if (s.end > mx)
              mx = s.end;
          }
        }

        if (mn != std::numeric_limits<int>::max()) {
          cb = mn;
          ce = std::max(mn, mx);
        }
      }

      begin = cb;
      end = ce;
    }

    bool Covers(int aStart, int aEnd) const noexcept {
      return begin >= 0 && end >= 0 && begin <= aStart && aEnd <= end;
    }
  };

  struct TokMapEntry {
    int pp;
    std::string file;
    int b;
    int e;
  };

  // ================================== Items ==================================
  struct HeaderDecl {
    std::string kind;
    std::string name;
    std::string file;
    int headerB;
    int headerE;
    PPSpan ppSpan;
  };

  struct IncludeItem {
    int id;
    std::string subkind;  // "#include" | "#include_next"
    std::string text;     // directive line (optional)
    std::string sitePath; // includer path
    int siteB;
    int siteE;
    std::string target; // as-written (e.g. "\"e.h\"" or "<vector>")
    std::optional<std::string> resolvedPath;
    bool angled;
    std::optional<int> parent; // parent include id
    std::vector<PPSpan> spans;
    PPCover cover;
    std::vector<HeaderDecl> decls; // header decls referenced by this include

    IncludeItem(int id, std::string subkind, std::string text,
                std::string sitePath, int siteB, int siteE, std::string target,
                std::optional<std::string> resolvedPath, bool angled,
                std::optional<int> parent, std::vector<PPSpan> spans,
                std::optional<int> coverBegin, std::optional<int> coverEnd,
                std::vector<HeaderDecl> decls) noexcept
        : id(id), subkind(std::move(subkind)), text(std::move(text)),
          sitePath(std::move(sitePath)), siteB(siteB), siteE(siteE),
          target(std::move(target)), resolvedPath(std::move(resolvedPath)),
          angled(angled), parent(std::move(parent)), spans(std::move(spans)),
          decls(std::move(decls)) {
      cover.Init(coverBegin, coverEnd, this->spans);
    }

    bool Covers(int aStart, int aEnd) const {
      return cover.Covers(aStart, aEnd);
    }
  };

  struct MacroInvocation {
    int id;
    std::string subkind; // "func" | "obj"
    std::string name;
    std::vector<PPSpan> spans; // expansion coverage (A tokens)
    std::vector<PPArgSpan> argSpans;
    std::vector<PPArgSpan> stringifySpans;
    std::vector<PPArgSpan> pasteSpans;
    std::vector<PPSpan> bodySpans;
    std::optional<std::string> invText;
    std::optional<std::string> invFile; // file containing invocation
    std::optional<int> invB, invE; // byte offsets within invFile
    std::optional<int> invPPByteBegin, invPPByteEnd; // A-stream byte envelope
    std::optional<int> ownerIncludeId;
    PPCover cover;

    MacroInvocation(int id, std::string subkind, std::string name,
                    std::optional<std::string> invText,
                    std::optional<std::string> invFile, std::optional<int> invB,
                    std::optional<int> invE, std::optional<int> invPPByteBegin,
                    std::optional<int> invPPByteEnd, std::optional<int> ownerIncludeId,
                    std::vector<PPSpan> spans, std::vector<PPArgSpan> argSpans,
                    std::vector<PPArgSpan> stringifySpans,
                    std::vector<PPArgSpan> pasteSpans,
                    std::vector<PPSpan> bodySpans,
                    std::optional<int> coverBegin,
                    std::optional<int> coverEnd) noexcept
        : id(id), subkind(std::move(subkind)), name(std::move(name)),
          spans(std::move(spans)), argSpans(std::move(argSpans)),
          stringifySpans(std::move(stringifySpans)),
          pasteSpans(std::move(pasteSpans)), bodySpans(std::move(bodySpans)),
          invText(std::move(invText)), invFile(std::move(invFile)),
          invB(std::move(invB)), invE(std::move(invE)),
          invPPByteBegin(std::move(invPPByteBegin)),
          invPPByteEnd(std::move(invPPByteEnd)),
          ownerIncludeId(std::move(ownerIncludeId)) {
      cover.Init(coverBegin, coverEnd, this->spans, &this->argSpans,
                 &this->bodySpans);
    }

    int GetInvB() const { return invB.has_value() ? *invB : -1; }
    int GetInvE() const { return invE.has_value() ? *invE : -1; }

    bool Covers(int aStart, int aEnd) const {
      return cover.Covers(aStart, aEnd);
    }
  };

  struct MacroDirective {
    int id;
    std::string subkind; // "#define" | "#undef"
    std::string text;
    std::string sitePath;
    int siteB;
    int siteE;
    std::optional<int> ownerIncludeId;
    std::vector<PPSpan> spans;
  };

  struct PragmaDirective {
    int id;
    std::string text;
    std::string sitePath;
    int siteB;
    int siteE;
  };

  struct FileItem {
    int id;
    std::optional<std::string> path;
    std::vector<PPSpan> spans;
  };

  // ================================== Slots ==================================
  struct Slot {
    int id;
    std::string file;
    std::string kind; // enum per schema
    std::optional<int> ref; // include id or cond-arm id
    int b;
    int e;
    std::optional<int> ownerIncludeId;
    std::optional<int> pp; // optional A-token index for tie-break
  };

  // ================================ Segments =================================
  struct Segment {
    std::string file;
    int b;
    int e;
    std::optional<int> ownerIncludeId; // current include ownership at [b,e)
    std::optional<int> ownerCondArmId; // current conditional arm at [b,e)

    std::string ToString() const {
      return formatv("Segment{file='{0}', b={1}, e={2}, ownerIncludeId={3} "
                     "ownerCondArgId={4}}",
                     file, b, e, ownerIncludeId, ownerCondArmId);
    }
  };

  // =============================== Conditionals ==============================
  struct CondArm {
    int id;
    int groupId; // owning CondGroup id
    std::string kind; // if/ifdef/ifndef/elif/else
    std::optional<std::string> cond;
    int bodyB;
    int bodyE;
    std::optional<PPSpan> ppSpan; // optional A-token span for this arm
    std::optional<bool> selected;

    bool ContainsByte(int byteOffset) const {
      return bodyB <= byteOffset && byteOffset < bodyE;
    }
  };

  struct CondGroup {
    int id;
    std::string file;
    std::optional<int> parentArmId;
    int groupB;
    int groupE;
    std::optional<int> parentIncludeId;
    std::vector<CondArm> arms;

    bool ContainsByte(int byteOffset) const {
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
  int GetTokensCountA() const { return tokensCountA_; }

  const DenseMap<int, TokMapEntry> &GetTokmapByPP() const {
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

  const IncludeItem *GetIncludeById(int id) const {
    auto it = includeById_.find(id);
    return it == includeById_.end() ? nullptr : it->second;
  }

  const CondGroup *GetCondGroupById(int id) const {
    auto it = condGroupById_.find(id);
    return it == condGroupById_.end() ? nullptr : it->second;
  }

  std::optional<ArmRef> GetArmRefById(int armId) const {
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
  unsigned GetIncludeDepth(const std::optional<int> &includeId) const;
  std::optional<int> InnermostIncludeAtPP(int ppIndex) const;
  std::optional<int> LeastCommonAncestorInclude(std::optional<int> a,
                                                std::optional<int> b) const;

  // --- Conditional nesting ---
  unsigned GetCondGroupDepth(const std::optional<int> &groupId) const;
  unsigned GetCondArmDepth(const std::optional<int> &armId) const {
    if (!armId)
      return 0;
    if (auto ref = GetArmRefById(*armId))
      return GetCondGroupDepth(ref->group->id);
    return 1;
  }

  std::vector<const CondGroup *>
  GetCondGroups(StringRef file,
                const std::optional<int> &parentIncludeId) const;

  std::optional<ArmRef>
  FindArmRefForByte(StringRef file, const std::optional<int> &parentIncludeId,
                    int byteOffset) const;

  std::optional<const CondArm *>
  FindArmForByte(StringRef file, const std::optional<int> &parentIncludeId,
                 int byteOffset) const {
    if (auto armRef = FindArmRefForByte(file, parentIncludeId, byteOffset))
      return (*armRef).arm;
    return std::nullopt;
  }

  std::optional<ArmRef> FindArmRefAtPP(int ppIndex) const;
  std::optional<const CondArm *> FindArmForPP(int ppIndex) const {
    auto ref = FindArmRefAtPP(ppIndex);
    if (!ref)
      return std::nullopt;
    return ref->arm;
  }

  int FirstConditionalArmStartA(const CondGroup &group) const;
  int FirstConditionalArmStartA(int groupId) const {
    const CondGroup *group = GetCondGroupById(groupId);
    if (!group)
      return -1;
    return FirstConditionalArmStartA(*group);
  }

  // --- Slot queries ---
  std::vector<const Slot *> FindSlots(const std::optional<std::string> &file,
                                      const std::optional<std::string> &kind,
                                      const std::optional<int> &ref,
                                      const std::optional<int> &ownerIncludeId) const;

  std::optional<const Slot *> GetBeforeIncludeSlot(int includeId) const {
    auto slots =
        FindSlots(std::nullopt, std::optional<std::string>("before_include"),
                  std::optional<int>(includeId), std::nullopt);
    if (slots.empty())
      return std::nullopt;
    return slots.front();
  }

  std::optional<const Slot *> GetAfterIncludeSlot(int includeId) const {
    auto slots =
        FindSlots(std::nullopt, std::optional<std::string>("after_include"),
                  std::optional<int>(includeId), std::nullopt);
    if (slots.empty())
      return std::nullopt;
    return slots.front();
  }

  std::optional<const Slot *> GetArmBeginSlot(int armId) const;
  std::optional<const Slot *> GetArmEndSlot(int armId) const;

  // --- Tokmap queries ---
  std::optional<TokMapEntry> MapPP(int pp) const {
    auto it = tokmapByPP_.find(pp);
    if (it == tokmapByPP_.end())
      return std::nullopt;
    return it->second;
  }

  std::vector<TokMapEntry> MapSpan(PPSpan span) const;

private:
  RefoldModel() = default;

  // =============================== Stored data ===============================
  std::string version_;
  std::string sourcePath_;
  std::string ppCwd_;
  int tokensCountA_ = 0;

  /// Optional per-token byte offsets in the preprocessed output (A stream).
  std::optional<std::vector<int64_t>> tokPPByteBeginA_;
  std::optional<std::vector<int64_t>> tokPPByteEndA_;

  DenseMap<int, TokMapEntry> tokmapByPP_; // key = pp
  std::vector<TokMapEntry> tokmap_;

  std::vector<IncludeItem> includes_;
  std::vector<MacroInvocation> macroInvs_;
  std::vector<MacroDirective> macroDirs_;
  std::vector<PragmaDirective> pragmas_;
  std::vector<FileItem> fileItems_;
  std::vector<Slot> slots_;
  std::vector<CondGroup> conds_;

  // ============================= Derived indices =============================
  DenseMap<int, const IncludeItem *> includeById_;
  DenseMap<int, const CondGroup *> condGroupById_;
  DenseMap<int, ArmRef> armById_;

  // file -> groups (all owners)
  StringMap<std::vector<const CondGroup *>> condsByFile_;

  // file -> ownerIncludeId -> groups
  StringMap<DenseMap<int, std::vector<const CondGroup *>>> condsByFileByOwner_;

  // file -> segments derived from slots
  StringMap<std::vector<Segment>> segmentsByFile_;

  // caches (computed on demand)
  mutable DenseMap<int, unsigned> includeDepthCache_;
  mutable DenseMap<int, unsigned> condGroupDepthCache_;

  // Internal helper to finalize indices and perform deterministic ordering.
  void BuildIndicesAndSort();

  std::vector<Segment>
  BuildSegmentsForFile(StringRef file,
                       const std::vector<const Slot *> &fileSlots) const;

};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMODEL_H
