/* VibeosLayout: see vibeos-layout.h.
 *
 * The window's size comes from its surface ("layout", each time it is configured), or from its default size while it
 * has none; the smallest width from the monitor the surface entered last, or the display's first one before that. */
#define G_LOG_DOMAIN "vibeos-layout"

#include "vibeos-layout.h"

struct _VibeosLayout {
  GObject            parent;
  GtkWindow         *window;          /* the owner: it holds the layout until it goes */
  GdkSurface        *surface;         /* while the window is realized */
  GdkFrameClock     *clock;           /* the surface's */
  gboolean           pending;         /* the monitor changed: recompute on the next frame */
  GdkMonitor        *monitor;         /* the monitor watched, or NULL */
  GListModel        *monitors;        /* the display's, watched for one coming or going */

  VibeosDeviceClass  device;
  VibeosWidthClass   width_class;
  gboolean           landscape;
  int                width, height, smallest_width;
};

G_DEFINE_FINAL_TYPE (VibeosLayout, vibeos_layout, G_TYPE_OBJECT)

enum { PROP_0, PROP_DEVICE_CLASS, PROP_WIDTH_CLASS, PROP_LANDSCAPE, PROP_WIDTH, PROP_HEIGHT, PROP_SMALLEST_WIDTH,
       N_PROPS };
static GParamSpec *props[N_PROPS];

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- the enums ------------------------------------------------------------------------------------------------- */

GType
vibeos_device_class_get_type (void)
{
  static gsize type;
  static const GEnumValue values[] = {
    { VIBEOS_DEVICE_PHONE, "VIBEOS_DEVICE_PHONE", "phone" },
    { VIBEOS_DEVICE_TABLET, "VIBEOS_DEVICE_TABLET", "tablet" },
    { 0, NULL, NULL },
  };
  if (g_once_init_enter (&type))
    g_once_init_leave (&type, g_enum_register_static ("VibeosDeviceClass", values));
  return type;
}

GType
vibeos_width_class_get_type (void)
{
  static gsize type;
  static const GEnumValue values[] = {
    { VIBEOS_WIDTH_COMPACT, "VIBEOS_WIDTH_COMPACT", "compact" },
    { VIBEOS_WIDTH_MEDIUM, "VIBEOS_WIDTH_MEDIUM", "medium" },
    { VIBEOS_WIDTH_EXPANDED, "VIBEOS_WIDTH_EXPANDED", "expanded" },
    { 0, NULL, NULL },
  };
  if (g_once_init_enter (&type))
    g_once_init_leave (&type, g_enum_register_static ("VibeosWidthClass", values));
  return type;
}

/* ---- the rules ------------------------------------------------------------------------------------------------- */

VibeosDeviceClass
vibeos_layout_classify_device (int smallest_width)
{
  return smallest_width >= VIBEOS_TABLET_MIN_WIDTH ? VIBEOS_DEVICE_TABLET : VIBEOS_DEVICE_PHONE;
}

VibeosWidthClass
vibeos_layout_classify_width (int width)
{
  if (width < VIBEOS_MEDIUM_MIN_WIDTH)
    return VIBEOS_WIDTH_COMPACT;
  return width < VIBEOS_EXPANDED_MIN_WIDTH ? VIBEOS_WIDTH_MEDIUM : VIBEOS_WIDTH_EXPANDED;
}

/* VIBEOS_LAYOUT: -1 where it says nothing */
static void
overrides (int *device, int *width_class)
{
  static gsize done;
  static int forced_device = -1, forced_width = -1;

  if (g_once_init_enter (&done))
    {
      const char *env = g_getenv ("VIBEOS_LAYOUT");
      g_auto(GStrv) words = env ? g_strsplit (env, ",", 0) : NULL;

      for (int i = 0; words && words[i]; i++)
        {
          const char *word = g_strstrip (words[i]);

          if (g_str_equal (word, "phone"))
            forced_device = VIBEOS_DEVICE_PHONE;
          else if (g_str_equal (word, "tablet"))
            forced_device = VIBEOS_DEVICE_TABLET;
          else if (g_str_equal (word, "compact"))
            forced_width = VIBEOS_WIDTH_COMPACT;
          else if (g_str_equal (word, "medium"))
            forced_width = VIBEOS_WIDTH_MEDIUM;
          else if (g_str_equal (word, "expanded"))
            forced_width = VIBEOS_WIDTH_EXPANDED;
          else if (*word)
            g_warning ("VIBEOS_LAYOUT: \"%s\" is none of phone, tablet, compact, medium, expanded", word);
        }
      g_once_init_leave (&done, 1);
    }
  *device = forced_device;
  *width_class = forced_width;
}

