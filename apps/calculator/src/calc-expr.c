/* calc-expr: see calc-expr.h. A port of ExactCalculator's CalculatorExpr. */
#include "calc-expr.h"
#include "calc-failure.h"

#include <string.h>

/* the kinds' numbers are those Java's TokenKind ordinals had in the saved form */
typedef enum {
  TOKEN_CONSTANT = 0,
  TOKEN_OPERATOR = 1,
  TOKEN_PRE_EVAL = 2,
} TokenKind;

/* a constant's flags in the saved form (Constant.SAW_DECIMAL, HAS_EXPONENT) */
#define SAW_DECIMAL  0x1
#define HAS_EXPONENT 0x2

typedef struct {
  TokenKind kind;
  CalcKey   op;            /* TOKEN_OPERATOR */
  GString  *whole;         /* TOKEN_CONSTANT: the digits before the point */
  GString  *fraction;      /* ... and after it */
  gboolean  saw_decimal;
  int       exponent;      /* only ever set by calc_expr_add_exponent (a paste) */
  long      index;         /* TOKEN_PRE_EVAL: the expression it stands for */
  char     *short_rep;     /* ... and how it is shown, not localized */
} Token;

struct _CalcExpr {
  GPtrArray *tokens;   /* of Token */
};

static void
token_free (Token *t)
{
  if (t->whole)
    g_string_free (t->whole, TRUE);
  if (t->fraction)
    g_string_free (t->fraction, TRUE);
  g_free (t->short_rep);
  g_free (t);
}

static Token *
operator_new (CalcKey op)
{
  Token *t = g_new0 (Token, 1);

  t->kind = TOKEN_OPERATOR;
  t->op = op;
  return t;
}

static Token *
constant_new (void)
{
  Token *t = g_new0 (Token, 1);

  t->kind = TOKEN_CONSTANT;
  t->whole = g_string_new (NULL);
  t->fraction = g_string_new (NULL);
  return t;
}

static Token *
pre_eval_new (long index, const char *short_rep)
{
  Token *t = g_new0 (Token, 1);

  t->kind = TOKEN_PRE_EVAL;
  t->index = index;
  t->short_rep = g_strdup (short_rep);
  return t;
}

static Token *
token_copy (const Token *t)
{
  Token *copy = g_new0 (Token, 1);

  *copy = *t;
  copy->whole = t->whole ? g_string_new (t->whole->str) : NULL;
  copy->fraction = t->fraction ? g_string_new (t->fraction->str) : NULL;
  copy->short_rep = g_strdup (t->short_rep);
  return copy;
}

/* --- constants (CalculatorExpr.Constant) */

/* a digit or the point: FALSE if refused (a second point, or a too long exponent) */
static gboolean
constant_add (Token *c, CalcKey key)
{
  if (key == CALC_KEY_DEC_POINT) {
    if (c->saw_decimal || c->exponent != 0)
      return FALSE;
    c->saw_decimal = TRUE;
    return TRUE;
  }
  int value = calc_key_digit_value (key);
  if (c->exponent != 0) {
    if (ABS (c->exponent) > 10000)
      return FALSE;
    c->exponent = c->exponent > 0 ? 10 * c->exponent + value : 10 * c->exponent - value;
    return TRUE;
  }
  g_string_append_c (c->saw_decimal ? c->fraction : c->whole, '0' + value);
  return TRUE;
}

static void
constant_delete (Token *c)
{
  if (c->exponent != 0)
    c->exponent /= 10;
  else if (c->fraction->len > 0)
    g_string_truncate (c->fraction, c->fraction->len - 1);
  else if (c->saw_decimal)
    c->saw_decimal = FALSE;
  else if (c->whole->len > 0)
    g_string_truncate (c->whole, c->whole->len - 1);
}

static gboolean
constant_is_empty (Token *c)
{
  return !c->saw_decimal && c->whole->len == 0;
}

/* as shown: grouped, unless there is an exponent */
static char *
constant_to_string (Token *c)
{
  GString *s = g_string_new (NULL);

  if (c->exponent != 0) {
    g_string_append (s, c->whole->str);
  } else {
    g_autofree char *grouped = calc_add_commas (c->whole->str, 0, c->whole->len);
    g_string_append (s, grouped);
  }
  if (c->saw_decimal) {
    g_string_append_c (s, '.');
    g_string_append (s, c->fraction->str);
  }
  if (c->exponent != 0)
    g_string_append_printf (s, "E%d", c->exponent);
  g_autofree char *raw = g_string_free (s, FALSE);
  return calc_translate_result (raw);
}

