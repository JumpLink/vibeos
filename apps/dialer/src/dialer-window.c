/* The Phone app's window: see dialer-window.h. The tabs, the FAB, the missed calls marked seen, and search and the
 * dialpad over the tabs. */
#include "dialer-window.h"
#include "call-details.h"
#include "call-history.h"
#include "calls-client.h"
#include "contacts-page.h"
#include "dialer-settings.h"
#include "dialpad-sheet.h"
#include "favorites-page.h"
#include "nav-bar.h"
#include "popup-menu.h"
#include "recents-page.h"
#include "search-bar.h"
#include "search-page.h"

#include <glib/gi18n.h>
#include <neo.h>

#define FAB_SIZE        56      /* the round dialpad button over the page's corner */
#define FAB_MARGIN      16
#define FAB_COLOR       0xb8ebf3        /* light: the button is not darkened with the rest of the app */
#define SEEN_AFTER_MS   3000    /* on Recents this long, the missed calls are seen */

enum { TAB_FAVORITES, TAB_RECENTS, TAB_CONTACTS, N_TABS };

typedef struct
{
  GtkWidget *window;
  GSettings *settings;
  Contacts *contacts;
  GtkWidget *toolbar;           /* a revealer: the toolbar slides up out of the way under the dialpad */
  GtkWidget *search_bar, *stack, *pages[N_TABS], *search, *fab, *nav;
  GtkWidget *sheet;
  GPtrArray *screens;           /* Call history, Call details, Settings over it all, the top one last */
  gboolean searching;           /* the search UI is up (regular or the dialpad's) */
  gboolean fab_shown;
  NeoSnackbar *snackbar;
  int tab;
  gint64 tab_since;             /* when the tab was shown, or 0 while the window is away */
} Shell;

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  g_signal_handlers_disconnect_by_data (calls_client_get (), shell);
  g_clear_pointer (&shell->screens, g_ptr_array_unref);
  g_clear_object (&shell->contacts);
  g_clear_object (&shell->settings);
  g_free (shell);
}

static Shell *
shell_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (window), "dialer-shell");
}

/* ---- the missed calls: the Recents badge, and seen once Recents has been looked at ------------------------- */

static void
update_badge (Shell *shell)
{
  dialer_nav_bar_set_count (DIALER_NAV_BAR (shell->nav), TAB_RECENTS,
                            calls_client_get_missed_count (calls_client_get ()));
}

static void
mark_seen (Shell *shell)
{
  if (calls_client_get_missed_count (calls_client_get ()))
    calls_client_mark_missed_read (calls_client_get ());
}

/* when the tab was shown, again each time the window comes back */
static void
start_tab_timer (Shell *shell)
{
  shell->tab_since = g_get_monotonic_time ();
}

/* the window going behind another (home, the in-call screen): after more than 3 s on Recents the missed calls are
 * seen; coming back restarts the clock */
static void
on_active (GtkWindow *window, GParamSpec *pspec, Shell *shell)
{
  if (gtk_window_is_active (window))
    {
      start_tab_timer (shell);
      return;
    }
  if (shell->tab == TAB_RECENTS && shell->tab_since
      && g_get_monotonic_time () - shell->tab_since > SEEN_AFTER_MS * 1000)
    mark_seen (shell);
  shell->tab_since = 0;
}

/* ---- the tabs ------------------------------------------------------------------------------------------------ */

static void
show_tab (Shell *shell, int tab)
{
  if (shell->tab == TAB_RECENTS && tab != TAB_RECENTS)
    mark_seen (shell);
  shell->tab = tab;
  /* a plain show and hide: no transition, and each list keeps where it was scrolled to */
  gtk_stack_set_visible_child (GTK_STACK (shell->stack), shell->pages[tab]);
  dialer_nav_bar_set_selected (DIALER_NAV_BAR (shell->nav), tab);
  g_settings_set_enum (shell->settings, "selected-tab", tab);
  if (gtk_window_is_active (GTK_WINDOW (shell->window)))
    start_tab_timer (shell);
}

static void
on_tab_selected (DialerNavBar *nav, int index, Shell *shell)
{
  show_tab (shell, index);
}

/* ---- search and the dialpad over the tabs ------------------------------------------------------------------- */

static void
set_fab_shown (Shell *shell, gboolean shown)
{
  /* hidden and shown at once, with no scale motion */
  shell->fab_shown = shown;
  neo_pressable_set_scale (NEO_PRESSABLE (shell->fab), shown ? 1 : 0);
}

