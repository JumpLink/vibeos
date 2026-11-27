/* Navi: a car navigator for VibeOS. See navi-app.h for its parts.
 *
 *   vibeos-navi [--simulate [--sim-speed F] [--sim-detour]] [--from LAT,LNG] [--to LAT,LNG[,NAME]] [--go | --free]
 *               [--search TEXT] [--night | --day] [--seconds N] [--close-at MODE[:S]] [--stats]
 *   vibeos-navi --leave-reminder
 *
 * The options are for testing on a phone without a GPS: --from puts the car somewhere (as a last known position),
 * --to asks for routes there, --go drives the best one at once, --simulate drives it (F times as fast; --sim-detour
 * takes the second route, so the guidance has to find a new one), --free drives the best one's line simulated
 * without a route (Navi browsing while the car moves), --seconds quits after N seconds, --close-at closes the
 * window as Recents does, S seconds after Navi shows MODE (routes, drive, arrived), and --stats prints the frame
 * pacing every second and over the drive. NAVI_SERVER and NAVI_PHOTON_URL point at a test server.
 *
 * --leave-reminder is the planned drive's timer's (navi-plan.h): it posts "Time to leave" and exits, no window. */
#include "navi-app.h"
#include "navi-alerts.h"
#include "navi-plan.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <locale.h>
#include <string.h>

typedef struct {
  gboolean simulate, sim_detour, go, free, have_from, have_to;
  double sim_speed;
  double from_lat, from_lng, to_lat, to_lng;
  char *to_name, *search, *close_at;
  int seconds, close_after;
} Options;

static Options opts = { .sim_speed = 1 };
static Navi *the_navi; /* the window's, once there is one */

static gboolean
parse_pair (const char *text, double *a, double *b, char **rest)
{
  g_auto (GStrv) parts = g_strsplit (text, ",", 3);
  if (!parts[0] || !parts[1])
    return FALSE;
  char *end;
  *a = g_ascii_strtod (parts[0], &end);
  if (*end)
    return FALSE;
  *b = g_ascii_strtod (parts[1], &end);
  if (*end)
    return FALSE;
  if (rest)
    *rest = parts[2] ? g_strdup (parts[2]) : NULL;
  return TRUE;
}

/* the style's tile sources, for the search's fallback */
static NaviSearch *
make_search (Navi *n)
{
  g_autofree char *file = g_build_filename (n->data_dir, "style-day.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  GArray *sources = g_array_new (FALSE, TRUE, sizeof (NaviTileSource));
  if (json_parser_load_from_file (parser, file, NULL)) {
    JsonObject *root = json_node_get_object (json_parser_get_root (parser));
    JsonObject *src = json_object_get_object_member (root, "sources");
    g_autoptr (GList) names = src ? json_object_get_members (src) : NULL;
    for (GList *l = names; l; l = l->next) {
      JsonObject *s = json_object_get_object_member (src, l->data);
      JsonArray *tiles = json_object_has_member (s, "tiles") ? json_object_get_array_member (s, "tiles") : NULL;
      JsonArray *b = json_object_has_member (s, "bounds") ? json_object_get_array_member (s, "bounds") : NULL;
      if (!tiles || !json_array_get_length (tiles) || !b || json_array_get_length (b) != 4)
        continue;
      NaviTileSource t = { (char *) json_array_get_string_element (tiles, 0), json_array_get_double_element (b, 0),
                           json_array_get_double_element (b, 1), json_array_get_double_element (b, 2),
                           json_array_get_double_element (b, 3) };
      g_array_append_val (sources, t);
    }
  }
  NaviSearch *tiles = navi_search_tiles_new ((NaviTileSource *) sources->data, sources->len);
  g_array_free (sources, TRUE);
  NaviSearch *search = navi_search_photon_new (NULL, tiles);
  g_object_unref (tiles);
  return search;
}

/* ---- back: Escape, the Back key, Alt+Left and VibeOS's back swipe (navi-back.h) ---- */

/* the dialog in front in the window (VibeosDialogs show in its root overlay, the last one on top), or NULL */
static VibeosDialog *
top_dialog (Navi *n)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (n->win);
  for (GtkWidget *w = overlay ? gtk_widget_get_last_child (GTK_WIDGET (overlay)) : NULL; w;
       w = gtk_widget_get_prev_sibling (w))
    if (VIBEOS_IS_DIALOG (w))
      return VIBEOS_DIALOG (w);
  return NULL;
}

