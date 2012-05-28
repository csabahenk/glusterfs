/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#ifndef _RANGEXP_H_
#define _RANGEXP_H_

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

/*
 * Here we implement "range expressions" (rangexp), that
 * can be used to specify sets of integers:
 *
 * - atoms specify finite and infite intervals, like:
 *   3 (the singleton of number 3)
 *   3..5 3...6 (these two both specify {3, 4, 5}
 *   ...0 (negative numbers)
 *   0.. (positive numbers)
 * - connectives "," and "&" mean union and intersection:
 *   3,5 (specifies {3, 5})
 *   0..&..10 (the same as 0..10)
 * - "!" is negation:
 *   !3 (all numbers except 3)
 * - you can use parens for grouping:
 *   1..&!(3,9) (all positive numbers except 3 and 9)
 *
 * whitespace is arbitrarily allowed.
 */

/* returns:
 * -1 if rangexp is not a valid range expression
 *  0 if n is not in the set specified by rangexp
 *  1 if n is in the set specfied by rangexp
 */
int
eval_rangexp (int n, char *rangexp);

#endif
