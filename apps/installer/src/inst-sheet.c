/* The question as a bottom sheet over the app that asked (inst-sheet.h) */
#include "inst-sheet.h"
#include "inst-demo.h"
#include "inst-view.h"

#include <glib/gi18n.h>
#include <string.h>
#include <vibeos.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#define SLIDE_MS 250
#define MAX_WIDTH 640   /* on a wider screen (a tablet) the sheet keeps to this, centred */

typedef struct {
  GtkWidget *window, *revealer, *scrim;
  InstJob   *job;
  char      *parent;
  gboolean   closing;
} Sheet;

static void
sheet_free (gpointer data)
{
  Sheet *sheet = data;

  g_signal_handlers_disconnect_by_data (sheet->job, sheet);
  g_object_unref (sheet->job);
  g_free (sheet->parent);
  g_free (sheet);
}

static gboolean
on_slid_away (gpointer data)
{
  gtk_window_destroy (GTK_WINDOW (data));
  return G_SOURCE_REMOVE;
}

static void
slide_away (Sheet *sheet)
{
  if (sheet->closing)
    return;
  sheet->closing = TRUE;
  gtk_revealer_set_reveal_child (GTK_REVEALER (sheet->revealer), FALSE);
  gtk_widget_add_css_class (sheet->scrim, "inst-scrim-out");
  g_timeout_add_full (G_PRIORITY_DEFAULT, SLIDE_MS + 20, on_slid_away, g_object_ref (sheet->window), g_object_unref);
}

static void
on_job_changed (InstJob *job, Sheet *sheet)
{
  /* answered: the job goes on without the sheet (the caller follows it through the API's signals) */
  if (inst_job_get_phase (job) != INST_PHASE_READY)
    slide_away (sheet);
}

static void
cancel (Sheet *sheet)
{
  if (!inst_job_is_finished (sheet->job) && inst_job_get_phase (sheet->job) != INST_PHASE_RUNNING)
    inst_job_cancel (sheet->job);
  slide_away (sheet);
}

static void
on_done (InstView *view, Sheet *sheet)
{
  cancel (sheet);
}

static void
on_scrim_pressed (GtkGestureClick *gesture, int n, double x, double y, Sheet *sheet)
{
  cancel (sheet);
}

static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  cancel (data);
  return TRUE;
}

static gboolean
on_close_request (GtkWindow *window, Sheet *sheet)
{
  if (!sheet->closing)
    {
      cancel (sheet);
      return TRUE;
    }
  return FALSE;
}

static void
on_realize (GtkWidget *window, Sheet *sheet)
{
#ifdef GDK_WINDOWING_WAYLAND
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));

  /* xdg-foreign: over the caller's window, as its child */
  if (sheet->parent && g_str_has_prefix (sheet->parent, "wayland:") && GDK_IS_WAYLAND_TOPLEVEL (surface))
    {
      if (!gdk_wayland_toplevel_set_transient_for_exported (GDK_WAYLAND_TOPLEVEL (surface),
                                                            sheet->parent + strlen ("wayland:")))
        g_debug ("the caller's window handle %s is not valid", sheet->parent);
    }
#endif
}

/* the sheet across a phone's width; on a wider window, MAX_WIDTH wide in the middle */
static void
update_width (VibeosLayout *layout, GtkWidget *revealer)
{
  int side = MAX (0, (vibeos_layout_get_width (layout) - MAX_WIDTH) / 2);

  gtk_widget_set_margin_start (revealer, side);
  gtk_widget_set_margin_end (revealer, side);
}

static gboolean
on_reveal (gpointer data)
{
  gtk_revealer_set_reveal_child (GTK_REVEALER (data), TRUE);
  return G_SOURCE_REMOVE;
}

