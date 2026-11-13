/* unified-real: see unified-real.h. A port of ExactCalculator's UnifiedReal, method for method; the
 * comments name the Java methods. */
#include "unified-real.h"
#include "calc-failure.h"

#include <math.h>
#include <string.h>

/* compare numbers that cannot be told apart exactly down to 2^-1000 (DEFAULT_COMPARE_TOLERANCE) */
#define DEFAULT_COMPARE_TOLERANCE -1000
/* extra bits used by toStringTruncated, so that it truncates rather than rounds (EXTRA_PREC) */
#define EXTRA_PREC 10

struct _UnifiedReal {
  gint             ref_count;
  BoundedRational *rat;   /* mRatFactor, never NULL */
  CReal           *cr;    /* mCrFactor */
};

/* --- the named constants (CR_ONE, CR_PI, CR_E, sSqrts, sLogs) */

#define N_NAMED 11

typedef struct {
  CReal *one;
  CReal *pi;
  CReal *e;
  CReal *sqrts[N_NAMED];   /* sqrt(i) for i = 1 (the same node as one), 2, 3, 5, 6, 7, 10 */
  CReal *logs[N_NAMED];    /* ln(i) for i = 2, 3, 5, 6, 7, 10 */
} Named;

static const Named *
named (void)
{
  static Named n;
  static gsize done;

  if (g_once_init_enter (&done)) {
    n.one = creal_one ();
    n.pi = creal_pi ();
    n.e = creal_exp (n.one);
    n.sqrts[1] = creal_ref (n.one);
    const int roots[] = { 2, 3, 5, 6, 7, 10 };
    for (guint i = 0; i < G_N_ELEMENTS (roots); i++) {
      g_autoptr (CReal) k = creal_from_long (roots[i]);
      n.sqrts[roots[i]] = creal_sqrt (k);
      n.logs[roots[i]] = creal_ln (k);
    }
    g_once_init_leave (&done, 1);
  }
  return &n;
}

/* k if cr is sqrt(k), else 0 (getSquare) */
static int
square_of (CReal *cr)
{
  for (int i = 1; i < N_NAMED; i++)
    if (named ()->sqrts[i] && named ()->sqrts[i] == cr)
      return i;
  return 0;
}

/* k if cr is ln(k), else 0 (getExp) */
static int
exp_of (CReal *cr)
{
  for (int i = 2; i < N_NAMED; i++)
    if (named ()->logs[i] && named ()->logs[i] == cr)
      return i;
  return 0;
}

/* the constant's name in toNiceString: "" for 1, "π", "e", "√2", "ln(2)"; NULL if unnamed (crName) */
static char *
cr_name (CReal *cr)
{
  int k;

  if (cr == named ()->one)
    return g_strdup ("");
  if (cr == named ()->pi)
    return g_strdup ("π");
  if (cr == named ()->e)
    return g_strdup ("e");
  if ((k = square_of (cr)))
    return g_strdup_printf ("√%d", k);
  if ((k = exp_of (cr)))
    return g_strdup_printf ("ln(%d)", k);
  return NULL;
}

static gboolean
is_named (CReal *cr)
{
  return cr == named ()->one || cr == named ()->pi || cr == named ()->e || square_of (cr) || exp_of (cr);
}

static gboolean
cr_definitely_algebraic (CReal *cr)
{
  return cr == named ()->one || square_of (cr);
}

/* --- making numbers */

UnifiedReal *
real_ref (UnifiedReal *x)
{
  g_atomic_int_inc (&x->ref_count);
  return x;
}

void
real_unref (UnifiedReal *x)
{
  if (!x || !g_atomic_int_dec_and_test (&x->ref_count))
    return;
  rational_unref (x->rat);
  creal_unref (x->cr);
  g_free (x);
}

/* the UnifiedReal(BoundedRational, CR) constructor: a NULL fraction is an ArithmeticException */
static UnifiedReal *
real_new (BoundedRational *rat, CReal *cr)
{
  UnifiedReal *x = g_new0 (UnifiedReal, 1);

  x->ref_count = 1;
  if (!rat) {
    calc_fail (CALC_DOMAIN_ERROR);   /* "Building UnifiedReal from null" */
    x->rat = rational_new (0, 1);
    x->cr = creal_one ();
    return x;
  }
  x->rat = rational_ref (rat);
  x->cr = creal_ref (cr);
  return x;
}

UnifiedReal *
real_from_rational (BoundedRational *r)
{
  return real_new (r, named ()->one);
}

UnifiedReal *
real_from_long (long n)
{
  g_autoptr (BoundedRational) r = rational_new (n, 1);
  return real_from_rational (r);
}

static UnifiedReal *
real_from_integer (const mpz_t n)
{
  g_autoptr (BoundedRational) r = rational_new_integer (n);
  return real_from_rational (r);
}

