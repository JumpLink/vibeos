/* calc-format: how a result's digits are chosen and laid out, the pure parts of ExactCalculator's
 * Evaluator (getPreferredPrec, getShortString, getMsdIndexOf, getLsdOffset, unflipZeroes,
 * exponentEnd) and CalculatorResult (initPositions, formatResult, expLen).
 *
 * The result line shows a window onto an infinite decimal string. Positions are counted in
 * characters from the decimal point ("offsets": 1 is the first decimal, -1 the units digit; the
 * "precision offset" of a string is the offset of its last digit) and in pixels for scrolling.
 * The cached digits are a "result string" like "-12.3400" with a known number of decimals.
 *
 * All strings here are ASCII ("-", ".", ",", "E", ' ' for a digit not known yet, '~' for the
 * ellipsis: calc-keys.h); calc_translate_result makes them presentable. */
#pragma once

#include "unified-real.h"

#include <glib.h>

#define CALC_INVALID_MSD          G_MAXINT
#define CALC_MAX_LEADING_ZEROES   6         /* CalculatorResult.MAX_LEADING_ZEROES */
#define CALC_MAX_TRAILING_ZEROES  6
#define CALC_MAX_RIGHT_SCROLL     10000000
#define CALC_INIT_PREC            50        /* Evaluator.INIT_PREC */
#define CALC_QUICK_MAX_RESULT_BITS 150000

/* what the result line can hold, in digit widths (Evaluator.CharMetricsInfo, as measured by
 * CalculatorResult.onMeasure) */
typedef struct {
  int   max_chars;
  float grouping_separator_width_ratio;   /* a ',' in digit widths */
  float decimal_credit;                   /* a digit's width less the point's */
  float no_ellipsis_credit;               /* room saved when no ellipsis is shown */
  float no_exponent_credit;               /* room saved when no exponent is shown */
} CalcCharMetrics;

/* the width of the separators the first len characters of s need, in digit widths */
float calc_separator_chars (const CalcCharMetrics *m, const char *s, int len);

/* the index of the first significant digit; CALC_INVALID_MSD if none is known (a final lone '1'
 * may be a rounded 0.999..., so it does not count) */
int   calc_get_msd_index_of (const char *s);
/* the offset of the last non-zero digit if it is known, G_MAXINT if the result never ends,
 * G_MININT for 0 (getLsdOffset) */
int   calc_get_lsd_offset (UnifiedReal *val, const char *cache, int dec_index);
/* the precision offset to show first (getPreferredPrec) */
int   calc_get_preferred_prec (const char *cache, int msd, int last_digit_offset, const CalcCharMetrics *m);
/* the short form shown for a result in an expression, e.g. "1.4142~", "6.02E23" (getShortString) */
char *calc_get_short_string (const char *cache, int msd_index, int lsd_offset);
/* the new digits, with 9s kept where a more precise result turned ...999 into ...000 (unflipZeroes) */
char *calc_unflip_zeroes (const char *old_digits, int old_prec_offset, const char *new_digits, int new_prec_offset);
/* where an exponent like "E-12" at offset ends; offset if there is none (exponentEnd) */
int   calc_exponent_end (const gunichar *s, int len, int offset);

/* the scrolling state of a result line (CalculatorResult's fields) */
typedef struct {
  CalcCharMetrics metrics;
  float    char_width;          /* px */
  int      last_pos;            /* px, the position already displayed */
  int      min_pos;             /* px, the leftmost position */
  int      max_pos;             /* px, the rightmost position */
  int      current_pos;         /* px, the right edge relative to the decimal point */
  int      whole_len;           /* digits of the whole part */
  int      max_char_offset;     /* offset of the rightmost digit that can be scrolled to */
  int      lsd_offset;          /* offset of the last non-zero digit, or G_MAXINT */
  int      last_displayed_offset;
  gboolean whole_part_fits;     /* no scientific notation needed for the first display */
  gboolean append_exponent;
  gboolean scrollable;
} CalcResultLayout;

/* the characters an exponent "E-12" takes (expLen) */
int   calc_exp_len (int exp);
/* sets the layout up for a new result (initPositions) */
void  calc_result_layout_init (CalcResultLayout *l, int init_prec_offset, int msd_index, int lsd_offset,
                               const char *truncated_whole_part);
/* the text shown for the digits `in` (from evaluator_get_string), as ASCII (formatResult) */
char *calc_result_layout_format (CalcResultLayout *l, const char *in, int prec_offset, int max_digs,
                                 gboolean truncated, gboolean negative, int *last_displayed_offset,
                                 gboolean force_precision, gboolean force_sci_notation, gboolean insert_commas);
int   calc_result_layout_char_offset (CalcResultLayout *l, int pos);
