//===--- RefoldOwnerClassifier.h -------------------------------*- C++ -*-===//
//
// Owner and translation-unit ownership classification service for clang-refold.
//
// This service owns the narrow question "which source owner does this A/B hunk
// belong to?"  It deliberately does not build TU text edits, widen TU byte
// spans, assemble accepted-result proof carriers, or order final TextEdits.
// TU edit planning lives in RefoldTUEditPlanner; final assembly lives at the
// text-edit assembler boundary.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERCLASSIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERCLASSIFIER_H

#include "core/RefoldModel.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace clang {
namespace refold {

class RefoldPathIdentity;
class RefoldTUEditPlanner;
struct SidebandPragmaEdit;

/// Classifies A-side hunk ownership without owning edit construction.
///
/// The classifier receives stable read-only model/path/sideband inputs directly
/// and queries TU-positioning through RefoldTUEditPlanner.  Keeping the planner
/// as a named constructor dependency removes the old duplicated owner/TU
/// callback bundle without letting this service build TU text edits or widen TU
/// spans itself.
class RefoldOwnerClassifier {
public:
  struct Deps {
    const RefoldModel &model;
    const RefoldPathIdentity &pathIdentity;
    const RefoldTUEditPlanner &tuEdits;
    llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits;
  };

  explicit RefoldOwnerClassifier(Deps deps);

  /// Classify one A-token hunk into its structural source owner.
  Owner ClassifyOwnerWithSegments(llvm::StringRef tuPath,
                                  const diffutils::Hunk &h) const;

  /// Return true when an A-token interval can be proven to map to the current
  /// translation-unit source surface.
  bool HunkMapsToTU(uint64_t a0, uint64_t a1,
                    llvm::StringRef tuPath) const;

private:
  Deps deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERCLASSIFIER_H
