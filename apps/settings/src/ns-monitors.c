/* The displays as mutter has them, and the configurations asked of it (see ns-monitors.h).
 *
 * Mutter checks every configuration (meta_verify_monitors_config): each logical monitor at x, y ≥ 0, none
 * overlapping, each touching another, the top left one at 0, 0, exactly one primary, and the displays in one logical
 * monitor (a mirror) all at a mode of the same size. A logical monitor's size is its mode's, turned with its
 * transform, divided by its scale while the layout is logical (rounded as mutter rounds it), and each scale must be
 * one the mode lists. The configurations made here keep to that: Join lays the screens out in a row at the top, left
 * to right as they were, and a scale is always one of the mode's own. */
#include "ns-monitors.h"

#include <math.h>

#define SAME_SCALE(a, b) (fabs ((a) - (b)) < 0.001)

static void
mode_free (NsMode *mo)
{
  g_free (mo->id);
  g_array_unref (mo->scales);
  g_free (mo);
}

static void
monitor_free (NsMonitor *mon)
{
  g_free (mon->connector);
  g_free (mon->vendor);
  g_free (mon->product);
  g_free (mon->serial);
  g_free (mon->name);
  g_ptr_array_unref (mon->modes);
  g_variant_unref (mon->props);
  g_free (mon);
}

static void
logical_free (NsLogical *l)
{
  g_ptr_array_unref (l->monitors);
  g_free (l);
}

void
ns_monitors_free (NsMonitors *m)
{
  if (!m)
    return;
  g_ptr_array_unref (m->logical);
  g_ptr_array_unref (m->monitors);
  g_free (m);
}

static gboolean
lookup_bool (GVariant *props, const char *key)
{
  gboolean b = FALSE;
  return g_variant_lookup (props, key, "b", &b) && b;
}

static NsMode *
mode_new (GVariant *v)
{
  NsMode *mo = g_new0 (NsMode, 1);
  g_autoptr (GVariant) scales = g_variant_get_child_value (v, 5);
  g_autoptr (GVariant) props = g_variant_get_child_value (v, 6);
  g_variant_get_child (v, 0, "s", &mo->id);
  g_variant_get_child (v, 1, "i", &mo->width);
  g_variant_get_child (v, 2, "i", &mo->height);
  g_variant_get_child (v, 3, "d", &mo->refresh);
  g_variant_get_child (v, 4, "d", &mo->preferred_scale);
  mo->scales = g_array_new (FALSE, FALSE, sizeof (double));
  for (gsize k = 0; k < g_variant_n_children (scales); k++) {
    double d;
    g_variant_get_child (scales, k, "d", &d);
    g_array_append_val (mo->scales, d);
  }
  mo->current = lookup_bool (props, "is-current");
  mo->preferred = lookup_bool (props, "is-preferred");
  mo->interlaced = lookup_bool (props, "is-interlaced");
  return mo;
}

/* what a configuration hands back as it found it: mutter would take a monitor without them as one at the defaults */
static GVariant *
monitor_props (GVariant *props)
{
  GVariantBuilder b;
  guint32 u;
  gboolean on;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
  if (g_variant_lookup (props, "color-mode", "u", &u))
    g_variant_builder_add (&b, "{sv}", "color-mode", g_variant_new_uint32 (u));
  if (g_variant_lookup (props, "rgb-range", "u", &u))
    g_variant_builder_add (&b, "{sv}", "rgb-range", g_variant_new_uint32 (u));
  if (g_variant_lookup (props, "is-underscanning", "b", &on))
    g_variant_builder_add (&b, "{sv}", "underscanning", g_variant_new_boolean (on));
  return g_variant_ref_sink (g_variant_builder_end (&b));
}

