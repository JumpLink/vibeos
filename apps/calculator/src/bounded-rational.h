/* bounded-rational: exact fractions that are not allowed to grow without bound; ported from
 * ExactCalculator's BoundedRational.
 *
 * The calculator keeps a result as an exact fraction for as long as it can: that is why 1÷3×3 is 1
 * and 0.1+0.2 is 0.3 here. A fraction whose numerator and denominator together pass MAX_SIZE
 * (10000 bits) is given up on: the operation returns NULL, Java's null, and the caller falls back on
 * a constructive real. Every operation passes a NULL argument on as a NULL result.
 *
 * Java kept fractions unreduced and reduced them now and then (at random, to save work); GMP's mpq_t
 * keeps them reduced always. The results are the same: Java returned null only when the REDUCED
 * fraction was too big.
 *
 * Values are immutable and reference counted; functions returning one return a new reference and
 * never consume their arguments. */
#pragma once

#include "creal.h"

#include <glib.h>
#include <gmp.h>

typedef struct _BoundedRational BoundedRational;

BoundedRational *rational_ref (BoundedRational *r);
void             rational_unref (BoundedRational *r);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BoundedRational, rational_unref)

BoundedRational *rational_new (long num, long den);
BoundedRational *rational_new_integer (const mpz_t n);
/* num/den, den != 0; NULL if too big */
BoundedRational *rational_new_fraction (const mpz_t num, const mpz_t den);

/* the reduced numerator and denominator (the denominator is positive) */
const __mpz_struct *rational_num (BoundedRational *r);
const __mpz_struct *rational_den (BoundedRational *r);

int       rational_signum (BoundedRational *r);
int       rational_compare (BoundedRational *a, BoundedRational *b);
gboolean  rational_equals_long (BoundedRational *r, long n);
gboolean  rational_is_integer (BoundedRational *r);
/* asBigInteger: TRUE and out = r if r is an integer (FALSE for NULL) */
gboolean  rational_as_integer (BoundedRational *r, mpz_t out);
/* bits left of the binary point, roughly; G_MININT for 0 (wholeNumberBits) */
int       rational_whole_number_bits (BoundedRational *r);

/* "num/den", or just "num" (toNiceString); ASCII '-' for minus */
char     *rational_to_nice_string (BoundedRational *r);
/* truncated towards 0 with n decimals, always with a '.' (toStringTruncated) */
char     *rational_to_string_truncated (BoundedRational *r, int n);
CReal    *rational_cr_value (BoundedRational *r);

BoundedRational *rational_add (BoundedRational *a, BoundedRational *b);
BoundedRational *rational_negate (BoundedRational *r);
BoundedRational *rational_subtract (BoundedRational *a, BoundedRational *b);
BoundedRational *rational_multiply (BoundedRational *a, BoundedRational *b);
/* 1/r; fails with CALC_DIVIDE_BY_ZERO for 0 */
BoundedRational *rational_inverse (BoundedRational *r);
BoundedRational *rational_divide (BoundedRational *a, BoundedRational *b);
/* the exact square root, or NULL; fails with CALC_DOMAIN_ERROR for a negative */
BoundedRational *rational_sqrt (BoundedRational *r);
BoundedRational *rational_pow_integer (BoundedRational *r, const mpz_t exp);
/* NULL unless exp is an integer */
BoundedRational *rational_pow (BoundedRational *base, BoundedRational *exp);

/* the decimals needed to show r exactly; G_MAXINT if no finite number does (digitsRequired) */
int rational_digits_required (BoundedRational *r);
