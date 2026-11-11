/* creal: see creal.h. Every algorithm here is com.hp.creals' (CR.java and its node classes), step for
 * step; the comments name the Java class each part comes from. */
#include "creal.h"
#include "calc-failure.h"

#include <limits.h>
#include <math.h>

typedef enum {
  NODE_INTEGER,         /* int_CR */
  NODE_ADD,             /* add_CR */
  NODE_SHIFTED,         /* shifted_CR */
  NODE_NEGATE,          /* neg_CR */
  NODE_SELECT,          /* select_CR */
  NODE_MULTIPLY,        /* mult_CR */
  NODE_INVERSE,         /* inv_CR */
  NODE_PRESCALED_EXP,   /* prescaled_exp_CR: e^x for |x| < 1/2 */
  NODE_PRESCALED_COS,   /* prescaled_cos_CR: cos x for |x| < 1 (a slow_CR) */
  NODE_INTEGRAL_ATAN,   /* integral_atan_CR: atan(1/n) (a slow_CR) */
  NODE_PRESCALED_LN,    /* prescaled_ln_CR: ln(1 + x) for |x| < 1/2 (a slow_CR) */
  NODE_PRESCALED_ASIN,  /* prescaled_asin_CR: asin x for |x| < 1/sqrt 2 (a slow_CR) */
  NODE_SQRT,            /* sqrt_CR */
} NodeKind;

struct _CReal {
  gint      ref_count;
  NodeKind  kind;
  CReal    *op1;        /* the operand(s); op1 alone for unary nodes */
  CReal    *op2;
  CReal    *selector;   /* NODE_SELECT */
  int       selector_sign;
  mpz_t     value;      /* NODE_INTEGER */
  int       count;      /* NODE_SHIFTED: the shift; NODE_INTEGRAL_ATAN: n */

  /* the best approximation so far: max_appr * 2^min_prec (CR.appr_valid, min_prec, max_appr) */
  gboolean  appr_valid;
  int       min_prec;
  mpz_t     max_appr;
};

static CReal *
node_new (NodeKind kind, CReal *op1, CReal *op2)
{
  CReal *x = g_new0 (CReal, 1);

  x->ref_count = 1;
  x->kind = kind;
  x->op1 = op1 ? creal_ref (op1) : NULL;
  x->op2 = op2 ? creal_ref (op2) : NULL;
  mpz_init (x->value);
  mpz_init (x->max_appr);
  return x;
}

CReal *
creal_ref (CReal *x)
{
  g_atomic_int_inc (&x->ref_count);
  return x;
}

void
creal_unref (CReal *x)
{
  if (!x || !g_atomic_int_dec_and_test (&x->ref_count))
    return;
  g_clear_pointer (&x->op1, creal_unref);
  g_clear_pointer (&x->op2, creal_unref);
  g_clear_pointer (&x->selector, creal_unref);
  mpz_clear (x->value);
  mpz_clear (x->max_appr);
  g_free (x);
}

gsize
calc_bit_length (const mpz_t n)
{
  if (mpz_sgn (n) == 0)
    return 0;
  if (mpz_sgn (n) > 0)
    return mpz_sizeinbase (n, 2);
  /* negative: bitLength(-n - 1), e.g. -1 -> 0, -4 -> 2 */
  mpz_t t;
  mpz_init (t);
  mpz_neg (t, n);
  mpz_sub_ui (t, t, 1);
  gsize bits = mpz_sgn (t) == 0 ? 0 : mpz_sizeinbase (t, 2);
  mpz_clear (t);
  return bits;
}

/* --- CR's static helpers */

/* precisions past +-2^28 bits are refused (CR.check_prec) */
static gboolean
check_prec (int n)
{
  int high = n >> 28;
  int high_shifted = n >> 29;

  if ((high ^ high_shifted) != 0) {
    calc_fail (CALC_PRECISION_OVERFLOW);
    return FALSE;
  }
  return TRUE;
}

/* ceil(log2(|n| + 1)) (CR.bound_log2) */
static int
bound_log2 (int n)
{
  return (int) ceil (log ((double) (ABS (n) + 1)) / log (2.0));
}

/* out = k * 2^n, truncated (CR.shift) */
static void
shift (mpz_t out, const mpz_t k, int n)
{
  if (n >= 0)
    mpz_mul_2exp (out, k, n);
  else
    mpz_fdiv_q_2exp (out, k, -n);   /* BigInteger.shiftRight rounds towards minus infinity */
}

/* out = k * 2^n, rounded to nearest (CR.scale) */
static void
scale (mpz_t out, const mpz_t k, int n)
{
  if (n >= 0) {
    mpz_mul_2exp (out, k, n);
  } else {
    shift (out, k, n + 1);
    mpz_add_ui (out, out, 1);
    mpz_fdiv_q_2exp (out, out, 1);
  }
}

