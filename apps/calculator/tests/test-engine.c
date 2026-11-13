/* the arithmetic as the calculator uses it: expressions typed key by key, evaluated, and their
 * results shown (calc-expr, unified-real, bounded-rational, calc-format, calc-keys) */
#include "calc-expr.h"
#include "calc-failure.h"
#include "calc-format.h"
#include "calc-keys.h"

#include <locale.h>
#include <string.h>

/* --- typing */

/* the keys of a formula written as the buttons show it: "sin(" is the sin key, "√" the root key,
 * "²" the square; everything else as calc_key_for_char reads it */
static CalcExpr *
type (const char *s)
{
  static const struct {
    const char *text;
    CalcKey     key;
  } extra[] = {
    { "exp(", CALC_KEY_FUN_EXP }, { "√", CALC_KEY_OP_SQRT }, { "²", CALC_KEY_OP_SQR },
    { "10^", CALC_KEY_FUN_10POW },
  };
  CalcExpr *expr = calc_expr_new ();

  while (*s) {
    gboolean done = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS (extra) && !done; i++) {
      if (g_str_has_prefix (s, extra[i].text)) {
        if (extra[i].key == CALC_KEY_FUN_10POW) {
          /* Evaluator.add10pow */
          calc_expr_add (expr, CALC_KEY_DIGIT_1);
          calc_expr_add (expr, CALC_KEY_DIGIT_0);
          calc_expr_add (expr, CALC_KEY_OP_POW);
        } else {
          g_assert_true (calc_expr_add (expr, extra[i].key));
        }
        s += strlen (extra[i].text);
        done = TRUE;
      }
    }
    if (done)
      continue;
    if (g_ascii_isalpha (*s) && g_ascii_isalpha (s[1])) {
      CalcKey key = calc_key_for_function_name (s);
      g_assert_cmpint (key, !=, CALC_KEY_NONE);
      g_assert_true (calc_expr_add (expr, key));
      s = strchr (s, '(') + 1;
      continue;
    }
    CalcKey key = calc_key_for_char (g_utf8_get_char (s));
    g_assert_cmpint (key, !=, CALC_KEY_NONE);
    calc_expr_add (expr, key);
    s = g_utf8_next_char (s);
  }
  return expr;
}

/* a resolver for expressions without earlier results in them */
static CalcExpr *
no_expr (gpointer data, long index)
{
  g_assert_not_reached ();
  return NULL;
}

static gboolean
no_degree_mode (gpointer data, long index)
{
  return FALSE;
}

static UnifiedReal *
no_result (gpointer data, long index)
{
  return NULL;
}

static UnifiedReal *
no_put (gpointer data, long index, UnifiedReal *result)
{
  return real_ref (result);
}

static const CalcExprResolver plain = { no_expr, no_degree_mode, no_result, no_put, NULL };

static UnifiedReal *
eval_full (const char *s, gboolean degrees, CalcFailure *failure)
{
  g_autoptr (CalcExpr) expr = type (s);

  calc_clear_failure ();
  UnifiedReal *value = calc_expr_eval (expr, degrees, &plain);
  *failure = calc_failure ();
  calc_clear_failure ();
  return value;
}

static UnifiedReal *
eval (const char *s)
{
  CalcFailure failure;
  UnifiedReal *value = eval_full (s, FALSE, &failure);

  if (!value)
    g_error ("%s failed: %d", s, failure);
  return value;
}

/* the value is exactly the fraction num/den (real_rational_value is not a new reference) */
static void
assert_rational (const char *s, long num, long den)
{
  g_autoptr (UnifiedReal) value = eval (s);
  BoundedRational *r = real_rational_value (value);
  g_autoptr (BoundedRational) want = rational_new (num, den);

  if (!r)
    g_error ("%s: not known to be rational", s);
  if (rational_compare (r, want) != 0)
    g_error ("%s: %s, not %ld/%ld", s, rational_to_nice_string (r), num, den);
}