/* ---- following the window -------------------------------------------------------------------------------------- */

static GdkMonitor *
first_monitor (VibeosLayout *self)
{
  return self->monitors && g_list_model_get_n_items (self->monitors)
         ? g_list_model_get_item (self->monitors, 0) : NULL;
}

/* `width` x `height`: the window's size when the caller knows it (from a configure), else 0 */
static void
recompute (VibeosLayout *self, int width, int height)
{
  GdkRectangle monitor = { 0 };
  int forced_device, forced_width, smallest;
  VibeosDeviceClass device;
  VibeosWidthClass width_class;
  gboolean landscape, changed = FALSE;

  if (self->monitor)
    gdk_monitor_get_geometry (self->monitor, &monitor);
  if (width <= 0 || height <= 0)
    {
      /* (a surface is 1 x 1 until its first configure) */
      if (self->surface && gdk_surface_get_width (self->surface) > 1 && gdk_surface_get_height (self->surface) > 1)
        {
          width = gdk_surface_get_width (self->surface);
          height = gdk_surface_get_height (self->surface);
        }
      else
        {
          gtk_window_get_default_size (self->window, &width, &height);
          width = width > 0 ? width : monitor.width;
          height = height > 0 ? height : monitor.height;
        }
    }
  /* no monitor yet (a display without one): the window is all there is to go by */
  smallest = monitor.width > 0 ? MIN (monitor.width, monitor.height) : MIN (width, height);

  overrides (&forced_device, &forced_width);
  device = forced_device >= 0 ? (VibeosDeviceClass) forced_device : vibeos_layout_classify_device (smallest);
  width_class = forced_width >= 0 ? (VibeosWidthClass) forced_width : vibeos_layout_classify_width (width);
  landscape = width > height;

  g_object_freeze_notify (G_OBJECT (self));
#define SET(field, value, prop)                                       \
  if (self->field != (value))                                          \
    {                                                                  \
      self->field = (value);                                           \
      g_object_notify_by_pspec (G_OBJECT (self), props[prop]);         \
      changed = TRUE;                                                  \
    }
  SET (width, width, PROP_WIDTH);
  SET (height, height, PROP_HEIGHT);
  SET (smallest_width, smallest, PROP_SMALLEST_WIDTH);
  SET (device, device, PROP_DEVICE_CLASS);
  SET (width_class, width_class, PROP_WIDTH_CLASS);
  SET (landscape, landscape, PROP_LANDSCAPE);
#undef SET
  g_object_thaw_notify (G_OBJECT (self));
  if (changed)
    g_signal_emit (self, signals[CHANGED], 0);
}

static void on_monitor_changed (VibeosLayout *self);

static void
watch_monitor (VibeosLayout *self, GdkMonitor *monitor)
{
  if (monitor == self->monitor)
    return;
  if (self->monitor)
    {
      g_signal_handlers_disconnect_by_data (self->monitor, self);
      g_clear_object (&self->monitor);
    }
  if (!monitor)
    return;
  self->monitor = g_object_ref (monitor);
  /* a turn or a display-size change changes the geometry; the scale on its own changes nothing of ours, but comes
   * first on some backends, with the geometry right after */
  g_signal_connect_swapped (monitor, "notify::geometry", G_CALLBACK (on_monitor_changed), self);
  g_signal_connect_swapped (monitor, "invalidate", G_CALLBACK (on_monitor_changed), self);
}

