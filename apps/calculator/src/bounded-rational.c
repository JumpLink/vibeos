/* bounded-rational: see bounded-rational.h */
#include "bounded-rational.h"
#include "calc-failure.h"

#include <string.h>

/* BoundedRational.MAX_SIZE: the most bits a fraction (numerator + denominator) may have */
#define MAX_SIZE 10000

/* Not in Java: the most bits an INTEGER power may have. Java let integers grow without a limit,
 * bounded only by memory and the 2 s timeout; GMP gives up the process when memory runs out, so a
 * power like 9^9^9 (a billion bits) is refused here (NULL, which ends as a timeout). 8 Mbit is ten
 * times more than any result the calculator would print (Evaluator.getMaxResultBits, 700000). */
#define MAX_INTEGER_POWER_BITS (8 << 20)

struct _BoundedRational {
  gint  ref_count;
  mpq_t value;
};

BoundedRational *
rational_ref (BoundedRational *r)
{
  g_atomic_int_inc (&r->ref_count);
  return r;
}

void
rational_unref (BoundedRational *r)
{
  if (!r || !g_atomic_int_dec_and_test (&r->ref_count))
    return;
  mpq_clear (r->value);
  g_free (r);
}

static BoundedRational *
rational_alloc (void)
{
  BoundedRational *r = g_new0 (BoundedRational, 1);

  r->ref_count = 1;
  mpq_init (r->value);
  return r;
}

/* the fraction is too big to keep (tooBig; integers never are) */
static gboolean
too_big (const mpq_t q)
{
  if (mpz_cmp_ui (mpq_denref (q), 1) == 0)
    return FALSE;
  return mpz_sizeinbase (mpq_numref (q), 2) + mpz_sizeinbase (mpq_denref (q), 2) > MAX_SIZE;
}

/* wraps q (canonical), or NULL if it is too big (maybeReduce) */
static BoundedRational *
rational_take (mpq_t q)
{
  if (too_big (q)) {
    mpq_clear (q);
    return NULL;
  }
  BoundedRational *r = g_new0 (BoundedRational, 1);
  r->ref_count = 1;
  memcpy (r->value, q, sizeof (mpq_t));   /* takes over q's limbs */
  return r;
}

BoundedRational *
rational_new (long num, long den)
{
  BoundedRational *r = rational_alloc ();

  mpz_set_si (mpq_numref (r->value), num);
  mpz_set_si (mpq_denref (r->value), den);
  mpq_canonicalize (r->value);
  return r;
}

BoundedRational *
rational_new_integer (const mpz_t n)
{
  BoundedRational *r = rational_alloc ();

  mpq_set_z (r->value, n);
  return r;
}

BoundedRational *
rational_new_fraction (const mpz_t num, const mpz_t den)
{
  mpq_t q;

  mpq_init (q);
  mpz_set (mpq_numref (q), num);
  mpz_set (mpq_denref (q), den);
  mpq_canonicalize (q);
  return rational_take (q);
}

const __mpz_struct *
rational_num (BoundedRational *r)
{
  return mpq_numref (r->value);
}

const __mpz_struct *
rational_den (BoundedRational *r)
{
  return mpq_denref (r->value);
}

int
rational_signum (BoundedRational *r)
{
  return mpq_sgn (r->value);
}

int
rational_compare (BoundedRational *a, BoundedRational *b)
{
  int c = mpq_cmp (a->value, b->value);

  return (c > 0) - (c < 0);
}

gboolean
rational_equals_long (BoundedRational *r, long n)
{
  return r && mpq_cmp_si (r->value, n, 1) == 0;
}

gboolean
rational_is_integer (BoundedRational *r)
{
  return r && mpz_cmp_ui (mpq_denref (r->value), 1) == 0;
}

gboolean
rational_as_integer (BoundedRational *r, mpz_t out)
{
  if (!rational_is_integer (r))
    return FALSE;
  mpz_set (out, mpq_numref (r->value));
  return TRUE;
}

int
rational_whole_number_bits (BoundedRational *r)
{
  if (mpq_sgn (r->value) == 0)
    return G_MININT;
  return (int) calc_bit_length (mpq_numref (r->value)) - (int) calc_bit_length (mpq_denref (r->value));
}

char *
rational_to_nice_string (BoundedRational *r)
{
  g_autofree char *num = mpz_get_str (NULL, 10, mpq_numref (r->value));

  if (mpz_cmp_ui (mpq_denref (r->value), 1) == 0)
    return g_steal_pointer (&num);
  g_autofree char *den = mpz_get_str (NULL, 10, mpq_denref (r->value));
  return g_strdup_printf ("%s/%s", num, den);
}