/* the search UI over the tabs; the bottom navigation is gone while it is up */
static void
show_search (Shell *shell, gboolean shown)
{
  shell->searching = shown;
  gtk_widget_set_visible (shell->nav, !shown);
  gtk_stack_set_visible_child (GTK_STACK (shell->stack), shown ? shell->search : shell->pages[shell->tab]);
}

/* a tap on the collapsed bar: regular search, the keyboard up */
static void
on_search_clicked (Shell *shell)
{
  set_fab_shown (shell, FALSE);
  dialer_search_bar_expand (DIALER_SEARCH_BAR (shell->search_bar), TRUE);
  search_page_set_query (SEARCH_PAGE (shell->search), dialer_search_bar_get_query (DIALER_SEARCH_BAR (shell->search_bar)),
                         FALSE);
  show_search (shell, TRUE);
}

/* closing the search: the dialpad goes too, and is emptied so a number is not kept for next time */
static void
close_search (Shell *shell)
{
  if (!shell->searching)
    return;
  shell->searching = FALSE;
  if (dialpad_sheet_get_shown (DIALPAD_SHEET (shell->sheet)))
    dialpad_sheet_hide (DIALPAD_SHEET (shell->sheet));
  else
    set_fab_shown (shell, TRUE);
  dialer_search_bar_collapse (DIALER_SEARCH_BAR (shell->search_bar));
  gtk_revealer_set_reveal_child (GTK_REVEALER (shell->toolbar), TRUE);
  dialpad_sheet_clear (DIALPAD_SHEET (shell->sheet));
  search_page_set_query (SEARCH_PAGE (shell->search), "", FALSE);
  show_search (shell, FALSE);
}

static void
on_back_clicked (Shell *shell)
{
  close_search (shell);
}

static void
on_query_changed (Shell *shell)
{
  search_page_set_query (SEARCH_PAGE (shell->search), dialer_search_bar_get_query (DIALER_SEARCH_BAR (shell->search_bar)),
                         FALSE);
}

