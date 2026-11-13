/* creal: constructive real numbers, a C port of the com.hp.creals library (Hans-J. Boehm's "CR")
 * that ExactCalculator is built on.
 *
 * A constructive real is not a number held to some precision: it is a recipe that can produce the
 * number to ANY precision asked for. creal_get_appr (x, p, out) gives an integer n with
 * |n * 2^p - x| < 2^p, i.e. x to within one unit of 2^p. The recipe is a tree of nodes (x + y is an
 * "add" node over x and y, and so on); each node remembers its best approximation so far.
 *
 * This is what lets the calculator show as many digits as the user scrolls to: each further digit
 * is just a get_appr at a finer precision. Big integers are GMP's mpz_t (Java used BigInteger).
 *
 * Failures (Java's exceptions) go to calc-failure.h: sqrt or ln of a negative number is
 * CALC_DOMAIN_ERROR, a precision past 2^28 bits is CALC_PRECISION_OVERFLOW, a cancelled computation
 * is CALC_ABORTED. After a failure, get_appr gives 0 and nothing more is cached.
 *
 * Nodes are reference counted. Every function returning a CReal * returns a new reference; the
 * arguments are never consumed. Approximations are only ever computed on one thread at a time (the
 * evaluator's worker), so the nodes have no lock; only the reference count is atomic.
 *
 * pi is computed with the library's own Machin formula (CR.atan_PI, 4*(4*atan(1/5) - atan(1/239)))
 * instead of its Gauss-Legendre gl_pi_CR: both are exact to any precision, Machin is far shorter. */
#pragma once

#include <glib.h>
#include <gmp.h>

typedef struct _CReal CReal;

CReal *creal_ref (CReal *x);
void   creal_unref (CReal *x);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CReal, creal_unref)

CReal *creal_from_long (long n);
CReal *creal_from_mpz (const mpz_t n);
CReal *creal_one (void);   /* the same node every time (CR.ONE) */
CReal *creal_pi (void);    /* the same node every time (CR.PI) */

CReal *creal_add (CReal *x, CReal *y);
CReal *creal_subtract (CReal *x, CReal *y);
CReal *creal_negate (CReal *x);
CReal *creal_multiply (CReal *x, CReal *y);
CReal *creal_inverse (CReal *x);
CReal *creal_divide (CReal *x, CReal *y);
CReal *creal_shift_left (CReal *x, int n);   /* x * 2^n */
CReal *creal_shift_right (CReal *x, int n);  /* x / 2^n */
/* x if selector < 0, y otherwise (CR.select: the selector need not be decidably non-zero) */
CReal *creal_select (CReal *selector, CReal *x, CReal *y);
CReal *creal_abs (CReal *x);

CReal *creal_sqrt (CReal *x);
CReal *creal_exp (CReal *x);
CReal *creal_ln (CReal *x);
CReal *creal_cos (CReal *x);
CReal *creal_sin (CReal *x);
CReal *creal_asin (CReal *x);
CReal *creal_acos (CReal *x);
CReal *creal_atan (CReal *x);   /* UnaryCRFunction.atanFunction */

/* out = an integer within 1 of x / 2^p */
void creal_get_appr (CReal *x, int p, mpz_t out);
/* the sign, deciding at precision a: 0 means |x| < 2^a, so maybe zero (CR.signum(int)) */
int  creal_signum_at (CReal *x, int a);
/* the sign; never returns for 0 until cancelled (CR.signum()) */
int  creal_signum (CReal *x);
/* -1, 0 or 1; 0 means |x - y| < 2^a (CR.compareTo(CR, int)) */
int  creal_compare_at (CReal *x, CReal *y, int a);
/* -1 or 1; never returns for equal numbers until cancelled (CR.compareTo(CR)) */
int  creal_compare (CReal *x, CReal *y);

/* BigInteger.bitLength(): bits of the two's complement value without its sign bit */
gsize calc_bit_length (const mpz_t n);
