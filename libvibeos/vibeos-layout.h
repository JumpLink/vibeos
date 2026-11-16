/* VibeosLayout: what kind of screen a window is on, for choosing between the phone and the tablet layouts.
 *
 * Every window has one (vibeos_layout_get_for_window). It answers three questions, from sizes only:
 *
 *   the device class   phone or tablet, from the smallest side of the monitor the window is on: 600 dp and more is a
 *                      tablet. Turning the device never changes it; a display-size change can (a tablet at its
 *                      largest display size has a phone's smallest width, and gets the phone layouts).
 *   the width class    compact (under 600 dp), medium (under 840 dp) or expanded, from the window's width.
 *   the orientation    landscape when the window is wider than tall.
 *
 * On a tablet the apps use their tablet layout always, upright too: two panes, a rail. The width class is for what
 * differs between an upright and a turned tablet (how many columns a grid has).
 *
 * It follows the window: a resize, a turn or a display-size change recomputes it, and whatever changed is notified
 * once, all of it together, then "changed" is emitted. Sizes are in dp, which is what a GTK px is here.
 *
 *   VibeosLayout *layout = vibeos_layout_get_for_window (window);
 *   g_signal_connect (layout, "changed", G_CALLBACK (on_layout_changed), self);
 *   if (vibeos_layout_is_tablet (layout)) ...
 *
 * VIBEOS_LAYOUT in the environment overrides the classes, for tests: a comma-separated list of "phone" or
 * "tablet", and "compact", "medium" or "expanded" (VIBEOS_LAYOUT=tablet, VIBEOS_LAYOUT=phone,expanded). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  VIBEOS_DEVICE_PHONE,
  VIBEOS_DEVICE_TABLET,
} VibeosDeviceClass;

typedef enum {
  VIBEOS_WIDTH_COMPACT,
  VIBEOS_WIDTH_MEDIUM,
  VIBEOS_WIDTH_EXPANDED,
} VibeosWidthClass;

/* the boundaries, in dp: a smallest width of VIBEOS_TABLET_MIN_WIDTH or more is a tablet; a window narrower than
 * VIBEOS_MEDIUM_MIN_WIDTH is compact, one narrower than VIBEOS_EXPANDED_MIN_WIDTH medium */
#define VIBEOS_TABLET_MIN_WIDTH   600
#define VIBEOS_MEDIUM_MIN_WIDTH   600
#define VIBEOS_EXPANDED_MIN_WIDTH 840

#define VIBEOS_TYPE_DEVICE_CLASS (vibeos_device_class_get_type ())
#define VIBEOS_TYPE_WIDTH_CLASS  (vibeos_width_class_get_type ())
GType vibeos_device_class_get_type (void);
GType vibeos_width_class_get_type  (void);

#define VIBEOS_TYPE_LAYOUT (vibeos_layout_get_type ())
G_DECLARE_FINAL_TYPE (VibeosLayout, vibeos_layout, VIBEOS, LAYOUT, GObject)

/* The window's layout, made on first use; the window owns it. Asked for before the window shows, it also makes the
 * window's default size the monitor's (the shell then fits it to its work area), so an app that lays itself out by
 * the layout starts at the size it will have. */
VibeosLayout     *vibeos_layout_get_for_window       (GtkWindow *window);
/* the layout of the window `widget` is in, or NULL while it is in none */
VibeosLayout     *vibeos_layout_get                  (GtkWidget *widget);

VibeosDeviceClass vibeos_layout_get_device_class     (VibeosLayout *self);
gboolean          vibeos_layout_is_tablet            (VibeosLayout *self);
VibeosWidthClass  vibeos_layout_get_width_class      (VibeosLayout *self);
gboolean          vibeos_layout_is_landscape         (VibeosLayout *self);
/* the window's size, and the monitor's smallest side, in dp */
int               vibeos_layout_get_width            (VibeosLayout *self);
int               vibeos_layout_get_height           (VibeosLayout *self);
int               vibeos_layout_get_smallest_width   (VibeosLayout *self);

/* The rules on their own, for code that has sizes but no window (tests, a preview of another size): the classes
 * for a monitor whose smallest side is `smallest_width` and a window `width` wide. No override applies. */
VibeosDeviceClass vibeos_layout_classify_device      (int smallest_width);
VibeosWidthClass  vibeos_layout_classify_width       (int width);

/* Properties (read-only, notified on change): "device-class", "width-class", "landscape", "width", "height",
 * "smallest-width". Signals: "changed" (): emitted once after any of them changed, with all of them up to date. */

G_END_DECLS