NsMonitors *
ns_monitors_new (GVariant *state, GError **error)
{
  if (!g_variant_is_of_type (state, G_VARIANT_TYPE (NS_MONITORS_STATE_TYPE))) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a DisplayConfig state: %s",
                 g_variant_get_type_string (state));
    return NULL;
  }
  NsMonitors *m = g_new0 (NsMonitors, 1);
  m->monitors = g_ptr_array_new_with_free_func ((GDestroyNotify) monitor_free);
  m->logical = g_ptr_array_new_with_free_func ((GDestroyNotify) logical_free);
  m->layout_mode = NS_LAYOUT_LOGICAL;
  g_autoptr (GVariant) monitors = g_variant_get_child_value (state, 1);
  g_autoptr (GVariant) logical = g_variant_get_child_value (state, 2);
  g_autoptr (GVariant) props = g_variant_get_child_value (state, 3);
  g_variant_get_child (state, 0, "u", &m->serial);
  g_variant_lookup (props, "layout-mode", "u", &m->layout_mode);
  m->can_change_layout_mode = lookup_bool (props, "supports-changing-layout-mode");

  for (gsize i = 0; i < g_variant_n_children (monitors); i++) {
    g_autoptr (GVariant) v = g_variant_get_child_value (monitors, i);
    g_autoptr (GVariant) spec = g_variant_get_child_value (v, 0);
    g_autoptr (GVariant) modes = g_variant_get_child_value (v, 1);
    g_autoptr (GVariant) mprops = g_variant_get_child_value (v, 2);
    /* leased out (a VR headset's): not the desktop's */
    if (lookup_bool (mprops, "is-for-lease"))
      continue;
    NsMonitor *mon = g_new0 (NsMonitor, 1);
    g_variant_get (spec, "(ssss)", &mon->connector, &mon->vendor, &mon->product, &mon->serial);
    if (!g_variant_lookup (mprops, "display-name", "s", &mon->name))
      mon->name = g_strdup (mon->connector);
    mon->builtin = lookup_bool (mprops, "is-builtin");
    mon->props = monitor_props (mprops);
    mon->modes = g_ptr_array_new_with_free_func ((GDestroyNotify) mode_free);
    for (gsize k = 0; k < g_variant_n_children (modes); k++) {
      g_autoptr (GVariant) mv = g_variant_get_child_value (modes, k);
      g_ptr_array_add (mon->modes, mode_new (mv));
    }
    g_ptr_array_add (m->monitors, mon);
  }

  for (gsize i = 0; i < g_variant_n_children (logical); i++) {
    g_autoptr (GVariant) v = g_variant_get_child_value (logical, i);
    g_autoptr (GVariant) specs = g_variant_get_child_value (v, 5);
    NsLogical *l = g_new0 (NsLogical, 1);
    l->monitors = g_ptr_array_new ();
    g_variant_get_child (v, 0, "i", &l->x);
    g_variant_get_child (v, 1, "i", &l->y);
    g_variant_get_child (v, 2, "d", &l->scale);
    g_variant_get_child (v, 3, "u", &l->transform);
    g_variant_get_child (v, 4, "b", &l->primary);
    for (gsize k = 0; k < g_variant_n_children (specs); k++) {
      const char *connector;
      g_autoptr (GVariant) spec = g_variant_get_child_value (specs, k);
      g_variant_get_child (spec, 0, "&s", &connector);
      NsMonitor *mon = ns_monitors_find (m, connector);
      if (!mon)
        continue;
      mon->logical = l;
      g_ptr_array_add (l->monitors, mon);
    }
    g_ptr_array_add (m->logical, l);
  }

  /* a display's current mode counts while it is on */
  for (guint i = 0; i < m->monitors->len; i++) {
    NsMonitor *mon = m->monitors->pdata[i];
    for (guint k = 0; mon->logical && k < mon->modes->len; k++)
      if (((NsMode *) mon->modes->pdata[k])->current)
        mon->mode = mon->modes->pdata[k];
    if (mon->logical && !mon->mode && mon->modes->len)
      mon->mode = ns_monitor_preferred_mode (mon);
  }
  return m;
}

NsMonitors *
ns_monitors_new_from_text (const char *text, GError **error)
{
  g_autoptr (GVariant) v = g_variant_parse (G_VARIANT_TYPE (NS_MONITORS_STATE_TYPE), text, NULL, NULL, error);
  return v ? ns_monitors_new (v, error) : NULL;
}

