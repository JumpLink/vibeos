/* SwWindow: see sw-window.h */
#include "sw-window.h"
#include "sw-account.h"
#include "sw-api.h"
#include "sw-installer.h"
#include "sw-json.h"
#include "sw-pages.h"
#include "sw-updates.h"
#include "sw-widgets.h"

#include <spongy-gtk.h>
#include <stdlib.h>
#include <string.h>
#include <vibeos.h>

#define NAV_HEIGHT 80
#define STEP_MS    900

typedef struct {
  GtkWidget *window, *stack, *tabs, *tab_stack, *nav, *snackbar, *marker;
  GtkWidget *homes[4];
  int        tab;
  char     **steps;
  guint      step, step_timeout;
} Win;

static Win *
win_of (GtkWidget *widget)
{
  GtkRoot *root = widget ? gtk_widget_get_root (widget) : NULL;

  return root ? g_object_get_data (G_OBJECT (root), "sw-win") : NULL;
}

/* the shell's back gesture asks the accessibility tree: a dialog showing means "send Escape", nothing means "leave
 * the app". So the marker shows whenever Back has somewhere to go in here. */
static void
update_marker (Win *win)
{
  gboolean deeper = vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (win->stack)) > 1 || win->tab != SW_TAB_FEATURED;

  gtk_widget_set_visible (win->marker, deeper);
  gtk_widget_set_margin_bottom (win->snackbar,
                                vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (win->stack)) > 1 ? 0 : NAV_HEIGHT);
}

static void
select_tab (Win *win, int tab)
{
  win->tab = tab;
  gtk_stack_set_visible_child (GTK_STACK (win->tab_stack), win->homes[tab]);
  vibeos_nav_bar_set_selected (VIBEOS_NAV_BAR (win->nav), tab);
  update_marker (win);
}

static void
on_nav_selected (VibeosNavBar *nav, int index, Win *win)
{
  /* the tab that shows already, tapped again: back to its top */
  if (index == win->tab)
    {
      if (index != SW_TAB_MINE)
        sw_home_reset (win->homes[index]);
      return;
    }
  select_tab (win, index);
}

/* Back: the page on top goes; on the tabs, the first tab; there, the window closes */
static gboolean
back (Win *win)
{
  if (vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (win->stack)) > 1)
    {
      vibeos_page_stack_pop (VIBEOS_PAGE_STACK (win->stack));
      return TRUE;
    }
  if (win->tab != SW_TAB_FEATURED)
    {
      select_tab (win, SW_TAB_FEATURED);
      return TRUE;
    }
  gtk_window_close (GTK_WINDOW (win->window));
  return TRUE;
}

static gboolean
on_back_key (GtkWidget *widget, GVariant *args, gpointer data)
{
  return back (data);
}

static void
on_top_changed (VibeosPageStack *stack, Win *win)
{
  update_marker (win);
}

/* ---- pages ----------------------------------------------------------------------------------------------------- */

void
sw_window_push (GtkWidget *from, GtkWidget *page)
{
  Win *win = win_of (from);

  if (win)
    vibeos_page_stack_push (VIBEOS_PAGE_STACK (win->stack), page);
}

void
sw_window_pop (GtkWidget *from)
{
  Win *win = win_of (from);
  GtkWidget *top;

  if (!win)
    return;
  top = vibeos_page_stack_get_top (VIBEOS_PAGE_STACK (win->stack));
  /* only the page `from` is in, and never the tabs */
  if (top != win->tabs && (from == top || gtk_widget_is_ancestor (from, top)))
    vibeos_page_stack_pop (VIBEOS_PAGE_STACK (win->stack));
}

void
sw_window_pop_to_tabs (GtkWidget *from)
{
  Win *win = win_of (from);

  if (win)
    vibeos_page_stack_pop_to_root (VIBEOS_PAGE_STACK (win->stack));
}