/* the value's first decimals */
static void
assert_digits (const char *s, gboolean degrees, int n, const char *want)
{
  CalcFailure failure;
  g_autoptr (UnifiedReal) value = eval_full (s, degrees, &failure);
  g_assert_nonnull (value);
  g_autofree char *digits = real_to_string_truncated (value, n);

  g_assert_cmpint (calc_failure (), ==, CALC_OK);
  if (strcmp (digits, want) != 0)
    g_error ("%s: %s, not %s", s, digits, want);
}

static void
assert_nice (const char *s, gboolean degrees, const char *want)
{
  CalcFailure failure;
  g_autoptr (UnifiedReal) value = eval_full (s, degrees, &failure);
  g_assert_nonnull (value);
  g_assert_true (real_exactly_displayable (value));
  g_autofree char *nice = real_to_nice_string (value);

  if (g_strcmp0 (nice, want) != 0)
    g_error ("%s: %s, not %s", s, nice, want);
}

static void
assert_failure (const char *s, CalcFailure want)
{
  CalcFailure failure;
  g_autoptr (UnifiedReal) value = eval_full (s, FALSE, &failure);

  if (value || failure != want)
    g_error ("%s: failure %d, not %d", s, failure, want);
}

/* --- the tests */

static void
test_exact (void)
{
  /* the reasons this calculator exists */
  assert_rational ("1÷3×3", 1, 1);
  assert_rational ("0.1+0.2", 3, 10);
  assert_digits ("0.1+0.2", FALSE, 20, "0.30000000000000000000");
  assert_rational ("sin(π)", 0, 1);
  assert_rational ("√8×√2", 4, 1);
  assert_rational ("ln(8)÷ln(2)", 3, 1);
  assert_rational ("exp(3ln(2))", 8, 1);
  assert_rational ("exp(ln(8))", 8, 1);
  assert_rational ("2^10", 1024, 1);
  assert_rational ("2^−2", 1, 4);
  assert_rational ("10^3", 1000, 1);
  assert_rational ("√(2)²", 2, 1);
  assert_rational ("cos(π÷3)", 1, 2);
  assert_rational ("tan(π÷4)", 1, 1);
  assert_rational ("log(1000)", 3, 1);
  assert_rational ("5!", 120, 1);
  assert_rational ("1÷7×7−1", 0, 1);
  /* ExactCalculator says 0 here, not "Can't divide by 0": BoundedRational.pow returns 0 for
   * any power of 0 before it looks at the sign */
  assert_rational ("0^−1", 0, 1);
}

static void
test_nice (void)
{
  assert_nice ("1÷3", FALSE, "1/3");
  assert_nice ("asin(0.5)", FALSE, "(1/6)π");
  assert_nice ("√8", FALSE, "2√2");
  assert_nice ("2π", FALSE, "2π");
  assert_nice ("ln(8)", FALSE, "3ln(2)");
}

static void
test_degrees (void)
{
  CalcFailure failure;

  g_autoptr (UnifiedReal) sin30 = eval_full ("sin(30)", TRUE, &failure);
  g_autoptr (BoundedRational) half = rational_new (1, 2);
  BoundedRational *r = real_rational_value (sin30);
  g_assert_nonnull (r);
  g_assert_cmpint (rational_compare (r, half), ==, 0);

  g_autoptr (UnifiedReal) asin1 = eval_full ("asin(1)", TRUE, &failure);
  r = real_rational_value (asin1);
  g_assert_nonnull (r);
  g_assert_true (rational_equals_long (r, 90));

  assert_digits ("sin(1)", TRUE, 30, "0.017452406437283512819418978516");
  assert_digits ("sin(1)", FALSE, 30, "0.841470984807896506652502321630");
}

static void
test_digits (void)
{
  assert_digits ("√2", FALSE, 60, "1.414213562373095048801688724209698078569671875376948073176679");
  assert_digits ("π", FALSE, 40, "3.1415926535897932384626433832795028841971");
  assert_digits ("e", FALSE, 40, "2.7182818284590452353602874713526624977572");
  assert_digits ("1÷3", FALSE, 10, "0.3333333333");
  assert_digits ("−2÷3", FALSE, 10, "-0.6666666666");
  assert_digits ("2^0.5", FALSE, 20, "1.41421356237309504880");
  assert_digits ("20!", FALSE, 0, "2432902008176640000.");
  assert_digits ("cos(1)", FALSE, 20, "0.54030230586813971740");
  assert_digits ("atan(1)×4", FALSE, 20, "3.14159265358979323846");
  /* truncated, not rounded */
  assert_digits ("2÷3", FALSE, 3, "0.666");
  assert_digits ("e^π−π", FALSE, 20, "19.99909997918947576726");
}

