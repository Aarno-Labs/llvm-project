// RUN: %clang-refold-tester header_owned_sideband_pragma_edit_crosses_boundary
// Regression: B changes the spelling of a pragma that survives into the
// preprocessed stream and whose source lives in a header, not the translation
// unit.  The header is materialized and the new spelling written into it.
//
// A re-emitted pragma is carried as a sideband edit rather than as ordinary
// token replay, and a sideband edit is proven against the bytes of the owner it
// belongs to.  When that owner is a header, the proof at
// `validateSidebandPragmaEditProof` can be run against the wrong extent and
// fail closed with
//
//   obligation=PragmaBoundaryKnown reason=UnknownPragmaCrossesBoundary
//   stage=pragma/sideband ... invalid sideband B-byte envelope
//
// which is the only way found so far to reach that obligation at all.  This
// test pins the fold rather than that refusal, because every lit-expressible
// form of the input folds: the refusal reproduces only outside the harness, and
// the difference has not been isolated.  Three discriminators are known and
// none of them explains it on its own -- deleting the pragma instead of
// respelling it folds, translation-unit material after the include folds, and
// the compared byte ranges refuse whether or not they coincide
// (`site=[11,27) b=[22,38)` refuses exactly as `site=[11,27) b=[11,27)` does,
// so the failure is the wrong extent and not an off-by-one).
//
// What this therefore covers is the sideband path succeeding across an include
// boundary, which nothing else in the suite exercises.  If a change makes it
// start refusing, `UnknownPragmaCrossesBoundary` is why.
int z = 0;
int v = 1;
#pragma pack(2)
int w = 2;
