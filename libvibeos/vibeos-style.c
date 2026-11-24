/* vibeos-style: the theme's colours, the stylesheet and the window frame. See vibeos-style.h. */
#include "vibeos-style.h"
#include "vibeos-text.h"

#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <tonal.h>

/* ---- the colours ------------------------------------------------------------------------------------------------ *
 *
 * Each VibeosColor is a role of the theme's scheme, or a fixed colour with a light and a dark value. Its CSS name is
 * its enum name in lower case: VIBEOS_ON_SURFACE is @vibeos_on_surface. */

#define FIXED (-1)

static const struct {
  const char *css;
  int         role;             /* a TonalRole, or FIXED */
  guint32     light, dark;      /* a fixed colour's values */
} colors[VIBEOS_N_COLORS] = {
  [VIBEOS_PRIMARY]                    = { "vibeos_primary", TONAL_PRIMARY },
  [VIBEOS_ON_PRIMARY]                 = { "vibeos_on_primary", TONAL_ON_PRIMARY },
  [VIBEOS_PRIMARY_CONTAINER]          = { "vibeos_primary_container", TONAL_PRIMARY_CONTAINER },
  [VIBEOS_ON_PRIMARY_CONTAINER]       = { "vibeos_on_primary_container", TONAL_ON_PRIMARY_CONTAINER },
  [VIBEOS_INVERSE_PRIMARY]            = { "vibeos_inverse_primary", TONAL_INVERSE_PRIMARY },
  [VIBEOS_SECONDARY]                  = { "vibeos_secondary", TONAL_SECONDARY },
  [VIBEOS_ON_SECONDARY]               = { "vibeos_on_secondary", TONAL_ON_SECONDARY },
  [VIBEOS_SECONDARY_CONTAINER]        = { "vibeos_secondary_container", TONAL_SECONDARY_CONTAINER },
  [VIBEOS_ON_SECONDARY_CONTAINER]     = { "vibeos_on_secondary_container", TONAL_ON_SECONDARY_CONTAINER },
  [VIBEOS_TERTIARY]                   = { "vibeos_tertiary", TONAL_TERTIARY },
  [VIBEOS_ON_TERTIARY]                = { "vibeos_on_tertiary", TONAL_ON_TERTIARY },
  [VIBEOS_TERTIARY_CONTAINER]         = { "vibeos_tertiary_container", TONAL_TERTIARY_CONTAINER },
  [VIBEOS_ON_TERTIARY_CONTAINER]      = { "vibeos_on_tertiary_container", TONAL_ON_TERTIARY_CONTAINER },
  [VIBEOS_ERROR]                      = { "vibeos_error", TONAL_ERROR },
  [VIBEOS_ERROR_CONTAINER]            = { "vibeos_error_container", TONAL_ERROR_CONTAINER },
  [VIBEOS_SURFACE]                    = { "vibeos_surface", TONAL_SURFACE },
  [VIBEOS_SURFACE_CONTAINER_LOWEST]   = { "vibeos_surface_container_lowest", TONAL_SURFACE_CONTAINER_LOWEST },
  [VIBEOS_SURFACE_CONTAINER_LOW]      = { "vibeos_surface_container_low", TONAL_SURFACE_CONTAINER_LOW },
  [VIBEOS_SURFACE_CONTAINER]          = { "vibeos_surface_container", TONAL_SURFACE_CONTAINER },
  [VIBEOS_SURFACE_CONTAINER_HIGH]     = { "vibeos_surface_container_high", TONAL_SURFACE_CONTAINER_HIGH },
  [VIBEOS_SURFACE_CONTAINER_HIGHEST]  = { "vibeos_surface_container_highest", TONAL_SURFACE_CONTAINER_HIGHEST },
  [VIBEOS_SURFACE_BRIGHT]             = { "vibeos_surface_bright", TONAL_SURFACE_BRIGHT },
  [VIBEOS_ON_SURFACE]                 = { "vibeos_on_surface", TONAL_ON_SURFACE },
  [VIBEOS_ON_SURFACE_VARIANT]         = { "vibeos_on_surface_variant", TONAL_ON_SURFACE_VARIANT },
  [VIBEOS_OUTLINE]                    = { "vibeos_outline", TONAL_OUTLINE },
  [VIBEOS_OUTLINE_VARIANT]            = { "vibeos_outline_variant", TONAL_OUTLINE_VARIANT },
  [VIBEOS_INVERSE_SURFACE]            = { "vibeos_inverse_surface", TONAL_INVERSE_SURFACE },
  [VIBEOS_WHITE]                      = { "vibeos_white", FIXED, 0xffffff, 0xffffff },
  [VIBEOS_ON_ERROR]                   = { "vibeos_on_error", TONAL_ON_ERROR },
  [VIBEOS_ON_ERROR_CONTAINER]         = { "vibeos_on_error_container", TONAL_ON_ERROR_CONTAINER },
  [VIBEOS_BACKGROUND]                 = { "vibeos_background", TONAL_BACKGROUND },
  [VIBEOS_ON_BACKGROUND]              = { "vibeos_on_background", TONAL_ON_BACKGROUND },
  [VIBEOS_SURFACE_DIM]                = { "vibeos_surface_dim", TONAL_SURFACE_DIM },
  [VIBEOS_SURFACE_VARIANT]            = { "vibeos_surface_variant", TONAL_SURFACE_VARIANT },
  [VIBEOS_INVERSE_ON_SURFACE]         = { "vibeos_inverse_on_surface", TONAL_INVERSE_ON_SURFACE },
  [VIBEOS_SURFACE_TINT]               = { "vibeos_surface_tint", TONAL_SURFACE_TINT },
  [VIBEOS_SHADOW]                     = { "vibeos_shadow", TONAL_SHADOW },
  [VIBEOS_SCRIM]                      = { "vibeos_scrim", TONAL_SCRIM },
  [VIBEOS_PRIMARY_FIXED]              = { "vibeos_primary_fixed", TONAL_PRIMARY_FIXED },
  [VIBEOS_PRIMARY_FIXED_DIM]          = { "vibeos_primary_fixed_dim", TONAL_PRIMARY_FIXED_DIM },
  [VIBEOS_ON_PRIMARY_FIXED]           = { "vibeos_on_primary_fixed", TONAL_ON_PRIMARY_FIXED },
  [VIBEOS_ON_PRIMARY_FIXED_VARIANT]   = { "vibeos_on_primary_fixed_variant", TONAL_ON_PRIMARY_FIXED_VARIANT },
  [VIBEOS_SECONDARY_FIXED]            = { "vibeos_secondary_fixed", TONAL_SECONDARY_FIXED },
  [VIBEOS_SECONDARY_FIXED_DIM]        = { "vibeos_secondary_fixed_dim", TONAL_SECONDARY_FIXED_DIM },
  [VIBEOS_ON_SECONDARY_FIXED]         = { "vibeos_on_secondary_fixed", TONAL_ON_SECONDARY_FIXED },
  [VIBEOS_ON_SECONDARY_FIXED_VARIANT] = { "vibeos_on_secondary_fixed_variant", TONAL_ON_SECONDARY_FIXED_VARIANT },
  [VIBEOS_TERTIARY_FIXED]             = { "vibeos_tertiary_fixed", TONAL_TERTIARY_FIXED },
  [VIBEOS_TERTIARY_FIXED_DIM]         = { "vibeos_tertiary_fixed_dim", TONAL_TERTIARY_FIXED_DIM },
  [VIBEOS_ON_TERTIARY_FIXED]          = { "vibeos_on_tertiary_fixed", TONAL_ON_TERTIARY_FIXED },
  [VIBEOS_ON_TERTIARY_FIXED_VARIANT]  = { "vibeos_on_tertiary_fixed_variant", TONAL_ON_TERTIARY_FIXED_VARIANT },
  /* fixed colours, as the reference devices draw them; where only the dark one is on record, it stands for both */
  [VIBEOS_DIALOG_TITLE]               = { "vibeos_dialog_title", FIXED, 0x1e1e1f, 0xffffff },
  [VIBEOS_CATEGORY_NETWORK]           = { "vibeos_category_network", FIXED, 0xbde9ff, 0x67d4ff },
  [VIBEOS_ON_CATEGORY_NETWORK]        = { "vibeos_on_category_network", FIXED, 0x004d68, 0x004d68 },
  [VIBEOS_CATEGORY_SYSTEM]            = { "vibeos_category_system", FIXED, 0xe3e3e3, 0xc7c7c7 },
  [VIBEOS_ON_CATEGORY_SYSTEM]         = { "vibeos_on_category_system", FIXED, 0x474747, 0x474747 },
  [VIBEOS_CALL]                       = { "vibeos_call", FIXED, 0x5bb974, 0x5bb974 },
  [VIBEOS_ON_CALL]                    = { "vibeos_on_call", FIXED, 0x202124, 0x202124 },
  [VIBEOS_MISSED_CALL]                = { "vibeos_missed_call", FIXED, 0xf2b8b5, 0xf2b8b5 },
  [VIBEOS_SEARCH_CURSOR]              = { "vibeos_search_cursor", FIXED, 0x03dac6, 0x03dac6 },
  [VIBEOS_EMOJI_TAB_SELECTED]         = { "vibeos_emoji_tab_selected", FIXED, 0x37474f, 0xd4d4d5 },
  [VIBEOS_EMOJI_TAB]                  = { "vibeos_emoji_tab", FIXED, 0xbbbdc5, 0x5e5e62 },
  /* the light circles of security and accessibility are not on record: the dark one's hue at tone 90, with the
   * chroma the recorded light circles have against their dark ones */
  [VIBEOS_CATEGORY_APPS]              = { "vibeos_category_apps", FIXED, 0xd0e4ff, 0xa1c9ff },
  [VIBEOS_ON_CATEGORY_APPS]           = { "vibeos_on_category_apps", FIXED, 0x04409f, 0x04409f },
  [VIBEOS_CATEGORY_NOTIFICATIONS]     = { "vibeos_category_notifications", FIXED, 0xffd8ef, 0xffaee4 },
  [VIBEOS_ON_CATEGORY_NOTIFICATIONS]  = { "vibeos_on_category_notifications", FIXED, 0x8d0053, 0x8d0053 },
  [VIBEOS_CATEGORY_DISPLAY]           = { "vibeos_category_display", FIXED, 0xffdcc3, 0xffb683 },
  [VIBEOS_ON_CATEGORY_DISPLAY]        = { "vibeos_on_category_display", FIXED, 0x753403, 0x753403 },
  [VIBEOS_CATEGORY_BATTERY]           = { "vibeos_category_battery", FIXED, 0xbeefbb, 0x80da88 },
  [VIBEOS_ON_CATEGORY_BATTERY]        = { "vibeos_on_category_battery", FIXED, 0x00522c, 0x00522c },
  [VIBEOS_CATEGORY_SECURITY]          = { "vibeos_category_security", FIXED, 0xbbeaf9, 0x60d5f3 },
  [VIBEOS_ON_CATEGORY_SECURITY]       = { "vibeos_on_category_security", FIXED, 0x004e5d, 0x004e5d },
  [VIBEOS_CATEGORY_ACCESSIBILITY]     = { "vibeos_category_accessibility", FIXED, 0xeedcfe, 0xd9bafd },
  [VIBEOS_ON_CATEGORY_ACCESSIBILITY]  = { "vibeos_on_category_accessibility", FIXED, 0x5629a4, 0x5629a4 },
};