static void
test_grammar (void)
{
  /* percentages: of what came before after + and −, a hundredth otherwise */
  assert_rational ("100+5%", 105, 1);
  assert_rational ("100−5%", 95, 1);
  assert_rational ("50%", 1, 2);
  assert_rational ("200×10%", 20, 1);
  /* juxtaposition multiplies */
  assert_digits ("2π", FALSE, 10, "6.2831853071");
  assert_rational ("3(4)", 12, 1);
  assert_rational ("2(3+4)5", 70, 1);
  /* a missing ')' at the end is fine, a trailing operator is ignored */
  assert_rational ("2×(3+4", 14, 1);
  assert_rational ("5×", 5, 1);
  assert_rational ("5+−", 5, 1);
  /* precedence */
  assert_rational ("2+3×4", 14, 1);
  assert_rational ("2^3^2", 512, 1);
  assert_rational ("−2^2", -4, 1);
  assert_rational ("√4+5", 7, 1);
  assert_rational ("3!²", 36, 1);
}

static void
test_errors (void)
{
  assert_failure ("1÷0", CALC_DIVIDE_BY_ZERO);
  assert_failure ("√(−1)", CALC_DOMAIN_ERROR);
  assert_failure ("ln(0)", CALC_DOMAIN_ERROR);
  assert_failure ("ln(−1)", CALC_DOMAIN_ERROR);
  assert_failure ("asin(2)", CALC_DOMAIN_ERROR);
  /* tan is sin÷cos */
  assert_failure ("tan(π÷2)", CALC_DIVIDE_BY_ZERO);
  assert_failure ("(−1)!", CALC_DOMAIN_ERROR);
  assert_failure ("0.5!", CALC_DOMAIN_ERROR);
  /* a negative base takes integer and half-integer powers only (UnifiedReal.pow) */
  assert_failure ("(−8)^(1÷3)", CALC_DOMAIN_ERROR);
  assert_failure ("5+(", CALC_SYNTAX_ERROR);
  assert_failure ("(", CALC_SYNTAX_ERROR);
}