/* the exact value; a lone point is a syntax error */
static BoundedRational *
constant_to_rational (Token *c)
{
  const char *whole = c->whole->str;

  if (c->whole->len == 0) {
    if (c->fraction->len == 0) {
      calc_fail (CALC_SYNTAX_ERROR);
      return NULL;
    }
    whole = "0";
  }
  g_autofree char *digits = g_strconcat (whole, c->fraction->str, NULL);
  mpz_t num, den;
  mpz_init_set_str (num, digits, 10);
  mpz_init (den);
  mpz_ui_pow_ui (den, 10, c->fraction->len);
  if (c->exponent > 0) {
    mpz_t power;
    mpz_init (power);
    mpz_ui_pow_ui (power, 10, c->exponent);
    mpz_mul (num, num, power);
    mpz_clear (power);
  } else if (c->exponent < 0) {
    mpz_t power;
    mpz_init (power);
    mpz_ui_pow_ui (power, 10, -c->exponent);
    mpz_mul (den, den, power);
    mpz_clear (power);
  }
  BoundedRational *r = rational_new_fraction (num, den);
  mpz_clears (num, den, NULL);
  if (!r)   /* Java kept any typed constant; one this long (thousands of digits) is refused */
    calc_fail (CALC_TOO_BIG);
  return r;
}

/* --- the expression */

CalcExpr *
calc_expr_new (void)
{
  CalcExpr *expr = g_new0 (CalcExpr, 1);

  expr->tokens = g_ptr_array_new_with_free_func ((GDestroyNotify) token_free);
  return expr;
}

void
calc_expr_free (CalcExpr *expr)
{
  if (!expr)
    return;
  g_ptr_array_unref (expr->tokens);
  g_free (expr);
}

CalcExpr *
calc_expr_copy (CalcExpr *expr)
{
  CalcExpr *copy = calc_expr_new ();

  for (guint i = 0; i < expr->tokens->len; i++)
    g_ptr_array_add (copy->tokens, token_copy (g_ptr_array_index (expr->tokens, i)));
  return copy;
}

static Token *
token_at (CalcExpr *expr, guint i)
{
  return g_ptr_array_index (expr->tokens, i);
}

static Token *
last_token (CalcExpr *expr)
{
  return expr->tokens->len ? token_at (expr, expr->tokens->len - 1) : NULL;
}

/* the last token's operator, or CALC_KEY_NONE */
static CalcKey
last_operator (CalcExpr *expr)
{
  Token *t = last_token (expr);

  return t && t->kind == TOKEN_OPERATOR ? t->op : CALC_KEY_NONE;
}

gboolean
calc_expr_has_trailing_constant (CalcExpr *expr)
{
  Token *t = last_token (expr);

  if (!t)
    return FALSE;
  if (t->kind == TOKEN_CONSTANT)
    return TRUE;
  return t->kind == TOKEN_OPERATOR && (t->op == CALC_KEY_CONST_PI || t->op == CALC_KEY_CONST_E);
}

gboolean
calc_expr_has_trailing_binary (CalcExpr *expr)
{
  return calc_key_is_binary (last_operator (expr));
}

gboolean
calc_expr_has_trailing_suffix (CalcExpr *expr)
{
  return calc_key_is_suffix (last_operator (expr));
}

gboolean
calc_expr_has_open_parentheses (CalcExpr *expr)
{
  int open = 0;

  for (guint i = 0; i < expr->tokens->len; i++) {
    Token *t = token_at (expr, i);
    if (t->kind != TOKEN_OPERATOR)
      continue;
    if (t->op == CALC_KEY_LPAREN || calc_key_is_func (t->op))
      open++;
    else if (t->op == CALC_KEY_RPAREN)
      open--;
  }
  return open > 0;
}

gboolean
calc_expr_has_trailing_left_paren (CalcExpr *expr)
{
  CalcKey op = last_operator (expr);

  return op == CALC_KEY_LPAREN || calc_key_is_func (op);
}