void
sw_window_pop_account_flow (GtkWidget *from)
{
  Win *win = win_of (from);
  GtkWidget *top;

  while (win && (top = vibeos_page_stack_get_top (VIBEOS_PAGE_STACK (win->stack))) != win->tabs
         && g_object_get_data (G_OBJECT (top), "sw-account-flow"))
    vibeos_page_stack_pop (VIBEOS_PAGE_STACK (win->stack));
}

void
sw_window_replace (GtkWidget *from, GtkWidget *page)
{
  Win *win = win_of (from);
  GtkWidget *top;

  if (!win)
    return;
  top = vibeos_page_stack_get_top (VIBEOS_PAGE_STACK (win->stack));
  /* `from` under another page by now: the new one goes on top, the one over it stays */
  if (top != win->tabs && (from == top || gtk_widget_is_ancestor (from, top)))
    vibeos_page_stack_replace (VIBEOS_PAGE_STACK (win->stack), page);
  else
    vibeos_page_stack_push (VIBEOS_PAGE_STACK (win->stack), page);
}

void
sw_window_toast (GtkWidget *from, const char *text)
{
  Win *win = win_of (from);

  if (win)
    vibeos_snackbar_show (VIBEOS_SNACKBAR (win->snackbar), text, NULL, NULL, NULL, 4000);
}

void
sw_window_show_tab (GtkWidget *from, int tab)
{
  Win *win = win_of (from);

  if (!win)
    return;
  vibeos_page_stack_pop_to_root (VIBEOS_PAGE_STACK (win->stack));
  select_tab (win, tab);
}

void
sw_window_open_app (GtkWidget *from, const char *app_id, JsonObject *summary)
{
  if (app_id)
    sw_window_push (from, sw_app_page_new (app_id, summary));
}

void
sw_window_open_list (GtkWidget *from, const char *title, const char *path)
{
  sw_window_push (from, sw_list_page_new (title, path));
}

void
sw_window_open_target (GtkWidget *from, JsonObject *target, const char *title)
{
  const char *type = sw_json_string (target, "type"), *id = sw_json_string (target, "id");
  g_autofree char *escaped = sw_api_escape (id);
  g_autofree char *path = NULL;

  if (!id)
    return;
  if (!g_strcmp0 (type, "app"))
    sw_window_open_app (from, id, NULL);
  else if (!g_strcmp0 (type, "collection"))
    {
      path = g_strdup_printf ("/collections/%s/apps", escaped);
      sw_window_open_list (from, title, path);
    }
  else if (!g_strcmp0 (type, "category"))
    {
      path = g_strdup_printf ("/apps?category=%s", escaped);
      sw_window_open_list (from, title, path);
    }
}

void
sw_window_open_account (GtkWidget *from)
{
  sw_window_push (from, sw_profile_page_new ());
}

void
sw_window_open_search (GtkWidget *from, const char *type)
{
  sw_search_open (from, type, NULL);
}

/* ---- jobs and the account -------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  char      *package, *name;
} OpenAfter;

static void
open_after_free (gpointer data)
{
  OpenAfter *open = data;

  g_free (open->package);
  g_free (open->name);
  g_free (open);
}

static void
on_snackbar_open (gpointer data)
{
  OpenAfter *open = data;
  g_autoptr (JsonObject) app = json_object_new ();

  json_object_set_string_member (app, "package", open->package);
  json_object_set_string_member (app, "name", open->name);
  sw_app_activate (open->window, app);
}

static void
on_job_finished (SwInstaller *installer, const char *package, const char *name, SwJobKind kind, const char *result,
                 const char *message, GtkWidget *window)
{
  Win *win = g_object_get_data (G_OBJECT (window), "sw-win");
  g_autofree char *text = NULL;
  OpenAfter *open = NULL;

  if (!name || !*name)
    name = package;
  if (!g_strcmp0 (result, "done"))
    {
      if (kind == SW_JOB_REMOVE)
        text = g_strdup_printf ("%s is uninstalled", name);
      else
        {
          text = g_strdup_printf (kind == SW_JOB_UPDATE ? "%s is updated" : "%s is installed", name);
          open = g_new0 (OpenAfter, 1);
          open->window = win->window;
          open->package = g_strdup (package);
          open->name = g_strdup (name);
          if (kind == SW_JOB_INSTALL)
            sw_account_note_installed (sw_account_get_default (), package);
        }
    }
  else if (!g_strcmp0 (result, "cancelled"))
    text = g_strdup_printf (kind == SW_JOB_REMOVE ? "%s was not uninstalled" : "%s was not installed", name);
  else
    text = g_strdup (message && *message ? message
                     : kind == SW_JOB_REMOVE ? "The app could not be uninstalled." : "The app could not be installed.");
  if (open)
    {
      /* kept until the next message: the snackbar's action may run until then */
      g_object_set_data_full (G_OBJECT (win->snackbar), "sw-open", open, open_after_free);
      vibeos_snackbar_show (VIBEOS_SNACKBAR (win->snackbar), text, "Open", on_snackbar_open, open, 6000);
    }
  else
    vibeos_snackbar_show (VIBEOS_SNACKBAR (win->snackbar), text, NULL, NULL, NULL, 6000);
}