/* colours only the stylesheet uses, by mode: the navigation bar's unselected items are a translucent white on the
 * dark bar */
static const struct {
  const char *css;
  const char *light, *dark;
} css_colors[] = {
  { "vibeos_nav_item", "@vibeos_on_surface_variant", "alpha(@vibeos_white, 0.5)" },
};

/* the theme without settings: the colours the apps had before they had a theme */
#define DEFAULT_SEED 0x007c88

/* ---- the theme ------------------------------------------------------------------------------------------------- */

/* what a theme is computed from */
typedef struct {
  guint32      seed;
  TonalVariant variant;
  double       contrast;
  gboolean     pure_black;
  gboolean     high_contrast;   /* Accessibility's High contrast: the contrast at its top, outlines (vibeos.css) */
} Source;

struct _VibeosTheme {
  GObject          parent;
  VibeosInitFlags  flags;
  gboolean         initialized;         /* vibeos_init_full has run: there is a display to style */
  GSettings       *settings;            /* vibeos.Theme, NULL without its schema */
  GSettings       *interface;           /* org.gnome.desktop.interface, NULL without its schema */
  GSettings       *a11y;                /* org.gnome.desktop.a11y.interface, NULL without its high-contrast key */
  GtkCssProvider  *provider;

  VibeosScheme     scheme;              /* the app's own choice of light or dark (vibeos_theme_set_scheme) */
  gboolean         computed;
  Source           source;
  gboolean         dark;
  TonalTheme       theme;
  guint32          rgb[VIBEOS_N_COLORS];
};

