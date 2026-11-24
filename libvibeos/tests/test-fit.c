/* VibeosFitLabel's scale without a display, on real text in VibeOS's own font (the one in libvibeos/fonts, so the
 * build machine's fonts play no part): words that fit are not shrunk, a word a little too wide is shrunk just enough
 * and one far too wide only down to the smallest scale, and text with too many lines is shrunk until it has few
 * enough, the largest scale that does it. Soft hyphens play no part in the scale. */
#include <fontconfig/fontconfig.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#include "vibeos.h"

/* vibeos-fit-label.c */
extern double _vibeos_fit_label_scale (PangoLayout *layout, int width, int lines, double minimum);

#define MIN_SCALE VIBEOS_FIT_LABEL_MIN_SCALE
#define STEP      0.02      /* the scale's steps (vibeos-fit-label.c) */

static PangoContext *context;

static PangoLayout *
layout_new (const char *text)
{
  PangoLayout *layout = pango_layout_new (context);
  PangoFontDescription *font = pango_font_description_from_string ("VibeOS Roboto 16px");

  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
  pango_layout_set_text (layout, text, -1);
  return layout;
}

/* `text` on one line, in px */
static int
width_of (const char *text)
{
  g_autoptr (PangoLayout) layout = layout_new (text);
  int width;

  pango_layout_get_pixel_size (layout, &width, NULL);
  return width;
}

/* the lines `layout` takes `width` px wide */
static int
lines_at (PangoLayout *layout, double width)
{
  g_autoptr (PangoLayout) copy = pango_layout_copy (layout);

  pango_layout_set_wrap (copy, PANGO_WRAP_WORD);
  pango_layout_set_width (copy, (int) floor (width) * PANGO_SCALE);
  return pango_layout_get_line_count (copy);
}

static void
test_word (void)
{
  g_autoptr (PangoLayout) layout = layout_new ("Documents");
  int word = width_of ("Documents");
  double scale;

  g_assert_cmpint (word, >, 50);
  /* it fits: as it is */
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, word, 2, MIN_SCALE), ==, 1);
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, word + 40, 2, MIN_SCALE), ==, 1);
  /* a tenth too wide: a tenth smaller, and no smaller */
  scale = _vibeos_fit_label_scale (layout, (int) (word * 0.9), 2, MIN_SCALE);
  g_assert_cmpfloat (scale, <, 1);
  g_assert_cmpfloat (scale, >=, MIN_SCALE);
  g_assert_cmpfloat (word * scale, <=, (int) (word * 0.9) + 0.01);
  g_assert_cmpfloat (word * (scale + 0.01), >, (int) (word * 0.9));
  /* twice too wide: the smallest scale, where the word breaks */
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, word / 2, 2, MIN_SCALE), ==, MIN_SCALE);
  /* no width given yet */
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, 0, 2, MIN_SCALE), ==, 1);
  /* the widest word counts, wherever it is */
  {
    g_autoptr (PangoLayout) words = layout_new ("My Documents");

    scale = _vibeos_fit_label_scale (words, (int) (word * 0.9), 0, MIN_SCALE);
    g_assert_cmpfloat (word * scale, <=, (int) (word * 0.9) + 0.01);
  }
}

static void
test_lines (void)
{
  const char *text = "Recently used files and folders";
  g_autoptr (PangoLayout) layout = layout_new (text);
  int widest = width_of ("Recently"), full = width_of (text);
  gboolean shrunk_for_lines = FALSE;

  /* at every width from the widest word's to the whole text's: the largest scale at which the words fit and two
   * lines are enough, or the smallest scale */
  for (int width = widest; width <= full; width += 2)
    {
      double scale = _vibeos_fit_label_scale (layout, width, 2, MIN_SCALE);

      g_assert_cmpfloat (scale, <=, 1);
      g_assert_cmpfloat (scale, >=, MIN_SCALE);
      if (scale > MIN_SCALE)
        {
          g_assert_cmpint (lines_at (layout, width / scale), <=, 2);
          if (scale + STEP <= 1)
            g_assert_cmpint (lines_at (layout, width / (scale + STEP)), >, 2);
          shrunk_for_lines |= scale < 1;
        }
      else
        g_assert_cmpint (lines_at (layout, width / (MIN_SCALE + STEP)), >, 2);
      if (lines_at (layout, width) <= 2)
        g_assert_cmpfloat (scale, ==, 1);
    }
  g_assert_true (shrunk_for_lines);

  /* no limit on the lines: only the words count */
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, widest, 0, MIN_SCALE), ==, 1);
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, widest, -1, MIN_SCALE), ==, 1);
  /* one line for all of it in half its width: the smallest scale */
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, full / 2, 1, MIN_SCALE), ==, MIN_SCALE);
}