static UnifiedReal *
real_from_fraction (long num, long den, CReal *cr)
{
  g_autoptr (BoundedRational) r = rational_new (num, den);
  return real_new (r, cr);
}

UnifiedReal *
real_from_cr (CReal *cr)
{
  g_autoptr (BoundedRational) one = rational_new (1, 1);
  return real_new (one, cr);
}

UnifiedReal *
real_pi (void)
{
  return real_from_cr (named ()->pi);
}

UnifiedReal *
real_e (void)
{
  return real_from_cr (named ()->e);
}

UnifiedReal *
real_radians_per_degree (void)
{
  return real_from_fraction (1, 180, named ()->pi);
}

static UnifiedReal *
real_zero (void)
{
  return real_from_long (0);
}

/* --- what is known about a number */

gboolean
real_definitely_zero (UnifiedReal *x)
{
  return rational_signum (x->rat) == 0;
}

static gboolean
definitely_rational (UnifiedReal *x)
{
  return x->cr == named ()->one || rational_signum (x->rat) == 0;
}

static gboolean
definitely_irrational (UnifiedReal *x)
{
  return !definitely_rational (x) && is_named (x->cr);
}

/* are r1 and r2 known to be linearly independent over the rationals? (definitelyIndependent) */
static gboolean
definitely_independent (CReal *r1, CReal *r2)
{
  if (r1 == r2)
    return FALSE;
  if (r1 == named ()->e || r1 == named ()->pi)
    return cr_definitely_algebraic (r2);
  if (r2 == named ()->e || r2 == named ()->pi)
    return cr_definitely_algebraic (r1);
  return is_named (r1) && is_named (r2);
}

BoundedRational *
real_rational_value (UnifiedReal *x)
{
  if (definitely_rational (x))
    return x->rat;
  return NULL;
}

/* the value as an integer, if it is known to be one (bigIntegerValue) */
static gboolean
real_integer_value (UnifiedReal *x, mpz_t out)
{
  return rational_as_integer (real_rational_value (x), out);
}

CReal *
real_cr_value (UnifiedReal *x)
{
  g_autoptr (CReal) rat = rational_cr_value (x->rat);
  return creal_multiply (rat, x->cr);
}

gboolean
real_exactly_displayable (UnifiedReal *x)
{
  g_autofree char *name = cr_name (x->cr);
  return name != NULL;
}

char *
real_to_nice_string (UnifiedReal *x)
{
  if (x->cr == named ()->one || rational_signum (x->rat) == 0)
    return rational_to_nice_string (x->rat);
  g_autofree char *name = cr_name (x->cr);
  /* Java printed 10 digits of an unnamed constant here; the calculator asks only when
   * real_exactly_displayable, and digits would need an approximation on this thread */
  if (!name)
    return NULL;
  g_autofree char *rat = rational_to_nice_string (x->rat);
  if (rational_is_integer (x->rat)) {
    if (rational_equals_long (x->rat, 1))
      return g_steal_pointer (&name);
    return g_strconcat (rat, name, NULL);
  }
  return g_strdup_printf ("(%s)%s", rat, name);
}

/* can truncated digits be computed exactly? (exactlyTruncatable) known rationals compare exactly;
 * known irrationals never equal a decimal, so comparisons with one end */
static gboolean
exactly_truncatable (UnifiedReal *x)
{
  return x->cr == named ()->one || rational_signum (x->rat) == 0 || definitely_irrational (x);
}

char *
real_to_string_truncated (UnifiedReal *x, int n)
{
  if (x->cr == named ()->one || rational_signum (x->rat) == 0)
    return rational_to_string_truncated (x->rat, n);

  mpz_t ten_pow, int_scaled;
  gboolean negative = FALSE;
  mpz_inits (ten_pow, int_scaled, NULL);
  mpz_ui_pow_ui (ten_pow, 10, n);
  g_autoptr (CReal) scale = creal_from_mpz (ten_pow);
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) scaled = creal_multiply (scale, value);
  if (exactly_truncatable (x)) {
    creal_get_appr (scaled, 0, int_scaled);
    if (mpz_sgn (int_scaled) < 0) {
      negative = TRUE;
      mpz_neg (int_scaled, int_scaled);
    }
    g_autoptr (CReal) candidate = creal_from_mpz (int_scaled);
    g_autoptr (CReal) abs = creal_abs (scaled);
    if (creal_compare (candidate, abs) > 0)
      mpz_sub_ui (int_scaled, int_scaled, 1);
  } else {
    creal_get_appr (scaled, -EXTRA_PREC, int_scaled);
    if (mpz_sgn (int_scaled) < 0) {
      negative = TRUE;
      mpz_neg (int_scaled, int_scaled);
    }
    mpz_fdiv_q_2exp (int_scaled, int_scaled, EXTRA_PREC);
  }

  g_autofree char *digits = mpz_get_str (NULL, 10, int_scaled);
  mpz_clears (ten_pow, int_scaled, NULL);
  GString *result = g_string_new (negative ? "-" : "");
  /* zeroes in front, for at least one digit before the point */
  GString *padded = g_string_new (NULL);
  for (int i = strlen (digits); i < n + 1; i++)
    g_string_append_c (padded, '0');
  g_string_append (padded, digits);
  int whole = padded->len - n;
  g_string_append_len (result, padded->str, whole);
  g_string_append_c (result, '.');
  g_string_append (result, padded->str + whole);
  g_string_free (padded, TRUE);
  return g_string_free (result, FALSE);
}

