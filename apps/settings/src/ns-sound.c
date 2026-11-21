/* Sound & vibration. The media and microphone volumes and the output device through libpulse (PipeWire's pulse
 * server here), connected once at start (ns_sound_init) and followed by its subscription. The sound mode and the
 * vibration strength are feedbackd's settings, which the volume panel, the Phone and the Clock all follow.
 *
 * Ringtones are the apps' own: nothing on the phone plays a sound theme's event sounds. The Phone app (its ringer,
 * apps/dialer/src/ringer.c) plays vibeos.Dialer ringtone at ring-volume, the Clock (alarm-klaxon.c) plays
 * vibeos.Clock default-alarm-ringtone at alarm-volume; both set the speaker to that volume while they ring. The
 * sounds to pick from are the ones those apps list: sounds/android/ringtones and sounds/android/alarms in the
 * system data dirs. */
#include "ns-caps.h"
#include "ns-pages.h"
#include "ns-sound.h"
#include "ns-ui.h"

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

static pa_context *ctx;
static gboolean ready;

typedef struct {
  GtkWidget *media, *mic, *outs;    /* two scales, the output card */
  char      *sink, *source;         /* the defaults */
  guint8     sink_ch, source_ch;
  char      *sink_list;             /* the sinks last listed, to rebuild the card only when they change */
  GPtrArray *names;
  gboolean   syncing;
} Sound;

static Sound *shown;                /* the open page's, or NULL */

static void refresh (void);

static void
subscribed (pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *data)
{
  refresh ();
}

static void
state (pa_context *c, void *data)
{
  pa_context_state_t s = pa_context_get_state (c);
  if (s == PA_CONTEXT_READY) {
    ready = TRUE;
    pa_context_set_subscribe_callback (c, subscribed, NULL);
    pa_operation *o = pa_context_subscribe (c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE
                                               | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
    if (o)
      pa_operation_unref (o);
    refresh ();
  } else if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) {
    ready = FALSE;
  }
}