#define SHY "\302\255"

static void
test_soft_hyphen (void)
{
  g_autoptr (PangoLayout) layout = layout_new ("Doku" SHY "mente");
  int word = width_of ("Dokumente"), width = (int) (word * 0.9);
  double scale;

  /* a tenth too wide: drawn as much smaller as the word without the hyphen, rather than broken at it */
  scale = _vibeos_fit_label_scale (layout, width, 2, MIN_SCALE);
  g_assert_cmpfloat (scale, <, 1);
  g_assert_cmpfloat (word * scale, <=, width + 0.01);
  g_assert_cmpfloat (word * (scale + 0.01), >, width);
  /* twice too wide: the smallest scale, where it breaks at the hyphen */
  g_assert_cmpfloat (_vibeos_fit_label_scale (layout, word / 2, 2, MIN_SCALE), ==, MIN_SCALE);

  /* on the lines too: at every width, the scale of the same text without them */
  {
    g_autoptr (PangoLayout) hyphenated = layout_new ("Zuletzt ver" SHY "wendete Dateien");
    g_autoptr (PangoLayout) plain = layout_new ("Zuletzt verwendete Dateien");
    int full = width_of ("Zuletzt verwendete Dateien");

    for (int w = (int) (width_of ("verwendete") * 0.7); w <= full; w += 3)
      g_assert_cmpfloat (_vibeos_fit_label_scale (hyphenated, w, 2, MIN_SCALE), ==,
                         _vibeos_fit_label_scale (plain, w, 2, MIN_SCALE));
  }

  /* the attributes stay on their letters: the part after the hyphen in a font half as large again */
  {
    g_autoptr (PangoLayout) plain = layout_new ("Dokumente");
    g_autoptr (PangoAttrList) attrs = pango_attr_list_new ();
    g_autoptr (PangoAttrList) shifted = pango_attr_list_new ();
    PangoAttribute *larger = pango_attr_size_new_absolute (24 * PANGO_SCALE);
    int big;

    larger->start_index = strlen ("Doku");
    pango_attr_list_insert (attrs, pango_attribute_copy (larger));
    pango_layout_set_attributes (plain, attrs);
    pango_layout_get_pixel_size (plain, &big, NULL);
    g_assert_cmpint (big, >, word);

    larger->start_index = strlen ("Doku" SHY);
    pango_attr_list_insert (shifted, larger);
    pango_layout_set_attributes (layout, shifted);
    scale = _vibeos_fit_label_scale (layout, big, 2, MIN_SCALE);
    g_assert_cmpfloat (scale, ==, 1);
    scale = _vibeos_fit_label_scale (layout, (int) (big * 0.9), 2, MIN_SCALE);
    g_assert_cmpfloat (big * scale, <=, (int) (big * 0.9) + 0.01);
    g_assert_cmpfloat (big * (scale + 0.01), >, (int) (big * 0.9));
  }
}

static void
test_layout_kept (void)
{
  g_autoptr (PangoLayout) layout = layout_new ("Doku" SHY "mente");

  pango_layout_set_width (layout, 33 * PANGO_SCALE);
  pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  _vibeos_fit_label_scale (layout, 40, 2, MIN_SCALE);
  g_assert_cmpint (pango_layout_get_width (layout), ==, 33 * PANGO_SCALE);
  g_assert_cmpint (pango_layout_get_ellipsize (layout), ==, PANGO_ELLIPSIZE_END);
  g_assert_cmpint (pango_layout_get_wrap (layout), ==, PANGO_WRAP_WORD_CHAR);
  g_assert_cmpstr (pango_layout_get_text (layout), ==, "Doku" SHY "mente");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!FcConfigAppFontAddFile (NULL, (const FcChar8 *) FONT))
    g_error ("cannot load %s", FONT);
  context = pango_font_map_create_context (pango_cairo_font_map_get_default ());
  g_test_add_func ("/fit/word", test_word);
  g_test_add_func ("/fit/lines", test_lines);
  g_test_add_func ("/fit/soft-hyphen", test_soft_hyphen);
  g_test_add_func ("/fit/layout-kept", test_layout_kept);
  return g_test_run ();
}
