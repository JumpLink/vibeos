/* calc-format: see calc-format.h. Ports of Evaluator's and CalculatorResult's static parts. */
#include "calc-format.h"
#include "calc-keys.h"

#include <math.h>
#include <string.h>

/* Evaluator's constants */
#define EXP_COST             3   /* the extra digits a short exponent costs */
#define SHORT_TARGET_LENGTH  8   /* the length getShortString aims for */
#define MAX_EXP_CHARS        8   /* the longest exponent a paste may carry */
/* CalculatorResult's: the extra digits type (1) scientific notation may hold */
#define SCI_NOTATION_EXTRA   1

/* Java's String.substring (begin, end) */
static char *
substring (const char *s, int begin, int end)
{
  return g_strndup (s + begin, end - begin);
}

static int
index_of (const char *s, char c)
{
  const char *p = strchr (s, c);
  return p ? (int) (p - s) : -1;
}

/* Java's Math.round for floats */
static int
round_float (float x)
{
  return (int) floorf (x + 0.5f);
}

float
calc_separator_chars (const CalcCharMetrics *m, const char *s, int len)
{
  int start = 0;

  while (start < len && !g_ascii_isdigit (s[start]))
    ++start;
  int n_digits = len - start;
  int n_separators = (n_digits - 1) / 3;
  return n_separators * m->grouping_separator_width_ratio;
}

int
calc_get_msd_index_of (const char *s)
{
  int len = strlen (s);
  int nonzero_index = -1;

  for (int i = 0; i < len; ++i) {
    char c = s[i];
    if (c != '-' && c != '.' && c != '0') {
      nonzero_index = i;
      break;
    }
  }
  if (nonzero_index >= 0 && (nonzero_index < len - 1 || s[nonzero_index] != '1'))
    return nonzero_index;
  return CALC_INVALID_MSD;
}

int
calc_get_lsd_offset (UnifiedReal *val, const char *cache, int dec_index)
{
  if (real_definitely_zero (val))
    return G_MININT;
  int result = real_digits_required (val);
  if (result == 0) {
    /* an integer: count its trailing zeroes */
    int i;
    for (i = -1; dec_index + i > 0 && cache[dec_index + i] == '0'; --i) {
    }
    result = i;
  }
  return result;
}

int
calc_get_preferred_prec (const char *cache, int msd, int last_digit_offset, const CalcCharMetrics *m)
{
  int line_length = m->max_chars;
  int whole_size = index_of (cache, '.');
  float raw_sep_chars = calc_separator_chars (m, cache, whole_size);
  float raw_sep_chars_no_decimal = raw_sep_chars - m->no_ellipsis_credit;
  float raw_sep_chars_with_decimal = raw_sep_chars_no_decimal - m->decimal_credit;
  int sep_chars_no_decimal = (int) ceilf (MAX (raw_sep_chars_no_decimal, 0.0f));
  int sep_chars_with_decimal = (int) ceilf (MAX (raw_sep_chars_with_decimal, 0.0f));
  int negative = cache[0] == '-' ? 1 : 0;

  /* don't display a decimal point if the result is an integer */
  if (last_digit_offset == 0)
    last_digit_offset = -1;
  if (last_digit_offset != G_MAXINT) {
    if (whole_size <= line_length - sep_chars_no_decimal && last_digit_offset <= 0)
      return -1;   /* the whole number fits, no point needed */
    if (last_digit_offset >= 0 && whole_size + last_digit_offset + 1 <= line_length - sep_chars_with_decimal)
      return last_digit_offset;   /* the exact result fits */
  }
  /* "0.000123" uses less space than "1.23E-4" with its exponent */
  if (msd > whole_size && msd <= whole_size + EXP_COST + 1)
    msd = whole_size - 1;
  if (msd > CALC_QUICK_MAX_RESULT_BITS)
    return line_length - 2;   /* a very small number: just show zeroes; scrolling shows the rest */
  /* the msd is the first character shown */
  int result = msd - whole_size + line_length - negative - 1;
  if (whole_size <= line_length - sep_chars_no_decimal) {
    /* the whole number will be shown with separators */
    if (whole_size < line_length - sep_chars_with_decimal)
      result -= sep_chars_with_decimal;
    else
      result -= sep_chars_no_decimal;
  }
  return result;
}