G_DEFINE_FINAL_TYPE (VibeosTheme, vibeos_theme, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static VibeosTheme *default_theme;

static gboolean
parse_rgb (const char *text, guint32 *rgb)
{
  char *end;
  guint64 value;

  if (text[0] == '#')
    text++;
  else if (g_str_has_prefix (text, "0x"))
    text += 2;
  value = g_ascii_strtoull (text, &end, 16);
  if (strlen (text) != 6 || *end)
    return FALSE;
  *rgb = (guint32) value;
  return TRUE;
}

/* light or dark: VIBEOS_COLOR_SCHEME, else the app's own choice (vibeos_theme_set_scheme), else dark for an app that
 * is always dark or without vibeos.Theme, else color-scheme */
static gboolean
want_dark (VibeosTheme *self)
{
  const char *env = g_getenv ("VIBEOS_COLOR_SCHEME");
  g_autofree char *scheme = NULL;

  if (env && *env)
    {
      if (g_str_equal (env, "dark") || g_str_equal (env, "prefer-dark"))
        return TRUE;
      if (g_str_equal (env, "light") || g_str_equal (env, "prefer-light") || g_str_equal (env, "default"))
        return FALSE;
      g_warning ("VIBEOS_COLOR_SCHEME=%s: not light or dark", env);
    }
  if (self->scheme != VIBEOS_SCHEME_SYSTEM)
    return self->scheme == VIBEOS_SCHEME_DARK;
  if ((self->flags & VIBEOS_INIT_ALWAYS_DARK) || !self->settings || !self->interface)
    return TRUE;
  scheme = g_settings_get_string (self->interface, "color-scheme");
  return g_strcmp0 (scheme, "prefer-dark") == 0;
}

static void
read_source (VibeosTheme *self, Source *source)
{
  const char *env;

  *source = (Source) { DEFAULT_SEED, TONAL_VARIANT_TONAL_SPOT, 0, FALSE, FALSE };
  if (self->settings)
    {
      g_autofree char *style = g_settings_get_string (self->settings, "style");

      source->seed = g_settings_get_uint (self->settings, "seed") & 0xffffff;
      tonal_variant_from_name (style, &source->variant);
      source->contrast = g_settings_get_double (self->settings, "contrast");
      source->pure_black = g_settings_get_boolean (self->settings, "pure-black");
    }
  env = g_getenv ("VIBEOS_SEED");
  if (env && *env && !parse_rgb (env, &source->seed))
    g_warning ("VIBEOS_SEED=%s: not a colour (#RRGGBB)", env);
  env = g_getenv ("VIBEOS_CONTRAST");
  if (env && *env)
    source->contrast = CLAMP (g_ascii_strtod (env, NULL), -1.0, 1.0);
  /* High contrast is Material's high contrast level (1, Android's own for it), whatever contrast the theme has */
  source->high_contrast = self->a11y && g_settings_get_boolean (self->a11y, "high-contrast");
  env = g_getenv ("VIBEOS_HIGH_CONTRAST");
  if (env && *env)
    source->high_contrast = g_str_equal (env, "1") || g_str_equal (env, "true");
  if (source->high_contrast)
    source->contrast = 1.0;
}

static gboolean
source_equal (const Source *a, const Source *b)
{
  return a->seed == b->seed && a->variant == b->variant && a->contrast == b->contrast
         && !a->pure_black == !b->pure_black && !a->high_contrast == !b->high_contrast;
}

/* every widget of every window redrawn: the ones that draw themselves read their colours as they draw */
static void
queue_draw_all (GtkWidget *widget)
{
  gtk_widget_queue_draw (widget);
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    queue_draw_all (child);
}

static void
load_stylesheet (VibeosTheme *self)
{
  g_autoptr (GString) css = g_string_new (NULL);
  g_autoptr (GBytes) sheet = g_resources_lookup_data ("/vibeos/libvibeos/vibeos.css", 0, NULL);
  g_autofree char *source = g_strndup (g_bytes_get_data (sheet, NULL), g_bytes_get_size (sheet));
  /* its sizes in sp and dp, its weights before Bold text: in px and weights for the font settings in effect */
  g_autofree char *converted = vibeos_css_convert (source, vibeos_font_scale (), vibeos_bold_text ());

  /* the colours first, as @define-color lines, then the stylesheet that uses them */
  for (int i = 0; i < VIBEOS_N_COLORS; i++)
    g_string_append_printf (css, "@define-color %s #%06x;\n", colors[i].css, self->rgb[i]);
  for (gsize i = 0; i < G_N_ELEMENTS (css_colors); i++)
    g_string_append_printf (css, "@define-color %s %s;\n", css_colors[i].css,
                            self->dark ? css_colors[i].dark : css_colors[i].light);
  g_string_append (css, converted);
  gtk_css_provider_load_from_string (self->provider, css->str);

  /* GTK's own widgets in the same mode */
#if GTK_CHECK_VERSION (4, 20, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme",
                self->dark ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", self->dark, NULL);
#endif
#if GTK_CHECK_VERSION (4, 20, 0)
  /* and in the same contrast: the stylesheets' @media (prefers-contrast: more) rules, libadwaita's high contrast */
  g_object_set (gtk_settings_get_default (), "gtk-interface-contrast",
                self->source.high_contrast ? GTK_INTERFACE_CONTRAST_MORE : GTK_INTERFACE_CONTRAST_NO_PREFERENCE, NULL);
#endif
}

/* Computes the colours again if what they come from changed; then, with a display, restyles and redraws */
static void
update (VibeosTheme *self)
{
  gboolean dark = want_dark (self);
  Source source;
  const guint32 *roles;

  read_source (self, &source);
  if (self->computed && dark == self->dark && source_equal (&source, &self->source))
    return;
  if (!self->computed || !source_equal (&source, &self->source))
    tonal_theme_compute (&self->theme, source.seed, source.variant, source.contrast, source.pure_black);
  self->source = source;
  self->dark = dark;
  roles = dark ? self->theme.dark : self->theme.light;
  for (int i = 0; i < VIBEOS_N_COLORS; i++)
    self->rgb[i] = (colors[i].role == FIXED ? (dark ? colors[i].dark : colors[i].light) : roles[colors[i].role])
                   & 0xffffff;
  if (!self->computed)
    {
      self->computed = TRUE;
      return;
    }
  if (!self->initialized)
    {
      g_signal_emit (self, signals[CHANGED], 0);
      return;
    }

  load_stylesheet (self);
  g_signal_emit (self, signals[CHANGED], 0);
  {
    GListModel *windows = gtk_window_get_toplevels ();

    for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
      {
        g_autoptr (GtkWidget) window = g_list_model_get_item (windows, i);

        queue_draw_all (window);
      }
  }
}

static void
on_settings_changed (GSettings *settings, const char *key, VibeosTheme *self)
{
  update (self);
}

/* a schema the system has, or NULL */
static GSettings *
settings_if_installed (const char *id)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, id, TRUE) : NULL;

  return schema ? g_settings_new_full (schema, NULL, NULL) : NULL;
}

