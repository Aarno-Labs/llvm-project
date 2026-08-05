#ifndef RF_COND_SPLICED_DIRECTIVE_H
#define RF_COND_SPLICED_DIRECTIVE_H

// The controlling directive is spliced across two physical lines, so the arm
// body starts well past the first newline.
#if defined(RF_SPLICE_A) || \
    defined(RF_SPLICE_B)
int cond_spliced_taken = 1;
#endif

#endif