static void
on_updates_changed (SwUpdates *updates, GtkWidget *window)
{
  Win *win = g_object_get_data (G_OBJECT (window), "sw-win");
  g_autoptr (GPtrArray) list = sw_updates_list (updates);
  g_autofree char *count = list->len ? g_strdup_printf ("%u", list->len) : NULL;

  vibeos_nav_bar_set_badge (VIBEOS_NAV_BAR (win->nav), SW_TAB_MINE, count);
}

static void
on_account_notice (SwAccount *account, const char *text, GtkWidget *window)
{
  Win *win = g_object_get_data (G_OBJECT (window), "sw-win");

  vibeos_snackbar_show (VIBEOS_SNACKBAR (win->snackbar), text, NULL, NULL, NULL, 6000);
}

/* ---- steps (VIBEOS_SOFTWARE_PAGE) ------------------------------------------------------------------------------ */

/* For pictures and tests: the steps run one after another, STEP_MS apart, as if tapped.
 *
 *   tab:featured|apps|games|mine   a tab              app:ID            an app's page
 *   list:TITLE|PATH                a list of apps     categories:TYPE   the categories of apps or games
 *   search:TEXT                    the search view    results:TEXT      a search's results
 *   reviews:ID   write:ID          an app's reviews, writing one        report:REVIEW-ID   reporting one
 *   signin  register  verify  forgot:EMAIL  profile  settings  delete  myreviews  about    the account's pages
 *   allapps:0|1                    All apps, on this device or not      install:PACKAGE    install it
 *   popular                        the tab's Popular  type:TEXT         TEXT into the focused field
 *   scroll:PX                      the page PX down   toast:TEXT        a snackbar
 *   press:LABEL                    tap the button LABEL               fill:LABEL=TEXT    TEXT into the field LABEL
 *   code:DIGITS                    DIGITS into the code's boxes       text:TEXT          TEXT into the review's field
 *   wait:MS                        MS before the next step */

static gboolean run_step (gpointer data);

/* the first mapped widget in `widget` for which `match` is TRUE */
static GtkWidget *
find_widget (GtkWidget *widget, gboolean (*match) (GtkWidget *widget, const char *text), const char *text)
{
  if (!widget || !gtk_widget_get_mapped (widget))
    return NULL;
  if (match (widget, text))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_widget (child, match, text);

      if (found)
        return found;
    }
  return NULL;
}

static gboolean
has_label (GtkWidget *widget, const char *text)
{
  if (GTK_IS_LABEL (widget))
    return !g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), text);
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    if (has_label (child, text))
      return TRUE;
  return FALSE;
}