static void
go_back (Navi *n)
{
  /* (Settings, About and the menu are sheets that take their own keys; the search over the map takes only Escape) */
  VibeosDialog *dialog = top_dialog (n);
  NaviBackState state = {
    .mode = n->mode,
    .dialog = dialog != NULL,
    .search = navi_panels_search_shown (n->panels),
    .off_car = n->cam != NAVI_CAM_FOLLOW && n->car_known,
  };
  switch (navi_back_decide (&state)) {
  case NAVI_BACK_CANCEL_DIALOG:
    /* as Escape: a second Back over "End navigation?" keeps the drive */
    vibeos_dialog_respond (dialog, VIBEOS_RESPONSE_CANCEL);
    break;
  case NAVI_BACK_CLOSE_SEARCH:
    navi_panels_back (n->panels);
    break;
  case NAVI_BACK_CLOSE_PLACE:
    navi_back_to_browse (n);
    break;
  case NAVI_BACK_TO_PLACE:
    navi_panels_routes_back (n->panels);
    break;
  case NAVI_BACK_ASK_END:
    navi_panels_ask_end_drive (n->panels);
    break;
  case NAVI_BACK_END_DRIVE:
    navi_stop_drive (n);
    break;
  case NAVI_BACK_RECENTER:
    navi_recenter (n);
    break;
  case NAVI_BACK_NOTHING:
    /* browsing never quits on a key: the menu's Exit does, and the shell's swipe takes Navi home */
    break;
  }
}

static gboolean
on_back (GtkWidget *widget, GVariant *args, gpointer data)
{
  go_back (data);
  return TRUE;
}

/* The shell's back swipe asks the app what Back means (extension/a11y.js): it presses a "Back" button in the
 * window's top 40 px, sends Escape over a dialog, and sends an app with neither home (minimized). Navi has somewhere to
 * go back to while a place, routes or a drive show: an unseen one-pixel "Back" at the top says so
 * (navi_back_button_shown; navi_window_set_mode shows it), and runs go_back. While driving that asks first, so a swipe
 * never drops the drive nor takes Navi off the screen. */
static void
back_clicked (GtkWidget *button, Navi *n)
{
  go_back (n);
}

static void
add_back_button (Navi *n)
{
  GtkWidget *label = gtk_label_new (NULL);
  n->back = vibeos_pressable_new (label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (n->back), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"), -1);
  gtk_widget_set_size_request (n->back, 1, 1);
  gtk_widget_set_halign (n->back, GTK_ALIGN_START);
  gtk_widget_set_valign (n->back, GTK_ALIGN_START);
  gtk_widget_set_opacity (n->back, 0);
  gtk_widget_set_can_target (n->back, FALSE);
  gtk_widget_set_visible (n->back, FALSE);
  g_signal_connect (n->back, "clicked", G_CALLBACK (back_clicked), n);
  gtk_overlay_add_overlay (GTK_OVERLAY (n->map_overlay), n->back);
}

static void
add_back (Navi *n)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (keys), GTK_SHORTCUT_SCOPE_GLOBAL);
  gtk_shortcut_controller_add_shortcut (
    GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_alternative_trigger_new (
                                                     gtk_shortcut_trigger_parse_string ("Escape"),
                                                     gtk_shortcut_trigger_parse_string ("Back"))),
                      gtk_callback_action_new (on_back, n, NULL)));
  gtk_widget_add_controller (n->win, keys);
}

/* ---- the tablet's rail ---- */

