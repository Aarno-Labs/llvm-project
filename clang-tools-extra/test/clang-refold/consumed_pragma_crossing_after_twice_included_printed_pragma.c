// RUN: %clang-refold-tester-relaxed consumed_pragma_crossing_after_twice_included_printed_pragma
// Regression: including a header with a printed pragma twice must not cost the
// rest of the file its consumed-pragma crossings.
//
// Crossing `#pragma region` needs the producer's word that the preprocessor
// printed nothing for it, which holds only when every printed pragma is bound
// to a record.  The producer used to keep one record per physical pragma line,
// so the second inclusion's print had none, the certificate failed for the
// whole translation unit, and the straddle below refused.  Each inclusion now
// has its own record, with its own image.
#include "printed_pragma_included_twice.h"
#include "printed_pragma_included_twice.h"
int arr[] = { 1,
#pragma region
2 };