/* --- comparisons */

/* can x and y be compared exactly, without the comparison running forever? (isComparable) */
static gboolean
is_comparable (UnifiedReal *x, UnifiedReal *y)
{
  if (x->cr == y->cr && (is_named (x->cr) || creal_signum_at (x->cr, DEFAULT_COMPARE_TOLERANCE) != 0))
    return TRUE;
  if (rational_signum (x->rat) == 0 && rational_signum (y->rat) == 0)
    return TRUE;
  if (definitely_independent (x->cr, y->cr))
    return TRUE;
  g_autoptr (CReal) xv = real_cr_value (x);
  g_autoptr (CReal) yv = real_cr_value (y);
  return creal_compare_at (xv, yv, DEFAULT_COMPARE_TOLERANCE) != 0;
}

/* may never return if x == y but that cannot be shown (compareTo(UnifiedReal)) */
static int
real_compare (UnifiedReal *x, UnifiedReal *y)
{
  if (real_definitely_zero (x) && real_definitely_zero (y))
    return 0;
  if (x->cr == y->cr) {
    int signum = creal_signum (x->cr);   /* can diverge if the constant is 0 */
    return signum * rational_compare (x->rat, y->rat);
  }
  g_autoptr (CReal) xv = real_cr_value (x);
  g_autoptr (CReal) yv = real_cr_value (y);
  return creal_compare (xv, yv);
}

int
real_compare_at (UnifiedReal *x, UnifiedReal *y, int a)
{
  if (is_comparable (x, y))
    return real_compare (x, y);
  g_autoptr (CReal) xv = real_cr_value (x);
  g_autoptr (CReal) yv = real_cr_value (y);
  return creal_compare_at (xv, yv, a);
}

static int
real_signum_at (UnifiedReal *x, int a)
{
  g_autoptr (UnifiedReal) zero = real_zero ();
  return real_compare_at (x, zero, a);
}

static int
real_signum (UnifiedReal *x)
{
  g_autoptr (UnifiedReal) zero = real_zero ();
  return real_compare (x, zero);
}

/* equal to within 2^a, or exactly equal where that is decidable (approxEquals) */
static gboolean
approx_equals (UnifiedReal *x, UnifiedReal *y, int a)
{
  if (is_comparable (x, y)) {
    if (definitely_independent (x->cr, y->cr)
        && (rational_signum (x->rat) != 0 || rational_signum (y->rat) != 0))
      return FALSE;
    return real_compare (x, y) == 0;
  }
  g_autoptr (CReal) xv = real_cr_value (x);
  g_autoptr (CReal) yv = real_cr_value (y);
  return creal_compare_at (xv, yv, a) == 0;
}

static gboolean
definitely_equals (UnifiedReal *x, UnifiedReal *y)
{
  return is_comparable (x, y) && real_compare (x, y) == 0;
}

/* --- arithmetic */

UnifiedReal *
real_add (UnifiedReal *x, UnifiedReal *y)
{
  if (x->cr == y->cr) {
    g_autoptr (BoundedRational) sum = rational_add (x->rat, y->rat);
    if (sum)
      return real_new (sum, x->cr);
  }
  if (real_definitely_zero (x))
    return real_ref (y);
  if (real_definitely_zero (y))
    return real_ref (x);
  g_autoptr (CReal) xv = real_cr_value (x);
  g_autoptr (CReal) yv = real_cr_value (y);
  g_autoptr (CReal) sum = creal_add (xv, yv);
  return real_from_cr (sum);
}

UnifiedReal *
real_negate (UnifiedReal *x)
{
  g_autoptr (BoundedRational) minus = rational_negate (x->rat);
  return real_new (minus, x->cr);
}

UnifiedReal *
real_subtract (UnifiedReal *x, UnifiedReal *y)
{
  g_autoptr (UnifiedReal) minus_y = real_negate (y);
  return real_add (x, minus_y);
}

