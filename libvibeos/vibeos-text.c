/* vibeos-text: Font size, Bold text and the stylesheets that follow them. See vibeos-text.h. */
#include "vibeos-text.h"
#include "vibeos-style.h"

#include <math.h>
#include <string.h>

/* ---- the font-size curve --------------------------------------------------------------------------------------- *
 *
 * A curve is the px of a few text sizes at one font size; sizes between them are interpolated, sizes under the
 * smallest one interpolated from 0, and sizes over the largest one keep its ratio (1: they do not grow). Font sizes
 * between two curves interpolate between them; under the first one, and over the last one, sp is plain
 * multiplication. The arithmetic is in single precision, as the curve was made, so that its values come out the
 * same. */

#define N_SIZES 9

static const float curve_sizes[N_SIZES] = { 8, 10, 12, 14, 18, 20, 24, 30, 100 };

static const struct {
  int   key;                    /* the font size times 100 */
  float px[N_SIZES];
} curves[] = {
  { 105, { 8.4f, 10.5f, 12.6f, 14.8f, 18.6f, 20.6f, 24.4f, 30, 100 } },
  { 110, { 8.8f, 11, 13.2f, 15.6f, 19.2f, 21.2f, 24.8f, 30, 100 } },
  { 115, { 9.2f, 11.5f, 13.8f, 16.4f, 19.8f, 21.8f, 25.2f, 30, 100 } },
  { 120, { 9.6f, 12, 14.4f, 17.2f, 20.4f, 22.4f, 25.6f, 30, 100 } },
  { 130, { 10.4f, 13, 15.6f, 18.8f, 21.6f, 23.6f, 26.4f, 30, 100 } },
  { 150, { 12, 15, 18, 22, 24, 26, 28, 30, 100 } },
  { 180, { 14.4f, 18, 21.6f, 24.4f, 27.6f, 30.8f, 32.8f, 34.8f, 100 } },
  { 200, { 16, 20, 24, 26, 30, 34, 36, 38, 100 } },
};

/* the curve starts a little under its first font size: below that, sp is plain multiplication */
#define CURVE_START (curves[0].key / 100.0f - 0.01f)

static float
lerp (float from, float to, float t)
{
  return from + (to - from) * t;
}

/* where `value` is between `from` and `to`, 0 to 1 */
static float
lerp_inverse (float from, float to, float value)
{
  return from != to ? CLAMP ((value - from) / (to - from), 0.0f, 1.0f) : 0.0f;
}

/* sp through one curve */
static float
curve_px (const float *px, float sp)
{
  float size = fabsf (sp), sign = sp < 0 ? -1.0f : sp > 0 ? 1.0f : 0.0f;
  float from_sp = 0, to_sp, from_px = 0, to_px;
  int below = -1;

  for (int i = 0; i < N_SIZES; i++)
    {
      if (curve_sizes[i] == size)
        return sign * px[i];
      if (curve_sizes[i] < size)
        below = i;
    }
  if (below == N_SIZES - 1)
    return sp * (px[N_SIZES - 1] / curve_sizes[N_SIZES - 1]);
  if (below >= 0)
    {
      from_sp = curve_sizes[below];
      from_px = px[below];
    }
  to_sp = curve_sizes[below + 1];
  to_px = px[below + 1];
  return sign * lerp (from_px, to_px, lerp_inverse (from_sp, to_sp, size));
}

double
vibeos_sp_at_scale (double sp, double scale)
{
  float s = (float) scale, value = (float) sp;
  int key, higher;
  float t, px[N_SIZES];

  if (s < CURVE_START)
    return sp * scale;
  key = (int) (s * 100.0f);
  for (higher = 0; higher < (int) G_N_ELEMENTS (curves) && curves[higher].key < key; higher++)
    ;
  if (higher < (int) G_N_ELEMENTS (curves) && curves[higher].key == key)
    return curve_px (curves[higher].px, value);
  /* between two curves: each size's px interpolated between theirs, then the curve that makes */
  if (higher == 0 || higher == (int) G_N_ELEMENTS (curves))
    return sp * scale;
  t = lerp_inverse (curves[higher - 1].key / 100.0f, curves[higher].key / 100.0f, s);
  for (int i = 0; i < N_SIZES; i++)
    px[i] = lerp (curve_px (curves[higher - 1].px, curve_sizes[i]), curve_px (curves[higher].px, curve_sizes[i]), t);
  return curve_px (px, value);
}

/* ---- stylesheets ----------------------------------------------------------------------------------------------- */