NsMonitor *
ns_monitors_find (NsMonitors *m, const char *connector)
{
  for (guint i = 0; i < m->monitors->len; i++)
    if (!g_strcmp0 (((NsMonitor *) m->monitors->pdata[i])->connector, connector))
      return m->monitors->pdata[i];
  return NULL;
}

NsMonitor *
ns_monitors_builtin (NsMonitors *m)
{
  for (guint i = 0; i < m->monitors->len; i++)
    if (((NsMonitor *) m->monitors->pdata[i])->builtin)
      return m->monitors->pdata[i];
  return NULL;
}

NsMonitor *
ns_monitors_primary (NsMonitors *m)
{
  for (guint i = 0; i < m->logical->len; i++) {
    NsLogical *l = m->logical->pdata[i];
    if (!l->primary || !l->monitors->len)
      continue;
    /* a mirror's: the built-in panel, if it is in it */
    for (guint k = 0; k < l->monitors->len; k++)
      if (((NsMonitor *) l->monitors->pdata[k])->builtin)
        return l->monitors->pdata[k];
    return l->monitors->pdata[0];
  }
  return NULL;
}

gboolean
ns_monitors_is_mirror (NsMonitors *m)
{
  for (guint i = 0; i < m->logical->len; i++)
    if (((NsLogical *) m->logical->pdata[i])->monitors->len > 1)
      return TRUE;
  return FALSE;
}

guint
ns_monitors_n_on (NsMonitors *m)
{
  guint n = 0;
  for (guint i = 0; i < m->monitors->len; i++)
    n += ((NsMonitor *) m->monitors->pdata[i])->logical != NULL;
  return n;
}

NsMode *
ns_monitor_find_mode (NsMonitor *mon, const char *id)
{
  for (guint i = 0; i < mon->modes->len; i++)
    if (!g_strcmp0 (((NsMode *) mon->modes->pdata[i])->id, id))
      return mon->modes->pdata[i];
  return NULL;
}

NsMode *
ns_monitor_mode_for_size (NsMonitor *mon, int width, int height, double refresh)
{
  NsMode *best = NULL;
  for (guint i = 0; i < mon->modes->len; i++) {
    NsMode *mo = mon->modes->pdata[i];
    if (mo->width != width || mo->height != height)
      continue;
    if (!best || (refresh > 0 ? fabs (mo->refresh - refresh) < fabs (best->refresh - refresh)
                              : mo->refresh > best->refresh))
      best = mo;
  }
  return best;
}

NsMode *
ns_monitor_preferred_mode (NsMonitor *mon)
{
  for (guint i = 0; i < mon->modes->len; i++)
    if (((NsMode *) mon->modes->pdata[i])->preferred)
      return mon->modes->pdata[i];
  return mon->modes->len ? mon->modes->pdata[0] : NULL;
}

gboolean
ns_mode_has_scale (NsMode *mode, double scale)
{
  for (guint i = 0; i < mode->scales->len; i++)
    if (SAME_SCALE (g_array_index (mode->scales, double, i), scale))
      return TRUE;
  return FALSE;
}

/* `scales` less those `mode` cannot take */
static void
keep_scales_of (GArray *scales, NsMode *mode)
{
  for (guint i = scales->len; i-- > 0;)
    if (!ns_mode_has_scale (mode, g_array_index (scales, double, i)))
      g_array_remove_index (scales, i);
}

static GArray *
scales_copy (NsMode *mode)
{
  GArray *a = g_array_new (FALSE, FALSE, sizeof (double));
  g_array_append_vals (a, mode->scales->data, mode->scales->len);
  return a;
}

/* of `scales`: `want` if it is there, else `fallback`, else the one nearest `fallback`; 1 from none */
static double
pick_scale (GArray *scales, double want, double fallback)
{
  double best = 0;
  for (guint i = 0; i < scales->len; i++)
    if (SAME_SCALE (g_array_index (scales, double, i), want))
      return g_array_index (scales, double, i);
  for (guint i = 0; i < scales->len; i++) {
    double s = g_array_index (scales, double, i);
    if (!best || fabs (s - fallback) < fabs (best - fallback))
      best = s;
  }
  return best ? best : 1;
}

