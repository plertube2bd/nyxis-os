/*
 * locale.h - C89 7.4 ("C" 로캘만 지원)
 */
#ifndef _NL_LOCALE_H
#define _NL_LOCALE_H

#define _NL_WANT_NULL
#include "_nl_types.h"

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#endif /* _NL_LOCALE_H */