static void
vibeos_theme_dispose (GObject *object)
{
  VibeosTheme *self = VIBEOS_THEME (object);

  g_clear_object (&self->settings);
  g_clear_object (&self->interface);
  g_clear_object (&self->a11y);
  g_clear_object (&self->provider);
  G_OBJECT_CLASS (vibeos_theme_parent_class)->dispose (object);
}

static void
vibeos_theme_class_init (VibeosThemeClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = vibeos_theme_dispose;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
vibeos_theme_init (VibeosTheme *self)
{
  self->settings = settings_if_installed ("vibeos.Theme");
  /* without vibeos.Theme, nothing to follow: dark, as the apps always were */
  if (self->settings)
    {
      self->interface = settings_if_installed ("org.gnome.desktop.interface");
      g_signal_connect (self->settings, "changed", G_CALLBACK (on_settings_changed), self);
      if (self->interface)
        g_signal_connect (self->interface, "changed::color-scheme", G_CALLBACK (on_settings_changed), self);
    }
  /* High contrast applies with or without a theme to follow */
  self->a11y = settings_if_installed ("org.gnome.desktop.a11y.interface");
  if (self->a11y)
    {
      g_autoptr (GSettingsSchema) schema = NULL;

      g_object_get (self->a11y, "settings-schema", &schema, NULL);
      if (!g_settings_schema_has_key (schema, "high-contrast"))
        g_clear_object (&self->a11y);
    }
  if (self->a11y)
    g_signal_connect (self->a11y, "changed::high-contrast", G_CALLBACK (on_settings_changed), self);
  update (self);
}

VibeosTheme *
vibeos_theme_get_default (void)
{
  if (!default_theme)
    default_theme = g_object_new (VIBEOS_TYPE_THEME, NULL);
  return default_theme;
}

gboolean
vibeos_theme_get_dark (VibeosTheme *theme)
{
  g_return_val_if_fail (VIBEOS_IS_THEME (theme), TRUE);
  return theme->dark;
}

void
vibeos_theme_set_scheme (VibeosTheme *theme, VibeosScheme scheme)
{
  g_return_if_fail (VIBEOS_IS_THEME (theme));

  if (theme->scheme == scheme)
    return;
  theme->scheme = scheme;
  update (theme);
}

VibeosScheme
vibeos_theme_get_scheme (VibeosTheme *theme)
{
  g_return_val_if_fail (VIBEOS_IS_THEME (theme), VIBEOS_SCHEME_SYSTEM);

  return theme->scheme;
}

guint32
vibeos_theme_get_seed (VibeosTheme *theme)
{
  g_return_val_if_fail (VIBEOS_IS_THEME (theme), DEFAULT_SEED);
  return theme->source.seed;
}

static GdkRGBA
rgba (guint32 rgb, float alpha)
{
  return (GdkRGBA) { ((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, alpha };
}

GdkRGBA
vibeos_color (VibeosColor color)
{
  return vibeos_color_alpha (color, 1);
}

GdkRGBA
vibeos_color_alpha (VibeosColor color, float alpha)
{
  g_return_val_if_fail (color < VIBEOS_N_COLORS, rgba (0, alpha));
  return rgba (vibeos_theme_get_default ()->rgb[color], alpha);
}

GdkRGBA
vibeos_palette_stop (VibeosPalette palette, int stop)
{
  VibeosTheme *theme = vibeos_theme_get_default ();

  g_return_val_if_fail (palette <= VIBEOS_PALETTE_ERROR, rgba (0, 1));
  for (int i = 0; i < TONAL_N_STOPS; i++)
    if (tonal_stops[i] == stop)
      return rgba ((theme->dark ? theme->theme.dark_stops : theme->theme.light_stops)[palette][i], 1);
  g_return_val_if_reached (rgba (0, 1));
}

GdkRGBA
vibeos_rgba_mix (const GdkRGBA *a, const GdkRGBA *b, float t)
{
  return (GdkRGBA) {
    a->red + (b->red - a->red) * t,
    a->green + (b->green - a->green) * t,
    a->blue + (b->blue - a->blue) * t,
    a->alpha + (b->alpha - a->alpha) * t,
  };
}

void
vibeos_init (void)
{
  vibeos_init_full (VIBEOS_INIT_DEFAULT);
}

void
vibeos_init_full (VibeosInitFlags flags)
{
  VibeosTheme *self = vibeos_theme_get_default ();

  if (self->initialized)
    return;
  self->flags = flags;
  /* the font settings first, so that the stylesheet is loaded once, with them (vibeos-text.c) */
  {
    extern void _vibeos_text_set_following (gboolean);
    _vibeos_text_set_following ((flags & VIBEOS_INIT_SCALED_TEXT) != 0);
  }
  /* the cursor's touch handle put away as the keyboard types (vibeos-text-handles.c) */
  {
    extern void _vibeos_text_handles_init (void);
    _vibeos_text_handles_init ();
  }
  self->initialized = TRUE;
  self->provider = gtk_css_provider_new ();
  /* its @media (prefers-contrast: more) rules with High contrast (vibeos-text.c) */
  {
    extern void _vibeos_css_follow_contrast (GtkCssProvider *provider);
    _vibeos_css_follow_contrast (self->provider);
  }
  /* above the theme (APPLICATION), so GTK's own look never shows through a libvibeos widget */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (self->provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* libvibeos's own icons (vibeos-external-link-symbolic), found by name like the icon theme's */
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                    "/vibeos/libvibeos/icons");
  /* the colours again, for the flags */
  self->computed = FALSE;
  update (self);
  load_stylesheet (self);
}

/* the font settings changed (vibeos-text.c): the stylesheet again, with their sizes and weights */
G_GNUC_INTERNAL void vibeos_style_reload (void);

void
vibeos_style_reload (void)
{
  if (default_theme && default_theme->initialized)
    load_stylesheet (default_theme);
}

/* ---- the window ------------------------------------------------------------------------------------------------ */

/* ---- VIBEOS_SHOT: a window's picture, for checking the UI ---------------------------------------------------------
 *
 * With VIBEOS_SHOT=/path/shot.png in the environment, a window renders itself to that file, at the screen's scale, a
 * second after it maps, and again on every SIGUSR1 (after the test has changed something). The picture is drawn
 * offscreen from the widgets, so it works where a screenshot cannot: behind the lock screen, on a phone nobody
 * unlocks for the test. (A window hidden there gets no frames, though, so its animations stop where they are: for
 * those, run the app on a display of its own, such as a headless compositor, with VIBEOS_SHOT_SCALE=3.) */

static gboolean
shoot (gpointer data)
{
  GtkWidget *window = data;
  const char *path = g_getenv ("VIBEOS_SHOT");
  int width = gtk_widget_get_width (window), height = gtk_widget_get_height (window);
  /* the screen's scale, or VIBEOS_SHOT_SCALE: a window on a test display at scale 1 still makes a phone-sized picture */
  double scale = vibeos_device_scale (window);
  int picture_w = (int) round (width * scale), picture_h = (int) round (height * scale);
  GdkPaintable *paintable = gtk_widget_paintable_new (window);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GskRenderNode *node;

  gtk_snapshot_scale (snapshot, scale, scale);
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  node = gtk_snapshot_free_to_node (snapshot);
  if (!gtk_widget_get_mapped (window) || !node)
    g_message ("VIBEOS_SHOT: nothing to draw (the window is %s)", gtk_widget_get_mapped (window) ? "empty" : "not shown");
  else
    {
      GdkTexture *texture = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)), node,
                                                         &GRAPHENE_RECT_INIT (0, 0, picture_w, picture_h));
      if (gdk_texture_save_to_png (texture, path))
        g_message ("VIBEOS_SHOT: %s, %d×%d", path, picture_w, picture_h);
      else
        g_message ("VIBEOS_SHOT: cannot write %s", path);
      g_object_unref (texture);
    }
  g_clear_pointer (&node, gsk_render_node_unref);
  g_object_unref (paintable);
  return G_SOURCE_REMOVE;
}