char *
rational_to_string_truncated (BoundedRational *r, int n)
{
  mpz_t scaled;
  GString *result = g_string_new (NULL);

  /* |num| * 10^n / |den|, truncated */
  mpz_init (scaled);
  mpz_ui_pow_ui (scaled, 10, n);
  mpz_mul (scaled, scaled, mpq_numref (r->value));
  mpz_abs (scaled, scaled);
  mpz_tdiv_q (scaled, scaled, mpq_denref (r->value));
  g_autofree char *digits = mpz_get_str (NULL, 10, scaled);
  mpz_clear (scaled);

  if (mpq_sgn (r->value) < 0)
    g_string_append_c (result, '-');
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

CReal *
rational_cr_value (BoundedRational *r)
{
  g_autoptr (CReal) num = creal_from_mpz (mpq_numref (r->value));

  if (mpz_cmp_ui (mpq_denref (r->value), 1) == 0)
    return g_steal_pointer (&num);
  g_autoptr (CReal) den = creal_from_mpz (mpq_denref (r->value));
  return creal_divide (num, den);
}

BoundedRational *
rational_add (BoundedRational *a, BoundedRational *b)
{
  mpq_t q;

  if (!a || !b)
    return NULL;
  mpq_init (q);
  mpq_add (q, a->value, b->value);
  return rational_take (q);
}

BoundedRational *
rational_negate (BoundedRational *r)
{
  if (!r)
    return NULL;
  BoundedRational *result = rational_alloc ();
  mpq_neg (result->value, r->value);
  return result;
}

BoundedRational *
rational_subtract (BoundedRational *a, BoundedRational *b)
{
  g_autoptr (BoundedRational) minus_b = rational_negate (b);
  return rational_add (a, minus_b);
}

BoundedRational *
rational_multiply (BoundedRational *a, BoundedRational *b)
{
  mpq_t q;

  if (!a || !b)
    return NULL;
  mpq_init (q);
  mpq_mul (q, a->value, b->value);
  return rational_take (q);
}

BoundedRational *
rational_inverse (BoundedRational *r)
{
  if (!r)
    return NULL;
  if (mpq_sgn (r->value) == 0) {
    calc_fail (CALC_DIVIDE_BY_ZERO);
    return NULL;
  }
  BoundedRational *result = rational_alloc ();
  mpq_inv (result->value, r->value);
  return result;
}

BoundedRational *
rational_divide (BoundedRational *a, BoundedRational *b)
{
  g_autoptr (BoundedRational) inverse = rational_inverse (b);
  return rational_multiply (a, inverse);
}

BoundedRational *
rational_sqrt (BoundedRational *r)
{
  if (!r)
    return NULL;
  if (mpq_sgn (r->value) < 0) {
    calc_fail (CALC_DOMAIN_ERROR);   /* "sqrt(negative)" */
    return NULL;
  }
  /* Java rounded a double's sqrt and squared it back, so it found exact roots of numbers up to 2^106
   * or so; mpz_perfect_square_p finds them all, which changes no result: a root it finds and Java
   * missed is found again by UnifiedReal.sqrt's sqrt(k) factors or not at all. */
  if (!mpz_perfect_square_p (mpq_numref (r->value)) || !mpz_perfect_square_p (mpq_denref (r->value)))
    return NULL;
  BoundedRational *result = rational_alloc ();
  mpz_sqrt (mpq_numref (result->value), mpq_numref (r->value));
  mpz_sqrt (mpq_denref (result->value), mpq_denref (r->value));
  return result;
}

/* r^exp for exp >= 1, by squaring; NULL when a step gets too big (rawPow) */
static BoundedRational *
raw_pow (BoundedRational *r, const mpz_t exp)
{
  if (mpz_cmp_ui (exp, 1) == 0)
    return rational_ref (r);

  mpz_t smaller;
  mpz_init (smaller);
  if (mpz_odd_p (exp)) {
    mpz_sub_ui (smaller, exp, 1);
    g_autoptr (BoundedRational) power = raw_pow (r, smaller);
    mpz_clear (smaller);
    return rational_multiply (power, r);
  }
  mpz_fdiv_q_2exp (smaller, exp, 1);
  g_autoptr (BoundedRational) half = raw_pow (r, smaller);
  mpz_clear (smaller);
  if (!half || calc_failed ())
    return NULL;
  if (mpz_sizeinbase (mpq_numref (half->value), 2) > MAX_INTEGER_POWER_BITS / 2)
    return NULL;
  return rational_multiply (half, half);
}

BoundedRational *
rational_pow_integer (BoundedRational *r, const mpz_t exp)
{
  if (!r)
    return NULL;
  if (mpz_sgn (exp) == 0)
    return rational_new (1, 1);
  if (mpz_cmp_ui (exp, 1) == 0)
    return rational_ref (r);
  if (rational_equals_long (r, 0) || rational_equals_long (r, 1))
    return rational_ref (r);
  if (rational_equals_long (r, -1))
    return rational_new (mpz_odd_p (exp) ? -1 : 1, 1);
  if (mpz_sizeinbase (exp, 2) > 1000)
    return NULL;
  if (mpz_sgn (exp) < 0) {
    mpz_t minus_exp;
    mpz_init (minus_exp);
    mpz_neg (minus_exp, exp);
    g_autoptr (BoundedRational) inverse = rational_inverse (r);
    BoundedRational *result = inverse ? raw_pow (inverse, minus_exp) : NULL;
    mpz_clear (minus_exp);
    return result;
  }
  return raw_pow (r, exp);
}

BoundedRational *
rational_pow (BoundedRational *base, BoundedRational *exp)
{
  if (!base || !exp || !rational_is_integer (exp))
    return NULL;
  return rational_pow_integer (base, mpq_numref (exp->value));
}

int
rational_digits_required (BoundedRational *r)
{
  if (!r)
    return G_MAXINT;
  if (mpz_cmp_ui (mpq_denref (r->value), 1) == 0)
    return 0;
  if (mpz_sizeinbase (mpq_denref (r->value), 2) > MAX_SIZE)
    return G_MAXINT;

  /* exact in decimal only if the denominator is 2^a 5^b; then max(a, b) digits are needed */
  mpz_t den, five;
  mpz_init_set (den, mpq_denref (r->value));
  mpz_init_set_ui (five, 5);
  int twos = mpz_scan1 (den, 0);
  mpz_fdiv_q_2exp (den, den, twos);
  int fives = mpz_remove (den, den, five);
  gboolean exact = mpz_cmp_ui (den, 1) == 0;
  mpz_clears (den, five, NULL);
  return exact ? MAX (twos, fives) : G_MAXINT;
}
