/*
 * locale.c - C89 7.4 ("C" 로캘만). setlocale(..., "") 도 "C" 를 고른다.
 */
#include <locale.h>
#include <limits.h>
#include <string.h>

static char c_name[] = "C";
static char empty[] = "";
static char dot[] = ".";

char *setlocale(int category, const char *locale)
{
    if (category < LC_ALL || category > LC_TIME)
        return NULL;
    if (locale == NULL || locale[0] == '\0' || strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
        return c_name;
    return NULL;
}

struct lconv *localeconv(void)
{
    static struct lconv c;

    c.decimal_point = dot;
    c.thousands_sep = empty;
    c.grouping = empty;
    c.int_curr_symbol = empty;
    c.currency_symbol = empty;
    c.mon_decimal_point = empty;
    c.mon_thousands_sep = empty;
    c.mon_grouping = empty;
    c.positive_sign = empty;
    c.negative_sign = empty;
    c.int_frac_digits = CHAR_MAX;
    c.frac_digits = CHAR_MAX;
    c.p_cs_precedes = CHAR_MAX;
    c.p_sep_by_space = CHAR_MAX;
    c.n_cs_precedes = CHAR_MAX;
    c.n_sep_by_space = CHAR_MAX;
    c.p_sign_posn = CHAR_MAX;
    c.n_sign_posn = CHAR_MAX;
    return &c;
}
