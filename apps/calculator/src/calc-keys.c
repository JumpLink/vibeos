/* calc-keys: see calc-keys.h */
#include "calc-keys.h"

#include <locale.h>
#include <string.h>

typedef struct {
  CalcKey     key;
  const char *text;   /* in the formula (KeyMaps.toString) */
  char        byte;   /* in a saved expression (KeyMaps.toByte) */
} KeyInfo;

static const KeyInfo keys[] = {
  { CALC_KEY_DIGIT_0, "0", 0 }, { CALC_KEY_DIGIT_1, "1", 0 }, { CALC_KEY_DIGIT_2, "2", 0 },
  { CALC_KEY_DIGIT_3, "3", 0 }, { CALC_KEY_DIGIT_4, "4", 0 }, { CALC_KEY_DIGIT_5, "5", 0 },
  { CALC_KEY_DIGIT_6, "6", 0 }, { CALC_KEY_DIGIT_7, "7", 0 }, { CALC_KEY_DIGIT_8, "8", 0 },
  { CALC_KEY_DIGIT_9, "9", 0 },
  { CALC_KEY_DEC_POINT, NULL, 0 },   /* the locale's: calc_decimal_point () */
  { CALC_KEY_CONST_PI, "π", 'p' },
  { CALC_KEY_CONST_E, "e", 'e' },
  { CALC_KEY_OP_SQRT, "√", 'r' },
  { CALC_KEY_OP_FACT, "!", '!' },
  { CALC_KEY_OP_PCT, "%", '%' },
  { CALC_KEY_OP_SQR, "²", '2' },   /* in the formula: the button's "x²" would not do */
  { CALC_KEY_FUN_SIN, "sin(", 's' },
  { CALC_KEY_FUN_COS, "cos(", 'c' },
  { CALC_KEY_FUN_TAN, "tan(", 't' },
  { CALC_KEY_FUN_ARCSIN, "sin⁻¹(", 'S' },
  { CALC_KEY_FUN_ARCCOS, "cos⁻¹(", 'C' },
  { CALC_KEY_FUN_ARCTAN, "tan⁻¹(", 'T' },
  { CALC_KEY_FUN_LN, "ln(", 'l' },
  { CALC_KEY_FUN_LOG, "log(", 'L' },
  { CALC_KEY_FUN_EXP, "exp(", 'E' },    /* in the formula: the button's "eˣ" would not do */
  { CALC_KEY_FUN_10POW, "", 0 },
  { CALC_KEY_LPAREN, "(", '(' },
  { CALC_KEY_RPAREN, ")", ')' },
  { CALC_KEY_OP_POW, "^", '^' },
  { CALC_KEY_OP_MUL, "×", '*' },
  { CALC_KEY_OP_DIV, "÷", '/' },
  { CALC_KEY_OP_ADD, "+", '+' },
  { CALC_KEY_OP_SUB, CALC_MINUS_SIGN, '-' },
};

static const KeyInfo *
key_info (CalcKey key)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    if (keys[i].key == key)
      return &keys[i];
  return NULL;
}

const char *
calc_key_to_string (CalcKey key)
{
  const KeyInfo *info = key_info (key);

  if (key == CALC_KEY_DEC_POINT)
    return calc_decimal_point ();
  return info && info->text ? info->text : "";
}

char
calc_key_to_byte (CalcKey key)
{
  const KeyInfo *info = key_info (key);

  return info ? info->byte : 0;
}

CalcKey
calc_key_from_byte (char byte)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    if (keys[i].byte && keys[i].byte == byte)
      return keys[i].key;
  return CALC_KEY_NONE;
}

gboolean
calc_key_is_binary (CalcKey key)
{
  return key == CALC_KEY_OP_POW || key == CALC_KEY_OP_MUL || key == CALC_KEY_OP_DIV || key == CALC_KEY_OP_ADD
      || key == CALC_KEY_OP_SUB;
}

gboolean
calc_key_is_trig_func (CalcKey key)
{
  return key == CALC_KEY_FUN_SIN || key == CALC_KEY_FUN_COS || key == CALC_KEY_FUN_TAN
      || key == CALC_KEY_FUN_ARCSIN || key == CALC_KEY_FUN_ARCCOS || key == CALC_KEY_FUN_ARCTAN;
}

gboolean
calc_key_is_func (CalcKey key)
{
  return calc_key_is_trig_func (key) || key == CALC_KEY_FUN_LN || key == CALC_KEY_FUN_LOG
      || key == CALC_KEY_FUN_EXP;
}

gboolean
calc_key_is_prefix (CalcKey key)
{
  return key == CALC_KEY_OP_SQRT || key == CALC_KEY_OP_SUB;
}

gboolean
calc_key_is_suffix (CalcKey key)
{
  return key == CALC_KEY_OP_FACT || key == CALC_KEY_OP_PCT || key == CALC_KEY_OP_SQR;
}

int
calc_key_digit_value (CalcKey key)
{
  if (key >= CALC_KEY_DIGIT_0 && key <= CALC_KEY_DIGIT_9)
    return key - CALC_KEY_DIGIT_0;
  return CALC_NOT_DIGIT;
}

CalcKey
calc_key_for_digit (int value)
{
  if (value >= 0 && value <= 9)
    return CALC_KEY_DIGIT_0 + value;
  return CALC_KEY_NONE;
}

/* --- the locale's separators (DecimalFormatSymbols) */

typedef struct {
  char decimal_point[8];
  char grouping_separator[8];
} Separators;