static void
rail_selected (VibeosNavRail *rail, int index, Navi *n)
{
  if (index == 1)
    navi_panels_open_search (n->panels, NULL);
  /* the map is the one place; Search opens as a card over it */
  vibeos_nav_rail_set_selected (rail, 0);
}

static void
rail_menu (GtkWidget *button, Navi *n)
{
  navi_panels_open_menu (n->panels, button);
}

/* ---- the test options ---- */

static gboolean
start_test_drive (gpointer data)
{
  Navi *n = data;
  if (opts.search)
    navi_panels_open_search (n->panels, opts.search);
  if (opts.have_to) {
    g_autoptr (NaviPlace) to = navi_place_new (NAVI_PLACE_ADDRESS, opts.to_name ? opts.to_name : _("Destination"),
                                               NULL, opts.to_lat, opts.to_lng);
    if (opts.go)
      g_object_set_data (G_OBJECT (n->win), "navi-go", GINT_TO_POINTER (1));
    else if (opts.free)
      g_object_set_data (G_OBJECT (n->win), "navi-free", GINT_TO_POINTER (1));
    navi_preview_routes (n, to);
  }
  return G_SOURCE_REMOVE;
}

static gboolean
quit_after (gpointer data)
{
  Navi *n = data;
  gtk_window_destroy (GTK_WINDOW (n->win));
  return G_SOURCE_REMOVE;
}

/* --close-at MODE[:S]: the window closed the way Recents closes it (a close request, which GTK answers with
 * gtk_window_destroy), S seconds after Navi first shows MODE. tests/test-close.sh closes a drive, the arrived screen
 * and the routes so, and checks that Navi ends cleanly with its settings saved. */
static const char *const mode_names[] = { "browse", "place", "routes", "drive", "arrived" };

static gboolean
close_at_now (gpointer data)
{
  Navi *n = data;
  g_message ("navi: --close-at: the window closes, in %s", mode_names[n->mode]);
  gtk_window_close (GTK_WINDOW (n->win));
  return G_SOURCE_REMOVE;
}

static gboolean
close_at_watch (gpointer data)
{
  Navi *n = data;
  if (strcmp (mode_names[n->mode], opts.close_at))
    return G_SOURCE_CONTINUE;
  g_message ("navi: --close-at: %s, the window closes in %d s", opts.close_at, opts.close_after);
  g_timeout_add_seconds (opts.close_after, close_at_now, n);
  return G_SOURCE_REMOVE;
}

/* The window closes (Recents, the menu's Exit, --seconds): a drive ends, everything stops, the settings are saved,
 * and the application quits. This runs as the window leaves the application, which gtk_window_destroy does once the
 * window is hidden and before it takes it apart. Its "destroy" comes too late: the window's content is gone by then,
 * and a drive (or the arrived screen) ended from there went back to browsing on freed widgets and crashed, before
 * the settings were saved. The quit is at once, as Exit's: an application merely left without a window runs what is
 * still queued before it ends (the panels' idle update of the floating buttons, queued by that going back to
 * browsing), and that would be on the freed widgets too. */
static void
close_navi (GtkApplication *app, GtkWindow *win, Navi *n)
{
  if (GTK_WIDGET (win) != n->win || n->closed)
    return;
  n->closed = TRUE;
  if (n->mode == NAVI_MODE_DRIVE || n->mode == NAVI_MODE_ARRIVED)
    navi_stop_drive (n);
  g_clear_handle_id (&n->recenter_source, g_source_remove);
  if (n->keep_awake) {
    gtk_application_uninhibit (n->app, n->keep_awake);
    n->keep_awake = 0;
  }
  navi_panels_quiet (n->panels);
  g_clear_handle_id (&n->fix_stale, g_source_remove);
  g_cancellable_cancel (n->place_cancel);
  g_clear_object (&n->place_cancel);
  g_clear_pointer (&n->alerts, navi_alerts_free);
  navi_settings_flush (n->settings);
  navi_window_stop_awake (n);
  navi_position_stop (n->position);
  navi_window_stop_compass (n);
  if (the_navi == n)
    the_navi = NULL;
  g_application_quit (G_APPLICATION (app));
}