gboolean
calc_expr_has_trailing_right_paren (CalcExpr *expr)
{
  return last_operator (expr) == CALC_KEY_RPAREN;
}

gboolean
calc_expr_add (CalcExpr *expr, CalcKey key)
{
  guint s = expr->tokens->len;
  int d = calc_key_digit_value (key);
  CalcKey last_op = last_operator (expr);

  if (calc_key_is_binary (key) && !calc_key_is_prefix (key)) {
    /* a binary operator needs something before it; it replaces a binary operator there */
    if (s == 0 || last_op == CALC_KEY_LPAREN || calc_key_is_func (last_op)
        || (calc_key_is_prefix (last_op) && last_op != CALC_KEY_OP_SUB))
      return FALSE;
    while (calc_expr_has_trailing_binary (expr))
      calc_expr_delete (expr);
  }
  gboolean is_const_piece = d != CALC_NOT_DIGIT || key == CALC_KEY_DEC_POINT;
  if (!is_const_piece) {
    g_ptr_array_add (expr->tokens, operator_new (key));
    return TRUE;
  }
  Token *last = last_token (expr);
  if (!last || last->kind != TOKEN_CONSTANT) {
    if (last && last->kind == TOKEN_PRE_EVAL)   /* a number right after a result multiplies */
      g_ptr_array_add (expr->tokens, operator_new (CALC_KEY_OP_MUL));
    g_ptr_array_add (expr->tokens, constant_new ());
  }
  return constant_add (last_token (expr), key);
}

void
calc_expr_add_exponent (CalcExpr *expr, int exponent)
{
  Token *last = last_token (expr);

  if (last && last->kind == TOKEN_CONSTANT)
    last->exponent = exponent;
}

void
calc_expr_remove_trailing_additive_operators (CalcExpr *expr)
{
  while (last_operator (expr) == CALC_KEY_OP_ADD || last_operator (expr) == CALC_KEY_OP_SUB)
    calc_expr_delete (expr);
}

void
calc_expr_append (CalcExpr *expr, CalcExpr *other)
{
  if (expr->tokens->len && other->tokens->len) {
    Token *last = last_token (expr);
    Token *first = token_at (other, 0);
    if (first->kind != TOKEN_OPERATOR && last->kind != TOKEN_OPERATOR)
      g_ptr_array_add (expr->tokens, operator_new (CALC_KEY_OP_MUL));
  }
  for (guint i = 0; i < other->tokens->len; i++)
    g_ptr_array_add (expr->tokens, token_copy (token_at (other, i)));
}

void
calc_expr_delete (CalcExpr *expr)
{
  Token *last = last_token (expr);

  if (!last)
    return;
  if (last->kind == TOKEN_CONSTANT) {
    constant_delete (last);
    if (!constant_is_empty (last))
      return;
  }
  g_ptr_array_remove_index (expr->tokens, expr->tokens->len - 1);
}

void
calc_expr_clear (CalcExpr *expr)
{
  g_ptr_array_set_size (expr->tokens, 0);
}

gboolean
calc_expr_is_empty (CalcExpr *expr)
{
  return expr->tokens->len == 0;
}

CalcExpr *
calc_expr_new_pre_evaluated (long index, const char *short_rep)
{
  CalcExpr *expr = calc_expr_new ();

  g_ptr_array_add (expr->tokens, pre_eval_new (index, short_rep));
  return expr;
}

/* where the trailing binary operators start: evaluation ignores them (trailingBinaryOpsStart) */
static guint
trailing_binary_ops_start (CalcExpr *expr)
{
  guint result = expr->tokens->len;

  while (result > 0) {
    Token *t = token_at (expr, result - 1);
    if (t->kind != TOKEN_OPERATOR || !calc_key_is_binary (t->op))
      break;
    --result;
  }
  return result;
}

static gboolean
pre_eval_has_ellipsis (Token *t)
{
  return strchr (t->short_rep, CALC_ELLIPSIS_CHAR) != NULL;
}

gboolean
calc_expr_has_interesting_ops (CalcExpr *expr)
{
  guint last = trailing_binary_ops_start (expr);
  guint first = 0;

  if (last > first && token_at (expr, first)->kind == TOKEN_OPERATOR && token_at (expr, first)->op == CALC_KEY_OP_SUB)
    first++;
  for (guint i = first; i < last; i++) {
    Token *t = token_at (expr, i);
    if (t->kind == TOKEN_OPERATOR || (t->kind == TOKEN_PRE_EVAL && pre_eval_has_ellipsis (t)))
      return TRUE;
  }
  return FALSE;
}