char *
calc_get_short_string (const char *cache, int msd_index, int lsd_offset)
{
  int len = strlen (cache);
  int dot_index = index_of (cache, '.');
  int negative = cache[0] == '-' ? 1 : 0;
  const char *negative_sign = negative ? "-" : "";

  /* a lone '1' at the very end might be a rounded 0.999... */
  if (msd_index >= len - SHORT_TARGET_LENGTH)
    msd_index = CALC_INVALID_MSD;
  if (msd_index == CALC_INVALID_MSD) {
    if (lsd_offset < CALC_INIT_PREC)
      return g_strdup ("0");
    return g_strdup ("0.00000~");   /* SHORT_UNCERTAIN_ZERO */
  }
  /* an integer with up to 7 trailing zeroes is shown whole */
  if (lsd_offset < -1 && dot_index - msd_index + negative <= SHORT_TARGET_LENGTH
      && lsd_offset >= -CALC_MAX_TRAILING_ZEROES - 1)
    lsd_offset = -1;
  if (msd_index > dot_index) {
    if (msd_index <= dot_index + EXP_COST + 1) {
      /* "0.00123" is not longer than with an exponent */
      msd_index = dot_index - 1;
    } else if (lsd_offset <= SHORT_TARGET_LENGTH - negative - 2
               && lsd_offset <= CALC_MAX_LEADING_ZEROES + 1) {
      /* the whole fraction fits */
      msd_index = dot_index - 1;
    }
  }
  int exponent = dot_index - msd_index;
  if (exponent > 0)
    exponent--;   /* the decimal point goes after the first digit */
  if (lsd_offset != G_MAXINT) {
    int lsd_index = dot_index + lsd_offset;
    int total_digits = lsd_index - msd_index + negative + 1;
    if (total_digits <= SHORT_TARGET_LENGTH && dot_index > msd_index && lsd_offset >= -1) {
      /* fits, no exponent needed */
      g_autofree char *whole = calc_add_commas (cache, msd_index, dot_index);
      g_autofree char *fraction = substring (cache, dot_index, lsd_index + 1);
      return g_strconcat (negative_sign, whole, fraction, NULL);
    }
    if (total_digits <= SHORT_TARGET_LENGTH - 3) {
      g_autofree char *digits = substring (cache, msd_index + 1, lsd_index + 1);
      return g_strdup_printf ("%s%c.%sE%d", negative_sign, cache[msd_index], digits, exponent);
    }
  }
  /* the result is not exactly representable in this space: show an ellipsis */
  if (dot_index > msd_index && dot_index < msd_index + SHORT_TARGET_LENGTH - negative - 1) {
    /* no exponent needed */
    g_autofree char *whole = calc_add_commas (cache, msd_index, dot_index);
    g_autofree char *fraction = substring (cache, dot_index, msd_index + SHORT_TARGET_LENGTH - negative - 1);
    return g_strdup_printf ("%s%s%s~", negative_sign, whole, fraction);
  }
  g_autofree char *digits = substring (cache, msd_index + 1, msd_index + SHORT_TARGET_LENGTH - negative - 4);
  return g_strdup_printf ("%s%c.%s~E%d", negative_sign, cache[msd_index], digits, exponent);
}

