/* App Installer's window (inst-window.h) */
#include "inst-window.h"
#include "inst-view.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <vibeos.h>

#define CONTENT_MAX 640   /* on a wider window (a tablet) a page's content keeps to this, centred, under its bar */

typedef struct {
  GtkWidget *window, *stack, *start, *job_page, *top_bar, *view, *marker;
  InstJob   *job;
  gboolean   ran;    /* the job has started installing or removing (only then is its end worth a notification) */
} Win;

static Win *
win_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (window), "inst-win");
}

static void
drop_job (Win *win)
{
  if (!win->job)
    return;
  g_signal_handlers_disconnect_by_data (win->job, win);
  g_clear_object (&win->job);
}

static void
win_free (gpointer data)
{
  Win *win = data;

  /* a job still being prepared or asked about goes with the window; a running one is PackageKit's now */
  if (win->job && !inst_job_is_finished (win->job) && inst_job_get_phase (win->job) != INST_PHASE_RUNNING)
    inst_job_cancel (win->job);
  drop_job (win);
  g_free (win);
}

/* a page's content across a phone's width; on a wider window CONTENT_MAX wide in the middle (a clamp, not margins
 * worked out from the width: those held the window too wide to turn upright) */
static GtkWidget *
cap_width (GtkWidget *content)
{
  return vibeos_clamp_new (content, CONTENT_MAX);
}

gboolean
inst_window_is_busy (GtkWidget *window)
{
  Win *win = win_of (window);

  return win->job && inst_job_get_phase (win->job) == INST_PHASE_RUNNING;
}

/* ---- Back ------------------------------------------------------------------------------------------------------ */

static void
leave_job (Win *win)
{
  if (win->job && !inst_job_is_finished (win->job))
    inst_job_cancel (win->job);
  drop_job (win);
  if (win->start && vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (win->stack)) > 1)
    {
      vibeos_page_stack_pop (VIBEOS_PAGE_STACK (win->stack));
      win->job_page = win->view = win->top_bar = NULL;
      gtk_widget_set_visible (win->marker, FALSE);
    }
  else
    gtk_window_close (GTK_WINDOW (win->window));
}

static void
back (Win *win)
{
  if (!win->job)
    {
      gtk_window_close (GTK_WINDOW (win->window));
      return;
    }
  /* a running install goes on: the window goes home, a notification says how it ended */
  if (inst_job_get_phase (win->job) == INST_PHASE_RUNNING)
    {
      gtk_window_minimize (GTK_WINDOW (win->window));
      return;
    }
  leave_job (win);
}

static gboolean
on_back_key (GtkWidget *widget, GVariant *args, gpointer data)
{
  back (data);
  return TRUE;
}

static void
on_top_bar_back (GtkWidget *bar, Win *win)
{
  back (win);
}

static void
on_view_done (InstView *view, Win *win)
{
  leave_job (win);
}

/* ---- the job page --------------------------------------------------------------------------------------------- */

static void
notify_end (Win *win)
{
  GApplication *app = G_APPLICATION (gtk_window_get_application (GTK_WINDOW (win->window)));
  const InstPlan *plan = inst_job_get_plan (win->job);
  g_autoptr (GNotification) n = NULL;
  g_autofree char *id = g_strdup_printf ("job-%u", inst_job_get_id (win->job));

  if (!app || !win->ran || gtk_window_is_active (GTK_WINDOW (win->window)))
    return;
  n = g_notification_new (inst_job_get_title (win->job) ? inst_job_get_title (win->job) : _("App Installer"));
  if (inst_job_get_text (win->job))
    g_notification_set_body (n, inst_job_get_text (win->job));
  g_notification_set_default_action (n, "app.show");
  if (inst_job_get_phase (win->job) == INST_PHASE_DONE && inst_job_get_kind (win->job) != INST_JOB_REMOVE
      && plan->desktop_id)
    g_notification_add_button_with_target (n, _("Open"), "app.open-app", "s", plan->desktop_id);
  g_application_send_notification (app, id, n);
}