gboolean
calc_expr_has_trig_funcs (CalcExpr *expr)
{
  for (guint i = 0; i < expr->tokens->len; i++) {
    Token *t = token_at (expr, i);
    if (t->kind == TOKEN_OPERATOR && calc_key_is_trig_func (t->op))
      return TRUE;
  }
  return FALSE;
}

static gboolean
index_listed (GArray *list, long index)
{
  for (guint i = 0; i < list->len; i++)
    if (g_array_index (list, long, i) == index)
      return TRUE;
  return FALSE;
}

/* adds the not yet evaluated expressions this one refers to (addReferencedExprs) */
static void
add_referenced (CalcExpr *expr, GArray *list, const CalcExprResolver *resolver)
{
  for (guint i = 0; i < expr->tokens->len; i++) {
    Token *t = token_at (expr, i);
    if (t->kind != TOKEN_PRE_EVAL)
      continue;
    g_autoptr (UnifiedReal) result = resolver->get_result (resolver->data, t->index);
    if (!result && !index_listed (list, t->index))
      g_array_append_val (list, t->index);
  }
}

GArray *
calc_expr_get_referenced (CalcExpr *expr, const CalcExprResolver *resolver)
{
  GArray *list = g_array_new (FALSE, FALSE, sizeof (long));
  guint scanned = 0;

  add_referenced (expr, list, resolver);
  while (scanned != list->len) {
    long index = g_array_index (list, long, scanned++);
    add_referenced (resolver->get_expr (resolver->data, index), list, resolver);
  }
  /* the deepest first */
  for (guint i = 0; i < list->len / 2; i++) {
    long tmp = g_array_index (list, long, i);
    g_array_index (list, long, i) = g_array_index (list, long, list->len - 1 - i);
    g_array_index (list, long, list->len - 1 - i) = tmp;
  }
  return list;
}

/* --- evaluation: recursive descent over the tokens (evalExpr and its helpers) */

typedef struct {
  CalcExpr               *expr;
  guint                   prefix_length;   /* tokens past this (trailing binary operators) are ignored */
  gboolean                degree_mode;
  const CalcExprResolver *resolver;
} EvalContext;

/* what the parse of a sub-expression returns: where it ended, and its value (EvalRet) */
typedef struct {
  guint        pos;
  UnifiedReal *val;   /* owned */
} EvalRet;

static EvalRet eval_expr (EvalContext *ec, guint i);
static EvalRet eval_signed_factor (EvalContext *ec, guint i);

static EvalRet
eval_ret (guint pos, UnifiedReal *val)
{
  return (EvalRet) { pos, val };
}

/* a failure: the position no longer matters, the value is a placeholder */
static EvalRet
eval_fail (guint pos, CalcFailure failure)
{
  calc_fail (failure);
  return eval_ret (pos, real_from_long (0));
}

static gboolean
is_operator_unchecked (CalcExpr *expr, guint i, CalcKey op)
{
  if (i >= expr->tokens->len)
    return FALSE;
  Token *t = token_at (expr, i);
  return t->kind == TOKEN_OPERATOR && t->op == op;
}

static gboolean
is_operator (EvalContext *ec, guint i, CalcKey op)
{
  return i < ec->prefix_length && is_operator_unchecked (ec->expr, i, op);
}

static UnifiedReal *
to_radians (EvalContext *ec, UnifiedReal *x)
{
  if (!ec->degree_mode)
    return real_ref (x);
  g_autoptr (UnifiedReal) factor = real_radians_per_degree ();
  return real_multiply (x, factor);
}

static UnifiedReal *
from_radians (EvalContext *ec, UnifiedReal *x)
{
  if (!ec->degree_mode)
    return real_ref (x);
  g_autoptr (UnifiedReal) factor = real_radians_per_degree ();
  return real_divide (x, factor);
}

/* a function's argument: an expression with an optional closing ')' */
static EvalRet
eval_argument (EvalContext *ec, guint i)
{
  EvalRet arg = eval_expr (ec, i);

  if (is_operator (ec, arg.pos, CALC_KEY_RPAREN))
    arg.pos++;
  return arg;
}