static void
test_editing (void)
{
  g_autoptr (CalcExpr) expr = calc_expr_new ();

  /* a binary operator needs something before it, and replaces one */
  g_assert_false (calc_expr_add (expr, CALC_KEY_OP_MUL));
  g_assert_true (calc_expr_add (expr, CALC_KEY_DIGIT_1));
  g_assert_true (calc_expr_add (expr, CALC_KEY_DIGIT_2));
  g_assert_true (calc_expr_add (expr, CALC_KEY_OP_ADD));
  g_assert_true (calc_expr_add (expr, CALC_KEY_OP_MUL));
  g_assert_true (calc_expr_add (expr, CALC_KEY_DIGIT_6));
  g_assert_true (calc_expr_add (expr, CALC_KEY_OP_MUL));
  g_assert_true (calc_expr_add (expr, CALC_KEY_FUN_SIN));
  g_assert_false (calc_expr_add (expr, CALC_KEY_OP_DIV));
  g_autofree char *s = calc_expr_to_string (expr);
  g_assert_cmpstr (s, ==, "12×6×sin(");
  g_assert_true (calc_expr_has_trig_funcs (expr));
  g_assert_true (calc_expr_has_open_parentheses (expr));

  /* deleting goes digit by digit, operator by operator */
  calc_expr_delete (expr);
  calc_expr_delete (expr);
  calc_expr_delete (expr);
  g_free (s);
  s = calc_expr_to_string (expr);
  g_assert_cmpstr (s, ==, "12×");
  g_assert_true (calc_expr_has_trailing_binary (expr));
  calc_expr_delete (expr);
  calc_expr_delete (expr);
  g_free (s);
  s = calc_expr_to_string (expr);
  g_assert_cmpstr (s, ==, "1");
  g_assert_true (calc_expr_has_trailing_constant (expr));
  g_assert_false (calc_expr_has_interesting_ops (expr));
  calc_expr_add (expr, CALC_KEY_OP_SUB);
  g_assert_false (calc_expr_has_interesting_ops (expr));
  calc_expr_add (expr, CALC_KEY_DIGIT_1);
  g_assert_true (calc_expr_has_interesting_ops (expr));
  calc_expr_clear (expr);
  g_assert_true (calc_expr_is_empty (expr));

  /* a leading minus alone is not interesting */
  g_autoptr (CalcExpr) neg = type ("−5");
  g_assert_false (calc_expr_has_interesting_ops (neg));

  /* the decimal point: one per number */
  g_autoptr (CalcExpr) dec = type ("1.5");
  g_assert_false (calc_expr_add (dec, CALC_KEY_DEC_POINT));
  g_free (s);
  s = calc_expr_to_string (dec);
  g_assert_cmpstr (s, ==, "1.5");

  /* a pasted exponent belongs to the number ("E" typed is e) */
  calc_expr_add_exponent (dec, -3);
  g_free (s);
  s = calc_expr_to_string (dec);
  g_assert_cmpstr (s, ==, "1.5E−3");
  calc_clear_failure ();
  g_autoptr (UnifiedReal) v = calc_expr_eval (dec, FALSE, &plain);
  g_autoptr (BoundedRational) want = rational_new (3, 2000);
  g_assert_nonnull (v);
  g_assert_cmpint (rational_compare (real_rational_value (v), want), ==, 0);
}

static void
test_bytes (void)
{
  const char *formulas[] = { "12+6×sin(30)", "√2÷3^2−e", "1.5E3+(2", "100+5%", "ln(8)!", "1.0" };

  for (guint i = 0; i < G_N_ELEMENTS (formulas); i++) {
    g_autoptr (CalcExpr) expr = type (formulas[i]);
    g_autoptr (GBytes) bytes = calc_expr_to_bytes (expr);
    gsize size;
    const guint8 *data = g_bytes_get_data (bytes, &size);
    g_autoptr (CalcExpr) back = calc_expr_new_from_bytes (data, size);
    g_assert_nonnull (back);
    g_autofree char *a = calc_expr_to_string (expr);
    g_autofree char *b = calc_expr_to_string (back);
    g_assert_cmpstr (a, ==, b);
    g_autoptr (GBytes) again = calc_expr_to_bytes (back);
    g_assert_true (g_bytes_equal (bytes, again));
  }
  /* and garbage is refused */
  const guint8 junk[] = { 0, 0, 0, 9, 1 };
  g_assert_null (calc_expr_new_from_bytes (junk, sizeof junk));
}

static const CalcCharMetrics phone = {
  /* the result line on the S10e, in digits (measured in the UI: 1080 px wide line) */
  .max_chars = 14, .grouping_separator_width_ratio = 0.5f, .decimal_credit = 0.5f,
  .no_ellipsis_credit = 1.0f, .no_exponent_credit = 0.0f,
};