static gint
size_bigger (gconstpointer a, gconstpointer b)
{
  const NsSize *x = a, *y = b;
  gint64 ax = (gint64) x->width * x->height, ay = (gint64) y->width * y->height;
  return ax > ay ? -1 : ax < ay;
}

GArray *
ns_monitors_mirror_sizes (NsMonitors *m)
{
  GArray *sizes = g_array_new (FALSE, FALSE, sizeof (NsSize));
  if (m->monitors->len < 2)
    return sizes;
  /* GNOME Settings starts from the first display that is on */
  NsMonitor *base = m->monitors->pdata[0];
  for (guint i = 0; i < m->monitors->len; i++)
    if (((NsMonitor *) m->monitors->pdata[i])->logical) {
      base = m->monitors->pdata[i];
      break;
    }
  for (guint i = 0; i < base->modes->len; i++) {
    NsMode *mo = base->modes->pdata[i];
    gboolean seen = FALSE;
    for (guint k = 0; k < sizes->len && !seen; k++)
      seen = g_array_index (sizes, NsSize, k).width == mo->width && g_array_index (sizes, NsSize, k).height == mo->height;
    if (seen)
      continue;
    g_autoptr (GArray) scales = scales_copy (mo);
    gboolean all = TRUE;
    for (guint k = 0; k < m->monitors->len && all; k++) {
      NsMonitor *other = m->monitors->pdata[k];
      NsMode *match = NULL;
      for (guint j = 0; other != base && j < other->modes->len && !match; j++) {
        NsMode *om = other->modes->pdata[j];
        if (om->width == mo->width && om->height == mo->height && om->interlaced == mo->interlaced)
          match = om;
      }
      if (other == base)
        continue;
      if (match)
        keep_scales_of (scales, match);
      all = match != NULL;
    }
    if (all && scales->len)
      g_array_append_val (sizes, ((NsSize) { mo->width, mo->height }));
  }
  g_array_sort (sizes, size_bigger);
  return sizes;
}

/* ---- configurations ---- */

static void
logical_config_free (NsLogicalConfig *l)
{
  g_ptr_array_unref (l->placed);
  g_free (l);
}

static NsLogicalConfig *
logical_config_add (NsConfig *c, int x, int y, double scale, guint32 transform, gboolean primary)
{
  NsLogicalConfig *l = g_new0 (NsLogicalConfig, 1);
  l->x = x;
  l->y = y;
  l->scale = scale;
  l->transform = transform;
  l->primary = primary;
  l->placed = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (c, l);
  return l;
}

static void
place (NsLogicalConfig *l, NsMonitor *mon, NsMode *mode)
{
  NsPlaced *p = g_new0 (NsPlaced, 1);
  p->monitor = mon;
  p->mode = mode;
  g_ptr_array_add (l->placed, p);
}

static NsConfig *
config_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) logical_config_free);
}

NsLogicalConfig *
ns_config_find (NsConfig *c, NsMonitor *mon)
{
  for (guint i = 0; i < c->len; i++) {
    NsLogicalConfig *l = c->pdata[i];
    for (guint k = 0; k < l->placed->len; k++)
      if (((NsPlaced *) l->placed->pdata[k])->monitor == mon)
        return l;
  }
  return NULL;
}

GArray *
ns_config_scales (NsConfig *c, NsMonitor *mon)
{
  NsLogicalConfig *l = ns_config_find (c, mon);
  if (!l || !l->placed->len)
    return g_array_new (FALSE, FALSE, sizeof (double));
  GArray *scales = scales_copy (((NsPlaced *) l->placed->pdata[0])->mode);
  for (guint k = 1; k < l->placed->len; k++)
    keep_scales_of (scales, ((NsPlaced *) l->placed->pdata[k])->mode);
  return scales;
}

/* the logical monitor's scale, made one all of its displays take at their modes */
static void
fit_scale (NsLogicalConfig *l, double fallback)
{
  if (!l->placed->len)
    return;
  g_autoptr (GArray) scales = scales_copy (((NsPlaced *) l->placed->pdata[0])->mode);
  for (guint k = 1; k < l->placed->len; k++)
    keep_scales_of (scales, ((NsPlaced *) l->placed->pdata[k])->mode);
  l->scale = pick_scale (scales, l->scale, fallback);
}

