// RUN: %clang-refold-tester-expect-refold-fail import_directive_once_state_unsupported
// Refusal shape: an `#import` directive anywhere in the translation unit.
//
// `#import` is a Clang language extension, not C and not C++.  It includes the
// named header and additionally marks it, so a later `#include` of the same
// file is skipped, and it skips itself if that file was already entered by any
// earlier directive.  Measured:
//
//   #include, #include            -> two copies  (no once-state)
//   #include, #include, #import   -> two copies  (the #import skipped)
//   #import,  #include, #include  -> one copy    (both #includes skipped)
//
// It is the effect of `#pragma once` applied from the inclusion site rather
// than from inside the header, and that difference is the whole difficulty
// below: a `#pragma once` guard travels inside the header, so every include
// site tests it automatically, while this state lives outside the header and
// every site must be made to test it by hand.
//
// The producer records it faithfully, under its own subkind and with its full
// identity (`target`, `resolved_path`, `opened_path`, token span).  It used to
// do neither: `onIncludeDirective` asserted that the keyword was `#include` or
// `#include_next`, so an assertions build aborted before emitting any map, and
// a build without assertions took the else branch and labelled the item
// `"#include"` -- presenting a directive that carries once-state as one that
// does not.  That second behaviour was the real defect, because it was silent.
//
// The consumer then refuses the map:
//
//   Directive subkind '#import' at items[N] is recorded but not supported:
//   no include proof discharges its once-state
//
// The refusal is at parse time, so it costs the whole translation unit rather
// than just this directive -- and it is the most conservative of several sound
// answers, not the only one.  The realizations differ:
//
//   preserve the directive untouched -- provable today, and needs no once-state
//     reasoning at all, because nothing about the state changes.  This input is
//     exactly that case.
//   materialize or inline it -- must make every *other* inclusion of that file
//     skip.  Those sites are routinely inside other headers, which may never be
//     modified, so this is likely refused permanently rather than merely
//     unimplemented.
//   relocate it -- the above, plus ordering: moving it across another inclusion
//     of the same file changes which one enters.
//   delete it -- must prove no later inclusion observed the mark.
//
// So admitting `#import` into the include model does not require solving the
// hard cases; it requires that the easy one cannot be mistaken for them.  The
// fear behind the blunt refusal is real -- every include proof narrows on
// `#include_next` rather than widening from `#include`, so an admitted `#import`
// that reached an ungated path would be replayed or materialized as a plain
// include, dropping the once-state silently.  A set of negative checks at the
// realization sites fails open if one is missed, which is exactly that.  A
// positive capability the include item grants only to state-neutral
// realizations, and which every path must demand, fails closed on any site
// nobody thought of.
//
// The groundwork is half laid: the schema already carries the `#import` subkind,
// and RefoldPragmaOnceGuardRewriter already has an `ImportEdge` rejection
// bucket, dead today because this parse refuses first.
//
// PINNED BY ASSERTION, NOT BY `XFAIL`.  The harness requires the refold to
// fail, so this reports as a passing test.  `XFAIL` would say the refusal is
// unfinished work waiting to be closed, and it is not: it is a scope decision,
// which is a different thing and should not sit in the remaining-work count
// waiting for someone to act on it.  If the shape ever starts folding this test
// fails, which is the same alarm `XPASS` used to raise.  The `.c.i` is still
// diffed, so producer drift is still caught.
//
// TRIAGE: correct as a scope decision, NOT as a statement that the shape cannot
// be refolded.  Do not read the refusal as "no proof exists" -- preserving an
// untouched `#import` needs no proof, and this very input would refold if the
// directive were admitted at all.  What justifies refusing is that `#import` is
// in neither C nor C++ and is therefore not a target, so the gate above is
// unwritten.  The cost of leaving it unwritten is this whole-file refusal, paid
// only by translation units that use the extension.  If `#import` ever becomes
// a target the work is that gate, not anything in the producer, which already
// records the directive faithfully.
//
// What admitting it would have to produce is written here rather than kept in
// an expected `.c.mod` beside this test.  That file was never compared to
// anything -- the refusal returns before the output diff, because there is no
// accepted source to diff -- so it could drift out of step with this test with
// nothing to notice, and it had: it still carried an `XFAIL` marker for the
// policy two paragraphs up, which this test does not follow.  A record only a
// reader checks belongs where the reader already is.
//
// The directive is untouched and a single token after it changes:
//
//   int arr[] = { 1,
//   #import "gap_import_body.h"
//   9 };
//
// so once-state never comes into question for this input -- which is what makes
// the whole-file cost of the refusal visible.
int arr[] = { 1,
#import "gap_import_body.h"
2 };
