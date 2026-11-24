/* vibeos-original-colors: see vibeos-original-colors.h.
 *
 * A role's tone comes from a scheme computed by the original rules. Its colour comes from that scheme's palette when
 * the role is a neutral one (the surfaces, their text, the outlines), whose original palettes are a little more
 * colourful, and from the palette the theme's own scheme gives it otherwise (the accents and the error colour), so
 * that they stay the theme's. */
#include "vibeos-original-colors.h"

#include <tonal.h>

#define FIXED (-1)

/* each role, its CSS name and the role it is in the scheme */
static const struct {
  const char *css;
  int         role;
} roles[VIBEOS_N_COLORS] = {
  [VIBEOS_PRIMARY]                    = { "primary", TONAL_PRIMARY },
  [VIBEOS_ON_PRIMARY]                 = { "on_primary", TONAL_ON_PRIMARY },
  [VIBEOS_PRIMARY_CONTAINER]          = { "primary_container", TONAL_PRIMARY_CONTAINER },
  [VIBEOS_ON_PRIMARY_CONTAINER]       = { "on_primary_container", TONAL_ON_PRIMARY_CONTAINER },
  [VIBEOS_INVERSE_PRIMARY]            = { "inverse_primary", TONAL_INVERSE_PRIMARY },
  [VIBEOS_SECONDARY]                  = { "secondary", TONAL_SECONDARY },
  [VIBEOS_ON_SECONDARY]               = { "on_secondary", TONAL_ON_SECONDARY },
  [VIBEOS_SECONDARY_CONTAINER]        = { "secondary_container", TONAL_SECONDARY_CONTAINER },
  [VIBEOS_ON_SECONDARY_CONTAINER]     = { "on_secondary_container", TONAL_ON_SECONDARY_CONTAINER },
  [VIBEOS_TERTIARY]                   = { "tertiary", TONAL_TERTIARY },
  [VIBEOS_ON_TERTIARY]                = { "on_tertiary", TONAL_ON_TERTIARY },
  [VIBEOS_TERTIARY_CONTAINER]         = { "tertiary_container", TONAL_TERTIARY_CONTAINER },
  [VIBEOS_ON_TERTIARY_CONTAINER]      = { "on_tertiary_container", TONAL_ON_TERTIARY_CONTAINER },
  [VIBEOS_ERROR]                      = { "error", TONAL_ERROR },
  [VIBEOS_ERROR_CONTAINER]            = { "error_container", TONAL_ERROR_CONTAINER },
  [VIBEOS_SURFACE]                    = { "surface", TONAL_SURFACE },
  [VIBEOS_SURFACE_CONTAINER_LOWEST]   = { "surface_container_lowest", TONAL_SURFACE_CONTAINER_LOWEST },
  [VIBEOS_SURFACE_CONTAINER_LOW]      = { "surface_container_low", TONAL_SURFACE_CONTAINER_LOW },
  [VIBEOS_SURFACE_CONTAINER]          = { "surface_container", TONAL_SURFACE_CONTAINER },
  [VIBEOS_SURFACE_CONTAINER_HIGH]     = { "surface_container_high", TONAL_SURFACE_CONTAINER_HIGH },
  [VIBEOS_SURFACE_CONTAINER_HIGHEST]  = { "surface_container_highest", TONAL_SURFACE_CONTAINER_HIGHEST },
  [VIBEOS_SURFACE_BRIGHT]             = { "surface_bright", TONAL_SURFACE_BRIGHT },
  [VIBEOS_ON_SURFACE]                 = { "on_surface", TONAL_ON_SURFACE },
  [VIBEOS_ON_SURFACE_VARIANT]         = { "on_surface_variant", TONAL_ON_SURFACE_VARIANT },
  [VIBEOS_OUTLINE]                    = { "outline", TONAL_OUTLINE },
  [VIBEOS_OUTLINE_VARIANT]            = { "outline_variant", TONAL_OUTLINE_VARIANT },
  [VIBEOS_INVERSE_SURFACE]            = { "inverse_surface", TONAL_INVERSE_SURFACE },
  [VIBEOS_WHITE]                      = { "white", FIXED },
  [VIBEOS_ON_ERROR]                   = { "on_error", TONAL_ON_ERROR },
  [VIBEOS_ON_ERROR_CONTAINER]         = { "on_error_container", TONAL_ON_ERROR_CONTAINER },
  [VIBEOS_BACKGROUND]                 = { "background", TONAL_BACKGROUND },
  [VIBEOS_ON_BACKGROUND]              = { "on_background", TONAL_ON_BACKGROUND },
  [VIBEOS_SURFACE_DIM]                = { "surface_dim", TONAL_SURFACE_DIM },
  [VIBEOS_SURFACE_VARIANT]            = { "surface_variant", TONAL_SURFACE_VARIANT },
  [VIBEOS_INVERSE_ON_SURFACE]         = { "inverse_on_surface", TONAL_INVERSE_ON_SURFACE },
  [VIBEOS_SURFACE_TINT]               = { "surface_tint", TONAL_SURFACE_TINT },
  [VIBEOS_SHADOW]                     = { "shadow", TONAL_SHADOW },
  [VIBEOS_SCRIM]                      = { "scrim", TONAL_SCRIM },
  [VIBEOS_PRIMARY_FIXED]              = { "primary_fixed", TONAL_PRIMARY_FIXED },
  [VIBEOS_PRIMARY_FIXED_DIM]          = { "primary_fixed_dim", TONAL_PRIMARY_FIXED_DIM },
  [VIBEOS_ON_PRIMARY_FIXED]           = { "on_primary_fixed", TONAL_ON_PRIMARY_FIXED },
  [VIBEOS_ON_PRIMARY_FIXED_VARIANT]   = { "on_primary_fixed_variant", TONAL_ON_PRIMARY_FIXED_VARIANT },
  [VIBEOS_SECONDARY_FIXED]            = { "secondary_fixed", TONAL_SECONDARY_FIXED },
  [VIBEOS_SECONDARY_FIXED_DIM]        = { "secondary_fixed_dim", TONAL_SECONDARY_FIXED_DIM },
  [VIBEOS_ON_SECONDARY_FIXED]         = { "on_secondary_fixed", TONAL_ON_SECONDARY_FIXED },
  [VIBEOS_ON_SECONDARY_FIXED_VARIANT] = { "on_secondary_fixed_variant", TONAL_ON_SECONDARY_FIXED_VARIANT },
  [VIBEOS_TERTIARY_FIXED]             = { "tertiary_fixed", TONAL_TERTIARY_FIXED },
  [VIBEOS_TERTIARY_FIXED_DIM]         = { "tertiary_fixed_dim", TONAL_TERTIARY_FIXED_DIM },
  [VIBEOS_ON_TERTIARY_FIXED]          = { "on_tertiary_fixed", TONAL_ON_TERTIARY_FIXED },
  [VIBEOS_ON_TERTIARY_FIXED_VARIANT]  = { "on_tertiary_fixed_variant", TONAL_ON_TERTIARY_FIXED_VARIANT },
  [VIBEOS_DIALOG_TITLE]               = { "dialog_title", FIXED },
  [VIBEOS_CATEGORY_NETWORK]           = { "category_network", FIXED },
  [VIBEOS_ON_CATEGORY_NETWORK]        = { "on_category_network", FIXED },
  [VIBEOS_CATEGORY_SYSTEM]            = { "category_system", FIXED },
  [VIBEOS_ON_CATEGORY_SYSTEM]         = { "on_category_system", FIXED },
  [VIBEOS_CALL]                       = { "call", FIXED },
  [VIBEOS_ON_CALL]                    = { "on_call", FIXED },
  [VIBEOS_MISSED_CALL]                = { "missed_call", FIXED },
  [VIBEOS_SEARCH_CURSOR]              = { "search_cursor", FIXED },
  [VIBEOS_EMOJI_TAB_SELECTED]         = { "emoji_tab_selected", FIXED },
  [VIBEOS_EMOJI_TAB]                  = { "emoji_tab", FIXED },
  [VIBEOS_CATEGORY_APPS]              = { "category_apps", FIXED },
  [VIBEOS_ON_CATEGORY_APPS]           = { "on_category_apps", FIXED },
  [VIBEOS_CATEGORY_NOTIFICATIONS]     = { "category_notifications", FIXED },
  [VIBEOS_ON_CATEGORY_NOTIFICATIONS]  = { "on_category_notifications", FIXED },
  [VIBEOS_CATEGORY_DISPLAY]           = { "category_display", FIXED },
  [VIBEOS_ON_CATEGORY_DISPLAY]        = { "on_category_display", FIXED },
  [VIBEOS_CATEGORY_BATTERY]           = { "category_battery", FIXED },
  [VIBEOS_ON_CATEGORY_BATTERY]        = { "on_category_battery", FIXED },
  [VIBEOS_CATEGORY_SECURITY]          = { "category_security", FIXED },
  [VIBEOS_ON_CATEGORY_SECURITY]       = { "on_category_security", FIXED },
  [VIBEOS_CATEGORY_ACCESSIBILITY]     = { "category_accessibility", FIXED },
  [VIBEOS_ON_CATEGORY_ACCESSIBILITY]  = { "on_category_accessibility", FIXED },
};