void
ns_sound_init (void)
{
  pa_glib_mainloop *ml = pa_glib_mainloop_new (NULL);
  pa_proplist *p = pa_proplist_new ();
  pa_proplist_sets (p, PA_PROP_APPLICATION_ID, "vibeos.Settings");
  ctx = pa_context_new_with_proplist (pa_glib_mainloop_get_api (ml), "Settings", p);
  pa_proplist_free (p);
  pa_context_set_state_callback (ctx, state, NULL);
  pa_context_connect (ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
}

static void
op (pa_operation *o)
{
  if (o)
    pa_operation_unref (o);
}

static double
percent (const pa_cvolume *v, int mute)
{
  return mute ? 0 : 100.0 * pa_cvolume_avg (v) / PA_VOLUME_NORM;
}

static void
show_scale (GtkWidget *scale, double v)
{
  /* not while a finger is on it */
  if (gtk_widget_has_focus (scale) && gtk_widget_get_state_flags (scale) & GTK_STATE_FLAG_ACTIVE)
    return;
  shown->syncing = TRUE;
  gtk_range_set_value (GTK_RANGE (scale), v);
  shown->syncing = FALSE;
}

static void
sink_got (pa_context *c, const pa_sink_info *i, int eol, void *data)
{
  if (eol || !i || !shown)
    return;
  shown->sink_ch = i->volume.channels;
  show_scale (shown->media, percent (&i->volume, i->mute));
}

static void
source_got (pa_context *c, const pa_source_info *i, int eol, void *data)
{
  if (eol || !i || !shown)
    return;
  shown->source_ch = i->volume.channels;
  show_scale (shown->mic, percent (&i->volume, i->mute));
  gtk_widget_set_visible (gtk_widget_get_ancestor (shown->mic, GTK_TYPE_LIST_BOX_ROW), TRUE);
}

static void
out_picked (GtkWidget *row, guint i, gpointer data)
{
  if (shown && i < shown->names->len)
    op (pa_context_set_default_sink (ctx, shown->names->pdata[i], NULL, NULL));
}

/* the output card: a picker when there is more than one place for the sound to go */
static void
sinks_got (pa_context *c, const pa_sink_info *i, int eol, void *data)
{
  static GString *names;
  static GPtrArray *labels;
  if (!names) {
    names = g_string_new (NULL);
    labels = g_ptr_array_new_with_free_func (g_free);
  }
  if (!eol && i) {
    g_string_append_printf (names, "%s\n", i->name);
    g_ptr_array_add (labels, g_strdup (i->description ? i->description : i->name));
    return;
  }
  if (shown && g_strcmp0 (shown->sink_list, names->str)) {
    g_free (shown->sink_list);
    shown->sink_list = g_strdup (names->str);
    g_ptr_array_set_size (shown->names, 0);
    g_auto (GStrv) n = g_strsplit (names->str, "\n", -1);
    guint sel = 0;
    for (guint k = 0; n[k] && *n[k]; k++) {
      if (!g_strcmp0 (n[k], shown->sink))
        sel = k;
      g_ptr_array_add (shown->names, g_strdup (n[k]));
    }
    g_ptr_array_add (labels, NULL);
    ns_group_clear (shown->outs);
    if (shown->names->len > 1)
      ns_row_pick (shown->outs, "Play sound on", (const char *const *) labels->pdata, sel, out_picked, NULL);
    else if (labels->len > 1)
      ns_row_info (shown->outs, "Play sound on", labels->pdata[0]);
    ns_group_set_visible (shown->outs, labels->len > 1);
  }
  g_string_truncate (names, 0);
  g_ptr_array_set_size (labels, 0);
}

static void
server_got (pa_context *c, const pa_server_info *i, void *data)
{
  if (!shown || !i)
    return;
  gboolean moved = g_strcmp0 (shown->sink, i->default_sink_name) != 0;
  g_free (shown->sink);
  g_free (shown->source);
  shown->sink = g_strdup (i->default_sink_name);
  shown->source = g_strdup (i->default_source_name);
  if (moved)
    g_clear_pointer (&shown->sink_list, g_free);
  if (shown->sink)
    op (pa_context_get_sink_info_by_name (c, shown->sink, sink_got, NULL));
  if (shown->source)
    op (pa_context_get_source_info_by_name (c, shown->source, source_got, NULL));
  op (pa_context_get_sink_info_list (c, sinks_got, NULL));
}

static void
refresh (void)
{
  if (ready && shown)
    op (pa_context_get_server_info (ctx, server_got, NULL));
}

static void
volume_set (GtkRange *r, gpointer is_mic)
{
  if (!shown || shown->syncing || !ready)
    return;
  const char *name = is_mic ? shown->source : shown->sink;
  guint8 ch = is_mic ? shown->source_ch : shown->sink_ch;
  if (!name || !ch)
    return;
  double p = gtk_range_get_value (r);
  pa_cvolume v;
  pa_cvolume_set (&v, ch, (pa_volume_t) (PA_VOLUME_NORM * p / 100.0));
  if (is_mic) {
    op (pa_context_set_source_volume_by_name (ctx, name, &v, NULL, NULL));
    op (pa_context_set_source_mute_by_name (ctx, name, p < 0.5, NULL, NULL));
  } else {
    op (pa_context_set_sink_volume_by_name (ctx, name, &v, NULL, NULL));
    op (pa_context_set_sink_mute_by_name (ctx, name, p < 0.5, NULL, NULL));
  }
}

static void
sound_free (Sound *s)
{
  if (shown == s)
    shown = NULL;
  g_free (s->sink);
  g_free (s->source);
  g_free (s->sink_list);
  g_ptr_array_unref (s->names);
  g_free (s);
}

/* ---- ringtones: the Phone's and the Clock's ---- */

/* A sound the apps can play: its file's URI, and its title, the file's name without the extension (how both apps
 * title them) */
typedef struct {
  char *uri, *title;
} Tone;

static void
tone_free (Tone *t)
{
  g_free (t->uri);
  g_free (t->title);
  g_free (t);
}

static char *
title_of (const char *uri)
{
  g_autofree char *path = g_filename_from_uri (uri, NULL, NULL);
  char *base = g_path_get_basename (path ? path : uri);
  char *dot = strrchr (base, '.');
  if (dot && dot != base)
    *dot = 0;
  return base;
}

static gint
tone_by_title (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate ((*(Tone **) a)->title, (*(Tone **) b)->title);
}

/* every sound in `subdir` of the system data dirs, by title; a title in two dirs is the first dir's, as in the
 * Phone app's own list */
static GPtrArray *
tones_in (const char *subdir)
{
  GPtrArray *tones = g_ptr_array_new_with_free_func ((GDestroyNotify) tone_free);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char *const *dirs = g_get_system_data_dirs ();
  for (guint i = 0; dirs[i]; i++) {
    g_autofree char *path = g_build_filename (dirs[i], subdir, NULL);
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    for (const char *n; dir && (n = g_dir_read_name (dir));) {
      if (!g_str_has_suffix (n, ".ogg") && !g_str_has_suffix (n, ".wav") && !g_str_has_suffix (n, ".mp3")
          && !g_str_has_suffix (n, ".flac"))
        continue;
      g_autofree char *file = g_build_filename (path, n, NULL);
      Tone *t = g_new0 (Tone, 1);
      t->uri = g_filename_to_uri (file, NULL, NULL);
      t->title = title_of (t->uri ? t->uri : file);
      if (!t->uri || g_hash_table_contains (seen, t->title)) {
        tone_free (t);
        continue;
      }
      g_hash_table_add (seen, g_strdup (t->title));
      g_ptr_array_add (tones, t);
    }
  }
  g_ptr_array_sort (tones, tone_by_title);
  return tones;
}

/* the preview: one sound at a time, stopped by the next one and when the picker is left */
static GSubprocess *preview;

static void
preview_stop (void)
{
  if (preview)
    g_subprocess_force_exit (preview);
  g_clear_object (&preview);
}

static void
preview_page_left (GtkWidget *page, gpointer data)
{
  preview_stop ();
}

static void
sound_back (NsPage *page)
{
  preview_stop ();
}

static void
preview_play (const char *uri)
{
  preview_stop ();
  g_autofree char *path = g_filename_from_uri (uri, NULL, NULL);
  g_autofree char *pw = g_find_program_in_path ("pw-play");
  if (path)
    preview = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, NULL, pw ? "pw-play" : "paplay", path, NULL);
}