GtkWidget *
inst_sheet_new (GtkApplication *app, InstJob *job, const char *parent_window)
{
  Sheet *sheet = g_new0 (Sheet, 1);
  GtkWidget *window = vibeos_window_new (app);
  GtkWidget *root = gtk_overlay_new (), *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *handle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0), *view;
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  GtkGesture *click = gtk_gesture_click_new ();

  sheet->window = window;
  sheet->job = g_object_ref (job);
  sheet->parent = g_strdup (parent_window && *parent_window ? parent_window : NULL);
  g_object_set_data_full (G_OBJECT (window), "inst-sheet", sheet, sheet_free);
  gtk_window_set_title (GTK_WINDOW (window), _("App Installer"));
  gtk_widget_add_css_class (window, "inst-sheet-window");
  /* the demo has no app under the sheet: a plain backdrop stands in for it */
  if (inst_demo_active ())
    gtk_widget_add_css_class (window, "inst-demo-backdrop");

  sheet->scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (sheet->scrim, "inst-scrim");
  gtk_widget_set_hexpand (sheet->scrim, TRUE);
  gtk_widget_set_vexpand (sheet->scrim, TRUE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_scrim_pressed), sheet);
  gtk_widget_add_controller (sheet->scrim, GTK_EVENT_CONTROLLER (click));
  gtk_overlay_set_child (GTK_OVERLAY (root), sheet->scrim);

  /* the sheet: its drag handle, the question and the buttons */
  gtk_widget_add_css_class (card, "inst-sheet");
  gtk_widget_add_css_class (handle, "inst-sheet-handle");
  gtk_widget_set_halign (handle, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (card), handle);
  view = inst_view_new (job, INST_VIEW_SHEET);
  g_signal_connect (view, "done", G_CALLBACK (on_done), sheet);
  gtk_box_append (GTK_BOX (card), view);
  /* the dialog role: the shell's back gesture then sends Escape, which answers no */
  gtk_accessible_update_property (GTK_ACCESSIBLE (card), GTK_ACCESSIBLE_PROPERTY_LABEL, _("App Installer"), -1);

  sheet->revealer = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (sheet->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_transition_duration (GTK_REVEALER (sheet->revealer), SLIDE_MS);
  gtk_revealer_set_child (GTK_REVEALER (sheet->revealer), card);
  gtk_widget_set_valign (sheet->revealer, GTK_ALIGN_END);
  gtk_widget_set_halign (sheet->revealer, GTK_ALIGN_FILL);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), sheet->revealer);
  {
    GtkWidget *marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);

    gtk_accessible_update_property (GTK_ACCESSIBLE (marker), GTK_ACCESSIBLE_PROPERTY_LABEL, _("App Installer"), -1);
    gtk_widget_set_can_target (marker, FALSE);
    gtk_box_append (GTK_BOX (sheet->scrim), marker);
  }
  vibeos_window_set_content (GTK_WINDOW (window), root);

  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_escape, sheet, NULL)));
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Back, 0),
                                                          gtk_callback_action_new (on_escape, sheet, NULL)));
  gtk_widget_add_controller (window, shortcuts);
  g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), sheet);
  g_signal_connect (window, "realize", G_CALLBACK (on_realize), sheet);
  g_signal_connect (job, "changed", G_CALLBACK (on_job_changed), sheet);
  {
    VibeosLayout *layout = vibeos_layout_get_for_window (GTK_WINDOW (window));

    g_signal_connect_object (layout, "changed", G_CALLBACK (update_width), sheet->revealer, 0);
    update_width (layout, sheet->revealer);
  }
  /* the whole screen, so that the scrim covers the app under it */
  if (!inst_demo_active ())
    gtk_window_maximize (GTK_WINDOW (window));
  /* VIBEOS_SHOT draws a second after the window maps: the sheet is up by then without its slide */
  if (g_getenv ("VIBEOS_SHOT"))
    gtk_revealer_set_transition_duration (GTK_REVEALER (sheet->revealer), 0);
  g_idle_add_full (G_PRIORITY_DEFAULT, on_reveal, g_object_ref (sheet->revealer), g_object_unref);
  return window;
}