/* what the colours were computed from */
typedef struct {
  guint32      seed;
  gboolean     dark;
  TonalVariant variant;
  double       contrast;
  gboolean     pure_black;
} Source;

static struct {
  gboolean        computed;
  Source          source;
  guint32         rgb[VIBEOS_N_COLORS];
  GSettings      *settings;       /* vibeos.Theme, NULL without its schema */
  gboolean        looked;         /* the schema was looked for */
  GtkCssProvider *provider;       /* the @vibeos_original_* colours, once initialised */
} state;

/* the palette a role's colour comes from */
static TonalPaletteId
role_palette (TonalRole role)
{
  switch (role)
    {
    case TONAL_PRIMARY: case TONAL_ON_PRIMARY: case TONAL_PRIMARY_CONTAINER: case TONAL_ON_PRIMARY_CONTAINER:
    case TONAL_PRIMARY_FIXED: case TONAL_PRIMARY_FIXED_DIM: case TONAL_ON_PRIMARY_FIXED:
    case TONAL_ON_PRIMARY_FIXED_VARIANT: case TONAL_INVERSE_PRIMARY: case TONAL_SURFACE_TINT:
    case TONAL_PRIMARY_PALETTE_KEY_COLOR:
      return TONAL_PALETTE_PRIMARY;
    case TONAL_SECONDARY: case TONAL_ON_SECONDARY: case TONAL_SECONDARY_CONTAINER: case TONAL_ON_SECONDARY_CONTAINER:
    case TONAL_SECONDARY_FIXED: case TONAL_SECONDARY_FIXED_DIM: case TONAL_ON_SECONDARY_FIXED:
    case TONAL_ON_SECONDARY_FIXED_VARIANT: case TONAL_SECONDARY_PALETTE_KEY_COLOR:
      return TONAL_PALETTE_SECONDARY;
    case TONAL_TERTIARY: case TONAL_ON_TERTIARY: case TONAL_TERTIARY_CONTAINER: case TONAL_ON_TERTIARY_CONTAINER:
    case TONAL_TERTIARY_FIXED: case TONAL_TERTIARY_FIXED_DIM: case TONAL_ON_TERTIARY_FIXED:
    case TONAL_ON_TERTIARY_FIXED_VARIANT: case TONAL_TERTIARY_PALETTE_KEY_COLOR:
      return TONAL_PALETTE_TERTIARY;
    case TONAL_ERROR: case TONAL_ON_ERROR: case TONAL_ERROR_CONTAINER: case TONAL_ON_ERROR_CONTAINER:
    case TONAL_ERROR_PALETTE_KEY_COLOR:
      return TONAL_PALETTE_ERROR;
    case TONAL_SURFACE_VARIANT: case TONAL_ON_SURFACE_VARIANT: case TONAL_OUTLINE: case TONAL_OUTLINE_VARIANT:
    case TONAL_NEUTRAL_VARIANT_PALETTE_KEY_COLOR:
      return TONAL_PALETTE_NEUTRAL_VARIANT;
    default:
      return TONAL_PALETTE_NEUTRAL;
    }
}