/* One app's sound setting, a string key: '' (or "default") plays the app's default file, `none` (when the app has
 * that choice) plays nothing, anything else is a sound's URI */
typedef struct {
  GSettings  *s;
  const char *key;
  const char *default_title;   /* what '' plays: "Orion", "Hassium" */
  const char *none;            /* the key's value for no sound, or NULL */
  GPtrArray  *tones;           /* the picker's rows after None: the list, and the current sound if not in it */
} ToneKey;

static void
tone_key_free (ToneKey *k)
{
  g_ptr_array_unref (k->tones);
  g_free (k);
}

static void
tone_picked (GtkWidget *row, guint i, gpointer data)
{
  ToneKey *k = data;
  if (k->none && i == 0) {
    preview_stop ();
    g_settings_set_string (k->s, k->key, k->none);
    return;
  }
  Tone *t = k->tones->pdata[i - (k->none ? 1 : 0)];
  g_settings_set_string (k->s, k->key, t->uri);
  preview_play (t->uri);
}

/* the picker's row for the key's value: None, the default's file when the key is unset, or the URI; G_MAXUINT for
 * a sound that is not in the list */
static guint
tone_index (ToneKey *k)
{
  g_autofree char *cur = g_settings_get_string (k->s, k->key);
  guint first = k->none ? 1 : 0;
  if (k->none && !g_strcmp0 (cur, k->none))
    return 0;
  gboolean unset = !*cur || !g_strcmp0 (cur, "default");
  for (guint i = 0; i < k->tones->len; i++) {
    Tone *t = k->tones->pdata[i];
    if (unset ? !g_strcmp0 (t->title, k->default_title) : !g_strcmp0 (t->uri, cur))
      return i + first;
  }
  return G_MAXUINT;
}

/* The key changed, here or in the app itself (the Phone's own ringtone list): the row shows what is set now. A
 * sound not in the list still gets its title, from its file name. */
static void
tone_changed (GSettings *s, const char *key, GtkWidget *row)
{
  ToneKey *k = g_object_get_data (G_OBJECT (row), "ns-tone");
  guint at = tone_index (k);
  ns_row_pick_select (row, at);
  if (at == G_MAXUINT) {
    g_autofree char *cur = g_settings_get_string (s, key);
    g_autofree char *title = !*cur || !g_strcmp0 (cur, "default") ? g_strdup (k->default_title) : title_of (cur);
    ns_row_set_subtitle (row, title);
  }
}