UnifiedReal *
real_multiply (UnifiedReal *x, UnifiedReal *y)
{
  /* keep an existing constant when we can */
  if (x->cr == named ()->one) {
    g_autoptr (BoundedRational) product = rational_multiply (x->rat, y->rat);
    if (product)
      return real_new (product, y->cr);
  }
  if (y->cr == named ()->one) {
    g_autoptr (BoundedRational) product = rational_multiply (x->rat, y->rat);
    if (product)
      return real_new (product, x->cr);
  }
  if (real_definitely_zero (x) || real_definitely_zero (y))
    return real_zero ();
  if (x->cr == y->cr) {
    /* sqrt(k) × sqrt(k) = k */
    int square = square_of (x->cr);
    if (square) {
      g_autoptr (BoundedRational) k = rational_new (square, 1);
      g_autoptr (BoundedRational) k_rat = rational_multiply (k, x->rat);
      g_autoptr (BoundedRational) product = rational_multiply (k_rat, y->rat);
      if (product)
        return real_from_rational (product);
    }
  }
  g_autoptr (BoundedRational) product = rational_multiply (x->rat, y->rat);
  if (product) {
    g_autoptr (CReal) crs = creal_multiply (x->cr, y->cr);
    return real_new (product, crs);
  }
  g_autoptr (CReal) xv = real_cr_value (x);
  g_autoptr (CReal) yv = real_cr_value (y);
  g_autoptr (CReal) all = creal_multiply (xv, yv);
  return real_from_cr (all);
}

UnifiedReal *
real_inverse (UnifiedReal *x)
{
  if (real_definitely_zero (x)) {
    calc_fail (CALC_DIVIDE_BY_ZERO);
    return real_zero ();
  }
  /* 1/(r sqrt(k)) = (1/(r k)) sqrt(k) */
  int square = square_of (x->cr);
  if (square) {
    g_autoptr (BoundedRational) k = rational_new (square, 1);
    g_autoptr (BoundedRational) rk = rational_multiply (x->rat, k);
    g_autoptr (BoundedRational) inverse = rational_inverse (rk);
    if (inverse)
      return real_new (inverse, x->cr);
  }
  g_autoptr (BoundedRational) rat_inverse = rational_inverse (x->rat);
  g_autoptr (CReal) cr_inverse = creal_inverse (x->cr);
  return real_new (rat_inverse, cr_inverse);
}

UnifiedReal *
real_divide (UnifiedReal *x, UnifiedReal *y)
{
  if (x->cr == y->cr) {
    if (real_definitely_zero (y)) {
      calc_fail (CALC_DIVIDE_BY_ZERO);
      return real_zero ();
    }
    g_autoptr (BoundedRational) quotient = rational_divide (x->rat, y->rat);
    if (quotient)
      return real_from_rational (quotient);
  }
  g_autoptr (UnifiedReal) inverse = real_inverse (y);
  return real_multiply (x, inverse);
}

UnifiedReal *
real_sqrt (UnifiedReal *x)
{
  if (real_definitely_zero (x))
    return real_zero ();
  if (x->cr == named ()->one) {
    /* try r = q^2 k for the named sqrt(k): then sqrt(r) = q sqrt(k) */
    for (int divisor = 1; divisor < N_NAMED; divisor++) {
      if (!named ()->sqrts[divisor])
        continue;
      g_autoptr (BoundedRational) d = rational_new (divisor, 1);
      g_autoptr (BoundedRational) quotient = rational_divide (x->rat, d);
      g_autoptr (BoundedRational) root = rational_sqrt (quotient);
      if (calc_failed ())   /* sqrt of a negative number */
        return real_zero ();
      if (root)
        return real_new (root, named ()->sqrts[divisor]);
    }
  }
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) root = creal_sqrt (value);
  return real_from_cr (root);
}

/* --- trigonometry */

/* x as a multiple of pi/12, modulo 24, if it is one (getPiTwelfths); -1 if not */
static int
pi_twelfths (UnifiedReal *x)
{
  if (real_definitely_zero (x))
    return 0;
  if (x->cr != named ()->pi)
    return -1;
  g_autoptr (BoundedRational) twelve = rational_new (12, 1);
  g_autoptr (BoundedRational) quotient = rational_multiply (x->rat, twelve);
  mpz_t n;
  int result = -1;
  mpz_init (n);
  if (rational_as_integer (quotient, n))
    result = (int) mpz_fdiv_ui (n, 24);   /* BigInteger.mod: never negative */
  mpz_clear (n);
  return result;
}

/* sin(n pi/12) where that is a known simple number, else NULL (sinPiTwelfths) */
static UnifiedReal *
sin_pi_twelfths (int n)
{
  if (n >= 12) {
    g_autoptr (UnifiedReal) negative = sin_pi_twelfths (n - 12);
    return negative ? real_negate (negative) : NULL;
  }
  switch (n) {
  case 0:
    return real_zero ();
  case 2:   /* 30 degrees */
  case 10:
    return real_from_fraction (1, 2, named ()->one);
  case 3:   /* 45 degrees */
  case 9:
    return real_from_fraction (1, 2, named ()->sqrts[2]);
  case 4:   /* 60 degrees */
  case 8:
    return real_from_fraction (1, 2, named ()->sqrts[3]);
  case 6:
    return real_from_long (1);
  default:
    return NULL;
  }
}

