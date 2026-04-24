//===--- RefoldEngine.ExpansionFallback.inc ------------------*- C++ -*-===//
//
// `RefoldEngine` member definitions physically split out of
// `RefoldEngine.cpp` to reduce the size of the main implementation file.
//
// This fragment contains the post-structural, pre-terminal fallback stage for
// proof-backed partial expansion. These members do not participate in normal
// structural candidate construction; they are only consulted after the main
// structural refolding pass has already requested terminal fallback and before
// the engine emits raw edited preprocessed text `B`.
//
// This is a physical extraction only:
//   - the methods remain `RefoldEngine` members
//   - the file is included directly into `RefoldEngine.cpp`
//   - private access, translation-unit scope, and behavior are unchanged
//
// The purpose of this fragment is organizational only: keep the explicit
// partial-expansion fallback seam and its witness-formatting helpers out of the
// main engine file while preserving the exact implementation structure and
// semantics.
//
//===----------------------------------------------------------------------===//

std::string RefoldEngine::FormatExpansionClosureWitness(
    const ExpansionClosureWitness &witness) const {
  std::string out = formatv("kind={0}", witness.kind).str();
  if (witness.hasRootMacroId)
    out += formatv(" rootMacro={0}", witness.rootMacroId).str();
  if (witness.hasOwnerIncludeId)
    out += formatv(" ownerInclude={0}", witness.ownerIncludeId).str();
  out += formatv(" source=[{0},{1})", witness.sourceByteBegin,
                 witness.sourceByteEnd)
             .str();
  out += formatv(" coverA=[{0},{1})", witness.coverATokBegin,
                 witness.coverATokEnd)
             .str();
  out += formatv(" B=[{0},{1})", witness.bTokBegin, witness.bTokEnd).str();
  out += formatv(" selfContained={0} claimsClipped={1}",
                 witness.selfContained ? 1 : 0,
                 witness.claimsClipped ? 1 : 0)
             .str();
  return out;
}

std::optional<RefoldEngine::ExpansionClosureWitness>
RefoldEngine::BuildMacroOwnerExpansionClosureWitness() const {
  // Step 1 intentionally leaves the fallback domain empty while we carve out
  // the explicit seam and witness carrier. Later patches will populate this
  // from a concrete single-owner macro whole-expansion proof.
  return std::nullopt;
}

std::optional<std::string> RefoldEngine::TryExpansionClosureFallback() const {
  if (!terminalFallbackRequested_)
    return std::nullopt;

  if (auto witness = BuildMacroOwnerExpansionClosureWitness()) {
    debug("fallback", "expansion-closure witness discovered: {0}",
          FormatExpansionClosureWitness(*witness));
  } else {
    debug("fallback",
          "expansion-closure stage: no proved single-owner macro fallback "
          "witness available");
  }

  // Step 1 is plumbing only: preserve the existing fail-closed behavior until
  // a concrete proof-backed closure class can synthesize a replacement source.
  return std::nullopt;
}
