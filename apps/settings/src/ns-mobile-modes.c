/* ModemManager's network modes (see ns-mobile-modes.h). MMModemMode is a bitmask, not the AccessTechnologies
 * bitmask: CS=1, 2G=2, 3G=4, 4G=8, 5G=16. SupportedModes is an array of allowed/preferred pairs; keeping its
 * order and filtering invalid pairs means Settings only offers combinations the modem said it can use. */
#include "ns-mobile-modes.h"

#include <glib/gi18n.h>

#define MODE_CS  (1u << 0)
#define MODE_2G  (1u << 1)
#define MODE_3G  (1u << 2)
#define MODE_4G  (1u << 3)
#define MODE_5G  (1u << 4)
#define MODE_ALL (MODE_CS | MODE_2G | MODE_3G | MODE_4G | MODE_5G)

static void
mode_free (NsMobileMode *mode)
{
  g_free (mode->label);
  g_free (mode);
}

void
ns_mobile_modes_free (NsMobileModes *m)
{
  if (m)
    g_ptr_array_unref (m);
}

static gboolean
valid (guint32 allowed, guint32 preferred)
{
  if (allowed == 0 || (allowed != G_MAXUINT32 && (allowed & ~MODE_ALL)))
    return FALSE;
  if (preferred && ((preferred & allowed) != preferred || (preferred & (preferred - 1))))
    return FALSE;
  return TRUE;
}

static guint
n_modes (guint32 mask)
{
  guint n = 0;
  while (mask) {
    n++;
    mask &= mask - 1;
  }
  return n;
}

static const char *
mode_name (guint32 mode)
{
  switch (mode) {
    case MODE_CS: return _("CS");
    case MODE_2G: return _("2G");
    case MODE_3G: return _("3G");
    case MODE_4G: return _("4G");
    case MODE_5G: return _("5G");
    default: return _("Unknown");
  }
}

static char *
mode_names (guint32 allowed)
{
  static const guint32 bits[] = { MODE_CS, MODE_2G, MODE_3G, MODE_4G, MODE_5G };
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (bits); i++) {
    if (!(allowed & bits[i]))
      continue;
    if (s->len)
      g_string_append_c (s, '/');
    g_string_append (s, mode_name (bits[i]));
  }
  return g_string_free (s, FALSE);
}

static guint32
highest_mode (guint32 mask)
{
  guint32 highest = 0;
  while (mask) {
    highest = mask & (0u - mask);
    mask &= mask - 1;
  }
  return highest;
}

static char *
mode_label (guint32 allowed, guint32 preferred, guint32 automatic)
{
  if (allowed == G_MAXUINT32 ||
      (n_modes (allowed) > 1 && allowed == automatic && (!preferred || preferred == highest_mode (allowed))))
    return g_strdup (_("Automatic"));
  g_autofree char *names = mode_names (allowed);
  if (n_modes (allowed) == 1 && !preferred)
    return g_strdup_printf (_("%s only"), names);
  if (preferred)
    return g_strdup_printf (_("%s, prefer %s"), names, mode_name (preferred));
  return g_steal_pointer (&names);
}

NsMobileModes *
ns_mobile_modes_new (GVariant *supported, GVariant *current, GError **error)
{
  if (!g_variant_is_of_type (supported, G_VARIANT_TYPE (NS_MOBILE_MODES_TYPE)) ||
      !g_variant_is_of_type (current, G_VARIANT_TYPE ("(uu)"))) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not ModemManager network modes");
    return NULL;
  }

  guint32 automatic = 0;
  gboolean any = FALSE;
  GVariantIter it;
  guint32 allowed, preferred;
  g_variant_iter_init (&it, supported);
  while (g_variant_iter_next (&it, "(uu)", &allowed, &preferred)) {
    if (!valid (allowed, preferred))
      continue;
    if (allowed == G_MAXUINT32)
      any = TRUE;
    else
      automatic |= allowed;
  }
  if (any)
    automatic = 0;

  NsMobileModes *m = g_ptr_array_new_with_free_func ((GDestroyNotify) mode_free);
  g_variant_iter_init (&it, supported);
  while (g_variant_iter_next (&it, "(uu)", &allowed, &preferred)) {
    if (!valid (allowed, preferred))
      continue;
    gboolean duplicate = FALSE;
    for (guint i = 0; i < m->len; i++) {
      NsMobileMode *old = g_ptr_array_index (m, i);
      if (old->allowed == allowed && old->preferred == preferred) {
        duplicate = TRUE;
        break;
      }
    }
    if (duplicate)
      continue;
    NsMobileMode *mode = g_new0 (NsMobileMode, 1);
    mode->allowed = allowed;
    mode->preferred = preferred;
    mode->label = mode_label (allowed, preferred, automatic);
    g_ptr_array_add (m, mode);
  }
  return m;
}

gchar **
ns_mobile_modes_labels (NsMobileModes *m)
{
  gchar **labels = g_new0 (gchar *, m->len + 1);
  for (guint i = 0; i < m->len; i++) {
    NsMobileMode *mode = g_ptr_array_index (m, i);
    labels[i] = g_strdup (mode->label);
  }
  return labels;
}

guint
ns_mobile_modes_current (NsMobileModes *m, GVariant *current)
{
  guint32 allowed, preferred;
  g_variant_get (current, "(uu)", &allowed, &preferred);
  for (guint i = 0; i < m->len; i++) {
    NsMobileMode *mode = g_ptr_array_index (m, i);
    if (mode->allowed == allowed && mode->preferred == preferred)
      return i;
  }
  return 0;
}

GVariant *
ns_mobile_mode_value (const NsMobileMode *mode)
{
  return g_variant_new ("(uu)", mode->allowed, mode->preferred);
}