UnifiedReal *
real_sin (UnifiedReal *x)
{
  int twelfths = pi_twelfths (x);
  if (twelfths >= 0) {
    UnifiedReal *result = sin_pi_twelfths (twelfths);
    if (result)
      return result;
  }
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) sin = creal_sin (value);
  return real_from_cr (sin);
}

UnifiedReal *
real_cos (UnifiedReal *x)
{
  int twelfths = pi_twelfths (x);
  if (twelfths >= 0) {
    int sin_arg = twelfths + 6;   /* cos(x) = sin(x + pi/2) (cosPiTwelfths) */
    if (sin_arg >= 24)
      sin_arg -= 24;
    UnifiedReal *result = sin_pi_twelfths (sin_arg);
    if (result)
      return result;
  }
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) cos = creal_cos (value);
  return real_from_cr (cos);
}

/* asin and acos are only defined on [-1, 1] (checkAsinDomain) */
static gboolean
check_asin_domain (UnifiedReal *x)
{
  g_autoptr (UnifiedReal) one = real_from_long (1);
  g_autoptr (UnifiedReal) minus_one = real_from_long (-1);
  if (is_comparable (x, one) && (real_compare (x, one) > 0 || real_compare (x, minus_one) < 0)) {
    calc_fail (CALC_DOMAIN_ERROR);   /* "inverse trig argument out of range" */
    return FALSE;
  }
  return TRUE;
}

/* asin(n/2) for n = -2..2 (asinHalves) */
static UnifiedReal *
asin_halves (int n)
{
  if (n < 0) {
    g_autoptr (UnifiedReal) positive = asin_halves (-n);
    return real_negate (positive);
  }
  switch (n) {
  case 0:
    return real_zero ();
  case 1:
    return real_from_fraction (1, 6, named ()->pi);
  default:
    return real_from_fraction (1, 2, named ()->pi);
  }
}

/* asinNonHalves: sqrt(2)/2 and sqrt(3)/2 are recognised */
static UnifiedReal *
asin_non_halves (UnifiedReal *x)
{
  g_autoptr (UnifiedReal) zero = real_zero ();
  if (real_compare_at (x, zero, -10) < 0) {
    g_autoptr (UnifiedReal) minus_x = real_negate (x);
    g_autoptr (UnifiedReal) positive = asin_non_halves (minus_x);
    return real_negate (positive);
  }
  g_autoptr (UnifiedReal) half_sqrt2 = real_from_fraction (1, 2, named ()->sqrts[2]);
  if (definitely_equals (x, half_sqrt2))
    return real_from_fraction (1, 4, named ()->pi);
  g_autoptr (UnifiedReal) half_sqrt3 = real_from_fraction (1, 2, named ()->sqrts[3]);
  if (definitely_equals (x, half_sqrt3))
    return real_from_fraction (1, 3, named ()->pi);
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) asin = creal_asin (value);
  return real_from_cr (asin);
}

UnifiedReal *
real_asin (UnifiedReal *x)
{
  if (!check_asin_domain (x))
    return real_zero ();
  g_autoptr (UnifiedReal) two = real_from_long (2);
  g_autoptr (UnifiedReal) doubled = real_multiply (x, two);
  mpz_t halves;
  mpz_init (halves);
  if (real_integer_value (doubled, halves)) {
    UnifiedReal *result = asin_halves ((int) mpz_get_si (halves));
    mpz_clear (halves);
    return result;
  }
  mpz_clear (halves);
  /* Java goes on with "if (mCrFactor == CR.ONE || mCrFactor != CR_SQRT2 || mCrFactor != CR_SQRT3)",
   * which is always true: asinNonHalves, always */
  return asin_non_halves (x);
}

UnifiedReal *
real_acos (UnifiedReal *x)
{
  g_autoptr (UnifiedReal) pi_over_2 = real_from_fraction (1, 2, named ()->pi);
  g_autoptr (UnifiedReal) asin = real_asin (x);
  return real_subtract (pi_over_2, asin);
}