/* the monitor to go by: the one the window is on, or before it is on one, the display's first */
static void
pick_monitor (VibeosLayout *self)
{
  g_autoptr(GdkMonitor) first = NULL;
  GdkMonitor *monitor = NULL;

  if (self->surface)
    monitor = gdk_display_get_monitor_at_surface (gdk_surface_get_display (self->surface), self->surface);
  if (monitor && !gdk_monitor_is_valid (monitor))
    monitor = NULL;
  if (!monitor)
    monitor = first = first_monitor (self);
  watch_monitor (self, monitor);
}

/* A display that turns or changes size tells the window twice: the monitor's new geometry, then the window's new size
 * when the compositor fits it to the monitor. Both come before the next frame, so the layout waits for that frame
 * and changes once, with both. */
static void
recompute_soon (VibeosLayout *self)
{
  if (!self->clock)
    {
      recompute (self, 0, 0);
      return;
    }
  self->pending = TRUE;
  gdk_frame_clock_request_phase (self->clock, GDK_FRAME_CLOCK_PHASE_LAYOUT);
}

static void
on_clock_layout (GdkFrameClock *clock, VibeosLayout *self)
{
  if (!self->pending)
    return;
  self->pending = FALSE;
  recompute (self, 0, 0);
}

static void
on_monitor_changed (VibeosLayout *self)
{
  GdkRectangle geometry = { 0 };

  pick_monitor (self);
  if (self->monitor)
    gdk_monitor_get_geometry (self->monitor, &geometry);
  g_debug ("monitor %p: %d×%d", (void *) self->monitor, geometry.width, geometry.height);
  recompute_soon (self);
}

static void
on_monitors_changed (GListModel *monitors, guint position, guint removed, guint added, VibeosLayout *self)
{
  on_monitor_changed (self);
}

/* the surface lays out on the frame clock before the layout's own handler there runs */
static void
on_surface_layout (GdkSurface *surface, int width, int height, VibeosLayout *self)
{
  /* a surface lays out at 1 x 1 before its first configure: no size yet */
  if (width <= 1 || height <= 1)
    return;
  g_debug ("window: %d×%d", width, height);
  self->pending = FALSE;
  recompute (self, width, height);
}

static void
on_enter_monitor (GdkSurface *surface, GdkMonitor *monitor, VibeosLayout *self)
{
  watch_monitor (self, monitor);
  recompute_soon (self);
}

static void
on_realize (GtkWidget *window, VibeosLayout *self)
{
  self->surface = g_object_ref (gtk_native_get_surface (GTK_NATIVE (window)));
  g_signal_connect (self->surface, "layout", G_CALLBACK (on_surface_layout), self);
  g_signal_connect (self->surface, "enter-monitor", G_CALLBACK (on_enter_monitor), self);
  self->clock = gdk_surface_get_frame_clock (self->surface);
  if (self->clock)
    {
      g_object_ref (self->clock);
      g_signal_connect (self->clock, "layout", G_CALLBACK (on_clock_layout), self);
    }
  pick_monitor (self);
  recompute (self, 0, 0);
}