static GtkWidget *
tone_row (GtkWidget *g, const char *title, GSettings *s, const char *key, const char *subdir,
          const char *default_title, const char *none)
{
  ToneKey *k = g_new0 (ToneKey, 1);
  k->s = s;
  k->key = key;
  k->default_title = default_title;
  k->none = none;
  k->tones = tones_in (subdir);

  /* A sound picked from elsewhere (the Clock's own "Add") gets a row of its own, so the picker shows what is set */
  g_autofree char *cur = g_settings_get_string (s, key);
  if (tone_index (k) == G_MAXUINT && *cur && g_strcmp0 (cur, "default")) {
    Tone *t = g_new0 (Tone, 1);
    t->uri = g_strdup (cur);
    t->title = title_of (cur);
    g_ptr_array_add (k->tones, t);
  }

  g_autoptr (GPtrArray) labels = g_ptr_array_new ();
  if (none)
    g_ptr_array_add (labels, (gpointer) "None");
  for (guint i = 0; i < k->tones->len; i++)
    g_ptr_array_add (labels, ((Tone *) k->tones->pdata[i])->title);
  g_ptr_array_add (labels, NULL);
  GtkWidget *row = ns_row_pick (g, title, (const char *const *) labels->pdata, tone_index (k), tone_picked, k);
  g_object_set_data_full (G_OBJECT (row), "ns-tone", k, (GDestroyNotify) tone_key_free);
  /* connected for as long as the row lives: the settings object is the page's and outlives it */
  g_autofree char *signal = g_strconcat ("changed::", key, NULL);
  g_signal_connect_object (s, signal, G_CALLBACK (tone_changed), row, 0);
  return row;
}

/* ---- a 0…1 volume key (the Phone's ring-volume, the Clock's alarm-volume) on a 0…100 slider ---- */

static gboolean
level_get (GValue *value, GVariant *v, gpointer data)
{
  g_value_set_double (value, 100.0 * g_variant_get_double (v));
  return TRUE;
}

static GVariant *
level_set (const GValue *value, const GVariantType *type, gpointer data)
{
  return g_variant_new_double (g_value_get_double (value) / 100.0);
}

static GtkWidget *
level_row (GtkWidget *g, const char *title, const char *icon, GSettings *s, const char *key)
{
  GtkWidget *scale;
  GtkWidget *row = ns_row_scale (g, title, icon, 0, 100, 1, &scale);
  g_settings_bind_with_mapping (s, key, gtk_range_get_adjustment (GTK_RANGE (scale)), "value",
                                G_SETTINGS_BIND_DEFAULT, level_get, level_set, NULL, NULL);
  return row;
}

/* ---- the page ---- */

static const char *const profiles[] = { "full", "quiet", "silent", NULL };
static const char *const profile_labels[] = { "Sound", "Vibrate", "Mute", NULL };

/* "Louder than 100%" moves the end of the media slider at once */
static void
louder_changed (GSettings *snd, const char *key, GtkWidget *media)
{
  gtk_range_set_range (GTK_RANGE (media), 0, g_settings_get_boolean (snd, key) ? 150 : 100);
}

static void
haptic_set (GtkRange *r, GSettings *fb)
{
  g_settings_set_double (fb, "max-haptic-strength", gtk_range_get_value (r) / 100.0);
}

/* Not on this page, and why:
 * - A notification and a chat sound (Phosh's sound theme links for message-new-sms and message-new-instant):
 *   GNOME Shell plays a notification's sound only when the notification names one (a sound-name or sound-file
 *   hint), and the Messages service sends none. It needs that hint from the app, with a key to choose it by.
 * - "Flash for notifications" (feedbackd prefer-flash): feedbackd lights an LED only for the events it is sent,
 *   and nothing sends it one: the Phone and the Clock vibrate through its Haptic interface, notifications do not
 *   go through it at all.
 * - "Volume down silences calls" (Phosh's quick-silent): the keys are VibeOS's (volume.js), which has no such
 *   rule. */