static void
on_job_changed (InstJob *job, Win *win)
{
  gboolean running = inst_job_get_phase (job) == INST_PHASE_RUNNING;

  win->ran |= running;
  /* while it runs there is no back arrow (Back takes the window home, the job goes on) */
  if (win->top_bar)
    vibeos_top_bar_set_back (VIBEOS_TOP_BAR (win->top_bar), win->start && !running);
}

static void
on_job_finished (InstJob *job, Win *win)
{
  notify_end (win);
}

/* ⋮: About, VibeOS's About sheet, as every VibeOS app has it */
static const VibeosAboutInfo inst_about = { .name = "App Installer", .icon_name = "vibeos.Installer" };

static void
on_more (VibeosPressable *button, Win *win)
{
  VibeosMenu *menu = vibeos_menu_new ();

  vibeos_menu_append_about (menu, &inst_about);
  vibeos_menu_present (menu, GTK_WIDGET (button));
}

static GtkWidget *
page_new (const char *title, gboolean back, Win *win, GtkWidget **bar_out)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = vibeos_top_bar_new (title);
  GtkWidget *more;

  gtk_widget_add_css_class (page, "inst-page");
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), back);
  g_signal_connect (bar, "back", G_CALLBACK (on_top_bar_back), win);
  more = vibeos_top_bar_add_action (VIBEOS_TOP_BAR (bar), "view-more-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (more), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  g_signal_connect (more, "clicked", G_CALLBACK (on_more), win);
  gtk_box_append (GTK_BOX (page), bar);
  if (bar_out)
    *bar_out = bar;
  return page;
}

void
inst_window_show_job (GtkWidget *window, InstJob *job, gboolean over_start)
{
  Win *win = win_of (window);
  GtkWidget *page;

  /* the job shown before (not running: inst_window_is_busy) makes way */
  if (win->job)
    {
      if (!inst_job_is_finished (win->job))
        inst_job_cancel (win->job);
      drop_job (win);
      if (vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (win->stack)) > 1)
        vibeos_page_stack_pop (VIBEOS_PAGE_STACK (win->stack));
    }
  win->job = g_object_ref (job);
  win->ran = FALSE;
  over_start = over_start && win->start;
  page = page_new (_("App Installer"), over_start, win, &win->top_bar);
  win->view = inst_view_new (job, INST_VIEW_PAGE);
  gtk_widget_set_vexpand (win->view, TRUE);
  g_signal_connect (win->view, "done", G_CALLBACK (on_view_done), win);
  gtk_box_append (GTK_BOX (page), cap_width (win->view));
  win->job_page = page;
  g_signal_connect (job, "changed", G_CALLBACK (on_job_changed), win);
  g_signal_connect (job, "finished", G_CALLBACK (on_job_finished), win);
  if (over_start)
    vibeos_page_stack_push (VIBEOS_PAGE_STACK (win->stack), page);
  else
    {
      /* no start page under it: Back leaves the window */
      vibeos_page_stack_set_root (VIBEOS_PAGE_STACK (win->stack), page);
      win->start = NULL;
    }
  /* the back gesture: Escape (handled above) rather than leaving the app unasked */
  gtk_widget_set_visible (win->marker, TRUE);
  inst_job_prepare (job);
}

/* ---- the start page ------------------------------------------------------------------------------------------- */

static void
on_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) window = data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, NULL);
  g_autoptr (InstJob) job = NULL;

  if (!file || !win_of (window))
    return;
  job = inst_job_new_file (file);
  inst_window_show_job (window, job, TRUE);
}