#define BOLD_EXTRA 300

static int
weight_for (int weight, gboolean bold)
{
  return CLAMP (bold ? weight + BOLD_EXTRA : weight, 1, 1000);
}

static gboolean
is_name_char (char c)
{
  return g_ascii_isalnum (c) || c == '-' || c == '_' || (guchar) c >= 0x80;
}

static gboolean
is_number_start (const char *p)
{
  if (*p == '+' || *p == '-')
    p++;
  return g_ascii_isdigit (p[0]) || (p[0] == '.' && g_ascii_isdigit (p[1]));
}

static void
append_px (GString *out, double px)
{
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];
  char *end;

  g_ascii_formatd (buffer, sizeof buffer, "%.4f", px);
  end = buffer + strlen (buffer) - 1;
  while (*end == '0')
    *end-- = '\0';
  if (*end == '.')
    *end = '\0';
  g_string_append (out, strcmp (buffer, "-0") ? buffer : "0");
  g_string_append (out, "px");
}

/* the end of a quoted string that starts at `p` */
static const char *
string_end (const char *p)
{
  char quote = *p++;

  while (*p && *p != quote)
    p += p[0] == '\\' && p[1] ? 2 : 1;
  return *p ? p + 1 : p;
}

char *
vibeos_css_convert (const char *css, double scale, gboolean bold)
{
  GString *out;
  const char *p = css;
  int depth = 0;
  gboolean declaration_start = FALSE;   /* the next name may be a property's */
  gboolean font_weight = FALSE;         /* in a font-weight declaration */

  g_return_val_if_fail (css != NULL, NULL);
  out = g_string_sized_new (strlen (css) + 64);
  while (*p)
    {
      const char *start = p;

      if (p[0] == '/' && p[1] == '*')
        {
          const char *end = strstr (p + 2, "*/");

          p = end ? end + 2 : p + strlen (p);
          g_string_append_len (out, start, p - start);
        }
      else if (*p == '"' || *p == '\'')
        {
          p = string_end (p);
          g_string_append_len (out, start, p - start);
        }
      else if (*p == '{' || *p == '}' || *p == ';')
        {
          depth += *p == '{' ? 1 : *p == '}' ? -1 : 0;
          declaration_start = depth > 0;
          font_weight = FALSE;
          g_string_append_c (out, *p++);
        }
      else if (is_number_start (p))
        {
          /* a sign, digits, a point and digits: no exponent, nothing hexadecimal */
          const char *end = p + (*p == '+' || *p == '-'), *unit;
          g_autofree char *number = NULL;
          double value;

          while (g_ascii_isdigit (*end))
            end++;
          if (end[0] == '.' && g_ascii_isdigit (end[1]))
            for (end++; g_ascii_isdigit (*end); end++)
              ;
          number = g_strndup (p, end - p);
          value = g_ascii_strtod (number, NULL);
          unit = end;
          while (g_ascii_isalpha (*end) || *end == '%')
            end++;
          if (end - unit == 2 && g_ascii_strncasecmp (unit, "sp", 2) == 0)
            append_px (out, vibeos_sp_at_scale (value, scale));
          else if (end - unit == 2 && g_ascii_strncasecmp (unit, "dp", 2) == 0)
            append_px (out, value);
          else if (unit == end && font_weight && bold && value == floor (value))
            g_string_append_printf (out, "%d", weight_for ((int) value, TRUE));
          else
            g_string_append_len (out, start, end - start);
          p = end;
          declaration_start = FALSE;
        }
      else if (is_name_char (*p) || *p == '#' || *p == '@' || *p == '.')
        {
          /* a name (a property, a keyword, a class, a colour's hex): copied whole, so that no number in it is
           * taken for a size */
          const char *after;
          gsize length;

          p++;
          while (is_name_char (*p))
            p++;
          length = p - start;
          for (after = p; *after == ' ' || *after == '\t' || *after == '\n' || *after == '\r'; after++)
            ;
          if (length == 3 && g_ascii_strncasecmp (start, "url", 3) == 0 && *p == '(')
            {
              /* a URL is copied as it is, up to its parenthesis */
              while (*p && *p != ')')
                p = *p == '"' || *p == '\'' ? string_end (p) : p + 1;
              if (*p)
                p++;
              g_string_append_len (out, start, p - start);
            }
          else if (declaration_start && *after == ':')
            {
              font_weight = length == 11 && g_ascii_strncasecmp (start, "font-weight", 11) == 0;
              g_string_append_len (out, start, length);
            }
          else if (font_weight && bold && length == 6 && g_ascii_strncasecmp (start, "normal", 6) == 0)
            g_string_append_printf (out, "%d", weight_for (400, TRUE));
          else if (font_weight && bold && length == 4 && g_ascii_strncasecmp (start, "bold", 4) == 0)
            g_string_append_printf (out, "%d", weight_for (700, TRUE));
          else
            g_string_append_len (out, start, length);
          declaration_start = FALSE;
        }
      else
        g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

/* ---- the settings ---------------------------------------------------------------------------------------------- */

typedef struct {
  GtkCssProvider *provider;
  char           *source;
} Sheet;

struct _VibeosText {
  GObject    parent;
  gboolean   following;         /* the app follows the settings (VIBEOS_INIT_SCALED_TEXT) */
  GSettings *interface;         /* org.gnome.desktop.interface, NULL without its schema */
  GSettings *settings;          /* vibeos.Interface, NULL without its schema */
  double     scale;             /* in effect */
  gboolean   bold;
  GArray    *sheets;
};

G_DEFINE_FINAL_TYPE (VibeosText, vibeos_text, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static VibeosText *default_text;

#define MIN_SCALE 0.5
#define MAX_SCALE 3.0

/* libvibeos's own stylesheet, converted again (vibeos-style.c) */
G_GNUC_INTERNAL void vibeos_style_reload (void);

static GSettings *
settings_if_installed (const char *id, const char *key)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, id, TRUE) : NULL;

  return schema && g_settings_schema_has_key (schema, key) ? g_settings_new_full (schema, NULL, NULL) : NULL;
}

/* the settings in effect: fixed (1, bold) in an app that does not follow them, else the environment's or the
 * settings' */
static void
read_settings (VibeosText *self, double *scale, gboolean *bold)
{
  const char *env;

  *scale = 1;
  *bold = TRUE;
  if (!self->following)
    return;
  if (self->interface)
    *scale = g_settings_get_double (self->interface, "text-scaling-factor");
  if (self->settings)
    *bold = g_settings_get_boolean (self->settings, "bold-text");
  env = g_getenv ("VIBEOS_FONT_SCALE");
  if (env && *env)
    {
      char *end;
      double value = g_ascii_strtod (env, &end);

      if (*end || !(value > 0))
        g_warning ("VIBEOS_FONT_SCALE=%s: not a font size (0.5 to 3)", env);
      else
        *scale = value;
    }
  env = g_getenv ("VIBEOS_BOLD_TEXT");
  if (env && *env)
    {
      if (g_str_equal (env, "1") || g_ascii_strcasecmp (env, "true") == 0 || g_ascii_strcasecmp (env, "on") == 0)
        *bold = TRUE;
      else if (g_str_equal (env, "0") || g_ascii_strcasecmp (env, "false") == 0 || g_ascii_strcasecmp (env, "off") == 0)
        *bold = FALSE;
      else
        g_warning ("VIBEOS_BOLD_TEXT=%s: not 1 or 0", env);
    }
  *scale = CLAMP (*scale, MIN_SCALE, MAX_SCALE);
}

static void
load_sheet (VibeosText *self, Sheet *sheet)
{
  g_autofree char *css = vibeos_css_convert (sheet->source, self->scale, self->bold);

  gtk_css_provider_load_from_string (sheet->provider, css);
}

/* takes the settings in again: if what is in effect changed, converts every stylesheet again and tells the app */
static void
update (VibeosText *self)
{
  double scale;
  gboolean bold;

  read_settings (self, &scale, &bold);
  if (scale == self->scale && !bold == !self->bold)
    return;
  self->scale = scale;
  self->bold = bold;
  for (guint i = 0; i < self->sheets->len; i++)
    load_sheet (self, &g_array_index (self->sheets, Sheet, i));
  vibeos_style_reload ();
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
on_setting_changed (GSettings *settings, const char *key, VibeosText *self)
{
  update (self);
}

/* vibeos_init_full's VIBEOS_INIT_SCALED_TEXT; exported for the tests, which have no display to init */
void _vibeos_text_set_following (gboolean following);

void
_vibeos_text_set_following (gboolean following)
{
  VibeosText *self = vibeos_text_get_default ();

  if (!following == !self->following)
    return;
  self->following = following;
  if (following && !self->interface && !self->settings)
    {
      self->interface = settings_if_installed ("org.gnome.desktop.interface", "text-scaling-factor");
      self->settings = settings_if_installed ("vibeos.Interface", "bold-text");
      if (self->interface)
        g_signal_connect (self->interface, "changed::text-scaling-factor", G_CALLBACK (on_setting_changed), self);
      if (self->settings)
        g_signal_connect (self->settings, "changed::bold-text", G_CALLBACK (on_setting_changed), self);
    }
  update (self);
}

/* every widget measured again: those that size their text themselves read vibeos_sp () as they measure */
static void
queue_resize_all (GtkWidget *widget)
{
  gtk_widget_queue_resize (widget);
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    queue_resize_all (child);
}

/* the class handler, after the app's: every window laid out again */
static void
vibeos_text_changed (VibeosText *self)
{
  GListModel *windows;

  if (!gdk_display_get_default ())
    return;
  windows = gtk_window_get_toplevels ();
  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (windows, i);

      queue_resize_all (window);
    }
}

static void
clear_sheet (Sheet *sheet)
{
  g_clear_object (&sheet->provider);
  g_clear_pointer (&sheet->source, g_free);
}

static void
vibeos_text_dispose (GObject *object)
{
  VibeosText *self = VIBEOS_TEXT (object);

  g_clear_object (&self->interface);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->sheets, g_array_unref);
  G_OBJECT_CLASS (vibeos_text_parent_class)->dispose (object);
}