/* a button (any pressable) whose label reads `text`, or a list row with that title */
static gboolean
is_button (GtkWidget *widget, const char *text)
{
  GtkWidget *child;

  if (VIBEOS_IS_LIST_ROW (widget))
    return has_label (widget, text);
  if (!VIBEOS_IS_PRESSABLE (widget))
    return FALSE;
  child = vibeos_pressable_get_child (VIBEOS_PRESSABLE (widget));
  while (child && !GTK_IS_LABEL (child))
    child = GTK_IS_BOX (child) ? gtk_widget_get_last_child (child) : NULL;
  return child && !g_strcmp0 (gtk_label_get_text (GTK_LABEL (child)), text);
}

/* a field (sw_field_new) labelled `text` */
static gboolean
is_field (GtkWidget *widget, const char *text)
{
  GtkWidget *label = gtk_widget_get_first_child (widget);

  return gtk_widget_has_css_class (widget, "sw-field") && GTK_IS_LABEL (label)
         && !g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), text);
}

static gboolean
is_text_view (GtkWidget *widget, const char *text)
{
  return GTK_IS_TEXT_VIEW (widget);
}

static gboolean
is_code (GtkWidget *widget, const char *text)
{
  return !g_strcmp0 (gtk_widget_get_css_name (widget), "sw-code");
}