static void
on_unrealize (GtkWidget *window, VibeosLayout *self)
{
  if (!self->surface)
    return;
  g_signal_handlers_disconnect_by_data (self->surface, self);
  g_clear_object (&self->surface);
  if (self->clock)
    g_signal_handlers_disconnect_by_data (self->clock, self);
  g_clear_object (&self->clock);
  self->pending = FALSE;
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_layout_dispose (GObject *object)
{
  VibeosLayout *self = VIBEOS_LAYOUT (object);

  watch_monitor (self, NULL);
  if (self->monitors)
    {
      g_signal_handlers_disconnect_by_data (self->monitors, self);
      g_clear_object (&self->monitors);
    }
  if (self->surface)
    {
      g_signal_handlers_disconnect_by_data (self->surface, self);
      g_clear_object (&self->surface);
    }
  if (self->clock)
    {
      g_signal_handlers_disconnect_by_data (self->clock, self);
      g_clear_object (&self->clock);
    }
  G_OBJECT_CLASS (vibeos_layout_parent_class)->dispose (object);
}

static void
vibeos_layout_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  VibeosLayout *self = VIBEOS_LAYOUT (object);

  switch (prop_id)
    {
    case PROP_DEVICE_CLASS:
      g_value_set_enum (value, self->device);
      break;
    case PROP_WIDTH_CLASS:
      g_value_set_enum (value, self->width_class);
      break;
    case PROP_LANDSCAPE:
      g_value_set_boolean (value, self->landscape);
      break;
    case PROP_WIDTH:
      g_value_set_int (value, self->width);
      break;
    case PROP_HEIGHT:
      g_value_set_int (value, self->height);
      break;
    case PROP_SMALLEST_WIDTH:
      g_value_set_int (value, self->smallest_width);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
vibeos_layout_class_init (VibeosLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY;

  object_class->dispose = vibeos_layout_dispose;
  object_class->get_property = vibeos_layout_get_property;

  props[PROP_DEVICE_CLASS] = g_param_spec_enum ("device-class", NULL, NULL, VIBEOS_TYPE_DEVICE_CLASS,
                                                VIBEOS_DEVICE_PHONE, flags);
  props[PROP_WIDTH_CLASS] = g_param_spec_enum ("width-class", NULL, NULL, VIBEOS_TYPE_WIDTH_CLASS,
                                               VIBEOS_WIDTH_COMPACT, flags);
  props[PROP_LANDSCAPE] = g_param_spec_boolean ("landscape", NULL, NULL, FALSE, flags);
  props[PROP_WIDTH] = g_param_spec_int ("width", NULL, NULL, 0, G_MAXINT, 0, flags);
  props[PROP_HEIGHT] = g_param_spec_int ("height", NULL, NULL, 0, G_MAXINT, 0, flags);
  props[PROP_SMALLEST_WIDTH] = g_param_spec_int ("smallest-width", NULL, NULL, 0, G_MAXINT, 0, flags);
  g_object_class_install_properties (object_class, N_PROPS, props);

  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vibeos_layout_init (VibeosLayout *self)
{
}

VibeosLayout *
vibeos_layout_get_for_window (GtkWindow *window)
{
  VibeosLayout *self = g_object_get_data (G_OBJECT (window), "vibeos-layout");
  g_autoptr(GdkMonitor) first = NULL;

  if (self)
    return self;
  self = g_object_new (VIBEOS_TYPE_LAYOUT, NULL);
  self->window = window;
  g_object_set_data_full (G_OBJECT (window), "vibeos-layout", self, g_object_unref);

  self->monitors = g_object_ref (gdk_display_get_monitors (gtk_widget_get_display (GTK_WIDGET (window))));
  g_signal_connect (self->monitors, "items-changed", G_CALLBACK (on_monitors_changed), self);
  g_signal_connect (window, "realize", G_CALLBACK (on_realize), self);
  g_signal_connect (window, "unrealize", G_CALLBACK (on_unrealize), self);

  /* not shown yet: it opens at the monitor's size, the size it is going to have */
  first = first_monitor (self);
  if (first && !gtk_widget_get_realized (GTK_WIDGET (window)))
    {
      GdkRectangle geometry;

      gdk_monitor_get_geometry (first, &geometry);
      gtk_window_set_default_size (window, geometry.width, geometry.height);
    }
  if (gtk_widget_get_realized (GTK_WIDGET (window)))
    on_realize (GTK_WIDGET (window), self);
  else
    on_monitor_changed (self);
  return self;
}

VibeosLayout *
vibeos_layout_get (GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root (widget);

  return GTK_IS_WINDOW (root) ? vibeos_layout_get_for_window (GTK_WINDOW (root)) : NULL;
}

VibeosDeviceClass
vibeos_layout_get_device_class (VibeosLayout *self)
{
  return self->device;
}

gboolean
vibeos_layout_is_tablet (VibeosLayout *self)
{
  return self->device == VIBEOS_DEVICE_TABLET;
}

VibeosWidthClass
vibeos_layout_get_width_class (VibeosLayout *self)
{
  return self->width_class;
}

gboolean
vibeos_layout_is_landscape (VibeosLayout *self)
{
  return self->landscape;
}

int
vibeos_layout_get_width (VibeosLayout *self)
{
  return self->width;
}

int
vibeos_layout_get_height (VibeosLayout *self)
{
  return self->height;
}

int
vibeos_layout_get_smallest_width (VibeosLayout *self)
{
  return self->smallest_width;
}