static EvalRet
eval_unary (EvalContext *ec, guint i)
{
  if (calc_failed ())
    return eval_fail (i, CALC_ABORTED);
  /* Java read the token without a bounds check and turned the IndexOutOfBoundsException into a
   * SyntaxException ("Unexpected expression end") */
  if (i >= ec->expr->tokens->len)
    return eval_fail (i, CALC_SYNTAX_ERROR);

  Token *t = token_at (ec->expr, i);
  if (t->kind == TOKEN_CONSTANT) {
    g_autoptr (BoundedRational) value = constant_to_rational (t);
    if (!value)
      return eval_fail (i + 1, CALC_SYNTAX_ERROR);
    return eval_ret (i + 1, real_from_rational (value));
  }
  if (t->kind == TOKEN_PRE_EVAL) {
    UnifiedReal *result = ec->resolver->get_result (ec->resolver->data, t->index);
    if (!result)
      result = calc_expr_nested_eval (t->index, ec->resolver);
    if (!result)
      return eval_fail (i + 1, CALC_SYNTAX_ERROR);
    return eval_ret (i + 1, result);
  }

  EvalRet arg;
  UnifiedReal *value;
  switch (t->op) {
  case CALC_KEY_CONST_PI:
    return eval_ret (i + 1, real_pi ());
  case CALC_KEY_CONST_E:
    return eval_ret (i + 1, real_e ());
  case CALC_KEY_OP_SQRT:
    /* seems to have the highest precedence; "√−4" takes the minus along */
    if (is_operator (ec, i + 1, CALC_KEY_OP_SUB)) {
      arg = eval_unary (ec, i + 2);
      g_autoptr (UnifiedReal) negative = real_negate (arg.val);
      value = real_sqrt (negative);
    } else {
      arg = eval_unary (ec, i + 1);
      value = real_sqrt (arg.val);
    }
    break;
  case CALC_KEY_LPAREN:
    arg = eval_argument (ec, i + 1);
    value = real_ref (arg.val);
    break;
  case CALC_KEY_FUN_SIN:
  case CALC_KEY_FUN_COS:
  case CALC_KEY_FUN_TAN: {
    arg = eval_argument (ec, i + 1);
    g_autoptr (UnifiedReal) radians = to_radians (ec, arg.val);
    if (t->op == CALC_KEY_FUN_SIN) {
      value = real_sin (radians);
    } else if (t->op == CALC_KEY_FUN_COS) {
      value = real_cos (radians);
    } else {
      g_autoptr (UnifiedReal) sin = real_sin (radians);
      g_autoptr (UnifiedReal) cos = real_cos (radians);
      value = real_divide (sin, cos);
    }
    break;
  }
  case CALC_KEY_FUN_LN:
    arg = eval_argument (ec, i + 1);
    value = real_ln (arg.val);
    break;
  case CALC_KEY_FUN_EXP:
    arg = eval_argument (ec, i + 1);
    value = real_exp (arg.val);
    break;
  case CALC_KEY_FUN_LOG: {
    arg = eval_argument (ec, i + 1);
    g_autoptr (UnifiedReal) ln = real_ln (arg.val);
    g_autoptr (UnifiedReal) ten = real_from_long (10);
    g_autoptr (UnifiedReal) ln10 = real_ln (ten);
    value = real_divide (ln, ln10);
    break;
  }
  case CALC_KEY_FUN_ARCSIN:
  case CALC_KEY_FUN_ARCCOS:
  case CALC_KEY_FUN_ARCTAN: {
    arg = eval_argument (ec, i + 1);
    g_autoptr (UnifiedReal) radians = t->op == CALC_KEY_FUN_ARCSIN ? real_asin (arg.val)
                                    : t->op == CALC_KEY_FUN_ARCCOS ? real_acos (arg.val)
                                    : real_atan (arg.val);
    value = from_radians (ec, radians);
    break;
  }
  default:
    return eval_fail (i, CALC_SYNTAX_ERROR);   /* "Unrecognized token in expression" */
  }
  real_unref (arg.val);
  return eval_ret (arg.pos, value);
}