static void
test_short_strings (void)
{
  struct {
    const char *formula;
    const char *want;
  } cases[] = {
    /* worked through Evaluator.getShortString by hand */
    { "1÷3", "0.33333~" },
    { "2", "2" },
    { "1.5", "1.5" },
    { "√2", "1.41421~" },
    { "1÷8", "0.125" },
    { "−1÷3", "-0.3333~" },
    { "2^100", "1.267~E30" },
    { "10^20", "1.E20" },
    { "1÷10^20", "1.E-20" },
    { "1234567", "1,234,567" },
    { "12345678", "12,345,678" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    g_autoptr (UnifiedReal) value = eval (cases[i].formula);
    g_autofree char *digits = real_to_string_truncated (value, 50);
    int dot = strchr (digits, '.') - digits;
    int msd = calc_get_msd_index_of (digits);
    int lsd = calc_get_lsd_offset (value, digits, dot);
    g_autofree char *got = calc_get_short_string (digits, msd, lsd);
    if (strcmp (got, cases[i].want) != 0)
      g_error ("%s: short \"%s\", not \"%s\"", cases[i].formula, got, cases[i].want);
  }
}

static void
test_format (void)
{
  g_assert_cmpint (calc_get_msd_index_of ("0.000123"), ==, 5);
  g_assert_cmpint (calc_get_msd_index_of ("-12.5"), ==, 1);
  g_assert_cmpint (calc_get_msd_index_of ("0.0000"), ==, CALC_INVALID_MSD);
  /* a lone trailing 1 might be a rounded ...999 */
  g_assert_cmpint (calc_get_msd_index_of ("0.0001"), ==, CALC_INVALID_MSD);

  g_autofree char *u = calc_unflip_zeroes ("0.0999", 4, "0.100000", 6);
  g_assert_cmpstr (u, ==, "0.099999");
  g_autofree char *v = calc_unflip_zeroes ("0.1234", 4, "0.123456", 6);
  g_assert_cmpstr (v, ==, "0.123456");

  gunichar e1[] = { '1', 'E', '-', '1', '2', '+' };
  g_assert_cmpint (calc_exponent_end (e1, 6, 1), ==, 5);
  gunichar e2[] = { '1', 'E', '+', '2' };
  g_assert_cmpint (calc_exponent_end (e2, 4, 1), ==, 1);

  g_assert_cmpint (calc_exp_len (0), ==, 0);
  g_assert_cmpint (calc_exp_len (5), ==, 2);
  g_assert_cmpint (calc_exp_len (-12), ==, 4);

  g_autofree char *c = calc_add_commas ("-1234567.5", 0, 8);
  g_assert_cmpstr (c, ==, "-1,234,567");

  /* the first precision shown: 1/3 fills the line with decimals */
  g_autoptr (UnifiedReal) third = eval ("1÷3");
  g_autofree char *digits = real_to_string_truncated (third, 50);
  int lsd = calc_get_lsd_offset (third, digits, 1);
  g_assert_cmpint (lsd, ==, G_MAXINT);
  int prec = calc_get_preferred_prec (digits, calc_get_msd_index_of (digits), lsd, &phone);
  g_assert_cmpint (prec, >, 8);
  /* a short exact result shows just its digits */
  g_autoptr (UnifiedReal) eighth = eval ("1÷8");
  g_autofree char *d8 = real_to_string_truncated (eighth, 50);
  int lsd8 = calc_get_lsd_offset (eighth, d8, 1);
  g_assert_cmpint (lsd8, ==, 3);
  g_assert_cmpint (calc_get_preferred_prec (d8, calc_get_msd_index_of (d8), lsd8, &phone), ==, 3);
  /* integers end at their last non-zero digit */
  g_autoptr (UnifiedReal) big = eval ("1200");
  g_autofree char *db = real_to_string_truncated (big, 50);
  g_assert_cmpint (calc_get_lsd_offset (big, db, 4), ==, -3);
}

static void
test_translate (void)
{
  g_autofree char *a = calc_translate_result ("-1,234.5E-7");
  g_assert_cmpstr (a, ==, "−1,234.5E−7");
  g_autofree char *b = calc_translate_result ("0.33~");
  g_assert_cmpstr (b, ==, "0.33…");
  g_autofree char *c = calc_translate_result ("1.4 ");
  g_assert_cmpstr (c, ==, "1.4 ");
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/engine/exact", test_exact);
  g_test_add_func ("/engine/nice", test_nice);
  g_test_add_func ("/engine/degrees", test_degrees);
  g_test_add_func ("/engine/digits", test_digits);
  g_test_add_func ("/engine/grammar", test_grammar);
  g_test_add_func ("/engine/errors", test_errors);
  g_test_add_func ("/engine/editing", test_editing);
  g_test_add_func ("/engine/bytes", test_bytes);
  g_test_add_func ("/engine/short-strings", test_short_strings);
  g_test_add_func ("/engine/format", test_format);
  g_test_add_func ("/engine/translate", test_translate);
  return g_test_run ();
}
