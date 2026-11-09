/* test-creal: the constructive reals give the right digits, as far as asked */
#include "../src/creal.h"
#include "../src/calc-failure.h"

#include <string.h>

/* the first `digits` decimals of x, truncated: floor(x * 10^digits) via a 10-bit finer approximation */
static char *
digits_of (CReal *x, int digits)
{
  mpz_t ten_pow, appr;
  mpz_inits (ten_pow, appr, NULL);
  mpz_ui_pow_ui (ten_pow, 10, digits);
  g_autoptr (CReal) scale = creal_from_mpz (ten_pow);
  g_autoptr (CReal) scaled = creal_multiply (scale, x);
  creal_get_appr (scaled, -10, appr);
  mpz_fdiv_q_2exp (appr, appr, 10);
  char *s = mpz_get_str (NULL, 10, appr);
  mpz_clears (ten_pow, appr, NULL);
  return s;
}

static void
check_digits (CReal *x, int digits, const char *expected)
{
  g_autofree char *s = digits_of (x, digits);
  g_assert_cmpint (calc_failure (), ==, CALC_OK);
  g_assert_cmpstr (s, ==, expected);
}

static void
test_pi (void)
{
  g_autoptr (CReal) pi = creal_pi ();
  check_digits (pi, 50, "314159265358979323846264338327950288419716939937510");
  /* far out: the 1000th decimal of pi is 9 (...2164201989) */
  g_autofree char *s = digits_of (pi, 1000);
  g_assert_cmpstr (s + strlen (s) - 8, ==, "64201989");
}

static void
test_functions (void)
{
  g_autoptr (CReal) one = creal_one ();
  g_autoptr (CReal) two = creal_from_long (2);
  g_autoptr (CReal) e = creal_exp (one);
  check_digits (e, 40, "27182818284590452353602874713526624977572");
  g_autoptr (CReal) sqrt2 = creal_sqrt (two);
  check_digits (sqrt2, 60, "1414213562373095048801688724209698078569671875376948073176679");
  g_autoptr (CReal) ln2 = creal_ln (two);
  check_digits (ln2, 40, "6931471805599453094172321214581765680755");
  g_autoptr (CReal) ten = creal_from_long (10);
  g_autoptr (CReal) ln10 = creal_ln (ten);
  check_digits (ln10, 30, "2302585092994045684017991454684");
  g_autoptr (CReal) sin1 = creal_sin (one);
  check_digits (sin1, 30, "841470984807896506652502321630");
  g_autoptr (CReal) cos1 = creal_cos (one);
  check_digits (cos1, 30, "540302305868139717400936607442");
  g_autoptr (CReal) atan1 = creal_atan (one);   /* pi/4 */
  check_digits (atan1, 30, "785398163397448309615660845819");
  g_autoptr (CReal) half = creal_shift_right (one, 1);
  g_autoptr (CReal) asin_half = creal_asin (half);   /* pi/6 */
  check_digits (asin_half, 30, "523598775598298873077107230546");
  g_autoptr (CReal) hundred = creal_from_long (100);
  g_autoptr (CReal) e100 = creal_exp (hundred);
  check_digits (e100, 5, "2688117141816135448412625551580013587361111877374");
  g_autoptr (CReal) zero = creal_from_long (0);
  g_autoptr (CReal) acos0 = creal_acos (zero);
  check_digits (acos0, 20, "157079632679489661923");
}

static void
test_comparisons (void)
{
  g_autoptr (CReal) pi = creal_pi ();
  g_autoptr (CReal) three = creal_from_long (3);
  g_assert_cmpint (creal_compare (pi, three), ==, 1);
  g_assert_cmpint (creal_compare (three, pi), ==, -1);
  g_autoptr (CReal) diff = creal_subtract (pi, pi);
  g_assert_cmpint (creal_compare_at (pi, pi, -100), ==, 0);
  g_assert_cmpint (creal_signum_at (diff, -100), ==, 0);
  g_autoptr (CReal) minus = creal_negate (three);
  g_autoptr (CReal) abs = creal_abs (minus);
  check_digits (abs, 3, "3000");
}

static void
test_failures (void)
{
  g_autoptr (CReal) minus_one = creal_from_long (-1);
  g_autoptr (CReal) root = creal_sqrt (minus_one);
  mpz_t appr;
  mpz_init (appr);
  creal_get_appr (root, -10, appr);
  g_assert_cmpint (calc_failure (), ==, CALC_DOMAIN_ERROR);
  calc_clear_failure ();

  g_autoptr (CReal) ln = creal_ln (minus_one);
  g_assert_cmpint (calc_failure (), ==, CALC_DOMAIN_ERROR);
  calc_clear_failure ();

  /* the inverse of 0 never finishes: a cancel stops it */
  gint cancel = 1;
  calc_set_cancel_flag (&cancel);
  g_autoptr (CReal) zero = creal_from_long (0);
  g_autoptr (CReal) inverse = creal_inverse (zero);
  creal_get_appr (inverse, -10, appr);
  g_assert_cmpint (calc_failure (), ==, CALC_ABORTED);
  calc_set_cancel_flag (NULL);
  calc_clear_failure ();
  mpz_clear (appr);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/creal/pi", test_pi);
  g_test_add_func ("/creal/functions", test_functions);
  g_test_add_func ("/creal/comparisons", test_comparisons);
  g_test_add_func ("/creal/failures", test_failures);
  return g_test_run ();
}