/* 1/100 (ONE_HUNDREDTH) */
static UnifiedReal *
one_hundredth (void)
{
  g_autoptr (BoundedRational) r = rational_new (1, 100);
  return real_from_rational (r);
}

/* suffixes: !, ² and % (which is ×1/100 here, unlike after + and −) */
static EvalRet
eval_suffix (EvalContext *ec, guint i)
{
  EvalRet tmp = eval_unary (ec, i);
  guint cpos = tmp.pos;
  UnifiedReal *val = tmp.val;

  for (;; ++cpos) {
    UnifiedReal *next;
    if (is_operator (ec, cpos, CALC_KEY_OP_FACT)) {
      next = real_fact (val);
    } else if (is_operator (ec, cpos, CALC_KEY_OP_SQR)) {
      next = real_multiply (val, val);
    } else if (is_operator (ec, cpos, CALC_KEY_OP_PCT)) {
      g_autoptr (UnifiedReal) hundredth = one_hundredth ();
      next = real_multiply (val, hundredth);
    } else {
      break;
    }
    real_unref (val);
    val = next;
  }
  return eval_ret (cpos, val);
}

/* x^y, y a signed factor: so 2^-3 works, and 2^3^2 is 2^9 */
static EvalRet
eval_factor (EvalContext *ec, guint i)
{
  EvalRet result = eval_suffix (ec, i);

  if (is_operator (ec, result.pos, CALC_KEY_OP_POW)) {
    EvalRet exp = eval_signed_factor (ec, result.pos + 1);
    UnifiedReal *power = real_pow (result.val, exp.val);
    real_unref (result.val);
    real_unref (exp.val);
    return eval_ret (exp.pos, power);
  }
  return result;
}

static EvalRet
eval_signed_factor (EvalContext *ec, guint i)
{
  gboolean negative = is_operator (ec, i, CALC_KEY_OP_SUB);
  EvalRet tmp = eval_factor (ec, negative ? i + 1 : i);

  if (negative) {
    UnifiedReal *minus = real_negate (tmp.val);
    real_unref (tmp.val);
    tmp.val = minus;
  }
  return tmp;
}

/* can a factor start at i, for implicit multiplication? Checks the whole expression, not only the
 * prefix, as Java did */
static gboolean
can_start_factor (EvalContext *ec, guint i)
{
  if (i >= ec->expr->tokens->len)
    return FALSE;
  Token *t = token_at (ec->expr, i);
  if (t->kind != TOKEN_OPERATOR)
    return TRUE;
  if (calc_key_is_binary (t->op))
    return FALSE;
  return t->op != CALC_KEY_OP_FACT && t->op != CALC_KEY_RPAREN;
}

static EvalRet
eval_term (EvalContext *ec, guint i)
{
  EvalRet tmp = eval_signed_factor (ec, i);
  guint cpos = tmp.pos;
  UnifiedReal *val = tmp.val;

  while (!calc_failed ()) {
    gboolean is_mul = is_operator (ec, cpos, CALC_KEY_OP_MUL);
    gboolean is_div = !is_mul && is_operator (ec, cpos, CALC_KEY_OP_DIV);
    if (!is_mul && !is_div && !can_start_factor (ec, cpos))
      break;
    if (is_mul || is_div)
      ++cpos;
    tmp = eval_signed_factor (ec, cpos);
    UnifiedReal *next = is_div ? real_divide (val, tmp.val) : real_multiply (val, tmp.val);
    real_unref (val);
    real_unref (tmp.val);
    val = next;
    cpos = tmp.pos;
  }
  return eval_ret (cpos, val);
}

/* is the token at pos a number followed by % and then nothing, +, − or ')'? (isPercent) */
static gboolean
is_percent (EvalContext *ec, guint pos)
{
  CalcExpr *expr = ec->expr;

  if (expr->tokens->len < pos + 2 || !is_operator_unchecked (expr, pos + 1, CALC_KEY_OP_PCT))
    return FALSE;
  if (token_at (expr, pos)->kind == TOKEN_OPERATOR)
    return FALSE;
  if (expr->tokens->len == pos + 2)
    return TRUE;
  Token *after = token_at (expr, pos + 2);
  if (after->kind != TOKEN_OPERATOR)
    return FALSE;
  return after->op == CALC_KEY_OP_ADD || after->op == CALC_KEY_OP_SUB || after->op == CALC_KEY_RPAREN;
}