static void
on_choose (GtkWidget *button, Win *win)
{
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  gtk_file_filter_set_name (filter, _("Software packages (.deb)"));
  gtk_file_filter_add_mime_type (filter, "application/vnd.debian.binary-package");
  gtk_file_filter_add_mime_type (filter, "application/x-deb");
  gtk_file_filter_add_suffix (filter, "deb");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_title (dialog, _("Choose a package"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, filter);
  gtk_file_dialog_set_modal (dialog, TRUE);
  {
    /* where downloads land */
    const char *dir = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
    g_autoptr (GFile) folder = dir ? g_file_new_for_path (dir) : NULL;

    if (folder)
      gtk_file_dialog_set_initial_folder (dialog, folder);
  }
  gtk_file_dialog_open (dialog, GTK_WINDOW (win->window), NULL, on_chosen, g_object_ref (win->window));
}

static GtkWidget *
start_page (Win *win)
{
  GtkWidget *page = page_new (_("App Installer"), FALSE, win, NULL);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *icon, *title, *text, *more, *button, *scroll;

  gtk_widget_add_css_class (box, "inst-start");
  gtk_widget_set_vexpand (box, TRUE);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);

  icon = gtk_image_new_from_icon_name ("vibeos.Installer");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 112);
  gtk_widget_add_css_class (icon, "inst-start-icon");
  gtk_box_append (GTK_BOX (box), icon);

  title = gtk_label_new (_("Install apps from packages"));
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_justify (GTK_LABEL (title), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (title, "inst-start-title");
  gtk_box_append (GTK_BOX (box), title);

  text = gtk_label_new (_("App Installer installs .deb package files. Tap one in Files, or choose one here: it "
                          "shows what the package is and asks before anything changes."));
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (text, "inst-start-text");
  gtk_box_append (GTK_BOX (box), text);

  button = vibeos_pressable_new (gtk_label_new (_("Choose a package")));
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 20);
  gtk_widget_add_css_class (button, "inst-filled-button");
  gtk_widget_add_css_class (button, "inst-start-button");
  gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
  g_signal_connect (button, "clicked", G_CALLBACK (on_choose), win);
  gtk_box_append (GTK_BOX (box), button);

  more = gtk_label_new (_("Software uses it too, to install and uninstall apps from the software sources."));
  gtk_label_set_wrap (GTK_LABEL (more), TRUE);
  gtk_label_set_justify (GTK_LABEL (more), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (more, "inst-start-more");
  gtk_box_append (GTK_BOX (box), more);

  /* centred in the page, and scrolled where a large font size makes it taller than the page */
  scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), cap_width (box));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  return page;
}

void
inst_window_show_start (GtkWidget *window)
{
  Win *win = win_of (window);

  if (win->start)
    return;
  win->start = start_page (win);
  vibeos_page_stack_set_root (VIBEOS_PAGE_STACK (win->stack), win->start);
}

GtkWidget *
inst_window_new (GtkApplication *app)
{
  Win *win = g_new0 (Win, 1);
  GtkWidget *window = vibeos_window_new (app), *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  GtkShortcutAction *action;

  win->window = window;
  g_object_set_data_full (G_OBJECT (window), "inst-win", win, win_free);
  gtk_window_set_title (GTK_WINDOW (window), _("App Installer"));
  gtk_widget_add_css_class (window, "inst-window");

  win->stack = vibeos_page_stack_new ();
  gtk_widget_set_vexpand (win->stack, TRUE);
  gtk_box_append (GTK_BOX (root), win->stack);
  /* the shell's back gesture asks the accessibility tree: a dialog showing means "send Escape" (handled by back
   * ()), nothing means "leave the app". A job page has its own Back, so it shows this marker. */
  win->marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (win->marker), GTK_ACCESSIBLE_PROPERTY_LABEL, _("App Installer"),
                                  -1);
  gtk_widget_set_can_target (win->marker, FALSE);
  gtk_widget_set_visible (win->marker, FALSE);
  gtk_box_append (GTK_BOX (root), win->marker);
  vibeos_window_set_content (GTK_WINDOW (window), root);

  action = gtk_callback_action_new (on_back_key, win, NULL);
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          g_object_ref (action)));
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Left, GDK_ALT_MASK),
                                                          g_object_ref (action)));
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Back, 0), action));
  gtk_widget_add_controller (window, shortcuts);
  return window;
}