char *
calc_unflip_zeroes (const char *old_digits, int old_prec_offset, const char *new_digits, int new_prec_offset)
{
  int old_len = strlen (old_digits);
  if (old_digits[old_len - 1] != '9')
    return g_strdup (new_digits);
  int new_len = strlen (new_digits);
  int prec_diff = new_prec_offset - old_prec_offset;
  int old_last_in_new = new_len - 1 - prec_diff;
  if (new_digits[old_last_in_new] != '0')
    return g_strdup (new_digits);
  /* the new digits must be all zeroes past the old ones, or the old approximation was wrong */
  for (int i = new_len - prec_diff; i < new_len; i++)
    if (new_digits[i] != '0')
      g_warning ("a new approximation invalidates the old one");
  GString *result = g_string_new (old_digits);
  for (int i = 0; i < prec_diff; i++)
    g_string_append_c (result, '9');
  return g_string_free (result, FALSE);
}

int
calc_exponent_end (const gunichar *s, int len, int offset)
{
  int i = offset;

  if (i >= len - 1 || s[i] != 'E')
    return offset;
  ++i;
  if (calc_key_for_char (s[i]) == CALC_KEY_OP_SUB)
    ++i;
  if (i == len || !g_unichar_isdigit (s[i]))
    return offset;
  ++i;
  while (i < len && g_unichar_isdigit (s[i])) {
    ++i;
    if (i > offset + MAX_EXP_CHARS)
      return offset;
  }
  return i;
}

/* --- the result line (CalculatorResult) */

int
calc_exp_len (int exp)
{
  if (exp == 0)
    return 0;
  int abs_exp_digits = (int) ceil (log10 (fabs ((double) exp)) + 0.0000000001);   /* 10 has 2 digits */
  return abs_exp_digits + (exp >= 0 ? 1 : 2);
}

int
calc_result_layout_char_offset (CalcResultLayout *l, int pos)
{
  return round_float (pos / l->char_width);
}

void
calc_result_layout_init (CalcResultLayout *l, int init_prec_offset, int msd_index, int lsd_offset,
                         const char *truncated_whole_part)
{
  int max_chars = l->metrics.max_chars;
  l->whole_len = strlen (truncated_whole_part);
  int n_separator_chars = (int) ceilf (calc_separator_chars (&l->metrics, truncated_whole_part, l->whole_len)
                                       - l->metrics.no_ellipsis_credit - 0.0001f);
  l->whole_part_fits = l->whole_len + n_separator_chars <= max_chars;
  l->last_pos = CALC_MAX_RIGHT_SCROLL + 10000;   /* INVALID */
  l->lsd_offset = lsd_offset;
  l->append_exponent = FALSE;
  /* prevent scrolling past the initial position, which is the msd as far left as it goes */
  l->current_pos = l->min_pos = round_float (init_prec_offset * l->char_width);
  if (msd_index == CALC_INVALID_MSD) {
    /* possibly zero: scrolling may show more */
    if (lsd_offset == G_MININT) {
      /* exactly zero */
      l->max_pos = l->min_pos;
      l->max_char_offset = round_float (l->max_pos / l->char_width);
      l->scrollable = FALSE;
    } else {
      l->max_pos = l->max_char_offset = CALC_MAX_RIGHT_SCROLL;
      l->min_pos = (int) (l->min_pos - l->char_width);   /* room for a minus sign that may appear */
      l->scrollable = TRUE;
    }
    return;
  }
  int negative = truncated_whole_part[0] == '-' ? 1 : 0;
  if (msd_index > l->whole_len && msd_index <= l->whole_len + 3)
    msd_index = l->whole_len - 1;   /* "0.0012" does not need an exponent */
  int min_char_offset = msd_index - l->whole_len;
  /* the offset of the leftmost digit to show; we show up to MAX_LEADING_ZEROES after the point */
  if (min_char_offset > -1 && min_char_offset < CALC_MAX_LEADING_ZEROES + 2)
    min_char_offset = -1;
  if (lsd_offset < CALC_MAX_RIGHT_SCROLL) {
    l->max_char_offset = lsd_offset;
    if (l->max_char_offset < -1 && l->max_char_offset > -(CALC_MAX_TRAILING_ZEROES + 2))
      l->max_char_offset = -1;
    /* the length of the scientific notation exponent that is required */
    int current_exp_len = 0;
    if (l->max_char_offset < -1)
      current_exp_len = calc_exp_len (-min_char_offset - 1);
    else if (min_char_offset > -1 || l->max_char_offset >= max_chars)
      current_exp_len = calc_exp_len (-min_char_offset);
    int separator_length = l->whole_part_fits && min_char_offset < -3 ? n_separator_chars : 0;
    l->scrollable = l->max_char_offset + current_exp_len + separator_length - min_char_offset + negative >= max_chars;
    int new_max_char_offset;
    if (current_exp_len > 0) {
      if (l->scrollable)
        new_max_char_offset = l->max_char_offset + calc_exp_len (-lsd_offset);   /* the exponent at the right end */
      else
        new_max_char_offset = l->max_char_offset + current_exp_len;
      if (l->max_char_offset <= -1 && new_max_char_offset > -1)
        l->max_char_offset = -1;   /* the exponent replaces the trailing zeroes */
      else
        l->max_char_offset = MIN (new_max_char_offset, CALC_MAX_RIGHT_SCROLL);
      l->max_pos = MIN (round_float (l->max_char_offset * l->char_width), CALC_MAX_RIGHT_SCROLL);
    } else if (!l->whole_part_fits && !l->scrollable) {
      /* the whole number does not fit with separators, but does without: an exponent then */
      l->scrollable = l->max_char_offset + calc_exp_len (-min_char_offset - 1) - min_char_offset + negative >= max_chars;
      if (l->scrollable) {
        l->max_pos = (int) ceilf (l->min_pos + l->char_width);   /* one character of scroll */
      } else {
        l->max_pos = l->min_pos;
        l->append_exponent = TRUE;
      }
    } else {
      l->max_pos = MIN (round_float (l->max_char_offset * l->char_width), CALC_MAX_RIGHT_SCROLL);
    }
    if (!l->scrollable)
      l->current_pos = l->max_pos;   /* the right end: the whole result */
  } else {
    l->max_pos = l->max_char_offset = CALC_MAX_RIGHT_SCROLL;
    l->scrollable = TRUE;
  }
}