static const Separators *
separators (void)
{
  static Separators s;
  static gsize done;

  if (g_once_init_enter (&done)) {
    const struct lconv *lc = localeconv ();
    const char *decimal = lc->decimal_point && *lc->decimal_point ? lc->decimal_point : ".";
    const char *grouping = lc->thousands_sep && *lc->thousands_sep ? lc->thousands_sep : NULL;
    /* the C locale has no grouping separator, and a few locales group with a space that would read
     * as a gap: Java's DecimalFormatSymbols has ',' or '.' for them */
    if (!grouping || g_unichar_isspace (g_utf8_get_char (grouping)) || strcmp (grouping, decimal) == 0)
      grouping = strcmp (decimal, ",") == 0 ? "." : ",";
    /* Arabic's own decimal separator: Calculator.getDecimalSeparator uses ',' for it */
    if (strcmp (decimal, "٫") == 0)
      decimal = ",";
    g_strlcpy (s.decimal_point, decimal, sizeof s.decimal_point);
    g_strlcpy (s.grouping_separator, grouping, sizeof s.grouping_separator);
    g_once_init_leave (&done, 1);
  }
  return &s;
}

const char *
calc_decimal_point (void)
{
  return separators ()->decimal_point;
}

const char *
calc_grouping_separator (void)
{
  return separators ()->grouping_separator;
}

/* --- typed and pasted text */

CalcKey
calc_key_for_char (gunichar c)
{
  if (g_unichar_isdigit (c))
    return calc_key_for_digit (g_unichar_digit_value (c));
  switch (c) {
  case '.':
  case ',':
    return CALC_KEY_DEC_POINT;
  case '-':
  case 0x2212:   /* MINUS SIGN */
    return CALC_KEY_OP_SUB;
  case '+':
    return CALC_KEY_OP_ADD;
  case '*':
  case 0x00D7:   /* MULTIPLICATION SIGN */
    return CALC_KEY_OP_MUL;
  case '/':
  case 0x00F7:   /* DIVISION SIGN */
    return CALC_KEY_OP_DIV;
  /* function names are not localized, so they cannot start with an 'e' or a 'p' */
  case 'e':
  case 'E':
    return CALC_KEY_CONST_E;
  case 'p':
  case 'P':
  case 0x03C0:   /* GREEK SMALL LETTER PI */
    return CALC_KEY_CONST_PI;
  case '^':
    return CALC_KEY_OP_POW;
  case '!':
    return CALC_KEY_OP_FACT;
  case '%':
    return CALC_KEY_OP_PCT;
  case '(':
    return CALC_KEY_LPAREN;
  case ')':
    return CALC_KEY_RPAREN;
  default:
    if (c == g_utf8_get_char (calc_decimal_point ()))
      return CALC_KEY_DEC_POINT;
    return CALC_KEY_NONE;
  }
}

CalcKey
calc_key_for_function_name (const char *s)
{
  static const struct {
    const char *name;
    CalcKey     key;
  } functions[] = {
    { "sin", CALC_KEY_FUN_SIN }, { "cos", CALC_KEY_FUN_COS }, { "tan", CALC_KEY_FUN_TAN },
    { "arcsin", CALC_KEY_FUN_ARCSIN }, { "arccos", CALC_KEY_FUN_ARCCOS }, { "arctan", CALC_KEY_FUN_ARCTAN },
    { "asin", CALC_KEY_FUN_ARCSIN }, { "acos", CALC_KEY_FUN_ARCCOS }, { "atan", CALC_KEY_FUN_ARCTAN },
    { "ln", CALC_KEY_FUN_LN }, { "log", CALC_KEY_FUN_LOG }, { "sqrt", CALC_KEY_OP_SQRT },
    /* and the buttons' labels (addButtonToFunMap) */
    { "sin⁻¹", CALC_KEY_FUN_ARCSIN }, { "cos⁻¹", CALC_KEY_FUN_ARCCOS },
    { "tan⁻¹", CALC_KEY_FUN_ARCTAN },
  };
  const char *paren = strchr (s, '(');

  if (!paren)
    return CALC_KEY_NONE;
  for (guint i = 0; i < G_N_ELEMENTS (functions); i++)
    if (strlen (functions[i].name) == (gsize) (paren - s) && strncmp (s, functions[i].name, paren - s) == 0)
      return functions[i].key;
  return CALC_KEY_NONE;
}

/* --- results */

char *
calc_translate_result (const char *s)
{
  GString *result = g_string_new (NULL);

  for (const char *p = s; *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    gboolean last = *g_utf8_next_char (p) == '\0';
    switch (c) {
    case 'e':
      /* KeyMaps drops a trailing 'e', though its comment says "a trailing e is treated as the
       * mathematical constant, not an exponent": kept here, as meant, so that "2e" stays "2e" */
      g_string_append (result, last ? "e" : "E");
      break;
    case 'E':
      g_string_append_c (result, 'E');
      break;
    case ' ':
      g_string_append (result, " ");   /* FIGURE SPACE: a digit not computed yet */
      break;
    case CALC_ELLIPSIS_CHAR:
      g_string_append (result, CALC_ELLIPSIS);
      break;
    case ',':
      g_string_append (result, calc_grouping_separator ());
      break;
    case '-':
      g_string_append (result, CALC_MINUS_SIGN);
      break;
    case '.':
      g_string_append (result, calc_decimal_point ());
      break;
    default:
      g_string_append_unichar (result, c);
      break;
    }
  }
  return g_string_free (result, FALSE);
}

char *
calc_add_commas (const char *s, int begin, int end)
{
  GString *result = g_string_new (NULL);
  int current = begin;

  while (current < end && (s[current] == '-' || s[current] == ' '))
    ++current;
  g_string_append_len (result, s + begin, current - begin);
  while (current < end) {
    g_string_append_c (result, s[current]);
    ++current;
    if ((end - current) % 3 == 0 && end != current)
      g_string_append_c (result, ',');
  }
  return g_string_free (result, FALSE);
}