UnifiedReal *
real_atan (UnifiedReal *x)
{
  g_autoptr (UnifiedReal) zero = real_zero ();
  if (real_compare_at (x, zero, -10) < 0) {
    g_autoptr (UnifiedReal) minus_x = real_negate (x);
    g_autoptr (UnifiedReal) positive = real_atan (minus_x);
    return real_negate (positive);
  }
  mpz_t as_integer;
  mpz_init (as_integer);
  if (real_integer_value (x, as_integer) && mpz_cmp_ui (as_integer, 1) <= 0) {
    gboolean is_one = mpz_cmp_ui (as_integer, 1) == 0;
    mpz_clear (as_integer);
    return is_one ? real_from_fraction (1, 4, named ()->pi) : real_zero ();
  }
  mpz_clear (as_integer);
  g_autoptr (UnifiedReal) third_sqrt3 = real_from_fraction (1, 3, named ()->sqrts[3]);
  if (definitely_equals (x, third_sqrt3))
    return real_from_fraction (1, 6, named ()->pi);
  g_autoptr (UnifiedReal) sqrt3 = real_from_cr (named ()->sqrts[3]);
  if (definitely_equals (x, sqrt3))
    return real_from_fraction (1, 3, named ()->pi);
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) atan = creal_atan (value);
  return real_from_cr (atan);
}

/* --- powers */

/* base^exp by squaring, exp >= 1 (recursivePow) */
static CReal *
recursive_pow (CReal *base, const mpz_t exp)
{
  if (mpz_cmp_ui (exp, 1) == 0)
    return creal_ref (base);
  mpz_t smaller;
  mpz_init (smaller);
  CReal *result;
  if (mpz_odd_p (exp)) {
    mpz_sub_ui (smaller, exp, 1);
    g_autoptr (CReal) power = recursive_pow (base, smaller);
    result = creal_multiply (base, power);
  } else {
    mpz_fdiv_q_2exp (smaller, exp, 1);
    g_autoptr (CReal) half = recursive_pow (base, smaller);
    result = calc_failed () ? creal_ref (half) : creal_multiply (half, half);
  }
  mpz_clear (smaller);
  return result;
}

/* x^exp through exp(exp ln x), the sign handled separately (expLnPow) */
static UnifiedReal *
exp_ln_pow (UnifiedReal *x, const mpz_t exp)
{
  int sign = real_signum_at (x, DEFAULT_COMPARE_TOLERANCE);
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) exp_cr = creal_from_mpz (exp);

  if (sign != 0) {
    g_autoptr (CReal) abs_value = sign > 0 ? creal_ref (value) : creal_negate (value);
    g_autoptr (CReal) ln = creal_ln (abs_value);
    g_autoptr (CReal) product = creal_multiply (ln, exp_cr);
    g_autoptr (CReal) result = creal_exp (product);
    if (sign < 0 && mpz_odd_p (exp)) {
      g_autoptr (CReal) negative = creal_negate (result);
      return real_from_cr (negative);
    }
    return real_from_cr (result);
  }
  /* |x| < 2^-1000: possibly 0, so no logarithm */
  if (mpz_sgn (exp) < 0) {
    mpz_t minus_exp;
    mpz_init (minus_exp);
    mpz_neg (minus_exp, exp);
    g_autoptr (CReal) power = recursive_pow (value, minus_exp);
    mpz_clear (minus_exp);
    g_autoptr (CReal) inverse = creal_inverse (power);
    return real_from_cr (inverse);
  }
  g_autoptr (CReal) power = recursive_pow (value, exp);
  return real_from_cr (power);
}

/* x^exp for an integer exp (pow(BigInteger)) */
static UnifiedReal *
pow_integer (UnifiedReal *x, const mpz_t exp)
{
  if (mpz_cmp_ui (exp, 1) == 0)
    return real_ref (x);
  if (mpz_sgn (exp) == 0)
    return real_from_long (1);

  gsize abs_exp_bits = mpz_sizeinbase (exp, 2);
  /* HARD_RECURSIVE_POW_LIMIT, 2^1000 */
  gboolean within_hard_limit = abs_exp_bits <= 1000 || (abs_exp_bits == 1001 && mpz_scan1 (exp, 0) == 1000);
  if (x->cr == named ()->one && within_hard_limit) {
    g_autoptr (BoundedRational) power = rational_pow_integer (x->rat, exp);
    if (power)
      return real_from_rational (power);
    if (calc_failed ())
      return real_zero ();
  }
  if (mpz_cmpabs_ui (exp, 1000) > 0)   /* RECURSIVE_POW_LIMIT */
    return exp_ln_pow (x, exp);

  /* (r sqrt(k))^n = r^n k^(n/2) [sqrt(k) if n is odd] */
  int square = square_of (x->cr);
  if (square) {
    mpz_t half_exp;
    mpz_init (half_exp);
    mpz_fdiv_q_2exp (half_exp, exp, 1);
    g_autoptr (BoundedRational) k = rational_new (square, 1);
    g_autoptr (BoundedRational) rat_power = rational_pow_integer (x->rat, exp);
    g_autoptr (BoundedRational) k_power = rational_pow_integer (k, half_exp);
    g_autoptr (BoundedRational) product = rational_multiply (rat_power, k_power);
    mpz_clear (half_exp);
    if (product)
      return mpz_odd_p (exp) ? real_new (product, x->cr) : real_from_rational (product);
  }
  return exp_ln_pow (x, exp);
}