/* 1 << n, n >= 0 */
static void
power_of_two (mpz_t out, int n)
{
  mpz_set_ui (out, 1);
  mpz_mul_2exp (out, out, n);
}

static void approximate (CReal *x, int p, mpz_t out);

static gboolean
is_slow (CReal *x)
{
  return x->kind == NODE_PRESCALED_COS || x->kind == NODE_INTEGRAL_ATAN || x->kind == NODE_PRESCALED_LN
      || x->kind == NODE_PRESCALED_ASIN;
}

/* CR.get_appr, and slow_CR.get_appr for the slow series: those are always evaluated to a precision of
 * -64 or a multiple of 32, so that small changes in the precision asked for reuse the cached value */
void
creal_get_appr (CReal *x, int p, mpz_t out)
{
  if (!check_prec (p) || calc_failed ()) {
    mpz_set_ui (out, 0);
    return;
  }
  if (x->appr_valid && p >= x->min_prec) {
    scale (out, x->max_appr, x->min_prec - p);
    return;
  }

  int eval_prec = p;
  if (is_slow (x))
    eval_prec = p >= -64 ? -64 : (p - 32 + 1) & ~(32 - 1);

  mpz_t result;
  mpz_init (result);
  approximate (x, eval_prec, result);
  if (calc_failed ()) {
    /* whatever was computed is not to be trusted, and must not be remembered */
    mpz_clear (result);
    mpz_set_ui (out, 0);
    return;
  }
  x->min_prec = eval_prec;
  mpz_set (x->max_appr, result);
  x->appr_valid = TRUE;
  scale (out, result, eval_prec - p);
  mpz_clear (result);
}

/* the position of the most significant bit of the cached approximation (CR.known_msd) */
static int
known_msd (CReal *x)
{
  /* bitLength of |max_appr|; GMP's sizeinbase says 1 for 0, BigInteger says 0 */
  int length = mpz_sgn (x->max_appr) == 0 ? 0 : (int) mpz_sizeinbase (x->max_appr, 2);

  return x->min_prec + length - 1;
}

/* the most significant bit, or INT_MIN if |x| may be below 2^n (CR.msd(int)) */
static int
msd_at (CReal *x, int n)
{
  if (!x->appr_valid || mpz_cmpabs_ui (x->max_appr, 1) <= 0) {
    mpz_t ignored;
    mpz_init (ignored);
    creal_get_appr (x, n - 1, ignored);
    mpz_clear (ignored);
    if (!x->appr_valid || mpz_cmpabs_ui (x->max_appr, 1) <= 0)
      return INT_MIN;
  }
  return known_msd (x);
}

/* msd_at, finding a non-zero number's msd at coarse precisions first (CR.iter_msd) */
static int
iter_msd (CReal *x, int n)
{
  for (int prec = 0; prec > n + 30; prec = (prec * 3) / 2 - 16) {
    int msd = msd_at (x, prec);
    if (msd != INT_MIN)
      return msd;
    if (!check_prec (prec) || calc_failed ())
      return INT_MIN;
  }
  return msd_at (x, n);
}

/* the msd of a number known to be non-zero; loops until failure for 0 (CR.msd()) */
static int
msd (CReal *x)
{
  return iter_msd (x, INT_MIN);
}

/* --- the nodes' approximate() */

static void
approximate_add (CReal *x, int p, mpz_t out)
{
  mpz_t a, b;

  mpz_inits (a, b, NULL);
  creal_get_appr (x->op1, p - 2, a);
  creal_get_appr (x->op2, p - 2, b);
  mpz_add (a, a, b);
  scale (out, a, -2);
  mpz_clears (a, b, NULL);
}

static void
approximate_select (CReal *x, int p, mpz_t out)
{
  if (x->selector_sign < 0) {
    creal_get_appr (x->op1, p, out);
    return;
  }
  if (x->selector_sign > 0) {
    creal_get_appr (x->op2, p, out);
    return;
  }

  mpz_t a1, a2, diff;
  mpz_inits (a1, a2, diff, NULL);
  creal_get_appr (x->op1, p - 1, a1);
  creal_get_appr (x->op2, p - 1, a2);
  mpz_sub (diff, a1, a2);
  if (mpz_cmpabs_ui (diff, 1) <= 0) {
    scale (out, a1, -1);
  } else if (creal_signum (x->selector) < 0) {
    /* op1 and op2 differ, so the selector is not 0, and its sign can be found */
    x->selector_sign = -1;
    scale (out, a1, -1);
  } else {
    x->selector_sign = 1;
    scale (out, a2, -1);
  }
  mpz_clears (a1, a2, diff, NULL);
}