/* the background surfaces pure black turns black, as the theme does */
static gboolean
is_background (TonalRole role)
{
  return role == TONAL_BACKGROUND || role == TONAL_SURFACE || role == TONAL_SURFACE_DIM
         || role == TONAL_SURFACE_CONTAINER_LOWEST;
}

static void
read_source (Source *source)
{
  VibeosTheme *theme = vibeos_theme_get_default ();
  const char *env;

  if (!state.looked)
    {
      GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
      g_autoptr (GSettingsSchema) schema = schemas ? g_settings_schema_source_lookup (schemas, "vibeos.Theme", TRUE)
                                                   : NULL;

      state.looked = TRUE;
      state.settings = schema ? g_settings_new_full (schema, NULL, NULL) : NULL;
    }
  *source = (Source) { vibeos_theme_get_seed (theme), vibeos_theme_get_dark (theme), TONAL_VARIANT_TONAL_SPOT, 0,
                       FALSE };
  if (state.settings)
    {
      g_autofree char *style = g_settings_get_string (state.settings, "style");

      tonal_variant_from_name (style, &source->variant);
      source->contrast = g_settings_get_double (state.settings, "contrast");
      source->pure_black = g_settings_get_boolean (state.settings, "pure-black");
    }
  env = g_getenv ("VIBEOS_CONTRAST");
  if (env && *env)
    source->contrast = CLAMP (g_ascii_strtod (env, NULL), -1.0, 1.0);
}