void
ns_logical_config_size (NsMonitors *m, NsLogicalConfig *l, int *width, int *height)
{
  NsMode *mode = l->placed->len ? ((NsPlaced *) l->placed->pdata[0])->mode : NULL;
  int w = mode ? mode->width : 0, h = mode ? mode->height : 0;
  /* 90° and 270°, flipped or not */
  if (l->transform % 2) {
    int t = w;
    w = h;
    h = t;
  }
  /* as mutter's derive_logical_monitor_size works it out, in floats */
  if (m->layout_mode == NS_LAYOUT_LOGICAL && l->scale > 0) {
    w = (int) roundf ((float) w / (float) l->scale);
    h = (int) roundf ((float) h / (float) l->scale);
  }
  *width = w;
  *height = h;
}

static gint
left_first (gconstpointer a, gconstpointer b)
{
  const NsLogicalConfig *x = *(NsLogicalConfig *const *) a, *y = *(NsLogicalConfig *const *) b;
  if (x->x != y->x)
    return x->x < y->x ? -1 : 1;
  return (x->y > y->y) - (x->y < y->y);
}

/* the screens in a row at the top, in the order they are in from the left (g_ptr_array_sort is stable) */
static void
lay_out (NsMonitors *m, NsConfig *c)
{
  g_ptr_array_sort (c, left_first);
  int x = 0;
  for (guint i = 0; i < c->len; i++) {
    NsLogicalConfig *l = c->pdata[i];
    int w, h;
    ns_logical_config_size (m, l, &w, &h);
    l->x = x;
    l->y = 0;
    x += w;
  }
}

static gboolean
has_primary (NsConfig *c)
{
  for (guint i = 0; i < c->len; i++)
    if (((NsLogicalConfig *) c->pdata[i])->primary)
      return TRUE;
  return FALSE;
}

/* a main display where there is none: the built-in panel's screen, if it is on, else the first */
static void
ensure_primary (NsMonitors *m, NsConfig *c)
{
  if (!c->len || has_primary (c))
    return;
  NsMonitor *builtin = ns_monitors_builtin (m);
  NsLogicalConfig *l = builtin ? ns_config_find (c, builtin) : NULL;
  (l ? l : (NsLogicalConfig *) c->pdata[0])->primary = TRUE;
}

NsConfig *
ns_config_current (NsMonitors *m)
{
  NsConfig *c = config_new ();
  for (guint i = 0; i < m->logical->len; i++) {
    NsLogical *ml = m->logical->pdata[i];
    NsLogicalConfig *l = logical_config_add (c, ml->x, ml->y, ml->scale, ml->transform, ml->primary);
    for (guint k = 0; k < ml->monitors->len; k++) {
      NsMonitor *mon = ml->monitors->pdata[k];
      place (l, mon, mon->mode);
    }
  }
  return c;
}

static gint
logical_left_first (gconstpointer a, gconstpointer b)
{
  const NsLogical *x = *(NsLogical *const *) a, *y = *(NsLogical *const *) b;
  if (x->x != y->x)
    return x->x < y->x ? -1 : 1;
  return (x->y > y->y) - (x->y < y->y);
}

