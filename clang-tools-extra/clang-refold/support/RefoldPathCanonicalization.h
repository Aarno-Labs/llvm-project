//===--- RefoldPathCanonicalization.h --------------------------*- C++ -*-===//
//
// Filesystem path identity primitive for clang-refold.
//
// `RefoldPathIdentity` is the path oracle that planning and proof services use,
// but it cannot be reached from every layer: its header depends on RefoldModel,
// so the model itself cannot compare paths through it.  This header carries the
// canonicalization policy alone, with no model dependency, so the one rule that
// two spellings name the same file iff their weak canonicalizations agree has a
// single definition.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATHCANONICALIZATION_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATHCANONICALIZATION_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace clang {
namespace refold {

/// Return the weakly-canonical spelling of one non-empty path.
///
/// Canonicalization failure is fatal: a path that cannot be resolved must not
/// silently compare unequal to the same file named another way, because callers
/// use that comparison to decide ownership.
std::string refoldWeaklyCanonicalPath(llvm::StringRef path);

/// Return the weakly-canonical spelling of \p path, memoized.
///
/// The empty path canonicalizes to itself.  The returned view stays valid for
/// the process: entries are never evicted, so callers may hold it.
///
/// This is the single canonical-path cache.  `RefoldPathIdentity` answers from
/// it too, so a path resolved for one comparison is not resolved again for the
/// next, whichever layer asks.  The refolder is single threaded and
/// canonicalization is a pure function of the filesystem, so memoizing changes
/// only cost, never the answer.
llvm::StringRef refoldCanonicalPath(llvm::StringRef path);

/// Compare two path spellings for physical file identity.
///
/// Empty paths compare by spelling, matching `RefoldPathIdentity::PathsEqual()`;
/// non-empty paths compare by weak canonicalization, so `a/../b.h` and `b.h`
/// name the same file.  The producer emits both normalized and unnormalized
/// spellings for the same header -- an include's `resolved_path` may retain
/// `bin/../` where its `opened_path` does not -- so spelling equality is not a
/// sound test for "same file".
///
/// Identical spellings answer without consulting the filesystem at all, which
/// is the common case when scanning a slot table.
bool refoldPathsEqual(llvm::StringRef lhs, llvm::StringRef rhs);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATHCANONICALIZATION_H
