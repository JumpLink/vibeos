/* unified-real: the calculator's numbers; ported from ExactCalculator's UnifiedReal.
 *
 * A number is the product of an exact fraction (bounded-rational.h) and a constructive real
 * (creal.h). Whenever possible the constructive real is one of a few known constants: 1, pi, e,
 * sqrt(2, 3, 5, 6, 7, 10) or ln(2, 3, 5, 6, 7, 10). Then the calculator knows more than digits:
 *  - a rational result (the constant is 1) is exact: 1÷3×3 is 1, not 0.99999...;
 *  - pi/6 is known to be pi/6, so sin(pi/6) is exactly 1/2 and sin(pi) is exactly 0;
 *  - sqrt(8) is known to be 2 sqrt(2), so sqrt(8) × sqrt(2) is exactly 4;
 *  - ln(8) is 3 ln(2), and e^(3 ln 2) is exactly 8.
 * The known constants are recognised by pointer: they are made once, and only these nodes count.
 *
 * Everything is immutable and reference counted; every function returns a new reference (never NULL)
 * and never consumes its arguments. A failed operation (calc-failure.h) returns 0 as a placeholder.
 *
 * Approximating a number (real_to_string_truncated, the comparisons) may take long or never end; it
 * is only done on the evaluator's worker thread. real_to_nice_string, real_exactly_displayable,
 * real_digits_required and real_rational_value never approximate and are safe anywhere. */
#pragma once

#include "bounded-rational.h"
#include "creal.h"

typedef struct _UnifiedReal UnifiedReal;

UnifiedReal *real_ref (UnifiedReal *x);
void         real_unref (UnifiedReal *x);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (UnifiedReal, real_unref)

UnifiedReal *real_from_long (long n);
UnifiedReal *real_from_rational (BoundedRational *r);
UnifiedReal *real_from_cr (CReal *cr);
UnifiedReal *real_pi (void);
UnifiedReal *real_e (void);
UnifiedReal *real_radians_per_degree (void);   /* pi/180 */

/* the exact fraction, if the number is known rational; NULL otherwise (boundedRationalValue) */
BoundedRational *real_rational_value (UnifiedReal *x);
/* the constructive real: fraction × constant (crValue) */
CReal       *real_cr_value (UnifiedReal *x);
gboolean     real_definitely_zero (UnifiedReal *x);

UnifiedReal *real_add (UnifiedReal *x, UnifiedReal *y);
UnifiedReal *real_subtract (UnifiedReal *x, UnifiedReal *y);
UnifiedReal *real_negate (UnifiedReal *x);
UnifiedReal *real_multiply (UnifiedReal *x, UnifiedReal *y);
UnifiedReal *real_inverse (UnifiedReal *x);
UnifiedReal *real_divide (UnifiedReal *x, UnifiedReal *y);
UnifiedReal *real_sqrt (UnifiedReal *x);
UnifiedReal *real_sin (UnifiedReal *x);
UnifiedReal *real_cos (UnifiedReal *x);
UnifiedReal *real_asin (UnifiedReal *x);
UnifiedReal *real_acos (UnifiedReal *x);
UnifiedReal *real_atan (UnifiedReal *x);
UnifiedReal *real_pow (UnifiedReal *x, UnifiedReal *exponent);
UnifiedReal *real_ln (UnifiedReal *x);
UnifiedReal *real_exp (UnifiedReal *x);
UnifiedReal *real_fact (UnifiedReal *x);

/* -1, 0 or 1; for numbers it cannot tell apart, 0 means they differ by less than 2^a */
int          real_compare_at (UnifiedReal *x, UnifiedReal *y, int a);

/* n decimals, truncated towards 0 when that is decidable, always with a '.'; ASCII '-' */
char        *real_to_string_truncated (UnifiedReal *x, int n);
/* an exact form like "1/3", "2√2", "(1/6)π" or "ln(2)", for exactly displayable numbers */
char        *real_to_nice_string (UnifiedReal *x);
/* is real_to_nice_string exact? (the constant is one of the named ones) */
gboolean     real_exactly_displayable (UnifiedReal *x);
/* decimals needed to show it exactly; G_MAXINT if no number of decimals does */
int          real_digits_required (UnifiedReal *x);
/* a lower bound on the binary zeroes after the point before the first 1; G_MAXINT if unknown */
int          real_leading_binary_zeroes (UnifiedReal *x);
/* is |x| probably more than 2^bound? (for refusing results too long to print) */
gboolean     real_approx_whole_number_bits_greater_than (UnifiedReal *x, int bound);