static void
vibeos_text_class_init (VibeosTextClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = vibeos_text_dispose;
  signals[CHANGED] = g_signal_new_class_handler ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                                 G_CALLBACK (vibeos_text_changed), NULL, NULL, NULL,
                                                 G_TYPE_NONE, 0);
}

static void
vibeos_text_init (VibeosText *self)
{
  self->scale = 1;
  self->bold = TRUE;
  self->sheets = g_array_new (FALSE, TRUE, sizeof (Sheet));
  g_array_set_clear_func (self->sheets, (GDestroyNotify) clear_sheet);
}

VibeosText *
vibeos_text_get_default (void)
{
  if (!default_text)
    default_text = g_object_new (VIBEOS_TYPE_TEXT, NULL);
  return default_text;
}

double
vibeos_font_scale (void)
{
  return vibeos_text_get_default ()->scale;
}

gboolean
vibeos_bold_text (void)
{
  return vibeos_text_get_default ()->bold;
}

double
vibeos_sp (double sp)
{
  return vibeos_sp_at_scale (sp, vibeos_text_get_default ()->scale);
}

int
vibeos_font_weight (int weight)
{
  return weight_for (weight, vibeos_text_get_default ()->bold);
}

GtkCssProvider *
vibeos_css_load_string (const char *css, guint priority)
{
  VibeosText *self = vibeos_text_get_default ();
  GdkDisplay *display = gdk_display_get_default ();
  Sheet sheet;

  g_return_val_if_fail (css != NULL, NULL);
  g_return_val_if_fail (display != NULL, NULL);
  sheet.provider = gtk_css_provider_new ();
  sheet.source = g_strdup (css);
  load_sheet (self, &sheet);
  gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (sheet.provider), priority);
  g_array_append_val (self->sheets, sheet);
  return sheet.provider;
}