/* ---- the planned drive's reminder, tapped ---- */

/* "app.planned-drive" (the plan's time): the routes to the planned place, from where the car is now. Navi may have
 * been started for it (D-Bus activation): then the window comes first. While driving, the drive goes on. */
static void
planned_drive (GSimpleAction *action, GVariant *param, gpointer app)
{
  g_application_activate (G_APPLICATION (app));
  Navi *n = the_navi;
  g_autoptr (NaviPlan) plan = navi_plan_load ();
  if (!n || !plan || (param && g_variant_get_int64 (param) != plan->time))
    return;
  if (n->mode == NAVI_MODE_DRIVE)
    return;
  /* this was its time: the plan is done with */
  navi_plan_cancel ();
  navi_preview_routes (n, plan->place);
}

/* ---- the window ---- */

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWindow *have = gtk_application_get_active_window (app);
  if (have) {
    gtk_window_present (have);
    return;
  }
  Navi *n = the_navi = g_new0 (Navi, 1);
  n->app = app;
  n->settings = navi_settings_load ();
  /* the voice, the route's narrative and Navi's own words in the language chosen in Settings */
  navi_route_set_language (n->settings->voice_language);
  n->data_dir = g_strdup (g_getenv ("NAVI_DATA_DIR") ? g_getenv ("NAVI_DATA_DIR") : PKGDATADIR);
  n->stats = g_getenv ("NAVI_STATS") != NULL;
  n->simulate = opts.simulate;
  n->sim_speed = opts.sim_speed;
  n->sim_detour = opts.sim_detour;
  n->search = make_search (n);
  navi_format_set_24h (vibeos_clock_uses_24h ());

  n->win = vibeos_window_new (app);
  gtk_window_set_title (GTK_WINDOW (n->win), "Navi");
  gtk_widget_add_css_class (n->win, "navi");
  n->tablet = vibeos_layout_is_tablet (vibeos_layout_get_for_window (GTK_WINDOW (n->win)));
  add_back (n);

  navi_window_build_map (n);
  n->panels = navi_panels_new (n);
  add_back_button (n);
  if (n->tablet) {
    /* the rail and the map */
    GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    n->rail = vibeos_nav_rail_new ();
    GtkWidget *menu = vibeos_icon_button_new ("open-menu-symbolic");
    gtk_accessible_update_property (GTK_ACCESSIBLE (menu), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Menu"), -1);
    vibeos_nav_rail_set_header (VIBEOS_NAV_RAIL (n->rail), menu);
    vibeos_nav_rail_append (VIBEOS_NAV_RAIL (n->rail), "mark-location-symbolic", _("Drive"));
    vibeos_nav_rail_append (VIBEOS_NAV_RAIL (n->rail), "edit-find-symbolic", _("Search"));
    vibeos_nav_rail_set_selected (VIBEOS_NAV_RAIL (n->rail), 0);
    g_signal_connect (n->rail, "selected", G_CALLBACK (rail_selected), n);
    g_signal_connect (menu, "clicked", G_CALLBACK (rail_menu), n);
    gtk_box_append (GTK_BOX (frame), n->rail);
    /* the map takes all the rest, edge to edge: the panels float over it as cards (Navi's exception to the
     * tablet apps' two panes, the owner's choice) */
    gtk_widget_set_hexpand (n->map_overlay, TRUE);
    gtk_box_append (GTK_BOX (frame), n->map_overlay);
    vibeos_window_set_content (GTK_WINDOW (n->win), frame);
  } else {
    vibeos_window_set_content (GTK_WINDOW (n->win), n->map_overlay);
  }
  g_signal_connect (app, "window-removed", G_CALLBACK (close_navi), n);
  /* the real position, the timers, the frames: while the window is seen (navi-awake.h); --from follows none */
  navi_window_start_awake (n, !opts.have_from);

  navi_window_set_mode (n, NAVI_MODE_BROWSE);
  navi_update_night (n);
  gtk_window_present (GTK_WINDOW (n->win));

  /* --from is a test's: it is not kept as the place the map opens at next time */
  if (opts.have_from)
    navi_position_set_last_known (n->position, opts.from_lat, opts.from_lng);
  else if (n->settings->have_last)
    navi_position_set_last_known (n->position, n->settings->last_lat, n->settings->last_lng);
  navi_window_start_compass (n);
  if (opts.have_to || opts.search)
    g_timeout_add (1500, start_test_drive, n);
  /* a planned drive's reminder again, in case a reboot took it */
  navi_plan_rearm ();
  if (opts.seconds > 0)
    g_timeout_add_seconds (opts.seconds, quit_after, n);
  if (opts.close_at)
    g_timeout_add (100, close_at_watch, n);
}