UnifiedReal *
real_pow (UnifiedReal *x, UnifiedReal *exponent)
{
  if (x->cr == named ()->e) {
    if (rational_equals_long (x->rat, 1))
      return real_exp (exponent);
    /* (r e)^y = e^y r^y */
    g_autoptr (UnifiedReal) rat = real_from_rational (x->rat);
    g_autoptr (UnifiedReal) rat_part = real_pow (rat, exponent);
    g_autoptr (UnifiedReal) e_part = real_exp (exponent);
    return real_multiply (e_part, rat_part);
  }

  BoundedRational *exp_rational = real_rational_value (exponent);
  if (exp_rational) {
    mpz_t exp;
    mpz_init (exp);
    if (rational_as_integer (exp_rational, exp)) {
      UnifiedReal *result = pow_integer (x, exp);
      mpz_clear (exp);
      return result;
    }
    /* a half-integer exponent: the square root of an integer power */
    g_autoptr (BoundedRational) two = rational_new (2, 1);
    g_autoptr (BoundedRational) doubled = rational_multiply (two, exp_rational);
    if (rational_as_integer (doubled, exp)) {
      g_autoptr (UnifiedReal) power = pow_integer (x, exp);
      mpz_clear (exp);
      return real_sqrt (power);
    }
    mpz_clear (exp);
  }
  if (real_definitely_zero (x))
    return real_zero ();
  if (real_signum_at (x, DEFAULT_COMPARE_TOLERANCE) < 0) {
    calc_fail (CALC_DOMAIN_ERROR);   /* "Negative base for pow() with non-integer exponent" */
    return real_zero ();
  }
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) ln = creal_ln (value);
  g_autoptr (CReal) exp_value = real_cr_value (exponent);
  g_autoptr (CReal) product = creal_multiply (ln, exp_value);
  g_autoptr (CReal) result = creal_exp (product);
  return real_from_cr (result);
}

/* --- logarithms and exponentials */

/* n^16 for n <= 10 (pow16) */
static unsigned long
pow16 (int n)
{
  unsigned long result = n * n;
  result *= result;
  result *= result;
  result *= result;
  return result;
}

/* k if n = base^k exactly, else 0 (getIntLog) */
static long
int_log (const mpz_t number, int base)
{
  double as_double = mpz_get_d (number);
  double approx = log (as_double) / log (base);
  if (!isinf (as_double) && fabs (approx - rint (approx)) > 1.0e-6)
    return 0;

  long result = 0;
  mpz_t n;
  mpz_init_set (n, number);
  while (mpz_divisible_ui_p (n, base) && !calc_failed ()) {
    mpz_divexact_ui (n, n, base);
    ++result;
    unsigned long base16th = pow16 (base);
    while (mpz_divisible_ui_p (n, base16th)) {
      mpz_divexact_ui (n, n, base16th);
      result += 16;
    }
  }
  gboolean exact = mpz_cmp_ui (n, 1) == 0;
  mpz_clear (n);
  return exact ? result : 0;
}

UnifiedReal *
real_ln (UnifiedReal *x)
{
  if (x->cr == named ()->e) {
    /* ln(r e) = ln(r) + 1 */
    g_autoptr (UnifiedReal) rat = real_from_rational (x->rat);
    g_autoptr (UnifiedReal) ln_rat = real_ln (rat);
    g_autoptr (UnifiedReal) one = real_from_long (1);
    return real_add (ln_rat, one);
  }
  g_autoptr (UnifiedReal) zero = real_zero ();
  if (is_comparable (x, zero)) {
    if (real_signum (x) <= 0) {
      calc_fail (CALC_DOMAIN_ERROR);   /* "log(non-positive)" */
      return real_zero ();
    }
    g_autoptr (UnifiedReal) one = real_from_long (1);
    int compare1 = real_compare_at (x, one, DEFAULT_COMPARE_TOLERANCE);
    if (compare1 == 0) {
      if (definitely_equals (x, one))
        return real_zero ();
    } else if (compare1 < 0) {
      g_autoptr (UnifiedReal) inverse = real_inverse (x);
      g_autoptr (UnifiedReal) ln_inverse = real_ln (inverse);
      return real_negate (ln_inverse);
    }
    mpz_t integer;
    mpz_init (integer);
    if (rational_as_integer (x->rat, integer)) {
      if (x->cr == named ()->one) {
        /* ln(k^n) = n ln(k) for the named ln(k) */
        for (int i = 2; i < N_NAMED; i++) {
          if (!named ()->logs[i])
            continue;
          long log = int_log (integer, i);
          if (log != 0) {
            mpz_clear (integer);
            return real_from_fraction (log, 1, named ()->logs[i]);
          }
        }
      } else {
        /* ln(k^n sqrt(k)) = (n + 1/2) ln(k) */
        int square = square_of (x->cr);
        if (square && named ()->logs[square]) {
          long log = int_log (integer, square);
          if (log != 0) {
            mpz_clear (integer);
            return real_from_fraction (2 * log + 1, 2, named ()->logs[square]);
          }
        }
      }
    }
    mpz_clear (integer);
  }
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) ln = creal_ln (value);
  return real_from_cr (ln);
}