static void
on_shot_map (GtkWidget *window)
{
  g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, shoot, g_object_ref (window), g_object_unref);
}

static gboolean
on_shot_signal (gpointer data)
{
  GListModel *windows = gtk_window_get_toplevels ();

  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr(GtkWidget) window = g_list_model_get_item (windows, i);

      if (gtk_widget_has_css_class (window, "vibeos"))
        shoot (window);
    }
  return G_SOURCE_CONTINUE;
}

GtkWidget *
vibeos_window_new (GtkApplication *app)
{
  static gboolean shots;
  GtkWidget *window = gtk_application_window_new (app);
  gtk_widget_add_css_class (window, "vibeos");
  /* no title bar: the app draws its own app bar, and on the phone the shell shows every window full screen */
  gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (window), 360, 760);
  if (g_getenv ("VIBEOS_SHOT"))
    {
      g_signal_connect (window, "map", G_CALLBACK (on_shot_map), NULL);
      if (!shots)
        g_unix_signal_add (SIGUSR1, on_shot_signal, NULL);
      shots = TRUE;
    }
  { extern void vibeos_dump_attach (GtkWidget *window); vibeos_dump_attach (window); }  /* VIBEOS_DUMP: vibeos-dump.c */
  return window;
}

void
vibeos_window_set_content (GtkWindow *window, GtkWidget *content)
{
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), content);
  g_object_set_data (G_OBJECT (window), "vibeos-overlay", overlay);
  gtk_window_set_child (window, overlay);
}