/* the first mapped scroll view in `widget` */
static GtkWidget *
find_scroll (GtkWidget *widget)
{
  if (!widget || !gtk_widget_get_mapped (widget))
    return NULL;
  if (SPONGY_IS_SCROLL_VIEW (widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_scroll (child);

      if (found)
        return found;
    }
  return NULL;
}

static void
do_step (Win *win, const char *step)
{
  g_auto (GStrv) parts = g_strsplit (step, ":", 2);
  const char *what = parts[0], *arg = parts[1] ? parts[1] : "";
  GtkWidget *from = win->stack;

  if (!strcmp (what, "tab"))
    select_tab (win, !strcmp (arg, "apps") ? SW_TAB_APPS : !strcmp (arg, "games") ? SW_TAB_GAMES
                     : !strcmp (arg, "mine") ? SW_TAB_MINE : SW_TAB_FEATURED);
  else if (!strcmp (what, "app"))
    sw_window_open_app (from, arg, NULL);
  else if (!strcmp (what, "list"))
    {
      g_auto (GStrv) list = g_strsplit (arg, "|", 2);

      if (list[0] && list[1])
        sw_window_open_list (from, list[0], list[1]);
    }
  else if (!strcmp (what, "categories"))
    sw_window_push (from, sw_categories_page_new (*arg ? arg : "apps"));
  else if (!strcmp (what, "search"))
    sw_search_open (from, NULL, arg);
  else if (!strcmp (what, "results"))
    sw_window_push (from, sw_search_results_new (arg, NULL));
  else if (!strcmp (what, "reviews"))
    sw_window_push (from, sw_reviews_page_new (arg, NULL));
  else if (!strcmp (what, "write"))
    sw_window_push (from, sw_review_editor_new (arg, NULL, 4, NULL));
  else if (!strcmp (what, "report"))
    sw_report_present (from, g_ascii_strtoll (arg, NULL, 10));
  else if (!strcmp (what, "signin"))
    sw_window_push (from, sw_sign_in_page_new ());
  else if (!strcmp (what, "register"))
    sw_window_push (from, sw_register_page_new ());
  else if (!strcmp (what, "verify"))
    sw_window_push (from, sw_verify_page_new (FALSE));
  else if (!strcmp (what, "forgot"))
    sw_window_push (from, sw_forgot_page_new (arg));
  else if (!strcmp (what, "profile"))
    sw_window_push (from, sw_profile_page_new ());
  else if (!strcmp (what, "settings"))
    sw_window_push (from, sw_settings_page_new ());
  else if (!strcmp (what, "delete"))
    sw_window_push (from, sw_delete_account_page_new ());
  else if (!strcmp (what, "myreviews"))
    sw_window_push (from, sw_my_reviews_page_new ());
  else if (!strcmp (what, "allapps"))
    sw_window_push (from, sw_all_apps_page_new (atoi (arg)));
  else if (!strcmp (what, "about"))
    sw_about_present (from);
  else if (!strcmp (what, "install"))
    sw_installer_install (sw_installer_get_default (), arg, NULL, win->window);
  else if (!strcmp (what, "popular"))
    {
      GtkWidget *tabs = g_object_get_data (G_OBJECT (win->homes[win->tab]), "sw-tabs");

      if (tabs)
        {
          sw_tabs_set_selected (SW_TABS (tabs), 1);
          g_signal_emit_by_name (tabs, "changed", 1);
        }
    }
  else if (!strcmp (what, "type"))
    {
      /* text typed into the focused field */
      GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (win->window));

      if (GTK_IS_EDITABLE (focus))
        gtk_editable_set_text (GTK_EDITABLE (focus), arg);
    }
  else if (!strcmp (what, "toast"))
    sw_window_toast (from, arg);
  else if (!strcmp (what, "text"))
    {
      GtkWidget *view = find_widget (GTK_WIDGET (win->window), is_text_view, NULL);

      if (view)
        gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), arg, -1);
    }
  else if (!strcmp (what, "press") || !strcmp (what, "fill") || !strcmp (what, "code"))
    {
      /* a sheet or a dialog over the pages first (they are the overlay's last children), then the page that
       * shows: the pages under it are still mapped */
      GtkWidget *top = vibeos_page_stack_get_top (VIBEOS_PAGE_STACK (win->stack)), *found = NULL;
      GtkWidget *page = top == win->tabs ? win->homes[win->tab] : top;
      GtkWidget *overlay = GTK_WIDGET (vibeos_window_get_overlay (win->window));
      g_auto (GStrv) pair = g_strsplit (arg, "=", 2);
      gboolean (*match) (GtkWidget *, const char *) = !strcmp (what, "press") ? is_button
                                                      : !strcmp (what, "fill") ? is_field : is_code;
      const char *key = !strcmp (what, "fill") ? pair[0] : arg;

      for (GtkWidget *child = overlay ? gtk_widget_get_last_child (overlay) : NULL; child && !found;
           child = gtk_widget_get_prev_sibling (child))
        if (child != gtk_overlay_get_child (GTK_OVERLAY (overlay)))
          found = find_widget (child, match, key);
      if (!found)
        found = find_widget (page, match, key);
      if (found && match == is_button)
        g_signal_emit_by_name (found, "clicked");
      else if (found && match == is_field && pair[1])
        sw_field_set_text (found, pair[1]);
      else if (found && match == is_code)
        gtk_editable_set_text (GTK_EDITABLE (gtk_widget_get_last_child (gtk_widget_get_first_child (found))), arg);
      else
        g_warning ("VIBEOS_SOFTWARE_PAGE: nothing for %s", step);
    }
  else if (!strcmp (what, "scroll"))
    {
      /* the showing page's scroll view, `arg` pixels down */
      GtkWidget *top = vibeos_page_stack_get_top (VIBEOS_PAGE_STACK (win->stack));
      GtkWidget *scroll = find_scroll (top == win->tabs ? win->homes[win->tab] : top);

      if (scroll)
        spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (scroll), atof (arg));
    }
  else if (strcmp (what, "wait"))
    g_warning ("VIBEOS_SOFTWARE_PAGE: unknown step %s", step);
}

static gboolean
run_step (gpointer data)
{
  Win *win = data;
  const char *step = win->steps ? win->steps[win->step] : NULL;
  guint delay = STEP_MS;

  win->step_timeout = 0;
  if (!step)
    return G_SOURCE_REMOVE;
  win->step++;
  if (g_str_has_prefix (step, "wait:"))
    delay = atoi (step + 5);
  else
    do_step (win, step);
  if (win->steps[win->step])
    win->step_timeout = g_timeout_add (delay, run_step, win);
  return G_SOURCE_REMOVE;
}

void
sw_window_run_steps (GtkWidget *window, const char *steps)
{
  Win *win = g_object_get_data (G_OBJECT (window), "sw-win");

  g_strfreev (win->steps);
  win->steps = g_strsplit (steps, ";", -1);
  win->step = 0;
  win->step_timeout = g_timeout_add (STEP_MS, run_step, win);
}