/* "+ N%" is "× (1 + N/100)" (getPercentFactor) */
static EvalRet
get_percent_factor (EvalContext *ec, guint pos, gboolean is_subtraction)
{
  EvalRet tmp = eval_unary (ec, pos);
  g_autoptr (UnifiedReal) signed_val = is_subtraction ? real_negate (tmp.val) : real_ref (tmp.val);
  g_autoptr (UnifiedReal) hundredth = one_hundredth ();
  g_autoptr (UnifiedReal) fraction = real_multiply (signed_val, hundredth);
  g_autoptr (UnifiedReal) one = real_from_long (1);
  real_unref (tmp.val);
  return eval_ret (pos + 2, real_add (one, fraction));
}

static EvalRet
eval_expr (EvalContext *ec, guint i)
{
  EvalRet tmp = eval_term (ec, i);
  guint cpos = tmp.pos;
  UnifiedReal *val = tmp.val;

  while (!calc_failed ()) {
    gboolean is_plus = is_operator (ec, cpos, CALC_KEY_OP_ADD);
    if (!is_plus && !is_operator (ec, cpos, CALC_KEY_OP_SUB))
      break;
    UnifiedReal *next;
    if (is_percent (ec, cpos + 1)) {
      tmp = get_percent_factor (ec, cpos + 1, !is_plus);
      next = real_multiply (val, tmp.val);
    } else {
      tmp = eval_term (ec, cpos + 1);
      next = is_plus ? real_add (val, tmp.val) : real_subtract (val, tmp.val);
    }
    real_unref (val);
    real_unref (tmp.val);
    val = next;
    cpos = tmp.pos;
  }
  return eval_ret (cpos, val);
}

UnifiedReal *
calc_expr_nested_eval (long index, const CalcExprResolver *resolver)
{
  CalcExpr *nested = resolver->get_expr (resolver->data, index);
  EvalContext ec = {
    .expr = nested,
    .prefix_length = trailing_binary_ops_start (nested),
    .degree_mode = resolver->get_degree_mode (resolver->data, index),
    .resolver = resolver,
  };
  EvalRet result = eval_expr (&ec, 0);

  if (calc_failed ()) {
    real_unref (result.val);
    return NULL;
  }
  UnifiedReal *stored = resolver->put_result_if_absent (resolver->data, index, result.val);
  real_unref (result.val);
  return stored;
}

UnifiedReal *
calc_expr_eval (CalcExpr *expr, gboolean degree_mode, const CalcExprResolver *resolver)
{
  g_autoptr (GArray) referenced = calc_expr_get_referenced (expr, resolver);

  for (guint i = 0; i < referenced->len && !calc_failed (); i++) {
    g_autoptr (UnifiedReal) nested = calc_expr_nested_eval (g_array_index (referenced, long, i), resolver);
  }
  if (calc_failed ())
    return NULL;

  EvalContext ec = {
    .expr = expr,
    .prefix_length = trailing_binary_ops_start (expr),
    .degree_mode = degree_mode,
    .resolver = resolver,
  };
  EvalRet result = eval_expr (&ec, 0);
  if (!calc_failed () && result.pos != ec.prefix_length)
    calc_fail (CALC_SYNTAX_ERROR);   /* "Failed to parse full expression" */
  if (calc_failed ()) {
    real_unref (result.val);
    return NULL;
  }
  return result.val;
}

/* --- shown and saved */

char *
calc_expr_to_string (CalcExpr *expr)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < expr->tokens->len; i++) {
    Token *t = token_at (expr, i);
    if (t->kind == TOKEN_CONSTANT) {
      g_autofree char *c = constant_to_string (t);
      g_string_append (s, c);
    } else if (t->kind == TOKEN_PRE_EVAL) {
      g_autofree char *rep = calc_translate_result (t->short_rep);
      g_string_append (s, rep);
    } else {
      g_string_append (s, calc_key_to_string (t->op));
    }
  }
  return g_string_free (s, FALSE);
}

/* Java's DataOutputStream: big-endian integers, and strings as a 16-bit length and (modified)
 * UTF-8 */
