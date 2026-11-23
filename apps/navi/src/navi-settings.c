/* navi-settings: see navi-settings.h. */
#include "navi-settings.h"
#include "navi-geo.h"
#include "navi-route.h"

#define RECENT_MAX 20
#define SAVED_MAX  200

static NaviSettings *the_settings;
static guint save_source;

static char *
path (void)
{
  return g_build_filename (g_get_user_config_dir (), "vibeos-navi", "navi.ini", NULL);
}

NaviPlace *
navi_settings_read_place (GKeyFile *kf, const char *group)
{
  if (!g_key_file_has_group (kf, group))
    return NULL;
  g_autofree char *name = g_key_file_get_string (kf, group, "name", NULL);
  if (!name || !*name)
    return NULL;
  g_autofree char *detail = g_key_file_get_string (kf, group, "detail", NULL);
  NaviPlace *p = navi_place_new (g_key_file_get_integer (kf, group, "kind", NULL), name, detail,
                                 g_key_file_get_double (kf, group, "lat", NULL),
                                 g_key_file_get_double (kf, group, "lng", NULL));
  p->category = g_key_file_get_string (kf, group, "category", NULL);
  return p;
}

static void
saved_free (NaviSaved *saved)
{
  navi_place_free (saved->place);
  g_free (saved->label);
  g_free (saved);
}

void
navi_settings_write_place (GKeyFile *kf, const char *group, const NaviPlace *p)
{
  if (!p || !p->name || !*p->name)
    return;
  g_key_file_set_integer (kf, group, "kind", p->kind);
  g_key_file_set_string (kf, group, "name", p->name);
  if (p->detail)
    g_key_file_set_string (kf, group, "detail", p->detail);
  if (p->category)
    g_key_file_set_string (kf, group, "category", p->category);
  g_key_file_set_double (kf, group, "lat", p->lat);
  g_key_file_set_double (kf, group, "lng", p->lng);
}

