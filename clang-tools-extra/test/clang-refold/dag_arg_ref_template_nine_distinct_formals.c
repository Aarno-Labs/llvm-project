// RUN: %clang-refold-tester dag_arg_ref_template_nine_distinct_formals
// Regression: an arg-ref template may name any number of distinct caller
// formals.
//
// `WIDE`'s body generates the `STRINGIFY` call, so that call's argument text
// is the template `a b c d e f g h i`, naming nine of `WIDE`'s formals.  The
// edit is visible only inside the resulting string literal, so preserving the
// invocation requires inverting the observed text back through that template
// to the one formal that changed.
//
// The inversion solver used to decline any template naming more than eight
// distinct caller parameters, and then declined this one on search cost when
// that limit was replaced: a placeholder delimited by a following literal may
// end at *every* occurrence of it, so nine space-delimited placeholders
// enumerate compositions of the observed text rather than one split.  Neither
// is a property of the template's width.  A template that names each formal
// once never has to re-match an assignment, which makes `(placeholder,
// offset)` a sufficient state and the inversion exactly decidable; the search
// budget now bounds only a template that binds one formal twice.
//
// Without that, `WIDE` loses its invocation and the line folds to the raw
// string literal.
#define STRINGIFY(X) #X
#define WIDE(a, b, c, d, e, f, g, h, i) STRINGIFY(a b c d e f g h i)
const char *s = WIDE(q1, q2, q3, q4, q5, q6, q7, q8, q9);
