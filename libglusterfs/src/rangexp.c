/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include "common-utils.h"
#include "rangexp.h"

typedef int (eval_rangexp_fun)(int n, char **rangexp);

static eval_rangexp_fun eval_rangexp_atom;
static eval_rangexp_fun eval_rangexp_neg;
static eval_rangexp_fun eval_rangexp_group;
static int eval_rangexp_buf (int n, char **rangexp, size_t len);

eval_rangexp_fun *eval_rangexp_funs[] = {
        eval_rangexp_atom,
        eval_rangexp_neg,
        eval_rangexp_group,
        NULL
};


/*
 * atom: expression of the form <lb><ellipsis><ub>
 * where lb,ub := integer
 *       ellipsis := .. or ...
 * - whitespace ignored
 * - leading separator ignored
 * - if both lb, ub are present, ellipsis must be there too,
 *   otherwise the components are optional
 */
static int
eval_rangexp_atom (int n, char **rangexp)
{
        int   _lb   = 0;
        int   _ub   = 0;
        int  *lb    = &_lb;
        int  *ub    = &_ub;
        int   m     = 0;
        char *r     = NULL;
        char *r0    = NULL;

        /* skip junk */
        skipwhite (rangexp);
        r0 = *rangexp;

        /* determine lb */
        *lb = strtol (*rangexp, &r, 10);
        if (r == *rangexp)
                lb = NULL;

        /* parse ellipsis */
        skipwhite (&r);
        *rangexp = r;
        while (*r == '.')
                r++;
        switch (r - *rangexp) {
        case 0:
                if (lb)
                        return (n == *lb);
                else
                        goto invalid;
        case 3:
                m = 1;
                /* fallthrough */
        case 2:
                break;
        default:
                goto invalid;
        }

        /* determine ub */
        *rangexp = r;
        skipwhite (rangexp);
        *ub = strtol (*rangexp, &r, 10);
        if (r == *rangexp)
                ub = NULL;

        *rangexp = r;
        skipwhite (rangexp);

        /* evalutate */
        return (!lb || n >= *lb) && (!ub || n <= *ub - m);

 invalid:
        *rangexp = r0;
        return -1;
}

/*
 * negated rangexp: !rangexp
 */
static int
eval_rangexp_neg (int n, char **rangexp)
{
        char              *r0  = NULL;
        eval_rangexp_fun **ee  = NULL;
        int                val = 0;

        /* skip junk */
        skipwhite (rangexp);
        r0 = *rangexp;

        if (**rangexp != '!')
                goto invalid;

        (*rangexp)++;
        for (ee = eval_rangexp_funs; *ee; ee++) {
                val = (*ee) (n, rangexp);
                if (val != -1)
                        break;
        }
        if (val == -1)
                goto invalid;

        return !val;

 invalid:
        *rangexp = r0;
        return -1;
}

/*
 * rangexp group: (rangexp)
 */
static int
eval_rangexp_group (int n, char **rangexp)
{
        char *r0  = NULL;
        char *r   = NULL;
        int   p   = 0;
        int   val = 0;

        /* skip junk */
        skipwhite (rangexp);
        r0 = *rangexp;

        if (**rangexp != '(')
                goto invalid;
        (*rangexp)++;

        /* find matching paren */
        for (r = *rangexp, p = 1 ;; r++) {
                switch (*r) {
                case '(':
                        p++;
                        break;
                case ')':
                        p--;
                        break;
                case '\0':
                        goto invalid;
                }
                if (p == 0)
                        break;
        }

        /* recurse to inner rangexp */
        val = eval_rangexp_buf (n, rangexp, r - *rangexp);
        if (val == -1)
                goto invalid;
        *rangexp = r + 1;
        skipwhite (rangexp);
        return val;

 invalid:
        *rangexp = r0;
        return -1;
}

static int
eval_rangexp_buf (int n, char **rangexp, size_t len)
{
        char              *r0   = *rangexp;
        char              *r    = NULL;
        eval_rangexp_fun **ee   = NULL;
        int                v    = 0;
        int                val  = 0;
        char               conn = '\0';

        for (;;) {
                for (ee = eval_rangexp_funs; *ee; ee++) {
                        v = (*ee) (n, rangexp);
                        if (v != -1)
                                break;
                }
                if (v == -1)
                        goto invalid;

                switch (conn) {
                case '\0':
                        val = v;
                        break;
                case ',':
                        val = (val || v);
                        break;
                case '&':
                        val = (val && v);
                        break;
                default:
                        GF_ASSERT (!"notreached");
                }

                GF_ASSERT (*rangexp <= r0 + len);
                if (*rangexp == r0 + len)
                        break;

                conn = **rangexp;
                if (!(conn == ',' || conn == '&'))
                        goto invalid;
                (*rangexp)++;
        }

        return val;

 invalid:
        *rangexp = r0;
        return -1;
}

int
eval_rangexp (int n, char *rangexp)
{
        return eval_rangexp_buf (n, &rangexp, strlen (rangexp));
}

#ifdef RANGEXP_STANDALONE
int
main (int argc, char **argv)
{
        char buf[80];
        int n = 0;
        int i = 0;
        int j = 0;
        int v = 0;

        if (strcmp (argv[1], "-p") == 0 ||
            strcmp (argv[1], "--perf") == 0) {
                i = strtol (argv[2], NULL, 10);
                n = strtol (argv[3], NULL, 10);

                for (j = 0; j < i; j++)
                         v = eval_rangexp (n, argv[4]);

                return v;
        }

        while (fgets (buf, 80, stdin)) {
                n = strtol (buf, NULL, 10);
                printf ("%2d: ", n);
                fflush (stdout);
                for (i = 1; i < argc; i++) {
                        printf ("%d%c", eval_rangexp (n, argv[i]),
                                (i == argc - 1) ? '\n' : ' ');
                        fflush (stdout);
                }
        }
}
#endif