NaviSettings *
navi_settings_load (void)
{
  if (the_settings)
    return the_settings;
  NaviSettings *s = the_settings = g_new0 (NaviSettings, 1);
  s->dark = NAVI_DARK_AT_NIGHT;
  s->view = NAVI_VIEW_AUTO;
  s->auto_zoom = TRUE;
  s->voice = NAVI_VOICE_ON;
  s->cameras = TRUE; /* the owner's choice, 2026-11-23 (see navi-cameras.c for where they stay off) */
  s->speeding = NAVI_SPEEDING_PLUS_5;
  s->recent = g_ptr_array_new_with_free_func ((GDestroyNotify) navi_place_free);
  s->saved = g_ptr_array_new_with_free_func ((GDestroyNotify) saved_free);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *file = path ();
  if (!g_key_file_load_from_file (kf, file, G_KEY_FILE_NONE, NULL))
    return s;
  GError *e = NULL;
  int v = g_key_file_get_integer (kf, "map", "dark-mode", &e);
  if (!e)
    s->dark = CLAMP (v, 0, NAVI_DARK_SYSTEM);
  g_clear_error (&e);
  v = g_key_file_get_integer (kf, "map", "view", &e);
  if (!e)
    s->view = CLAMP (v, 0, NAVI_VIEW_2D);
  g_clear_error (&e);
  s->north_up = g_key_file_get_boolean (kf, "map", "north-up", NULL);
  gboolean b = g_key_file_get_boolean (kf, "map", "auto-zoom", &e);
  if (!e)
    s->auto_zoom = b;
  g_clear_error (&e);
  v = g_key_file_get_integer (kf, "voice", "mode", &e);
  if (!e)
    s->voice = CLAMP (v, 0, NAVI_VOICE_OFF);
  g_clear_error (&e);
  s->voice_language = g_key_file_get_string (kf, "voice", "language", NULL);
  if (!navi_spoken_language_name (s->voice_language))
    g_clear_pointer (&s->voice_language, g_free);
  b = g_key_file_get_boolean (kf, "alerts", "speed-cameras", &e);
  if (!e)
    s->cameras = b;
  g_clear_error (&e);
  s->cameras_notice_seen = g_key_file_get_boolean (kf, "alerts", "camera-notice-seen", NULL);
  v = g_key_file_get_integer (kf, "alerts", "speeding", &e);
  if (!e)
    s->speeding = CLAMP (v, 0, NAVI_SPEEDING_PERCENT_10);
  g_clear_error (&e);
  s->avoid_tolls = g_key_file_get_boolean (kf, "route", "avoid-tolls", NULL);
  s->avoid_highways = g_key_file_get_boolean (kf, "route", "avoid-highways", NULL);
  s->avoid_ferries = g_key_file_get_boolean (kf, "route", "avoid-ferries", NULL);
  s->vehicle = CLAMP (g_key_file_get_integer (kf, "route", "vehicle", NULL), 0, NAVI_VEHICLE_MOTORBIKE);
  s->units = CLAMP (g_key_file_get_integer (kf, "general", "units", NULL), 0, NAVI_UNITS_MI);
  s->home = navi_settings_read_place (kf, "home");
  s->work = navi_settings_read_place (kf, "work");
  if (g_key_file_has_key (kf, "map", "last-lat", NULL)) {
    s->have_last = TRUE;
    s->last_lat = g_key_file_get_double (kf, "map", "last-lat", NULL);
    s->last_lng = g_key_file_get_double (kf, "map", "last-lng", NULL);
  }
  /* a bad entry (no name: an older build saved a nameless place) is skipped, the rest kept */
  for (int i = 0; i < RECENT_MAX * 2 && s->recent->len < RECENT_MAX; i++) {
    g_autofree char *group = g_strdup_printf ("recent-%d", i);
    NaviPlace *p = navi_settings_read_place (kf, group);
    if (p)
      g_ptr_array_add (s->recent, p);
  }
  for (int i = 0; i < SAVED_MAX; i++) {
    g_autofree char *group = g_strdup_printf ("saved-%d", i);
    NaviPlace *p = navi_settings_read_place (kf, group);
    if (!p)
      continue;
    NaviSaved *saved = g_new0 (NaviSaved, 1);
    saved->place = p;
    saved->label = g_key_file_get_string (kf, group, "label", NULL);
    g_ptr_array_add (s->saved, saved);
  }
  return s;
}

void
navi_settings_flush (NaviSettings *s)
{
  g_clear_handle_id (&save_source, g_source_remove);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_integer (kf, "map", "dark-mode", s->dark);
  g_key_file_set_integer (kf, "map", "view", s->view);
  g_key_file_set_boolean (kf, "map", "north-up", s->north_up);
  g_key_file_set_boolean (kf, "map", "auto-zoom", s->auto_zoom);
  if (s->have_last) {
    g_key_file_set_double (kf, "map", "last-lat", s->last_lat);
    g_key_file_set_double (kf, "map", "last-lng", s->last_lng);
  }
  g_key_file_set_integer (kf, "voice", "mode", s->voice);
  if (s->voice_language)
    g_key_file_set_string (kf, "voice", "language", s->voice_language);
  g_key_file_set_boolean (kf, "alerts", "speed-cameras", s->cameras);
  g_key_file_set_boolean (kf, "alerts", "camera-notice-seen", s->cameras_notice_seen);
  g_key_file_set_integer (kf, "alerts", "speeding", s->speeding);
  /* the route options and units only once one is not the default: an ini nobody changed stays as it was */
  if (s->avoid_tolls || s->avoid_highways || s->avoid_ferries || s->vehicle != NAVI_VEHICLE_CAR) {
    g_key_file_set_boolean (kf, "route", "avoid-tolls", s->avoid_tolls);
    g_key_file_set_boolean (kf, "route", "avoid-highways", s->avoid_highways);
    g_key_file_set_boolean (kf, "route", "avoid-ferries", s->avoid_ferries);
    g_key_file_set_integer (kf, "route", "vehicle", s->vehicle);
  }
  if (s->units != NAVI_UNITS_AUTO)
    g_key_file_set_integer (kf, "general", "units", s->units);
  navi_settings_write_place (kf, "home", s->home);
  navi_settings_write_place (kf, "work", s->work);
  for (guint i = 0; i < s->recent->len; i++) {
    g_autofree char *group = g_strdup_printf ("recent-%u", i);
    navi_settings_write_place (kf, group, s->recent->pdata[i]);
  }
  for (guint i = 0; i < s->saved->len; i++) {
    NaviSaved *saved = s->saved->pdata[i];
    g_autofree char *group = g_strdup_printf ("saved-%u", i);
    navi_settings_write_place (kf, group, saved->place);
    if (saved->label)
      g_key_file_set_string (kf, group, "label", saved->label);
  }
  g_autofree char *file = path ();
  g_autofree char *dir = g_path_get_dirname (file);
  g_mkdir_with_parents (dir, 0700);
  g_autoptr (GError) error = NULL;
  if (!g_key_file_save_to_file (kf, file, &error))
    g_message ("settings: %s", error->message);
}