NsConfig *
ns_config_join (NsMonitors *m)
{
  NsConfig *c = config_new ();
  NsMonitor *primary = ns_monitors_primary (m);
  /* the screens on now, from the left; then the displays that are off */
  g_autoptr (GPtrArray) row = g_ptr_array_new ();
  g_autoptr (GPtrArray) logical = g_ptr_array_new ();
  g_ptr_array_extend (logical, m->logical, NULL, NULL);
  g_ptr_array_sort (logical, logical_left_first);
  for (guint i = 0; i < logical->len; i++) {
    NsLogical *ml = logical->pdata[i];
    for (guint k = 0; k < ml->monitors->len; k++)
      g_ptr_array_add (row, ml->monitors->pdata[k]);
  }
  for (guint i = 0; i < m->monitors->len; i++)
    if (!((NsMonitor *) m->monitors->pdata[i])->logical)
      g_ptr_array_add (row, m->monitors->pdata[i]);

  int x = 0;
  for (guint i = 0; i < row->len; i++) {
    NsMonitor *mon = row->pdata[i];
    NsMode *mode = mon->mode ? mon->mode : ns_monitor_preferred_mode (mon);
    if (!mode)
      continue;
    /* out of a mirror, the built-in panel keeps the turn it had; the others start upright */
    NsLogical *ml = mon->logical;
    guint32 transform = ml && (ml->monitors->len == 1 || mon->builtin) ? ml->transform : 0;
    NsLogicalConfig *l = logical_config_add (c, x++, 0, ml ? ml->scale : mode->preferred_scale, transform,
                                             mon == primary);
    place (l, mon, mode);
    fit_scale (l, mode->preferred_scale);
  }
  ensure_primary (m, c);
  lay_out (m, c);
  return c;
}

NsConfig *
ns_config_mirror (NsMonitors *m, int width, int height)
{
  g_autoptr (GArray) sizes = ns_monitors_mirror_sizes (m);
  gboolean offered = FALSE;
  for (guint i = 0; i < sizes->len && !offered; i++)
    offered = g_array_index (sizes, NsSize, i).width == width && g_array_index (sizes, NsSize, i).height == height;
  if (!offered)
    return NULL;
  NsMonitor *primary = ns_monitors_primary (m);
  NsConfig *c = config_new ();
  /* GNOME Settings' choice: the main display's scale where every display takes it, else the mode's preferred one;
   * the main display's turn */
  NsLogical *pl = primary ? primary->logical : NULL;
  NsLogicalConfig *l = logical_config_add (c, 0, 0, pl ? pl->scale : 0, pl ? pl->transform : 0, TRUE);
  for (guint i = 0; i < m->monitors->len; i++) {
    NsMonitor *mon = m->monitors->pdata[i];
    /* a display already at that size keeps its refresh rate, the others take their fastest */
    double refresh = mon->mode && mon->mode->width == width && mon->mode->height == height ? mon->mode->refresh : 0;
    place (l, mon, ns_monitor_mode_for_size (mon, width, height, refresh));
  }
  NsMode *first = ((NsPlaced *) l->placed->pdata[0])->mode;
  if (!pl)
    l->scale = first->preferred_scale;
  fit_scale (l, first->preferred_scale);
  return c;
}

void
ns_config_set_primary (NsConfig *c, NsMonitor *mon)
{
  NsLogicalConfig *with = ns_config_find (c, mon);
  if (!with)
    return;
  for (guint i = 0; i < c->len; i++)
    ((NsLogicalConfig *) c->pdata[i])->primary = c->pdata[i] == with;
}

void
ns_config_set_mode (NsMonitors *m, NsConfig *c, NsMonitor *mon, NsMode *mode)
{
  NsLogicalConfig *l = ns_config_find (c, mon);
  if (!l || !mode)
    return;
  for (guint k = 0; k < l->placed->len; k++) {
    NsPlaced *p = l->placed->pdata[k];
    if (p->monitor == mon)
      p->mode = mode;
    else if (p->mode->width != mode->width || p->mode->height != mode->height) {
      /* a mirror at a new size: the others to their mode of it nearest the rate they had */
      NsMode *same = ns_monitor_mode_for_size (p->monitor, mode->width, mode->height, p->mode->refresh);
      if (same)
        p->mode = same;
    }
  }
  fit_scale (l, mode->preferred_scale);
  lay_out (m, c);
}

