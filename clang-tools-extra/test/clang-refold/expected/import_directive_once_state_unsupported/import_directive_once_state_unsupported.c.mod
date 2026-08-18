// RUN: %clang-refold-tester import_directive_once_state_unsupported
// XFAIL: *
// Refusal shape: an `#import` directive anywhere in the translation unit.
//
// `#import` is a Clang language extension, not C and not C++.  It includes the
// named header and additionally marks it as imported, so a later `#include` of
// the same file is skipped -- the effect of `#pragma once`, applied from the
// inclusion site rather than from inside the header.
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
// than just this directive.  Localizing it would mean admitting `#import` into
// the include model, and every include proof narrows on `#include_next` instead
// of widening from `#include`, so an admitted `#import` would be replayed,
// materialized or relocated as a plain include -- dropping the once-state
// silently.  Making the refusal local therefore requires an explicit gate at
// each realization site, which is only worth writing if `#import` needs to be
// supported.
//
// TRIAGE: correct.  This should stay expected-to-fail unless `#import` becomes
// a target, in which case the work is the realization-site audit above and not
// anything in the producer.  The expected refold below records what admitting
// it would have to produce: the directive is untouched and a single token after
// it changes, so once-state never comes into question for this input -- which
// is what makes the whole-file cost of the refusal visible.
int arr[] = { 1,
#import "gap_import_body.h"
9 };