static void
compute (const Source *source)
{
  TonalScheme current, original;

  tonal_scheme_init (&current, 0xff000000 | source->seed, source->variant, source->dark, source->contrast,
                     TONAL_SPEC_2025);
  tonal_scheme_init (&original, 0xff000000 | source->seed, source->variant, source->dark, source->contrast,
                     TONAL_SPEC_2021);
  for (int i = 0; i < VIBEOS_N_COLORS; i++)
    {
      TonalRole role = roles[i].role;
      GdkRGBA fixed;

      if (roles[i].role == FIXED)
        {
          fixed = vibeos_color (i);
          state.rgb[i] = ((guint32) (fixed.red * 255 + 0.5) << 16) | ((guint32) (fixed.green * 255 + 0.5) << 8)
                         | (guint32) (fixed.blue * 255 + 0.5);
        }
      else if (source->dark && source->pure_black && is_background (role))
        state.rgb[i] = 0;
      else if (role == TONAL_SHADOW || role == TONAL_SCRIM)
        state.rgb[i] = 0;
      else
        {
          TonalPaletteId palette = role_palette (role);
          const TonalScheme *from = palette == TONAL_PALETTE_NEUTRAL || palette == TONAL_PALETTE_NEUTRAL_VARIANT
                                    ? &original : &current;

          state.rgb[i] = tonal_palette_tone (&from->palettes[palette], tonal_scheme_tone (&original, role)) & 0xffffff;
        }
    }
  state.source = *source;
  state.computed = TRUE;
}

static gboolean
source_equal (const Source *a, const Source *b)
{
  return a->seed == b->seed && !a->dark == !b->dark && a->variant == b->variant && a->contrast == b->contrast
         && !a->pure_black == !b->pure_black;
}

/* computed again when the theme changed since */
static void
ensure (void)
{
  Source source;

  read_source (&source);
  if (!state.computed || !source_equal (&source, &state.source))
    compute (&source);
}

GdkRGBA
vibeos_original_color_alpha (VibeosColor color, float alpha)
{
  guint32 rgb;

  g_return_val_if_fail (color < VIBEOS_N_COLORS, ((GdkRGBA) { 0, 0, 0, alpha }));
  ensure ();
  rgb = state.rgb[color];
  return (GdkRGBA) { ((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, alpha };
}

GdkRGBA
vibeos_original_color (VibeosColor color)
{
  return vibeos_original_color_alpha (color, 1);
}

static void
load_stylesheet (void)
{
  g_autoptr (GString) css = g_string_new (NULL);

  ensure ();
  for (int i = 0; i < VIBEOS_N_COLORS; i++)
    g_string_append_printf (css, "@define-color vibeos_original_%s #%06x;\n", roles[i].css, state.rgb[i]);
  gtk_css_provider_load_from_string (state.provider, css->str);
}

/* before the theme's windows are redrawn: the stylesheet with the new colours */
static void
on_theme_changed (VibeosTheme *theme, gpointer data)
{
  load_stylesheet ();
}

void
vibeos_original_colors_init (void)
{
  if (state.provider)
    return;
  state.provider = gtk_css_provider_new ();
  load_stylesheet ();
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (state.provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), NULL);
}
