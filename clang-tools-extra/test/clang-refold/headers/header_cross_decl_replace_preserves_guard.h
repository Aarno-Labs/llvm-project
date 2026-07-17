#ifndef HEADER_CROSS_DECL_REPLACE_PRESERVES_GUARD_H
#define HEADER_CROSS_DECL_REPLACE_PRESERVES_GUARD_H

typedef int kept_before_t;
typedef int(*first_callback_t)(int);

typedef long(*second_callback_t)(long);
typedef int kept_after_t;

#endif