static void
write_int (GByteArray *out, gint32 n)
{
  guint8 b[4] = { (guint32) n >> 24, (guint32) n >> 16, (guint32) n >> 8, (guint32) n };
  g_byte_array_append (out, b, 4);
}

static void
write_byte (GByteArray *out, guint8 byte)
{
  g_byte_array_append (out, &byte, 1);
}

static void
write_utf (GByteArray *out, const char *s)
{
  gsize len = strlen (s);
  guint8 b[2] = { len >> 8, len };
  g_byte_array_append (out, b, 2);
  g_byte_array_append (out, (const guint8 *) s, len);
}

GBytes *
calc_expr_to_bytes (CalcExpr *expr)
{
  GByteArray *out = g_byte_array_new ();

  write_int (out, expr->tokens->len);
  for (guint i = 0; i < expr->tokens->len; i++) {
    Token *t = token_at (expr, i);
    switch (t->kind) {
    case TOKEN_CONSTANT:
      write_byte (out, TOKEN_CONSTANT);
      write_utf (out, t->whole->str);
      write_byte (out, (t->saw_decimal ? SAW_DECIMAL : 0) | (t->exponent != 0 ? HAS_EXPONENT : 0));
      if (t->saw_decimal)
        write_utf (out, t->fraction->str);
      if (t->exponent != 0)
        write_int (out, t->exponent);
      break;
    case TOKEN_PRE_EVAL:
      write_byte (out, TOKEN_PRE_EVAL);
      write_int (out, (gint32) t->index);
      write_utf (out, t->short_rep);
      break;
    case TOKEN_OPERATOR:
      write_byte (out, calc_key_to_byte (t->op));
      break;
    }
  }
  return g_byte_array_free_to_bytes (out);
}

typedef struct {
  const guint8 *p;
  const guint8 *end;
  gboolean      error;
} Reader;

static guint8
read_byte (Reader *r)
{
  if (r->p >= r->end) {
    r->error = TRUE;
    return 0;
  }
  return *r->p++;
}

static gint32
read_int (Reader *r)
{
  guint32 n = 0;

  for (int i = 0; i < 4; i++)
    n = n << 8 | read_byte (r);
  return (gint32) n;
}

static char *
read_utf (Reader *r)
{
  gsize len = read_byte (r) << 8;
  len |= read_byte (r);
  if (r->error || (gsize) (r->end - r->p) < len) {
    r->error = TRUE;
    return g_strdup ("");
  }
  char *s = g_strndup ((const char *) r->p, len);
  r->p += len;
  return s;
}

CalcExpr *
calc_expr_new_from_bytes (const guint8 *bytes, gsize length)
{
  Reader r = { bytes, bytes + length, FALSE };
  g_autoptr (CalcExpr) expr = calc_expr_new ();
  gint32 size = read_int (&r);

  for (gint32 i = 0; i < size && !r.error; i++) {
    guint8 kind = read_byte (&r);
    if (kind >= 0x20) {   /* an operator's byte is a printable character */
      CalcKey op = calc_key_from_byte (kind);
      if (op == CALC_KEY_NONE)
        return NULL;
      g_ptr_array_add (expr->tokens, operator_new (op));
    } else if (kind == TOKEN_CONSTANT) {
      Token *c = constant_new ();
      g_autofree char *whole = read_utf (&r);
      g_string_assign (c->whole, whole);
      guint8 flags = read_byte (&r);
      if (flags & SAW_DECIMAL) {
        g_autofree char *fraction = read_utf (&r);
        c->saw_decimal = TRUE;
        g_string_assign (c->fraction, fraction);
      }
      if (flags & HAS_EXPONENT)
        c->exponent = read_int (&r);
      g_ptr_array_add (expr->tokens, c);
    } else if (kind == TOKEN_PRE_EVAL) {
      long index = read_int (&r);
      g_autofree char *short_rep = read_utf (&r);
      if (index == -1) {
        /* an old format's way to save a lone point (CalculatorExpr.newToken) */
        Token *c = constant_new ();
        constant_add (c, CALC_KEY_DEC_POINT);
        g_ptr_array_add (expr->tokens, c);
      } else {
        g_ptr_array_add (expr->tokens, pre_eval_new (index, short_rep));
      }
    } else {
      return NULL;   /* "Bad save file format" */
    }
  }
  return r.error ? NULL : g_steal_pointer (&expr);
}
