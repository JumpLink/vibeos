/* calc-expr: an expression as the user builds it, and its evaluation, ExactCalculator's
 * CalculatorExpr (CalculatorExpr.java).
 *
 * An expression is a list of tokens:
 *  - a constant: the digits typed so far ("12.5", "3E-7" after a paste);
 *  - an operator: any other key (calc-keys.h);
 *  - a pre-evaluated expression: an earlier result standing for itself, shown abbreviated
 *    ("1.4142135~E2"), evaluated by looking up the history entry it refers to. This is what the
 *    formula holds after "=" and then an operator: the result, not its digits.
 *
 * Evaluating parses the tokens by recursive descent (calc_expr_eval). The grammar has a few
 * calculator-specific rules: a missing ')' at the end is fine, juxtaposition multiplies ("2π",
 * "3(4)"), a trailing operator is ignored ("5×" is 5), and "N%" right after + or − means "N percent
 * of what came before" (100+5% is 105). */
#pragma once

#include "calc-keys.h"
#include "unified-real.h"

#include <glib.h>

typedef struct _CalcExpr CalcExpr;

/* how a nested evaluation finds the expressions and results that pre-evaluated tokens refer to
 * (CalculatorExpr.ExprResolver) */
typedef struct {
  CalcExpr    *(*get_expr) (gpointer data, long index);                 /* not a new reference */
  gboolean     (*get_degree_mode) (gpointer data, long index);
  UnifiedReal *(*get_result) (gpointer data, long index);               /* a new reference, or NULL */
  /* stores the result unless one is there already; returns the one stored (a new reference) */
  UnifiedReal *(*put_result_if_absent) (gpointer data, long index, UnifiedReal *result);
  gpointer       data;
} CalcExprResolver;

CalcExpr *calc_expr_new (void);
void      calc_expr_free (CalcExpr *expr);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CalcExpr, calc_expr_free)
CalcExpr *calc_expr_copy (CalcExpr *expr);
/* from the saved form (calc_expr_to_bytes); NULL if it is not one */
CalcExpr *calc_expr_new_from_bytes (const guint8 *bytes, gsize length);
/* the saved form: the format of ExpressionDB's expression column (DataOutputStream of write()) */
GBytes   *calc_expr_to_bytes (CalcExpr *expr);

/* adds a key; FALSE if it cannot go there (e.g. "×" right after "(") */
gboolean  calc_expr_add (CalcExpr *expr, CalcKey key);
/* sets the exponent of the trailing constant ("1.5E-7" pasted) */
void      calc_expr_add_exponent (CalcExpr *expr, int exponent);
void      calc_expr_remove_trailing_additive_operators (CalcExpr *expr);
/* appends another expression, with a × between two operands */
void      calc_expr_append (CalcExpr *expr, CalcExpr *other);
/* deletes the last digit or token */
void      calc_expr_delete (CalcExpr *expr);
void      calc_expr_clear (CalcExpr *expr);
gboolean  calc_expr_is_empty (CalcExpr *expr);
/* an expression of one pre-evaluated token (abbreviate) */
CalcExpr *calc_expr_new_pre_evaluated (long index, const char *short_rep);

gboolean  calc_expr_has_trailing_constant (CalcExpr *expr);
gboolean  calc_expr_has_trailing_binary (CalcExpr *expr);
gboolean  calc_expr_has_trailing_suffix (CalcExpr *expr);
gboolean  calc_expr_has_open_parentheses (CalcExpr *expr);
gboolean  calc_expr_has_trailing_left_paren (CalcExpr *expr);
gboolean  calc_expr_has_trailing_right_paren (CalcExpr *expr);
/* would evaluating show anything new? A lone number, or one with a leading minus, would not */
gboolean  calc_expr_has_interesting_ops (CalcExpr *expr);
gboolean  calc_expr_has_trig_funcs (CalcExpr *expr);
/* the indices of the pre-evaluated tokens, and theirs, in the order they must be evaluated:
 * those whose results the resolver does not have yet (getTransitivelyReferencedExprs) */
GArray   *calc_expr_get_referenced (CalcExpr *expr, const CalcExprResolver *resolver);

/* the value, or NULL with calc_failure () set */
UnifiedReal *calc_expr_eval (CalcExpr *expr, gboolean degree_mode, const CalcExprResolver *resolver);
/* evaluates the expression at index and stores its result through the resolver (nestedEval) */
UnifiedReal *calc_expr_nested_eval (long index, const CalcExprResolver *resolver);

/* as shown in the formula, e.g. "12+6×sin(" */
char     *calc_expr_to_string (CalcExpr *expr);
