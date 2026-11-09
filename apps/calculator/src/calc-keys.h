/* calc-keys: the calculator's keys and how they are written, ExactCalculator's KeyMaps
 * (KeyMaps.java) and StringUtils.
 *
 * A key is what a button (or a typed or pasted character) adds to the expression. The expression
 * stores keys; this module says how each is shown in the formula, how it is saved (one byte, the
 * format of ExpressionDB's expression column), and what kind of operator it is.
 *
 * Results are computed with '.', ',' and '-' and shown with the locale's decimal point and grouping
 * separator and a real minus sign: calc_translate_result does that last step. */
#pragma once

#include <glib.h>

typedef enum {
  CALC_KEY_NONE = 0,
  CALC_KEY_DIGIT_0, CALC_KEY_DIGIT_1, CALC_KEY_DIGIT_2, CALC_KEY_DIGIT_3, CALC_KEY_DIGIT_4,
  CALC_KEY_DIGIT_5, CALC_KEY_DIGIT_6, CALC_KEY_DIGIT_7, CALC_KEY_DIGIT_8, CALC_KEY_DIGIT_9,
  CALC_KEY_DEC_POINT,
  CALC_KEY_CONST_PI,
  CALC_KEY_CONST_E,
  CALC_KEY_OP_SQRT,     /* √, a prefix */
  CALC_KEY_OP_FACT,     /* !, a suffix */
  CALC_KEY_OP_PCT,      /* %, a suffix */
  CALC_KEY_OP_SQR,      /* ², a suffix (the inverse of √) */
  CALC_KEY_FUN_SIN,
  CALC_KEY_FUN_COS,
  CALC_KEY_FUN_TAN,
  CALC_KEY_FUN_ARCSIN,
  CALC_KEY_FUN_ARCCOS,
  CALC_KEY_FUN_ARCTAN,
  CALC_KEY_FUN_LN,
  CALC_KEY_FUN_LOG,
  CALC_KEY_FUN_EXP,     /* eˣ, shown as exp( */
  CALC_KEY_FUN_10POW,   /* 10ˣ: not a key of its own, it adds "10^" (Evaluator.add10pow) */
  CALC_KEY_LPAREN,
  CALC_KEY_RPAREN,
  CALC_KEY_OP_POW,
  CALC_KEY_OP_MUL,
  CALC_KEY_OP_DIV,
  CALC_KEY_OP_ADD,
  CALC_KEY_OP_SUB,
} CalcKey;

#define CALC_NOT_DIGIT 10
#define CALC_ELLIPSIS "…"
/* Results are built as ASCII strings, one byte per character, as Java's strings had one char per
 * character: the index arithmetic of Evaluator and CalculatorResult then carries over unchanged.
 * Inside them the ellipsis is written '~', and becomes "…" only in calc_translate_result. */
#define CALC_ELLIPSIS_CHAR '~'
#define CALC_MINUS_SIGN "−"

/* how the key is shown in the formula: "7", "×", "sin(", "exp(", "²" (KeyMaps.toString) */
const char *calc_key_to_string (CalcKey key);
/* the byte that stands for the key in a saved expression (toByte, fromByte) */
char        calc_key_to_byte (CalcKey key);
CalcKey     calc_key_from_byte (char byte);

gboolean    calc_key_is_binary (CalcKey key);     /* ^ × ÷ + − */
gboolean    calc_key_is_trig_func (CalcKey key);
gboolean    calc_key_is_func (CalcKey key);       /* the trigonometric ones, ln, log, exp */
gboolean    calc_key_is_prefix (CalcKey key);     /* √ − */
gboolean    calc_key_is_suffix (CalcKey key);     /* ! % ² */
int         calc_key_digit_value (CalcKey key);   /* 0..9, or CALC_NOT_DIGIT */
CalcKey     calc_key_for_digit (int value);

/* the key a typed or pasted character stands for (keyForChar) */
CalcKey     calc_key_for_char (gunichar c);
/* the function named at s (up to its '('), e.g. "sin(" or "sqrt(" (funForString) */
CalcKey     calc_key_for_function_name (const char *s);

/* the locale's decimal point and grouping separator, as shown */
const char *calc_decimal_point (void);
const char *calc_grouping_separator (void);
/* a result as computed ("-1,234.5E-7") as shown: locale separators, − for -, E (translateResult);
 * a trailing 'e' is dropped, a ' ' (digit not yet known) becomes a figure space */
char       *calc_translate_result (const char *s);
/* s[begin, end) with ',' every three digits; leading '-' and ' ' are skipped (StringUtils.addCommas) */
char       *calc_add_commas (const char *s, int begin, int end);