static void
approximate_multiply (CReal *x, int p, mpz_t out)
{
  int half_prec = (p >> 1) - 1;
  int msd_op1 = msd_at (x->op1, half_prec);

  if (msd_op1 == INT_MIN) {
    int msd_op2 = msd_at (x->op2, half_prec);
    if (msd_op2 == INT_MIN) {
      /* both are small enough for the product to be 0 at this precision */
      mpz_set_ui (out, 0);
      return;
    }
    /* make op1 the larger one: swapped for good, as mult_CR does */
    CReal *tmp = x->op1;
    x->op1 = x->op2;
    x->op2 = tmp;
    msd_op1 = msd_op2;
  }

  int prec2 = p - msd_op1 - 3;
  mpz_t appr1, appr2;
  mpz_inits (appr1, appr2, NULL);
  creal_get_appr (x->op2, prec2, appr2);
  if (mpz_sgn (appr2) == 0) {
    mpz_set_ui (out, 0);
  } else {
    int msd_op2 = known_msd (x->op2);
    int prec1 = p - msd_op2 - 3;
    creal_get_appr (x->op1, prec1, appr1);
    mpz_mul (appr1, appr1, appr2);
    scale (out, appr1, prec1 + prec2 - p);
  }
  mpz_clears (appr1, appr2, NULL);
}

static void
approximate_inverse (CReal *x, int p, mpz_t out)
{
  int msd_op = msd (x->op1);
  if (msd_op == INT_MIN) {   /* failed (cancelled, or the operand is 0) */
    mpz_set_ui (out, 0);
    return;
  }
  int inv_msd = 1 - msd_op;
  int digits_needed = inv_msd - p + 3;
  int prec_needed = msd_op - digits_needed;
  int log_scale_factor = -p - prec_needed;

  if (log_scale_factor < 0) {
    mpz_set_ui (out, 0);
    return;
  }
  mpz_t dividend, divisor, abs_divisor;
  mpz_inits (dividend, divisor, abs_divisor, NULL);
  power_of_two (dividend, log_scale_factor);
  creal_get_appr (x->op1, prec_needed, divisor);
  mpz_abs (abs_divisor, divisor);
  if (mpz_sgn (abs_divisor) == 0) {
    mpz_set_ui (out, 0);   /* only after a failure */
  } else {
    mpz_t half;
    mpz_init (half);
    mpz_fdiv_q_2exp (half, abs_divisor, 1);
    mpz_add (dividend, dividend, half);
    mpz_tdiv_q (out, dividend, abs_divisor);
    if (mpz_sgn (divisor) < 0)
      mpz_neg (out, out);
    mpz_clear (half);
  }
  mpz_clears (dividend, divisor, abs_divisor, NULL);
}

/* e^x by its Taylor series, for |x| < 1/2 (prescaled_exp_CR) */
static void
approximate_prescaled_exp (CReal *x, int p, mpz_t out)
{
  if (p >= 1) {
    mpz_set_ui (out, 0);
    return;
  }
  int iterations_needed = -p / 2 + 2;   /* conservative estimate > 0 */
  int calc_precision = p - bound_log2 (2 * iterations_needed) - 4;
  int op_prec = p - 3;
  mpz_t op_appr, current_term, current_sum, max_trunc_error;
  mpz_inits (op_appr, current_term, current_sum, max_trunc_error, NULL);

  creal_get_appr (x->op1, op_prec, op_appr);
  power_of_two (current_term, -calc_precision);
  mpz_set (current_sum, current_term);
  power_of_two (max_trunc_error, p - 4 - calc_precision);
  for (int n = 1; mpz_cmpabs (current_term, max_trunc_error) >= 0 && !calc_failed (); n++) {
    mpz_mul (current_term, current_term, op_appr);
    scale (current_term, current_term, op_prec);
    mpz_tdiv_q_ui (current_term, current_term, n);
    mpz_add (current_sum, current_sum, current_term);
  }
  scale (out, current_sum, calc_precision - p);
  mpz_clears (op_appr, current_term, current_sum, max_trunc_error, NULL);
}

/* cos x by its Taylor series, for |x| < 1 (prescaled_cos_CR) */
static void
approximate_prescaled_cos (CReal *x, int p, mpz_t out)
{
  if (p >= 1) {
    mpz_set_ui (out, 0);
    return;
  }
  int iterations_needed = -p / 2 + 4;
  int calc_precision = p - bound_log2 (2 * iterations_needed) - 4;
  int op_prec = p - 2;
  mpz_t op_appr, current_term, current_sum, max_trunc_error, divisor;
  mpz_inits (op_appr, current_term, current_sum, max_trunc_error, divisor, NULL);

  creal_get_appr (x->op1, op_prec, op_appr);
  power_of_two (max_trunc_error, p - 4 - calc_precision);
  power_of_two (current_term, -calc_precision);
  mpz_set (current_sum, current_term);
  for (long n = 2; mpz_cmpabs (current_term, max_trunc_error) >= 0 && !calc_failed (); n += 2) {
    mpz_mul (current_term, current_term, op_appr);
    scale (current_term, current_term, op_prec);
    mpz_mul (current_term, current_term, op_appr);
    scale (current_term, current_term, op_prec);
    mpz_set_si (divisor, -n);
    mpz_mul_si (divisor, divisor, n - 1);
    mpz_tdiv_q (current_term, current_term, divisor);
    mpz_add (current_sum, current_sum, current_term);
  }
  scale (out, current_sum, calc_precision - p);
  mpz_clears (op_appr, current_term, current_sum, max_trunc_error, divisor, NULL);
}