int
main (int argc, char **argv)
{
  /* the user's language, but numbers in C's notation: printf's %f goes into the requests' JSON and URLs, MapLibre's
   * parsing and the like, where a German "52,5" breaks them; the few decimals shown are written with the user's
   * separator (navi-geo.c). GTK would set it all again from the environment. */
  setlocale (LC_ALL, "");
  setlocale (LC_NUMERIC, "C");
  gtk_disable_setlocale ();
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  if (argc == 2 && !strcmp (argv[1], "--leave-reminder"))
    return navi_plan_remind ();

  GPtrArray *rest = g_ptr_array_new ();
  g_ptr_array_add (rest, argv[0]);
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp (a, "--simulate"))
      opts.simulate = TRUE;
    else if (!strcmp (a, "--sim-detour"))
      opts.simulate = opts.sim_detour = TRUE;
    else if (!strcmp (a, "--sim-speed") && v)
      opts.sim_speed = g_ascii_strtod (v, NULL), i++;
    else if (!strcmp (a, "--go"))
      opts.go = TRUE;
    else if (!strcmp (a, "--free"))
      opts.simulate = opts.free = TRUE;
    else if (!strcmp (a, "--from") && v && parse_pair (v, &opts.from_lat, &opts.from_lng, NULL))
      opts.have_from = TRUE, i++;
    else if (!strcmp (a, "--to") && v && parse_pair (v, &opts.to_lat, &opts.to_lng, &opts.to_name))
      opts.have_to = TRUE, i++;
    else if (!strcmp (a, "--search") && v)
      opts.search = g_strdup (v), i++;
    else if (!strcmp (a, "--night"))
      g_setenv ("NAVI_NIGHT", "1", TRUE);
    else if (!strcmp (a, "--day"))
      g_setenv ("NAVI_NIGHT", "0", TRUE);
    else if (!strcmp (a, "--seconds") && v)
      opts.seconds = atoi (v), i++;
    else if (!strcmp (a, "--close-at") && v) {
      g_auto (GStrv) parts = g_strsplit (v, ":", 2);
      opts.close_at = g_strdup (parts[0]);
      opts.close_after = parts[1] ? atoi (parts[1]) : 0;
      i++;
    }
    else if (!strcmp (a, "--stats"))
      g_setenv ("NAVI_STATS", "1", TRUE);
    else
      g_ptr_array_add (rest, argv[i]);
  }
  /* a test run is its own instance, beside one the user has open */
  GApplicationFlags flags = opts.have_to || opts.simulate || opts.search || opts.have_from
                              ? G_APPLICATION_NON_UNIQUE
                              : G_APPLICATION_DEFAULT_FLAGS;
  g_autoptr (GtkApplication) app = gtk_application_new ("vibeos.Navi", flags);
  g_signal_connect (app, "startup", G_CALLBACK (vibeos_init), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  g_autoptr (GSimpleAction) planned = g_simple_action_new ("planned-drive", G_VARIANT_TYPE_INT64);
  g_signal_connect (planned, "activate", G_CALLBACK (planned_drive), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (planned));
  int status = g_application_run (G_APPLICATION (app), rest->len, (char **) rest->pdata);
  g_ptr_array_free (rest, TRUE);
  return status;
}