UnifiedReal *
real_exp (UnifiedReal *x)
{
  g_autoptr (UnifiedReal) zero = real_zero ();
  if (definitely_equals (x, zero))
    return real_from_long (1);
  g_autoptr (UnifiedReal) one = real_from_long (1);
  if (definitely_equals (x, one))
    return real_e ();

  /* e^(r ln k) = k^r, exact when r is an integer, a square root when it is a half */
  int k = exp_of (x->cr);
  if (k) {
    gboolean need_sqrt = FALSE;
    g_autoptr (BoundedRational) rat_exponent = rational_ref (x->rat);
    if (!rational_is_integer (rat_exponent)) {
      g_autoptr (BoundedRational) two = rational_new (2, 1);
      BoundedRational *doubled = rational_multiply (rat_exponent, two);
      rational_unref (rat_exponent);
      rat_exponent = doubled;
      need_sqrt = TRUE;
    }
    g_autoptr (BoundedRational) base = rational_new (k, 1);
    g_autoptr (BoundedRational) power = rational_pow (base, rat_exponent);
    if (power) {
      g_autoptr (UnifiedReal) result = real_from_rational (power);
      return need_sqrt ? real_sqrt (result) : g_steal_pointer (&result);
    }
  }
  g_autoptr (CReal) value = real_cr_value (x);
  g_autoptr (CReal) exp = creal_exp (value);
  return real_from_cr (exp);
}

/* n (n - step) (n - 2 step) ..., split in halves to multiply numbers of similar size (genFactorial) */
static void
gen_factorial (mpz_t out, long n, long step)
{
  if (n > 4 * step) {
    mpz_t prod2;
    gen_factorial (out, n, 2 * step);
    if (calc_failed ())
      return;
    mpz_init (prod2);
    gen_factorial (prod2, n - step, 2 * step);
    mpz_mul (out, out, prod2);
    mpz_clear (prod2);
    return;
  }
  if (n == 0) {
    mpz_set_ui (out, 1);
    return;
  }
  mpz_set_si (out, n);
  for (long i = n - step; i > 1; i -= step)
    mpz_mul_si (out, out, i);
}

UnifiedReal *
real_fact (UnifiedReal *x)
{
  mpz_t n;
  mpz_init (n);
  if (!real_integer_value (x, n)) {
    /* not known to be an integer: it must be one to within 2^-1000 */
    g_autoptr (CReal) value = real_cr_value (x);
    creal_get_appr (value, 0, n);
    g_autoptr (UnifiedReal) rounded = real_from_integer (n);
    if (!approx_equals (x, rounded, DEFAULT_COMPARE_TOLERANCE)) {
      calc_fail (CALC_DOMAIN_ERROR);   /* "Non-integral factorial argument" */
      mpz_clear (n);
      return real_zero ();
    }
  }
  if (mpz_sgn (n) < 0 || calc_bit_length (n) > 20) {
    calc_fail (CALC_DOMAIN_ERROR);   /* "Negative factorial argument", "Factorial argument too big" */
    mpz_clear (n);
    return real_zero ();
  }
  mpz_t factorial;
  mpz_init (factorial);
  gen_factorial (factorial, mpz_get_si (n), 1);
  UnifiedReal *result = real_from_integer (factorial);
  mpz_clears (n, factorial, NULL);
  return result;
}

/* --- for the display */

int
real_digits_required (UnifiedReal *x)
{
  if (x->cr == named ()->one || rational_signum (x->rat) == 0)
    return rational_digits_required (x->rat);
  return G_MAXINT;
}

int
real_leading_binary_zeroes (UnifiedReal *x)
{
  if (is_named (x->cr)) {
    int whole_bits = rational_whole_number_bits (x->rat);
    if (whole_bits == G_MININT)
      return G_MAXINT;
    if (whole_bits >= 3)
      return 0;
    return -whole_bits + 3;
  }
  return G_MAXINT;
}

gboolean
real_approx_whole_number_bits_greater_than (UnifiedReal *x, int bound)
{
  if (is_named (x->cr))
    return rational_whole_number_bits (x->rat) > bound;
  g_autoptr (CReal) value = real_cr_value (x);
  mpz_t appr;
  mpz_init (appr);
  creal_get_appr (value, bound - 2, appr);
  gboolean greater = calc_bit_length (appr) > 2;
  mpz_clear (appr);
  return greater;
}