/* atan(1/n) by its Taylor series (integral_atan_CR) */
static void
approximate_integral_atan (CReal *x, int p, mpz_t out)
{
  if (p >= 1) {
    mpz_set_ui (out, 0);
    return;
  }
  long op = x->count;
  int iterations_needed = -p / 2 + 2;
  int calc_precision = p - bound_log2 (2 * iterations_needed) - 2;
  mpz_t scaled_1, current_power, current_term, current_sum, max_trunc_error;
  mpz_inits (scaled_1, current_power, current_term, current_sum, max_trunc_error, NULL);

  power_of_two (scaled_1, -calc_precision);
  mpz_tdiv_q_ui (current_power, scaled_1, op);
  mpz_set (current_term, current_power);
  mpz_set (current_sum, current_power);
  power_of_two (max_trunc_error, p - 2 - calc_precision);
  int current_sign = 1;
  for (long n = 1; mpz_cmpabs (current_term, max_trunc_error) >= 0 && !calc_failed ();) {
    n += 2;
    mpz_tdiv_q_ui (current_power, current_power, op * op);
    current_sign = -current_sign;
    mpz_tdiv_q_ui (current_term, current_power, n);
    if (current_sign < 0)
      mpz_neg (current_term, current_term);
    mpz_add (current_sum, current_sum, current_term);
  }
  scale (out, current_sum, calc_precision - p);
  mpz_clears (scaled_1, current_power, current_term, current_sum, max_trunc_error, NULL);
}

/* ln(1 + x) by its Taylor series, for |x| < 1/2 (prescaled_ln_CR) */
static void
approximate_prescaled_ln (CReal *x, int p, mpz_t out)
{
  if (p >= 0) {
    mpz_set_ui (out, 0);
    return;
  }
  int iterations_needed = -p;
  int calc_precision = p - bound_log2 (2 * iterations_needed) - 4;
  int op_prec = p - 3;
  mpz_t op_appr, x_nth, current_term, current_sum, max_trunc_error;
  mpz_inits (op_appr, x_nth, current_term, current_sum, max_trunc_error, NULL);

  creal_get_appr (x->op1, op_prec, op_appr);
  scale (x_nth, op_appr, op_prec - calc_precision);
  mpz_set (current_term, x_nth);
  mpz_set (current_sum, current_term);
  power_of_two (max_trunc_error, p - 4 - calc_precision);
  int current_sign = 1;
  for (long n = 1; mpz_cmpabs (current_term, max_trunc_error) >= 0 && !calc_failed ();) {
    n += 1;
    current_sign = -current_sign;
    mpz_mul (x_nth, x_nth, op_appr);
    scale (x_nth, x_nth, op_prec);
    mpz_tdiv_q_ui (current_term, x_nth, n);
    if (current_sign < 0)
      mpz_neg (current_term, current_term);
    mpz_add (current_sum, current_sum, current_term);
  }
  scale (out, current_sum, calc_precision - p);
  mpz_clears (op_appr, x_nth, current_term, current_sum, max_trunc_error, NULL);
}

/* asin x by its Taylor series, for |x| < 1/sqrt 2 (prescaled_asin_CR) */
static void
approximate_prescaled_asin (CReal *x, int p, mpz_t out)
{
  if (p >= 2) {
    mpz_set_ui (out, 0);
    return;
  }
  int iterations_needed = -3 * p / 2 + 4;
  int calc_precision = p - bound_log2 (2 * iterations_needed) - 4;
  int op_prec = p - 3;   /* always <= -2 */
  mpz_t op_appr, max_trunc_error, current_term, current_sum, current_factor;
  mpz_inits (op_appr, max_trunc_error, current_term, current_sum, current_factor, NULL);

  creal_get_appr (x->op1, op_prec, op_appr);
  power_of_two (max_trunc_error, p - 4 - calc_precision);
  mpz_mul_2exp (current_term, op_appr, op_prec - calc_precision);
  mpz_set (current_sum, current_term);
  mpz_set (current_factor, current_term);
  for (long exp = 1; mpz_cmpabs (current_term, max_trunc_error) >= 0 && !calc_failed ();) {
    exp += 2;
    /* factor = factor * (exp - 2) * x^2 / (exp - 1), with 2 extra bits kept along the way */
    mpz_mul_si (current_factor, current_factor, exp - 2);
    mpz_mul (current_factor, current_factor, op_appr);
    scale (current_factor, current_factor, op_prec + 2);
    mpz_mul (current_factor, current_factor, op_appr);
    mpz_tdiv_q_ui (current_factor, current_factor, exp - 1);
    scale (current_factor, current_factor, op_prec - 2);
    mpz_tdiv_q_ui (current_term, current_factor, exp);
    mpz_add (current_sum, current_sum, current_term);
  }
  scale (out, current_sum, calc_precision - p);
  mpz_clears (op_appr, max_trunc_error, current_term, current_sum, current_factor, NULL);
}