static gboolean
save_now (gpointer data)
{
  save_source = 0;
  navi_settings_flush (data);
  return G_SOURCE_REMOVE;
}

void
navi_settings_save (NaviSettings *s)
{
  if (!save_source)
    save_source = g_timeout_add_seconds (2, save_now, s);
}

void
navi_settings_add_recent (NaviSettings *s, const NaviPlace *place)
{
  if (!place->name || !*place->name)
    return;
  for (guint i = 0; i < s->recent->len; i++) {
    NaviPlace *p = s->recent->pdata[i];
    if (!g_strcmp0 (p->name, place->name) && navi_geo_distance (p->lat, p->lng, place->lat, place->lng) < 30) {
      g_ptr_array_remove_index (s->recent, i);
      break;
    }
  }
  g_ptr_array_insert (s->recent, 0, navi_place_copy (place));
  if (s->recent->len > RECENT_MAX)
    g_ptr_array_set_size (s->recent, RECENT_MAX);
  navi_settings_save (s);
}

int
navi_settings_find_saved (NaviSettings *s, const NaviPlace *place)
{
  for (guint i = 0; i < s->saved->len; i++) {
    NaviPlace *p = ((NaviSaved *) s->saved->pdata[i])->place;
    if (navi_geo_distance (p->lat, p->lng, place->lat, place->lng) < 30)
      return i;
  }
  return -1;
}

void
navi_settings_add_saved (NaviSettings *s, const NaviPlace *place)
{
  if (!place->name || !*place->name || navi_settings_find_saved (s, place) >= 0 || s->saved->len >= SAVED_MAX)
    return;
  NaviSaved *saved = g_new0 (NaviSaved, 1);
  saved->place = navi_place_copy (place);
  g_ptr_array_add (s->saved, saved);
  navi_settings_save (s);
}

void
navi_settings_remove_saved (NaviSettings *s, guint index)
{
  if (index >= s->saved->len)
    return;
  g_ptr_array_remove_index (s->saved, index);
  navi_settings_save (s);
}

void
navi_settings_rename_saved (NaviSettings *s, guint index, const char *label)
{
  if (index >= s->saved->len)
    return;
  NaviSaved *saved = s->saved->pdata[index];
  g_autofree char *trimmed = label ? g_strstrip (g_strdup (label)) : NULL;
  g_free (saved->label);
  saved->label = trimmed && *trimmed && g_strcmp0 (trimmed, saved->place->name) ? g_steal_pointer (&trimmed) : NULL;
  navi_settings_save (s);
}

const char *
navi_saved_name (const NaviSaved *saved)
{
  return saved->label ? saved->label : saved->place->name;
}
