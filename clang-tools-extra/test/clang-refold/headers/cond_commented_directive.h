#ifndef RF_COND_COMMENTED_DIRECTIVE_H
#define RF_COND_COMMENTED_DIRECTIVE_H

/* Documentation that shows how to use the macro, the way glibc headers do.
   The example directives are indented but are not prefixed by anything, so a
   raw line scan sees a '#' as the first non-whitespace character:

   #if defined(RF_COMMENTED_OUT)
   ... example body ...
   #endif

   Note the unbalanced closer below is also inside this comment.  */
#if defined(RF_REAL_CONDITION)
int cond_commented_taken = 1;
#endif

// #if defined(RF_LINE_COMMENTED_OUT)
// #endif

#endif