/* sqrt x: by Newton's iteration from a coarser approximation of itself for many digits, from a
 * double's sqrt for 50 bits or fewer (sqrt_CR) */
static void
approximate_sqrt (CReal *x, int p, mpz_t out)
{
  const int fp_prec = 50;      /* conservative estimate of the bits a double sqrt gets right */
  const int fp_op_prec = 60;
  int max_op_prec_needed = 2 * p - 1;
  int msd_op = iter_msd (x->op1, max_op_prec_needed);

  if (msd_op <= max_op_prec_needed) {
    mpz_set_ui (out, 0);
    return;
  }
  int result_msd = msd_op / 2;   /* +- 1 */
  int result_digits = result_msd - p;   /* +- 2 */

  if (result_digits > fp_prec) {
    /* compute less precise approximation and use a Newton iteration */
    int appr_digits = result_digits / 2 + 6;   /* this should be conservative; 6 is enough */
    int appr_prec = result_msd - appr_digits;
    int prod_prec = 2 * appr_prec;
    mpz_t op_appr, last_appr, prod;
    mpz_inits (op_appr, last_appr, prod, NULL);
    creal_get_appr (x->op1, prod_prec, op_appr);
    creal_get_appr (x, appr_prec, last_appr);
    if (mpz_sgn (last_appr) == 0) {
      mpz_set_ui (out, 0);   /* only after a failure */
    } else {
      /* (last^2 + op) / (2 last), at precision p; the + 1 >> 1 halves and rounds */
      mpz_mul (prod, last_appr, last_appr);
      mpz_add (prod, prod, op_appr);
      scale (prod, prod, appr_prec - p);
      mpz_tdiv_q (out, prod, last_appr);
      mpz_add_ui (out, out, 1);
      mpz_fdiv_q_2exp (out, out, 1);
    }
    mpz_clears (op_appr, last_appr, prod, NULL);
    return;
  }

  /* use a double */
  int op_prec = (msd_op - fp_op_prec) & ~1;
  int working_prec = op_prec - fp_op_prec;
  mpz_t scaled_bi_appr;
  mpz_init (scaled_bi_appr);
  creal_get_appr (x->op1, op_prec, scaled_bi_appr);
  mpz_mul_2exp (scaled_bi_appr, scaled_bi_appr, fp_op_prec);
  double scaled_appr = mpz_get_d (scaled_bi_appr);
  if (scaled_appr < 0.0) {
    calc_fail (CALC_DOMAIN_ERROR);   /* "sqrt(negative)" */
    mpz_set_ui (out, 0);
  } else {
    mpz_t scaled_sqrt;
    mpz_init_set_d (scaled_sqrt, sqrt (scaled_appr));
    shift (out, scaled_sqrt, working_prec / 2 - p);
    mpz_clear (scaled_sqrt);
  }
  mpz_clear (scaled_bi_appr);
}

static void
approximate (CReal *x, int p, mpz_t out)
{
  switch (x->kind) {
  case NODE_INTEGER:
    scale (out, x->value, -p);
    break;
  case NODE_ADD:
    approximate_add (x, p, out);
    break;
  case NODE_SHIFTED:
    creal_get_appr (x->op1, p - x->count, out);
    break;
  case NODE_NEGATE:
    creal_get_appr (x->op1, p, out);
    mpz_neg (out, out);
    break;
  case NODE_SELECT:
    approximate_select (x, p, out);
    break;
  case NODE_MULTIPLY:
    approximate_multiply (x, p, out);
    break;
  case NODE_INVERSE:
    approximate_inverse (x, p, out);
    break;
  case NODE_PRESCALED_EXP:
    approximate_prescaled_exp (x, p, out);
    break;
  case NODE_PRESCALED_COS:
    approximate_prescaled_cos (x, p, out);
    break;
  case NODE_INTEGRAL_ATAN:
    approximate_integral_atan (x, p, out);
    break;
  case NODE_PRESCALED_LN:
    approximate_prescaled_ln (x, p, out);
    break;
  case NODE_PRESCALED_ASIN:
    approximate_prescaled_asin (x, p, out);
    break;
  case NODE_SQRT:
    approximate_sqrt (x, p, out);
    break;
  }
}