GtkCssProvider *
vibeos_css_load_resource (const char *resource_path, guint priority)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = g_resources_lookup_data (resource_path, 0, &error);
  g_autofree char *css = NULL;

  if (!bytes)
    {
      g_critical ("vibeos_css_load_resource: %s", error->message);
      return NULL;
    }
  css = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
  return vibeos_css_load_string (css, priority);
}

/* ---- labels ---------------------------------------------------------------------------------------------------- */

#define PADDING_SP "vibeos-font-padding-sp"

static void
apply_font_padding (GtkWidget *label)
{
  double *sp = g_object_get_data (G_OBJECT (label), PADDING_SP);

  vibeos_label_set_font_padding (label, vibeos_sp (*sp));
}

void
vibeos_label_set_font_padding_sp (GtkWidget *label, double sp)
{
  double *stored = g_object_get_data (G_OBJECT (label), PADDING_SP);

  g_return_if_fail (GTK_IS_LABEL (label));
  if (!stored)
    {
      stored = g_new (double, 1);
      g_object_set_data_full (G_OBJECT (label), PADDING_SP, stored, g_free);
      g_signal_connect_object (vibeos_text_get_default (), "changed", G_CALLBACK (apply_font_padding), label,
                               G_CONNECT_SWAPPED);
    }
  *stored = sp;
  apply_font_padding (label);
}
