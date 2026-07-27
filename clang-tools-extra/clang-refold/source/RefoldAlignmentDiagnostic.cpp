//===--- RefoldAlignmentDiagnostic.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bridges source-structure identities into the evidence-only LCS diagnostic
// request. The certifier owns all theorem collection while its local dynamic
// program is live; this file contributes no proof scheduling or anchor policy.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldAlignmentDiagnostic.h"

#include <cassert>

namespace clang {
namespace refold {

void initializeAlignmentDiagnosticEvidence(
    const AlignmentProtectedBoundarySurfaces &protectedBoundaries,
    diffutils::LcsCertificationDiagnosticEvidence &diagnosticEvidence) {
  auto &requests = diagnosticEvidence.protectedBoundaryIdentities;
  requests.clear();
  requests.reserve(protectedBoundaries.diagnosticIdentities.size());
  for (const AlignmentProtectedBoundaryIdentity &identity :
       protectedBoundaries.diagnosticIdentities) {
    assert(identity.projectionComplete == identity.aBoundary.has_value());
    // A missing owner source proves only that the physical identity census is
    // incomplete. It does not name a concrete protected boundary and must not
    // downgrade every local window's boundary-projection evidence.
    if (identity.role == AlignmentBoundaryRole::SourceOwnerUnavailable)
      continue;
    requests.push_back(diffutils::LcsProtectedBoundaryDiagnosticIdentity{
        identity.identityId, identity.aBoundary});
  }
}

} // namespace refold
} // namespace clang