/* --- making numbers */

CReal *
creal_from_mpz (const mpz_t n)
{
  CReal *x = node_new (NODE_INTEGER, NULL, NULL);

  mpz_set (x->value, n);
  return x;
}

CReal *
creal_from_long (long n)
{
  CReal *x = node_new (NODE_INTEGER, NULL, NULL);

  mpz_set_si (x->value, n);
  return x;
}

CReal *
creal_one (void)
{
  static CReal *one;

  if (g_once_init_enter_pointer (&one))
    g_once_init_leave_pointer (&one, creal_from_long (1));
  return creal_ref (one);
}

static CReal *
atan_reciprocal (int n)
{
  CReal *x = node_new (NODE_INTEGRAL_ATAN, NULL, NULL);

  x->count = n;
  return x;
}

CReal *
creal_pi (void)
{
  static CReal *pi;

  if (g_once_init_enter_pointer (&pi)) {
    /* CR.atan_PI: 4 * (4 * atan(1/5) - atan(1/239)) */
    g_autoptr (CReal) four = creal_from_long (4);
    g_autoptr (CReal) atan5 = atan_reciprocal (5);
    g_autoptr (CReal) atan239 = atan_reciprocal (239);
    g_autoptr (CReal) four_atan5 = creal_multiply (four, atan5);
    g_autoptr (CReal) difference = creal_subtract (four_atan5, atan239);
    g_once_init_leave_pointer (&pi, creal_multiply (four, difference));
  }
  return creal_ref (pi);
}

static CReal *
half_pi (void)
{
  static CReal *half;

  if (g_once_init_enter_pointer (&half)) {
    g_autoptr (CReal) pi = creal_pi ();
    g_once_init_leave_pointer (&half, creal_shift_right (pi, 1));
  }
  return creal_ref (half);
}

/* --- arithmetic */

CReal *
creal_add (CReal *x, CReal *y)
{
  return node_new (NODE_ADD, x, y);
}

CReal *
creal_negate (CReal *x)
{
  return node_new (NODE_NEGATE, x, NULL);
}

CReal *
creal_subtract (CReal *x, CReal *y)
{
  g_autoptr (CReal) minus_y = creal_negate (y);
  return creal_add (x, minus_y);
}

CReal *
creal_multiply (CReal *x, CReal *y)
{
  return node_new (NODE_MULTIPLY, x, y);
}

CReal *
creal_inverse (CReal *x)
{
  return node_new (NODE_INVERSE, x, NULL);
}

CReal *
creal_divide (CReal *x, CReal *y)
{
  g_autoptr (CReal) inverse = creal_inverse (y);
  return creal_multiply (x, inverse);
}

CReal *
creal_shift_left (CReal *x, int n)
{
  CReal *shifted;

  check_prec (n);
  shifted = node_new (NODE_SHIFTED, x, NULL);
  shifted->count = n;
  return shifted;
}

CReal *
creal_shift_right (CReal *x, int n)
{
  CReal *shifted;

  check_prec (n);
  shifted = node_new (NODE_SHIFTED, x, NULL);
  shifted->count = -n;
  return shifted;
}

CReal *
creal_select (CReal *selector, CReal *x, CReal *y)
{
  CReal *s = node_new (NODE_SELECT, x, y);
  mpz_t appr;

  s->selector = creal_ref (selector);
  /* select_CR's constructor looks at the selector once, coarsely */
  mpz_init (appr);
  creal_get_appr (selector, -20, appr);
  s->selector_sign = mpz_sgn (appr);
  mpz_clear (appr);
  return s;
}

CReal *
creal_abs (CReal *x)
{
  g_autoptr (CReal) minus_x = creal_negate (x);
  return creal_select (x, minus_x, x);
}

/* --- functions: these look at a rough approximation of x to bring it into the range their series
 * converges in */

CReal *
creal_sqrt (CReal *x)
{
  return node_new (NODE_SQRT, x, NULL);
}

/* CR.exp: e^x = (e^(x/2))^2 until |x| <= 2 (at precision 2^-10) */
CReal *
creal_exp (CReal *x)
{
  mpz_t rough_appr;
  CReal *result;

  mpz_init (rough_appr);
  creal_get_appr (x, -10, rough_appr);
  if (mpz_cmp_si (rough_appr, 2) > 0 || mpz_cmp_si (rough_appr, -2) < 0) {
    g_autoptr (CReal) half = creal_shift_right (x, 1);
    g_autoptr (CReal) square_root = creal_exp (half);
    result = creal_multiply (square_root, square_root);
  } else {
    result = node_new (NODE_PRESCALED_EXP, x, NULL);
  }
  mpz_clear (rough_appr);
  return result;
}

