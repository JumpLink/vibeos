/* The displays as mutter has them (org.gnome.Mutter.DisplayConfig's GetCurrentState), and the configurations Display
 * asks it for (ApplyMonitorsConfig): Join, Mirror, a main display, a display's mode, on or off, the built-in panel's
 * scale. GLib only: the tests use it as it is. */
#pragma once

#include <gio/gio.h>

/* GetCurrentState's answer */
#define NS_MONITORS_STATE_TYPE "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"

/* ApplyMonitorsConfig's methods */
enum { NS_MONITORS_VERIFY = 0, NS_MONITORS_TEMPORARY = 1, NS_MONITORS_PERSISTENT = 2 };

/* how logical monitors are sized: their mode divided by their scale, or the mode's pixels */
enum { NS_LAYOUT_LOGICAL = 1, NS_LAYOUT_PHYSICAL = 2 };

typedef struct {
  char    *id;                  /* "1920x1080@60.000" */
  int      width, height;
  double   refresh;
  double   preferred_scale;
  GArray  *scales;              /* double: the scales the mode can take */
  gboolean current, preferred, interlaced;
} NsMode;

typedef struct _NsLogical NsLogical;

typedef struct {
  char      *connector, *vendor, *product, *serial;
  char      *name;              /* "Built-in display", "LG Electronics LG TV SSCR2" */
  gboolean   builtin;
  GPtrArray *modes;             /* NsMode, as mutter lists them: biggest first */
  NsMode    *mode;              /* the current one, NULL while the display is off */
  NsLogical *logical;           /* the logical monitor it is in, NULL while off */
  GVariant  *props;             /* what goes back to mutter as it is: color-mode, rgb-range, underscanning */
} NsMonitor;

struct _NsLogical {
  int        x, y;
  double     scale;
  guint32    transform;
  gboolean   primary;
  GPtrArray *monitors;          /* NsMonitor, not owned */
};

typedef struct {
  guint32    serial;
  GPtrArray *monitors;          /* NsMonitor: the connected displays (not those leased out) */
  GPtrArray *logical;           /* NsLogical */
  guint32    layout_mode;
  gboolean   can_change_layout_mode;
} NsMonitors;

NsMonitors *ns_monitors_new (GVariant *state, GError **error);
/* GetCurrentState's answer as text, as gdbus prints it */
NsMonitors *ns_monitors_new_from_text (const char *text, GError **error);
void        ns_monitors_free (NsMonitors *m);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NsMonitors, ns_monitors_free)

NsMonitor  *ns_monitors_find (NsMonitors *m, const char *connector);
NsMonitor  *ns_monitors_builtin (NsMonitors *m);
/* the main display's monitor (the primary logical monitor's first), NULL when none is on */
NsMonitor  *ns_monitors_primary (NsMonitors *m);
/* one logical monitor holding more than one display */
gboolean    ns_monitors_is_mirror (NsMonitors *m);
guint       ns_monitors_n_on (NsMonitors *m);

NsMode     *ns_monitor_find_mode (NsMonitor *mon, const char *id);
/* the mode of `width` x `height` with the refresh rate closest to `refresh` (0: the highest), or NULL */
NsMode     *ns_monitor_mode_for_size (NsMonitor *mon, int width, int height, double refresh);
NsMode     *ns_monitor_preferred_mode (NsMonitor *mon);
gboolean    ns_mode_has_scale (NsMode *mode, double scale);

/* The sizes every connected display has a mode of, with a scale they all take, as GNOME Settings finds its mirror
 * modes: the first display's modes, kept where every other one has one of the same size. Mutter mirrors only
 * displays at the same resolution. An array of NsSize, biggest first, empty when there is none. */
typedef struct {
  int width, height;
} NsSize;
GArray     *ns_monitors_mirror_sizes (NsMonitors *m);

/* ---- configurations: what is asked of mutter ---- */

typedef struct {
  NsMonitor *monitor;
  NsMode    *mode;
} NsPlaced;

typedef struct {
  int        x, y;
  double     scale;
  guint32    transform;
  gboolean   primary;
  GPtrArray *placed;            /* NsPlaced, owned */
} NsLogicalConfig;

/* NsLogicalConfig, owned */
typedef GPtrArray NsConfig;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NsConfig, g_ptr_array_unref)

/* the displays as they are now */
NsConfig   *ns_config_current (NsMonitors *m);
/* every connected display on, each a screen of its own, side by side: the ones on now stay where they are in the
 * row, at their mode and scale; the others join them on the right, at their preferred mode and scale */
NsConfig   *ns_config_join (NsMonitors *m);
/* every connected display showing one screen of `width` x `height`, or NULL when that is no mirror size */
NsConfig   *ns_config_mirror (NsMonitors *m, int width, int height);
/* the main display: the logical monitor holding `mon` */
void        ns_config_set_primary (NsConfig *c, NsMonitor *mon);
/* a display's mode; in a mirror, a new size takes the others along */
void        ns_config_set_mode (NsMonitors *m, NsConfig *c, NsMonitor *mon, NsMode *mode);
/* a display on (on the right, at its preferred mode) or off; the last one on stays on */
void        ns_config_set_on (NsMonitors *m, NsConfig *c, NsMonitor *mon, gboolean on);
/* the scale of the logical monitor holding `mon` */
void        ns_config_set_scale (NsMonitors *m, NsConfig *c, NsMonitor *mon, double scale);
/* the scales `mon`'s logical monitor can take: those every display in it takes at its mode */
GArray     *ns_config_scales (NsConfig *c, NsMonitor *mon);
NsLogicalConfig *ns_config_find (NsConfig *c, NsMonitor *mon);
/* the logical monitor's size as mutter works it out */
void        ns_logical_config_size (NsMonitors *m, NsLogicalConfig *l, int *width, int *height);

/* ApplyMonitorsConfig's arguments */
GVariant   *ns_config_to_variant (NsMonitors *m, NsConfig *c, guint method);

/* ---- Display size ---- */

/* one size Display size offers: its step from the screen's own scale (0 the default, below 0 smaller, above 0
 * larger) and the scale it is set at */
typedef struct {
  int    step;
  double scale;
} NsSizeStop;

/* The sizes Display size offers, smallest first: for each step from the screen's own scale `native`, the scale of
 * `scales` (those the screen takes) closest to it; steps that come to the same scale are offered once. A screen whose
 * shorter side (`short_side`, in screen pixels) is 600 or more at `native` is a tablet's, with three steps larger
 * rather than one. An array of NsSizeStop, empty without scales. */
GArray     *ns_display_size_stops (GArray *scales, double native, int short_side);