/* ---- the window ------------------------------------------------------------------------------------------------ */

static void
win_free (gpointer data)
{
  Win *win = data;

  g_clear_handle_id (&win->step_timeout, g_source_remove);
  g_strfreev (win->steps);
  g_free (win);
}

GtkWidget *
sw_window_new (GtkApplication *app)
{
  Win *win = g_new0 (Win, 1);
  GtkWidget *window = vibeos_window_new (app), *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  GtkShortcutAction *action;
  static const char *const icons[] = { "sw-featured-symbolic", "sw-apps-symbolic", "sw-games-symbolic",
                                       "sw-mine-symbolic" };
  static const char *const labels[] = { "Featured", "Apps", "Games", "Mine" };

  win->window = window;
  g_object_set_data_full (G_OBJECT (window), "sw-win", win, win_free);
  gtk_window_set_title (GTK_WINDOW (window), "Software");
  gtk_widget_add_css_class (window, "sw-window");
  /* pictures (VIBEOS_SHOT): the phone's window, whatever the test display's size (libvibeos's layout opens a window
   * at its monitor's size: it is asked first, then overruled) */
  if (g_getenv ("VIBEOS_SHOT"))
    {
      vibeos_layout_get_for_window (GTK_WINDOW (window));
      gtk_window_set_default_size (GTK_WINDOW (window), 360, 760);
    }

  /* the tabs: the four homes over the nav bar */
  win->tabs = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  win->tab_stack = gtk_stack_new ();
  /* a tab shows at once, as a nav bar's tabs do */
  gtk_stack_set_transition_type (GTK_STACK (win->tab_stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_vexpand (win->tab_stack, TRUE);
  win->homes[SW_TAB_FEATURED] = sw_home_new (SW_HOME_FEATURED);
  win->homes[SW_TAB_APPS] = sw_home_new (SW_HOME_APPS);
  win->homes[SW_TAB_GAMES] = sw_home_new (SW_HOME_GAMES);
  win->homes[SW_TAB_MINE] = sw_mine_new ();
  for (int i = 0; i < 4; i++)
    gtk_stack_add_child (GTK_STACK (win->tab_stack), win->homes[i]);
  gtk_box_append (GTK_BOX (win->tabs), win->tab_stack);
  win->nav = vibeos_nav_bar_new ();
  for (int i = 0; i < 4; i++)
    vibeos_nav_bar_append (VIBEOS_NAV_BAR (win->nav), icons[i], labels[i]);
  g_signal_connect (win->nav, "selected", G_CALLBACK (on_nav_selected), win);
  gtk_box_append (GTK_BOX (win->tabs), win->nav);

  win->stack = vibeos_page_stack_new ();
  gtk_widget_set_vexpand (win->stack, TRUE);
  vibeos_page_stack_set_root (VIBEOS_PAGE_STACK (win->stack), win->tabs);
  g_signal_connect (win->stack, "top-changed", G_CALLBACK (on_top_changed), win);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), win->stack);
  win->snackbar = vibeos_snackbar_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), win->snackbar);
  gtk_widget_set_vexpand (overlay, TRUE);
  gtk_box_append (GTK_BOX (root), overlay);

  win->marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (win->marker), GTK_ACCESSIBLE_PROPERTY_LABEL, "Software", -1);
  gtk_widget_set_can_target (win->marker, FALSE);
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

  g_signal_connect_object (sw_installer_get_default (), "finished", G_CALLBACK (on_job_finished), window, 0);
  g_signal_connect_object (sw_updates_get_default (), "changed", G_CALLBACK (on_updates_changed), window, 0);
  g_signal_connect_object (sw_account_get_default (), "notice", G_CALLBACK (on_account_notice), window, 0);
  select_tab (win, SW_TAB_FEATURED);
  on_updates_changed (sw_updates_get_default (), window);
  return window;
}