/* CR.cos: subtract multiples of pi, then halve the angle until |x| < 1 */
CReal *
creal_cos (CReal *x)
{
  g_autoptr (CReal) pi = creal_pi ();
  g_autoptr (CReal) over_pi = creal_divide (x, pi);
  mpz_t halfpi_multiples, rough;
  CReal *result;

  mpz_inits (halfpi_multiples, rough, NULL);
  creal_get_appr (over_pi, -1, halfpi_multiples);
  if (mpz_cmpabs_ui (halfpi_multiples, 2) >= 0) {
    mpz_t pi_multiples;
    mpz_init (pi_multiples);
    scale (pi_multiples, halfpi_multiples, -1);
    g_autoptr (CReal) multiple = creal_from_mpz (pi_multiples);
    g_autoptr (CReal) adjustment = creal_multiply (pi, multiple);
    g_autoptr (CReal) reduced = creal_subtract (x, adjustment);
    g_autoptr (CReal) cos_reduced = creal_cos (reduced);
    result = mpz_odd_p (pi_multiples) ? creal_negate (cos_reduced) : creal_ref (cos_reduced);
    mpz_clear (pi_multiples);
  } else {
    creal_get_appr (x, -1, rough);
    if (mpz_cmpabs_ui (rough, 2) >= 0) {
      /* cos(2x) = 2 cos(x)^2 - 1 */
      g_autoptr (CReal) half = creal_shift_right (x, 1);
      g_autoptr (CReal) cos_half = creal_cos (half);
      g_autoptr (CReal) square = creal_multiply (cos_half, cos_half);
      g_autoptr (CReal) twice = creal_shift_left (square, 1);
      g_autoptr (CReal) one = creal_one ();
      result = creal_subtract (twice, one);
    } else {
      result = node_new (NODE_PRESCALED_COS, x, NULL);
    }
  }
  mpz_clears (halfpi_multiples, rough, NULL);
  return result;
}

/* CR.sin: cos(pi/2 - x) */
CReal *
creal_sin (CReal *x)
{
  g_autoptr (CReal) half = half_pi ();
  g_autoptr (CReal) complement = creal_subtract (half, x);
  return creal_cos (complement);
}

/* CR.asin: the series near 0, acos(sqrt(1 - x^2)) near +-1 */
CReal *
creal_asin (CReal *x)
{
  mpz_t rough_appr;
  CReal *result;

  mpz_init (rough_appr);
  creal_get_appr (x, -10, rough_appr);
  if (mpz_cmp_si (rough_appr, 750) > 0) {   /* 1/sqrt(2) + a bit */
    g_autoptr (CReal) one = creal_one ();
    g_autoptr (CReal) square = creal_multiply (x, x);
    g_autoptr (CReal) difference = creal_subtract (one, square);
    g_autoptr (CReal) new_arg = creal_sqrt (difference);
    result = creal_acos (new_arg);
  } else if (mpz_cmp_si (rough_appr, -750) < 0) {
    g_autoptr (CReal) minus_x = creal_negate (x);
    g_autoptr (CReal) asin_minus_x = creal_asin (minus_x);
    result = creal_negate (asin_minus_x);
  } else {
    result = node_new (NODE_PRESCALED_ASIN, x, NULL);
  }
  mpz_clear (rough_appr);
  return result;
}

CReal *
creal_acos (CReal *x)
{
  g_autoptr (CReal) half = half_pi ();
  g_autoptr (CReal) asin_x = creal_asin (x);
  return creal_subtract (half, asin_x);
}

/* ln(1 + (x - 1)) by the series, for x near 1 (CR.simple_ln) */
static CReal *
simple_ln (CReal *x)
{
  g_autoptr (CReal) one = creal_one ();
  g_autoptr (CReal) x_minus_1 = creal_subtract (x, one);
  return node_new (NODE_PRESCALED_LN, x_minus_1, NULL);
}

/* CR.ln2: 7 ln(10/9) - 2 ln(25/24) + 3 ln(81/80) */
static CReal *
ln2 (void)
{
  static CReal *value;

  if (g_once_init_enter_pointer (&value)) {
    const int factors[3] = { 7, -2, 3 };
    const int nums[3] = { 10, 25, 81 }, dens[3] = { 9, 24, 80 };
    CReal *sum = NULL;
    for (int i = 0; i < 3; i++) {
      g_autoptr (CReal) num = creal_from_long (nums[i]);
      g_autoptr (CReal) den = creal_from_long (dens[i]);
      g_autoptr (CReal) ratio = creal_divide (num, den);
      g_autoptr (CReal) ln_ratio = simple_ln (ratio);
      g_autoptr (CReal) factor = creal_from_long (ABS (factors[i]));
      g_autoptr (CReal) term = creal_multiply (factor, ln_ratio);
      if (!sum) {
        sum = creal_ref (term);
      } else {
        CReal *next = factors[i] < 0 ? creal_subtract (sum, term) : creal_add (sum, term);
        creal_unref (sum);
        sum = next;
      }
    }
    g_once_init_leave_pointer (&value, sum);
  }
  return creal_ref (value);
}