GtkOverlay *
vibeos_window_get_overlay (GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root (widget);
  return root ? g_object_get_data (G_OBJECT (root), "vibeos-overlay") : NULL;
}

double
vibeos_device_scale (GtkWidget *widget)
{
  const char *shot_scale = g_getenv ("VIBEOS_SHOT_SCALE");
  double value = shot_scale ? g_ascii_strtod (shot_scale, NULL) : 0;
  GtkNative *native;
  GdkSurface *surface;

  if (value > 0)
    return value;
  native = gtk_widget_get_native (widget);
  surface = native ? gtk_native_get_surface (native) : NULL;
  return surface ? gdk_surface_get_scale (surface) : gtk_widget_get_scale_factor (widget);
}

/* ---- text ---------------------------------------------------------------------------------------------------- */

void
vibeos_label_set_font_padding (GtkWidget *label, double size)
{
  gtk_widget_set_margin_top (label, (int) round (size * (2163 - 1900) / 2048.0));
  gtk_widget_set_margin_bottom (label, (int) round (size * (555 - 500) / 2048.0));
  gtk_label_set_yalign (GTK_LABEL (label), 0);
}

/* ---- icons ----------------------------------------------------------------------------------------------------- */

void
vibeos_snapshot_icon (GtkSnapshot *snapshot, GtkWidget *widget, const char *icon_name, double x, double y, double size,
                   const GdkRGBA *color)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (widget));
  GtkIconPaintable *icon = gtk_icon_theme_lookup_icon (theme, icon_name, NULL, (int) size,
                                                       gtk_widget_get_scale_factor (widget), GTK_TEXT_DIR_NONE, 0);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, y));
  /* the symbolic colours are foreground, error, warning, success: an Android tint is the first of them */
  GdkRGBA colors[4] = { *color, *color, *color, *color };
  gtk_symbolic_paintable_snapshot_symbolic (GTK_SYMBOLIC_PAINTABLE (icon), snapshot, size, size, colors, 4);
  gtk_snapshot_restore (snapshot);
  g_object_unref (icon);
}
