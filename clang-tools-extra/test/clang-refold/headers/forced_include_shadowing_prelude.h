#ifndef FORCED_INCLUDE_SHADOWING_PRELUDE_H
#define FORCED_INCLUDE_SHADOWING_PRELUDE_H
/* Declared as a function, then shadowed by a function-like macro of the same
   name: the order curses.h uses for clear() and its siblings. */
extern int prelude_reset(void);
extern int prelude_wreset(int);
#define prelude_reset() prelude_wreset(0)
#endif