/* the first significant digit, a lone '1' included (getNaiveMsdIndexOf) */
static int
naive_msd_index_of (const char *s)
{
  for (int i = 0; s[i]; ++i)
    if (s[i] != '-' && s[i] != '.' && s[i] != '0')
      return i;
  return CALC_INVALID_MSD;
}

char *
calc_result_layout_format (CalcResultLayout *l, const char *in, int prec_offset, int max_digs,
                           gboolean truncated, gboolean negative, int *last_displayed_offset,
                           gboolean force_precision, gboolean force_sci_notation, gboolean insert_commas)
{
  int minus_space = negative ? 1 : 0;
  int msd_index = truncated ? -1 : naive_msd_index_of (in);   /* CALC_INVALID_MSD is fine */
  GString *result = g_string_new (in);
  gboolean need_ellipsis = FALSE;

  if (truncated || (negative && result->str[0] != '-')) {
    need_ellipsis = TRUE;
    result->str[0] = CALC_ELLIPSIS_CHAR;   /* may be removed again for type (1) notation */
  }
  int dec_index = index_of (result->str, '.');
  if (last_displayed_offset)
    *last_displayed_offset = prec_offset;

  if (force_sci_notation
      || ((dec_index == -1 || (msd_index != CALC_INVALID_MSD && msd_index - dec_index > CALC_MAX_LEADING_ZEROES + 1))
          && prec_offset != -1)) {
    /* Either the msd is too far to the right, or the whole number does not fit: scientific
     * notation. Type (1): the msd is shown, with one digit before the point ("1.2345E10"). Type
     * (2): it is not, the exponent says where the right end is ("~3456E10"). */
    int init_exponent = prec_offset > 0 ? -prec_offset : -prec_offset - 1;
    int exponent = init_exponent;
    gboolean has_point = FALSE;
    if (!truncated && msd_index < max_digs - 1
        && (int) result->len - msd_index + 1 + minus_space <= max_digs + SCI_NOTATION_EXTRA) {
      /* type (1): the leading digit is in the window */
      if (dec_index > msd_index)
        g_string_erase (result, dec_index, 1);
      int res_len = result->len;
      g_autofree char *fraction = substring (result->str, msd_index + 1, res_len);
      char lead = result->str[msd_index];
      g_string_printf (result, "%s%c.%s", negative ? "-" : "", lead, fraction);
      /* the exponent was right for a point at the right of the fraction */
      exponent = init_exponent + res_len - msd_index - 1;
      has_point = TRUE;
    }
    if (!force_precision) {
      /* drop as many digits as the exponent needs room for */
      int drop_digits;
      if (has_point) {
        drop_digits = calc_exp_len (exponent);
        if (drop_digits >= (int) result->len - 1)
          drop_digits = MAX ((int) result->len - 2, 0);   /* keep the point and the leading digit */
      } else {
        for (drop_digits = 2; calc_exp_len (init_exponent + drop_digits) > drop_digits; ++drop_digits) {
        }
        exponent = init_exponent + drop_digits;
        /* no trailing zero shown just because it is free: "...1E10", not "...10E9" */
        if (prec_offset - drop_digits > l->lsd_offset) {
          ++drop_digits;
          ++exponent;
        }
      }
      if (drop_digits >= (int) result->len - 1) {
        /* the display is too small for anything meaningful */
        g_string_free (result, TRUE);
        return g_strdup ("~E~");
      }
      g_string_truncate (result, result->len - drop_digits);
      if (last_displayed_offset)
        *last_displayed_offset -= drop_digits;
    }
    g_string_append_printf (result, "E%d", exponent);
  } else if (insert_commas) {
    /* the whole number fits, with separators: insert them, and drop digits at the left (an
     * ellipsis in their place) to make room for them */
    int whole_start = need_ellipsis ? 1 : 0;
    int orig_length = result->len;
    float n_comma_chars;
    g_autofree char *grouped = NULL;
    if (dec_index != -1) {
      n_comma_chars = calc_separator_chars (&l->metrics, result->str, dec_index);
      g_autofree char *whole = calc_add_commas (result->str, whole_start, dec_index);
      grouped = g_strconcat (whole, result->str + dec_index, NULL);
    } else {
      n_comma_chars = calc_separator_chars (&l->metrics, result->str, orig_length);
      grouped = calc_add_commas (result->str, whole_start, orig_length);
    }
    g_string_assign (result, grouped);
    if (need_ellipsis)
      orig_length -= 1;   /* the ellipsis does not count */
    float len = orig_length + n_comma_chars;
    int deleted_chars = 0;
    float effective_len = len - (dec_index == -1 ? 0 : l->metrics.decimal_credit);
    float ellipsis_adjustment = need_ellipsis ? l->metrics.no_exponent_credit : l->metrics.no_ellipsis_credit;
    if (effective_len - ellipsis_adjustment > (float) (max_digs - whole_start) + 0.0001f && !force_precision) {
      float deleted_width = 0.0f;
      while (effective_len - l->metrics.no_exponent_credit - deleted_width > (float) (max_digs - 1)) {
        if (result->str[deleted_chars] == ',')
          deleted_width += l->metrics.grouping_separator_width_ratio;
        else
          deleted_width += 1.0f;
        deleted_chars++;
      }
    }
    if (deleted_chars > 0) {
      g_string_erase (result, 0, deleted_chars);
      g_string_prepend_c (result, CALC_ELLIPSIS_CHAR);
    } else if (need_ellipsis) {
      g_string_prepend_c (result, CALC_ELLIPSIS_CHAR);
    }
  }
  return g_string_free (result, FALSE);
}