void
ns_config_set_on (NsMonitors *m, NsConfig *c, NsMonitor *mon, gboolean on)
{
  NsLogicalConfig *l = ns_config_find (c, mon);
  if (on && !l) {
    NsMode *mode = ns_monitor_preferred_mode (mon);
    if (!mode)
      return;
    /* on the right of the row */
    l = logical_config_add (c, G_MAXINT, 0, mode->preferred_scale, 0, FALSE);
    place (l, mon, mode);
    fit_scale (l, mode->preferred_scale);
  } else if (!on && l) {
    guint placed = 0;
    for (guint i = 0; i < c->len; i++)
      placed += ((NsLogicalConfig *) c->pdata[i])->placed->len;
    if (placed < 2)
      return;
    for (guint k = 0; k < l->placed->len; k++)
      if (((NsPlaced *) l->placed->pdata[k])->monitor == mon)
        g_ptr_array_remove_index (l->placed, k--);
    if (!l->placed->len)
      g_ptr_array_remove (c, l);
    else
      fit_scale (l, ((NsPlaced *) l->placed->pdata[0])->mode->preferred_scale);
    ensure_primary (m, c);
  } else {
    return;
  }
  lay_out (m, c);
}

void
ns_config_set_scale (NsMonitors *m, NsConfig *c, NsMonitor *mon, double scale)
{
  NsLogicalConfig *l = ns_config_find (c, mon);
  if (!l)
    return;
  l->scale = scale;
  fit_scale (l, scale);
  lay_out (m, c);
}

GVariant *
ns_config_to_variant (NsMonitors *m, NsConfig *c, guint method)
{
  GVariantBuilder logical, props;
  g_variant_builder_init (&logical, G_VARIANT_TYPE ("a(iiduba(ssa{sv}))"));
  for (guint i = 0; i < c->len; i++) {
    NsLogicalConfig *l = c->pdata[i];
    GVariantBuilder mons;
    g_variant_builder_init (&mons, G_VARIANT_TYPE ("a(ssa{sv})"));
    for (guint k = 0; k < l->placed->len; k++) {
      NsPlaced *p = l->placed->pdata[k];
      g_variant_builder_add (&mons, "(ss@a{sv})", p->monitor->connector, p->mode->id, p->monitor->props);
    }
    g_variant_builder_add (&logical, "(iidub@a(ssa{sv}))", l->x, l->y, l->scale, l->transform, l->primary,
                           g_variant_builder_end (&mons));
  }
  g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
  /* as GNOME Settings does: the layout mode it is in, where mutter lets it be changed */
  if (m->can_change_layout_mode)
    g_variant_builder_add (&props, "{sv}", "layout-mode", g_variant_new_uint32 (m->layout_mode));
  return g_variant_new ("(uu@a(iiduba(ssa{sv}))@a{sv})", m->serial, method, g_variant_builder_end (&logical),
                        g_variant_builder_end (&props));
}

/* ---- Display size ---- */

/* the sizes from the screen's own scale, smallest first: a phone has one larger, a tablet three */
static const double phone_steps[] = { 0.70, 0.80, 0.90, 1.0, 1.125 };
static const double tablet_steps[] = { 0.70, 0.80, 0.90, 1.0, 1.1625, 1.331, 1.5 };

GArray *
ns_display_size_stops (GArray *scales, double native, int short_side)
{
  GArray *stops = g_array_new (FALSE, FALSE, sizeof (NsSizeStop));
  if (!scales || !scales->len || native <= 0)
    return stops;
  gboolean tablet = short_side / native >= 600;
  const double *f = tablet ? tablet_steps : phone_steps;
  guint n = tablet ? G_N_ELEMENTS (tablet_steps) : G_N_ELEMENTS (phone_steps);
  for (guint i = 0; i < n; i++) {
    double want = native * f[i], best = 0;
    for (guint k = 0; k < scales->len; k++) {
      double s = g_array_index (scales, double, k);
      if (!best || fabs (s - want) < fabs (best - want))
        best = s;
    }
    NsSizeStop stop = { (int) i - 3, best };
    /* two steps the compositor gives the same scale: the one whose own size it is closer to stays, and the default
     * always does */
    NsSizeStop *last = stops->len ? &g_array_index (stops, NsSizeStop, stops->len - 1) : NULL;
    if (last && fabs (last->scale - best) < 1e-6) {
      double was = native * f[last->step + 3];
      if (last->step != 0 && (stop.step == 0 || fabs (best - want) < fabs (best - was)))
        *last = stop;
      continue;
    }
    g_array_append_val (stops, stop);
  }
  return stops;
}