void
ns_page_sound (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Sound & vibration", &c);
  Sound *s = g_new0 (Sound, 1);
  s->names = g_ptr_array_new_with_free_func (g_free);
  g_object_set_data_full (G_OBJECT (page), "sound", s, (GDestroyNotify) sound_free);
  shown = s;
  /* back from a ringtone picker (or gone): its preview stops */
  ns_page_on_back (page, sound_back);
  g_signal_connect (page, "destroy", G_CALLBACK (preview_page_left), NULL);

  GSettings *fb = ns_page_settings (page, "org.sigxcpu.feedbackd");
  GSettings *phone = ns_page_settings (page, "vibeos.Dialer");
  GSettings *clock = ns_page_settings (page, "vibeos.Clock");
  GtkWidget *g;
  if (fb) {
    g = ns_group (c, NULL);
    ns_row_choice (g, "Sound mode", fb, "profile", profiles, profile_labels);
  }

  GSettings *snd = ns_page_settings (page, "org.gnome.desktop.sound");
  double top = snd && ns_has_key (snd, "allow-volume-above-100-percent")
               && g_settings_get_boolean (snd, "allow-volume-above-100-percent") ? 150 : 100;
  g = ns_group (c, "Volume");
  /* what is for calls shows with a modem only */
  if (ns_has_key (phone, "ring-volume"))
    ns_caps_bind_visible (level_row (g, "Ring volume", "call-start-symbolic", phone, "ring-volume"), NS_CAP_MODEM);
  ns_row_scale (g, "Media volume", "audio-volume-high-symbolic", 0, top, 1, &s->media);
  g_signal_connect (s->media, "value-changed", G_CALLBACK (volume_set), NULL);
  if (ns_has_key (clock, "alarm-volume"))
    level_row (g, "Alarm volume", "alarm-symbolic", clock, "alarm-volume");
  GtkWidget *mr = ns_row_scale (g, "Microphone", "audio-input-microphone-symbolic", 0, 100, 1, &s->mic);
  gtk_widget_set_visible (mr, FALSE);
  g_signal_connect (s->mic, "value-changed", G_CALLBACK (volume_set), GINT_TO_POINTER (1));
  s->outs = ns_group (c, NULL);
  ns_group_set_visible (s->outs, FALSE);

  if (fb || phone) {
    g = ns_group (c, "Vibration");
    if (ns_has_key (fb, "max-haptic-strength")) {
      GtkWidget *scale;
      ns_row_scale (g, "Vibration intensity", "phone-symbolic", 0, 100, 10, &scale);
      gtk_range_set_value (GTK_RANGE (scale), 100 * g_settings_get_double (fb, "max-haptic-strength"));
      g_signal_connect (scale, "value-changed", G_CALLBACK (haptic_set), fb);
    }
    if (ns_has_key (phone, "vibrate-when-ringing"))
      ns_caps_bind_visible (ns_row_switch (g, "Vibrate while ringing", "Calls vibrate as well as ring in Sound mode",
                                           phone, "vibrate-when-ringing"), NS_CAP_MODEM);
    /* nothing but the calls' row in it: the card goes with it */
    if (!ns_has_key (fb, "max-haptic-strength"))
      ns_caps_bind_visible (g, NS_CAP_MODEM);
  }

  if (phone || clock) {
    g = ns_group (c, "Sound patterns");
    /* the defaults are the apps' own (ringer.c DEFAULT_RINGTONE, alarm-klaxon.c's kinds[]) */
    if (ns_has_key (phone, "ringtone"))
      ns_caps_bind_visible (tone_row (g, "Phone ringtone", phone, "ringtone", "sounds/android/ringtones", "Orion",
                                      "none"), NS_CAP_MODEM);
    if (ns_has_key (clock, "default-alarm-ringtone"))
      tone_row (g, "Default alarm sound", clock, "default-alarm-ringtone", "sounds/android/alarms", "Hassium", NULL);
  }

  /* the Phone's key tones: a phone's only system sound VibeOS has a switch for */
  if (ns_has_key (phone, "dtmf-tone-when-dialing")) {
    g = ns_group (c, "System sounds");
    ns_row_switch (g, "Dial pad tones", NULL, phone, "dtmf-tone-when-dialing");
    ns_caps_bind_visible (g, NS_CAP_MODEM);
  }

  if (ns_has_key (snd, "allow-volume-above-100-percent")) {
    g = ns_group (c, NULL);
    ns_row_switch (g, "Louder than 100%", "Can distort the sound", snd, "allow-volume-above-100-percent");
    g_signal_connect_object (snd, "changed::allow-volume-above-100-percent", G_CALLBACK (louder_changed), s->media,
                             0);
  }
  refresh ();
  ns_push (page);
}