/* CR.ln: bring x into [1/2, 3/2] by inverting, taking square roots, or dividing by powers of 2 */
CReal *
creal_ln (CReal *x)
{
  const int low_prec = -4;
  mpz_t rough_appr;
  CReal *result;

  mpz_init (rough_appr);
  creal_get_appr (x, low_prec, rough_appr);   /* in sixteenths */
  if (calc_failed ()) {
    /* cancelled or timed out: rough_appr is 0 then, which reads as x <= 1/2: inverting and recursing on would
     * never end (the stack ran out) */
    result = creal_from_long (0);
  } else if (mpz_sgn (rough_appr) < 0) {
    calc_fail (CALC_DOMAIN_ERROR);   /* "ln(negative)" */
    result = creal_from_long (0);
  } else if (mpz_cmp_ui (rough_appr, 8) <= 0) {   /* low_ln_limit: x <= 1/2 */
    g_autoptr (CReal) inverse = creal_inverse (x);
    g_autoptr (CReal) ln_inverse = creal_ln (inverse);
    result = creal_negate (ln_inverse);
  } else if (mpz_cmp_ui (rough_appr, 24) >= 0) {   /* high_ln_limit: x >= 3/2 */
    if (mpz_cmp_ui (rough_appr, 64) <= 0) {        /* scaled_4: x <= 4 */
      g_autoptr (CReal) root = creal_sqrt (x);
      g_autoptr (CReal) quarter_root = creal_sqrt (root);
      g_autoptr (CReal) quarter = creal_ln (quarter_root);
      result = creal_shift_left (quarter, 2);
    } else {
      int extra_bits = (int) mpz_sizeinbase (rough_appr, 2) - 3;
      g_autoptr (CReal) scaled = creal_shift_right (x, extra_bits);
      g_autoptr (CReal) scaled_result = creal_ln (scaled);
      g_autoptr (CReal) bits = creal_from_long (extra_bits);
      g_autoptr (CReal) log2 = ln2 ();
      g_autoptr (CReal) correction = creal_multiply (bits, log2);
      result = creal_add (scaled_result, correction);
    }
  } else {
    result = simple_ln (x);
  }
  mpz_clear (rough_appr);
  return result;
}

/* UnaryCRFunction.atanFunction: asin(x / sqrt(1 + x^2)), with the sign of x */
CReal *
creal_atan (CReal *x)
{
  g_autoptr (CReal) one = creal_one ();
  g_autoptr (CReal) x2 = creal_multiply (x, x);
  g_autoptr (CReal) one_plus_x2 = creal_add (one, x2);
  g_autoptr (CReal) ratio = creal_divide (x2, one_plus_x2);
  g_autoptr (CReal) abs_sin_atan = creal_sqrt (ratio);
  g_autoptr (CReal) minus = creal_negate (abs_sin_atan);
  g_autoptr (CReal) sin_atan = creal_select (x, minus, abs_sin_atan);
  return creal_asin (sin_atan);
}

/* --- comparisons */

int
creal_signum_at (CReal *x, int a)
{
  mpz_t appr;
  int sign;

  if (x->appr_valid && mpz_sgn (x->max_appr) != 0)
    return mpz_sgn (x->max_appr);
  mpz_init (appr);
  creal_get_appr (x, a - 1, appr);
  sign = mpz_sgn (appr);
  mpz_clear (appr);
  return sign;
}

int
creal_signum (CReal *x)
{
  for (int a = -20;; a *= 2) {
    if (!check_prec (a) || calc_failed ())
      return 0;
    int result = creal_signum_at (x, a);
    if (result != 0)
      return result;
  }
}

int
creal_compare_at (CReal *x, CReal *y, int a)
{
  int needed_prec = a - 1;
  mpz_t x_appr, y_appr;
  int result = 0;

  mpz_inits (x_appr, y_appr, NULL);
  creal_get_appr (x, needed_prec, x_appr);
  creal_get_appr (y, needed_prec, y_appr);
  mpz_sub (x_appr, x_appr, y_appr);
  if (mpz_cmp_si (x_appr, 1) > 0)
    result = 1;
  else if (mpz_cmp_si (x_appr, -1) < 0)
    result = -1;
  mpz_clears (x_appr, y_appr, NULL);
  return result;
}

int
creal_compare (CReal *x, CReal *y)
{
  for (int a = -20;; a *= 2) {
    if (!check_prec (a) || calc_failed ())
      return 0;
    int result = creal_compare_at (x, y, a);
    if (result != 0)
      return result;
  }
}
