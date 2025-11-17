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
  // ======= Coordinate primitives =======
  struct PPSpan {
    int begin; // inclusive A-token index
    int end;   // exclusive A-token index

    std::string toString() { return formatv("[{0},{1})", begin, end); }

    bool isValid() const { return begin >= 0 && end >= 0 && end > begin; }
  };

  struct PPCover {
    int begin; // inclusive
    int end;   // exclusive

    void init(std::optional<int> coverBeginOpt, std::optional<int> coverEndOpt,
              const std::vector<PPSpan> &spans) noexcept {
      int cb = -1, ce = -1;
      if (coverBeginOpt && coverEndOpt) {
        cb = *coverBeginOpt;
        ce = *coverEndOpt;
      }

      if ((cb < 0 || ce < 0) && !spans.empty()) {
        int mn = INT32_MAX, mx = INT32_MIN;
        for (const auto &s : spans) {
          if (s.begin < mn)
            mn = s.begin;
          if (s.end > mx)
            mx = s.end;
        }
        if (mn != INT32_MAX) {
          cb = mn;
          ce = std::max(mn, mx);
        }
      }
      begin = cb;
      end = ce;
    }

    bool covers(int aStart, int aEnd) const noexcept {
      return begin >= 0 && end >= 0 && begin <= aStart && aEnd <= end;
    }
  };

  struct TokMapEntry {
    int pp;
    std::string file;
    int b;
    int e;
  };

  // ======= Items =======
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

    IncludeItem(int id, std::string subkind, std::string text,
                std::string sitePath, int siteB, int siteE, std::string target,
                std::optional<std::string> resolvedPath, bool angled,
                std::optional<int> parent, std::vector<PPSpan> spans,
                std::optional<int> coverBegin,
                std::optional<int> coverEnd) noexcept
        : id(id), subkind(std::move(subkind)), text(std::move(text)),
          sitePath(std::move(sitePath)), siteB(siteB), siteE(siteE),
          target(std::move(target)), resolvedPath(std::move(resolvedPath)),
          angled(angled), parent(std::move(parent)), spans(std::move(spans)) {
      cover.init(coverBegin, coverEnd, this->spans);
    }

    bool covers(int aStart, int aEnd) const {
      return cover.covers(aStart, aEnd);
    }
  };

  struct MacroInvocation {
    int id;
    std::string subkind; // "func" | "obj"
    std::string name;
    std::vector<PPSpan> spans; // expansion coverage (A tokens)
    std::vector<PPSpan> argSpans;
    std::vector<PPSpan> bodySpans;
    std::optional<std::string> invText;
    std::optional<std::string> invFile;
    std::optional<int> invB;
    std::optional<int> invE;
    std::optional<int> ownerIncludeId;
    PPCover cover;

    MacroInvocation(int id, std::string subkind, std::string name,
                    std::optional<std::string> invText,
                    std::optional<std::string> invFile, std::optional<int> invB,
                    std::optional<int> invE, std::optional<int> ownerIncludeId,
                    std::vector<PPSpan> spans, std::vector<PPSpan> argSpans,
                    std::vector<PPSpan> bodySpans,
                    std::optional<int> coverBegin,
                    std::optional<int> coverEnd) noexcept
        : id(id), subkind(std::move(subkind)), name(std::move(name)),
          spans(std::move(spans)), argSpans(std::move(argSpans)),
          bodySpans(std::move(bodySpans)), invText(std::move(invText)),
          invFile(std::move(invFile)), invB(std::move(invB)),
          invE(std::move(invE)), ownerIncludeId(std::move(ownerIncludeId)) {
      cover.init(coverBegin, coverEnd, this->spans);
    }

    int getInvB() const { return invB.has_value() ? *invB : -1; }
    int getInvE() const { return invE.has_value() ? *invE : -1; }

    bool covers(int aStart, int aEnd) const {
      return cover.covers(aStart, aEnd);
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

  // ======= Slots =======
  struct Slot {
    int id;
    std::string file;
    std::string kind;       // enum per schema
    std::optional<int> ref; // include id or cond-arm id
    int b;
    int e;
    std::optional<int> ownerIncludeId;
    std::optional<int> pp; // tiebreak
  };

  // ======= Conditionals =======
  struct CondArm {
    int id;
    std::string tag; // "if" | "ifdef" | "ifndef" | "elif" | "else"
    std::optional<std::string> cond;
    int bodyB;
    int bodyE;
    std::optional<bool> selected;

    bool containsByte(int off) const { return bodyB <= off && off < bodyE; }
  };

  struct CondGroup {
    int id;
    std::string file;
    std::optional<int> parent; // nested group parent
    int groupB;
    int groupE;
    std::optional<int> parentIncludeId;
    std::vector<CondArm> arms;
  };

  struct ArmRef {
    const CondGroup *group;
    const CondArm *arm;
  };

public:
  // ======= Construction / Parsing =======

  /// \brief Constructs a RefoldModel instance from a parsed JSON object.
  ///
  /// This factory method deserializes a validated `clang-refold` map JSON
  /// (produced by the modified Clang preprocessor) into a strongly typed
  /// `RefoldModel`. The model mirrors the structure of `RefoldSchema.json`
  /// and provides direct access to tokens, items, and tokmap entries used
  /// during the refolding process.
  ///
  /// \param Root The top-level JSON object parsed from the refold map file.
  /// \return An `llvm::Expected<RefoldModel>` containing the constructed model
  ///         on success, or an error if required fields are missing or invalid.
  ///
  /// \see RefoldSchema.h
  static Expected<RefoldModel> FromJson(const json::Object &Root);

  // ======= Basic getters (match Java API) =======
  StringRef GetVersion() const { return version_; }
  StringRef GetSourcePath() const { return sourcePath_; }
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

  // ======= Helpers for the engine (ported from Java) =======
  // Conditional queries
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

  // Slot queries
  std::vector<const Slot *>
  FindSlots(const std::optional<std::string> &file,
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

  // Tokmap queries
  std::optional<TokMapEntry> MapPP(int pp) const {
    auto it = tokmapByPP_.find(pp);
    if (it == tokmapByPP_.end())
      return std::nullopt;
    return it->second;
  }
  std::vector<TokMapEntry> MapSpan(PPSpan span) const;

private:
  RefoldModel() = default;

  // ======= Stored data =======
  std::string version_;
  std::string sourcePath_;
  int tokensCountA_ = 0;

  DenseMap<int, TokMapEntry> tokmapByPP_; // key = pp
  std::vector<TokMapEntry> tokmap_;

  std::vector<IncludeItem> includes_;
  std::vector<MacroInvocation> macroInvs_;
  std::vector<MacroDirective> macroDirs_;
  std::vector<PragmaDirective> pragmas_;
  std::vector<FileItem> fileItems_;
  std::vector<Slot> slots_;
  std::vector<CondGroup> conds_;

  // ======= Derived indices =======
  DenseMap<int, const IncludeItem *> includeById_;

  // file -> groups (all owners)
  StringMap<std::vector<const CondGroup *>> condsByFile_;

  // file -> ownerIncludeId -> groups
  StringMap<DenseMap<int, std::vector<const CondGroup *>>> condsByFileByOwner_;

  // Internal helper to finalize indices and perform deterministic ordering.
  void BuildIndicesAndSort();
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMODEL_H