static void
on_voice_clicked (Shell *shell)
{
  /* voice search: no recogniser on this phone */
  neo_snackbar_show (shell->snackbar, _("Voice search not available"), NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

/* the dialpad's digits are the query of a dialpad search */
static void
on_digits_changed (Shell *shell)
{
  if (dialpad_sheet_get_shown (DIALPAD_SHEET (shell->sheet)))
    search_page_set_query (SEARCH_PAGE (shell->search), dialpad_sheet_get_digits (DIALPAD_SHEET (shell->sheet)), TRUE);
}

static void
on_sheet_shown (Shell *shell)
{
  const char *digits = dialpad_sheet_get_digits (DIALPAD_SHEET (shell->sheet));

  if (dialpad_sheet_get_shown (DIALPAD_SHEET (shell->sheet)))
    {
      /* the dialpad shown: the FAB goes, the toolbar slides up out of the way expanded but unfocused, and the search
       * comes up under the dialpad in dialpad mode */
      set_fab_shown (shell, FALSE);
      gtk_revealer_set_reveal_child (GTK_REVEALER (shell->toolbar), FALSE);
      dialer_search_bar_expand (DIALER_SEARCH_BAR (shell->search_bar), FALSE);
      search_page_set_query (SEARCH_PAGE (shell->search), digits, TRUE);
      show_search (shell, TRUE);
      return;
    }
  if (!shell->searching)
    return;                     /* closing the search put it away */
  /* the dialpad was put away (Back, a tap above it, the list touched). With nothing typed that closes the search
   * as well; with digits it is hidden: the toolbar slides back with the digits as its query and the results stay */
  if (!*digits)
    {
      close_search (shell);
      return;
    }
  gtk_revealer_set_reveal_child (GTK_REVEALER (shell->toolbar), TRUE);
  dialer_search_bar_set_query (DIALER_SEARCH_BAR (shell->search_bar), digits);
  shell->fab_shown = TRUE;
}

/* the sheet has slid away: the FAB is back, only now, at the end of the slide */
static void
on_sheet_visible (GtkWidget *sheet, GParamSpec *pspec, Shell *shell)
{
  if (!gtk_widget_get_visible (sheet) && shell->fab_shown)
    set_fab_shown (shell, TRUE);
}

static void
on_fab_clicked (Shell *shell)
{
  dialpad_sheet_show (DIALPAD_SHEET (shell->sheet), NULL);
}

static void
on_open_dialpad (Shell *shell)
{
  dialpad_sheet_show (DIALPAD_SHEET (shell->sheet), NULL);
}

static void
on_edit_number (Shell *shell, const char *number)
{
  dialpad_sheet_show (DIALPAD_SHEET (shell->sheet), number);
}

/* a call placed from the results closes search */
static void
on_search_call_placed (Shell *shell)
{
  close_search (shell);
}

/* a touch on the results: an empty query closes the search; otherwise the dialpad goes (keeping the results), or in
 * regular search the keyboard */
static void
on_search_touched (Shell *shell)
{
  gboolean dialpad = dialpad_sheet_get_shown (DIALPAD_SHEET (shell->sheet));
  const char *query = dialpad ? dialpad_sheet_get_digits (DIALPAD_SHEET (shell->sheet))
                              : dialer_search_bar_get_query (DIALER_SEARCH_BAR (shell->search_bar));

  if (!*query)
    close_search (shell);
  else if (dialpad)
    dialpad_sheet_hide (DIALPAD_SHEET (shell->sheet));
  else
    dialer_search_bar_hide_keyboard (DIALER_SEARCH_BAR (shell->search_bar));
}

/* ---- the ⋮ menu and the screens it opens -------------------------------------------------------- */

static void
remove_screen (Shell *shell, GtkWidget *screen)
{
  g_signal_handlers_disconnect_by_data (screen, shell);
  gtk_overlay_remove_overlay (neo_window_get_overlay (shell->window), screen);
  g_ptr_array_remove (shell->screens, screen);
  if (shell->screens->len)
    gtk_widget_grab_focus (g_ptr_array_index (shell->screens, shell->screens->len - 1));
}

/* the top screen goes (Back, Up, ✕): the one under it shows again */
static void
close_screen (Shell *shell)
{
  if (shell->screens->len)
    remove_screen (shell, g_ptr_array_index (shell->screens, shell->screens->len - 1));
}

static void
close_screens (Shell *shell)
{
  while (shell->screens->len)
    close_screen (shell);
}

/* a screen that closes itself: it, and anything opened over it, go */
static void
on_screen_closed (GtkWidget *screen, Shell *shell)
{
  while (shell->screens->len && g_ptr_array_find (shell->screens, screen, NULL))
    close_screen (shell);
}

static void
on_history_edit_number (Shell *shell, const char *number)
{
  /* the dialpad comes up in the main screen, which clears Call history and Call details off the top */
  close_screens (shell);
  dialpad_sheet_show (DIALPAD_SHEET (shell->sheet), number);
}

/* a screen of its own over the main one, its dialpad and the screens under it, under the snackbar, which a call
 * that fails from it uses */
static void
push_screen (Shell *shell, GtkWidget *screen)
{
  g_ptr_array_add (shell->screens, screen);
  g_signal_connect (screen, "closed", G_CALLBACK (on_screen_closed), shell);
  gtk_widget_insert_before (screen, GTK_WIDGET (neo_window_get_overlay (shell->window)), GTK_WIDGET (shell->snackbar));
  gtk_widget_grab_focus (screen);
}

/* an entry's "Call details", from Recents or from Call history (over it, so Back goes back to it) */
static void
on_show_details (Shell *shell, const char *number, int presentation, GArray *ids)
{
  GtkWidget *details = call_details_new (shell->contacts, shell->snackbar, number, presentation,
                                         (const gint64 *) ids->data, ids->len);

  g_signal_connect_swapped (details, "edit-number", G_CALLBACK (on_history_edit_number), shell);
  push_screen (shell, details);
}

/* a number's statistics, from Call history's Statistics, over it */
static void
on_show_screen (Shell *shell, GtkWidget *screen)
{
  g_signal_connect_swapped (screen, "edit-number", G_CALLBACK (on_history_edit_number), shell);
  push_screen (shell, screen);
}

static void
on_call_history (gpointer data)
{
  Shell *shell = data;
  GtkWidget *history = call_history_new (shell->contacts, shell->snackbar);

  g_signal_connect_swapped (history, "edit-number", G_CALLBACK (on_history_edit_number), shell);
  g_signal_connect_swapped (history, "show-details", G_CALLBACK (on_show_details), shell);
  g_signal_connect_swapped (history, "show-screen", G_CALLBACK (on_show_screen), shell);
  close_screens (shell);
  push_screen (shell, history);
}

static void
on_settings (gpointer data)
{
  Shell *shell = data;

  close_screens (shell);
  push_screen (shell, dialer_settings_new (shell->settings));
}

/* Helplines is a screen still to be written (its list is Lineage's, by country); Clear frequents never shows,
 * Favorites keeping no frequents */
static void
on_menu_clicked (Shell *shell)
{
  DialerPopupMenu *menu = dialer_popup_menu_new ();

  dialer_popup_menu_append (menu, _("Call history"), on_call_history, shell);
  dialer_popup_menu_append (menu, _("Settings"), on_settings, shell);
  dialer_popup_menu_present (menu, dialer_search_bar_get_menu_button (DIALER_SEARCH_BAR (shell->search_bar)),
                             POPUP_MENU_END);
}

/* Back: Call history or Settings goes first, then the dialpad with digits; otherwise the search closes,
 * dialpad and all */
static gboolean
on_close_request (GtkWindow *window, Shell *shell)
{
  if (shell->screens->len)
    {
      close_screen (shell);
      return TRUE;
    }
  if (dialpad_sheet_get_shown (DIALPAD_SHEET (shell->sheet)) && *dialpad_sheet_get_digits (DIALPAD_SHEET (shell->sheet)))
    {
      dialpad_sheet_hide (DIALPAD_SHEET (shell->sheet));
      return TRUE;
    }
  if (shell->searching)
    {
      close_search (shell);
      return TRUE;
    }
  return FALSE;
}

static GtkWidget *
make_fab (Shell *shell)
{
  GtkWidget *icon = gtk_image_new_from_icon_name ("dialer-dialpad-symbolic");
  GdkRGBA fill = { ((FAB_COLOR >> 16) & 0xff) / 255.0, ((FAB_COLOR >> 8) & 0xff) / 255.0, (FAB_COLOR & 0xff) / 255.0,
                   1 };
  GtkWidget *fab;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "dial-icon");
  fab = neo_pressable_new (icon);
  gtk_widget_set_size_request (fab, FAB_SIZE, FAB_SIZE);
  neo_pressable_set_fill (NEO_PRESSABLE (fab), &fill, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (fab), NEO_RIPPLE_CIRCLE, FAB_SIZE / 2.0);
  gtk_widget_set_halign (fab, GTK_ALIGN_END);
  gtk_widget_set_valign (fab, GTK_ALIGN_END);
  gtk_widget_set_margin_end (fab, FAB_MARGIN);
  gtk_widget_set_margin_bottom (fab, FAB_MARGIN);
  gtk_accessible_update_property (GTK_ACCESSIBLE (fab), GTK_ACCESSIBLE_PROPERTY_LABEL, _("key pad"), -1);
  g_signal_connect_swapped (fab, "clicked", G_CALLBACK (on_fab_clicked), shell);
  return fab;
}

GtkWidget *
dialer_window_new (GtkApplication *app, GSettings *settings)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = neo_window_new (app);
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content = gtk_overlay_new ();
  GtkOverlay *overlay;

  shell->window = window;
  shell->screens = g_ptr_array_new ();
  shell->settings = g_object_ref (settings);
  shell->contacts = contacts_new ();
  g_object_set_data_full (G_OBJECT (window), "dialer-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Phone"));
  gtk_widget_add_css_class (window, "dialer");
  gtk_widget_add_css_class (page, "dialer-page");
  neo_window_set_content (GTK_WINDOW (window), page);
  overlay = neo_window_get_overlay (window);
  shell->snackbar = NEO_SNACKBAR (neo_snackbar_new ());

  /* top to bottom: the search bar, the tab's page with the FAB over its corner, the bottom navigation */
  shell->search_bar = dialer_search_bar_new ();
  /* the search bar slides up out of the way, the content following, over 300 ms */
  shell->toolbar = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (shell->toolbar), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_transition_duration (GTK_REVEALER (shell->toolbar), 300);
  gtk_revealer_set_reveal_child (GTK_REVEALER (shell->toolbar), TRUE);
  gtk_revealer_set_child (GTK_REVEALER (shell->toolbar), shell->search_bar);
  gtk_box_append (GTK_BOX (page), shell->toolbar);
  g_signal_connect_swapped (shell->search_bar, "search-clicked", G_CALLBACK (on_search_clicked), shell);
  g_signal_connect_swapped (shell->search_bar, "back-clicked", G_CALLBACK (on_back_clicked), shell);
  g_signal_connect_swapped (shell->search_bar, "query-changed", G_CALLBACK (on_query_changed), shell);
  g_signal_connect_swapped (shell->search_bar, "voice-clicked", G_CALLBACK (on_voice_clicked), shell);
  g_signal_connect_swapped (shell->search_bar, "menu-clicked", G_CALLBACK (on_menu_clicked), shell);

  shell->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (shell->stack, TRUE);
  shell->pages[TAB_FAVORITES] = favorites_page_new (shell->contacts, settings, shell->snackbar);
  shell->pages[TAB_RECENTS] = recents_page_new (shell->contacts, shell->snackbar);
  shell->pages[TAB_CONTACTS] = contacts_page_new (shell->contacts, shell->snackbar);
  shell->search = search_page_new (shell->contacts, settings, shell->snackbar);
  for (int i = 0; i < N_TABS; i++)
    gtk_stack_add_child (GTK_STACK (shell->stack), shell->pages[i]);
  gtk_stack_add_child (GTK_STACK (shell->stack), shell->search);
  g_signal_connect_swapped (shell->pages[TAB_RECENTS], "open-dialpad", G_CALLBACK (on_open_dialpad), shell);
  g_signal_connect_swapped (shell->pages[TAB_RECENTS], "edit-number", G_CALLBACK (on_edit_number), shell);
  g_signal_connect_swapped (shell->pages[TAB_RECENTS], "show-details", G_CALLBACK (on_show_details), shell);
  g_signal_connect_swapped (shell->search, "call-placed", G_CALLBACK (on_search_call_placed), shell);
  g_signal_connect_swapped (shell->search, "touched", G_CALLBACK (on_search_touched), shell);
  gtk_overlay_set_child (GTK_OVERLAY (content), shell->stack);
  shell->fab = make_fab (shell);
  shell->fab_shown = TRUE;
  gtk_overlay_add_overlay (GTK_OVERLAY (content), shell->fab);
  gtk_box_append (GTK_BOX (page), content);

  shell->nav = dialer_nav_bar_new ();
  dialer_nav_bar_append (DIALER_NAV_BAR (shell->nav), "dialer-star-outline-symbolic", "dialer-star-symbolic",
                         C_("tab_title_speed_dial", "Favorites"));
  dialer_nav_bar_append (DIALER_NAV_BAR (shell->nav), "dialer-access-time-symbolic", "dialer-clock-filled-symbolic",
                         C_("tab_title_call_history", "Recents"));
  dialer_nav_bar_append (DIALER_NAV_BAR (shell->nav), "dialer-people-outline-symbolic", "dialer-people-symbolic",
                         C_("tab_all_contacts", "Contacts"));
  gtk_box_append (GTK_BOX (page), shell->nav);
  g_signal_connect (shell->nav, "selected", G_CALLBACK (on_tab_selected), shell);

  /* the dialpad over everything but the snackbar */
  shell->sheet = dialpad_sheet_new (shell->snackbar);
  gtk_overlay_add_overlay (overlay, shell->sheet);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (shell->snackbar));
  g_signal_connect_swapped (shell->sheet, "shown-changed", G_CALLBACK (on_sheet_shown), shell);
  g_signal_connect_swapped (shell->sheet, "digits-changed", G_CALLBACK (on_digits_changed), shell);
  g_signal_connect (shell->sheet, "notify::visible", G_CALLBACK (on_sheet_visible), shell);

  g_signal_connect_swapped (calls_client_get (), "missed-changed", G_CALLBACK (update_badge), shell);
  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_active), shell);
  g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), shell);

  /* the tab of the last time (the default is Favorites) */
  shell->tab = g_settings_get_enum (settings, "selected-tab");
  if (shell->tab < 0 || shell->tab >= N_TABS)
    shell->tab = TAB_FAVORITES;
  gtk_stack_set_visible_child (GTK_STACK (shell->stack), shell->pages[shell->tab]);
  dialer_nav_bar_set_selected (DIALER_NAV_BAR (shell->nav), shell->tab);
  update_badge (shell);
  return window;
}

void
dialer_window_show_recents (GtkWidget *window)
{
  Shell *shell = shell_of (window);

  close_screens (shell);
  close_search (shell);
  recents_page_reset (RECENTS_PAGE (shell->pages[TAB_RECENTS]));
  show_tab (shell, TAB_RECENTS);
}

void
dialer_window_show_dialpad (GtkWidget *window, const char *number)
{
  Shell *shell = shell_of (window);

  close_screens (shell);
  dialpad_sheet_show (DIALPAD_SHEET (shell->sheet), number);
}

void
dialer_window_show_message (GtkWidget *window, const char *message)
{
  neo_snackbar_show (shell_of (window)->snackbar, message, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
}
