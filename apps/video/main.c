/* vibeos-video: the video player. mpv plays the video (libmpv: its demuxers, decoders and seeking, which play every
 * file the owner tried, over a Samba share too, and skip anywhere at once), drawn by its OpenGL renderer into a
 * GtkGLArea; the controls over it are an Android player's: a back arrow, the title and ⋮ at the top, replay 10 s,
 * play/pause and forward 10 s in the middle, the time, the seek bar and the length at the bottom. A tap shows or hides
 * them (they hide by themselves after a few seconds of playing); a double tap on the left or right third skips 10 s.
 *
 * A finger slid up or down over the picture sets the volume on its right half and the brightness on its left, as
 * tablet players do (more is up; the picture's whole height is the whole range). It starts when the finger has moved
 * past the touch slop more up or down than sideways, and not on a button, the seek bar, the system's side strips (the
 * back swipe's) or its gesture bar along the bottom. The volume is the sound server's default output (the one the
 * volume keys and the shell's slider set), the brightness the shell's (vibeos.Shell's Brightness, the shade's slider;
 * never below 2 %). Both are asked for without waiting, one at a time: with no sound server, or no backlight the shell
 * drives, the slide does nothing. A bar on that side shows the level, and fades a second after the finger is up.
 *
 * The phones' and tablets' hardware decoders (V4L2 memory-to-memory: Qualcomm's venus and iris, Samsung's MFC) decode
 * H.264 and HEVC: a third to a tenth of the CPU time of FFmpeg's decoders, and the phone stays cooler. Other codecs
 * only where the device says its decoder takes them reliably (VP9 wedged one's firmware until a reboot): the
 * device layer's /usr/share/vibeos-device/video.conf, a key file, [Video] hwdec-codecs=h264,hevc,vp9 (mpv's list).
 * A film the decoder still cannot take (10-bit HEVC on some) is decoded in software, by mpv's own fallback.
 *
 * Where nothing takes the film's sound, mpv would wait for it and the film would stand still: a sound server with no
 * output (PipeWire with only its dummy sink, or none). Then it plays without sound (mpv's ao "null") and the controls
 * say "No audio output": at once when PipeWire lists no real output as the film opens; when no output would open (the
 * film then goes on without its sound, or, with nothing else to play, ends and is opened again where it was); else
 * when a film that started has not moved for 2 s while not paused, buffering or seeking. A film opened once an output
 * is there has sound again.
 *
 * A file opened from a share (smb://…) is played from GVfs's FUSE path to it: mpv's FFmpeg cannot open smb:// itself.
 * Where a film was left is remembered (mpv's watch-later files, in the user's state directory), and it carries on
 * from there the next time.
 *
 * ⋮ opens the menu: Audio track and Subtitles (when the film has any) open a sheet listing the film's tracks (mpv's
 * track-list), the one playing ticked; Settings has one switch, "Black bar at the camera"; About is VibeOS's About
 * sheet.
 *
 * Full screen goes under the camera hole (or notch): the picture fills the panel and the hole sits inside it. The
 * switch puts the black bar back along the hole, as the shell keeps every other app's full screen clear of it. The
 * shell reads it from vibeos.Shell's cutout-extend-apps, where this app's id is listed (the default) or not, and
 * follows a change at once, while the film plays.
 *
 * The button beside ⋮ puts the player full screen (no status bar) and takes it out again. On a screen wider than tall
 * (a phone or tablet turned sideways, a TV) it opens full screen, and turning the screen chooses again: sideways full
 * screen, upright not; the button changes that until the next turn. Full screen, a film shot the way the screen is
 * turned fills it, cropped to the screen's shape (mpv's pan-and-scan), as Android's players zoom a film to fill; one
 * shot the other way (a wide film on an upright phone) stays whole: filled, most of it would be cut away.
 *
 * On the session bus the player is an MPRIS one (org.mpris.MediaPlayer2.vibeos.Video), as an Android player has a
 * media session: what plays and whether it plays, and play, pause and seek from outside. WirePlumber pauses it
 * there when the car or headphones it plays on go (Android's "becoming noisy"; linking/mpris-pause.lua). */
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>
#include <string.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <spongy-gtk.h>
#include <unicode/ucasemap.h>
#include <unicode/uloc.h>
#include <unicode/ustring.h>
#include <vibeos.h>

#define APP_ID "vibeos.Video"
#define CONTROLS_HIDE_MS 2000
#define DOUBLE_TAP_MS 250
#define SKIP_S 10.0
#define CENTER_BUTTON_DP 72
#define SIDE_BUTTON_DP 56
#define TOP_BUTTON_DP 48
#define STALL_MS 2000
#define LEVEL_HIDE_MS 1000
#define EDGE_DP 30              /* the shell's back swipe starts in a strip this wide at each side */
#define NAV_DP 32               /* and its gesture bar along the bottom */
#define MIN_BRIGHTNESS 0.02
#define DEVICE_VIDEO_CONF "/usr/share/vibeos-device/video.conf"
#define HWDEC_CODECS "h264,hevc"

typedef enum { ICON_BACK, ICON_PLAY, ICON_PAUSE, ICON_REPLAY, ICON_FORWARD, ICON_MORE, ICON_FULLSCREEN } Icon;

typedef struct {
  GtkApplication *app;
  GtkWidget *win, *area, *controls, *title, *notice, *full, *more, *play, *position, *length, *seek, *spinner, *message;
  mpv_handle *mpv;
  mpv_render_context *render;
  gboolean paused, buffering, seeking, ended, have_file, dragging, setting_seek, named, fullscreen, cropped;
  /* the sound: played to nothing (mpv's ao "null"), as nothing took it; PipeWire lists a real output: 1, or none: 0,
   * or not known: -1; the watch for a film that does not move (since when it last moved, and from where) */
  gboolean silent, watching;
  int outputs;
  gint64 moved_at;
  double watch_from;
  guint stall_id;
  char *playing;          /* what was given to mpv to play: a path or a web address */
  double time, duration;
  double aspect;          /* the film's picture as it is shown, width / height (0: no picture) */
  gint64 rotate;          /* and the turn it is shown at (a phone's video filmed upright: 90) */
  int width, height;      /* the window's size */
  int landscape;          /* the screen the window is on is wider than tall: 1, or 0; -1 not known yet */
  guint hide_id, tap_id, events_id, shape_id;
  guint inhibit;
  double tap_x;
  /* the volume and brightness drags (see on_level_update): where the finger went down, whether it is one (1), is
   * not (0) or is not decided yet (-1), which of the two it sets, the level it began from and the finger's travel at
   * that point */
  GtkWidget *level;
  double drag_x, drag_y, drag_from;
  int drag_state;
  gboolean drag_volume;
  double level_value, drag_base;
  guint level_id;
  /* the sound server's default sink (PulseAudio's API, as PipeWire answers it) and the volume asked of it */
  pa_glib_mainloop *pa_loop;
  pa_context *pa;
  pa_cvolume sink_volume;
  gboolean pa_up, sink_known, sink_muted, vol_busy;
  double vol_want, vol_sent;
  /* the shell's brightness over the session bus, and the level asked of it */
  gboolean light_known, light_busy;
  double light_want, light_sent;
  /* MPRIS (see the end): the bus, its object's registrations and name, and what it last told */
  GDBusConnection *bus;
  GDBusNodeInfo *mpris_info;
  guint mpris_root, mpris_player, mpris_name;
  const char *mpris_status;
  char *mpris_title;
  gint64 mpris_length;
} Player;

static void mpris_sync (Player *p);

/* ---- the controls' icons, drawn as Android's player draws them (Material icons, white) ---- */

static void
draw_icon (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Icon icon = GPOINTER_TO_INT (data);
  Player *p = g_object_get_data (G_OBJECT (area), "player");
  double s = MIN (w, h), cx = w / 2.0, cy = h / 2.0;
  gboolean round = icon != ICON_BACK && icon != ICON_MORE && icon != ICON_FULLSCREEN;

  if (icon == ICON_PLAY && p && p->have_file && !p->paused && !p->ended)
    icon = ICON_PAUSE;
  /* the middle buttons on a dark disc, so that they read over any picture */
  if (round) {
    cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
    cairo_arc (cr, cx, cy, s / 2, 0, 2 * G_PI);
    cairo_fill (cr);
  }
  cairo_set_source_rgb (cr, 1, 1, 1);
  /* icons on Material's 24-unit grid, filling half the button (the back arrow 24 dp in its 48) */
  double u = s / 2 / 24;
  cairo_translate (cr, cx - 12 * u, cy - 12 * u);
  cairo_scale (cr, u, u);
  switch (icon) {
  case ICON_BACK:
    cairo_move_to (cr, 20, 11); cairo_line_to (cr, 7.83, 11); cairo_line_to (cr, 13.42, 5.41);
    cairo_line_to (cr, 12, 4); cairo_line_to (cr, 4, 12); cairo_line_to (cr, 12, 20); cairo_line_to (cr, 13.41, 18.59);
    cairo_line_to (cr, 7.83, 13); cairo_line_to (cr, 20, 13); cairo_close_path (cr);
    cairo_fill (cr);
    break;
  case ICON_MORE:
    /* Material's more_vert: three dots in a column */
    for (int i = 0; i < 3; i++) {
      cairo_new_sub_path (cr);
      cairo_arc (cr, 12, 6 + 6 * i, 2, 0, 2 * G_PI);
    }
    cairo_fill (cr);
    break;
  case ICON_FULLSCREEN: {
    /* Material's fullscreen (four corners pointing out), fullscreen_exit when full screen (pointing in), as bars */
    static const double out[8][4] = {
      {5, 5, 5, 2}, {5, 5, 2, 5}, {14, 5, 5, 2}, {17, 5, 2, 5},
      {5, 17, 5, 2}, {5, 14, 2, 5}, {14, 17, 5, 2}, {17, 14, 2, 5},
    };
    static const double in[8][4] = {
      {5, 8, 5, 2}, {8, 5, 2, 5}, {14, 8, 5, 2}, {14, 5, 2, 5},
      {5, 14, 5, 2}, {8, 14, 2, 5}, {14, 14, 5, 2}, {14, 14, 2, 5},
    };
    const double (*bars)[4] = p && p->fullscreen ? in : out;
    for (int i = 0; i < 8; i++)
      cairo_rectangle (cr, bars[i][0], bars[i][1], bars[i][2], bars[i][3]);
    cairo_fill (cr);
    break;
  }
  case ICON_PLAY:
    cairo_move_to (cr, 8, 5); cairo_line_to (cr, 8, 19); cairo_line_to (cr, 19, 12); cairo_close_path (cr);
    cairo_fill (cr);
    break;
  case ICON_PAUSE:
    cairo_rectangle (cr, 6, 5, 4, 14);
    cairo_rectangle (cr, 14, 5, 4, 14);
    cairo_fill (cr);
    break;
  case ICON_REPLAY:
  case ICON_FORWARD: {
    /* a circle open at the top with an arrowhead, "10" inside */
    gboolean fwd = icon == ICON_FORWARD;
    double dir = fwd ? 1 : -1;
    cairo_set_line_width (cr, 2);
    if (fwd)
      cairo_arc (cr, 12, 13, 7, -G_PI / 2 - 0.0, 1.5 * G_PI - 0.7);
    else
      cairo_arc_negative (cr, 12, 13, 7, -G_PI / 2, -G_PI / 2 - 2 * G_PI + 0.7);
    cairo_stroke (cr);
    cairo_move_to (cr, 12 - dir * 0.5, 2.5);
    cairo_line_to (cr, 12 + dir * 4, 6);
    cairo_line_to (cr, 12 - dir * 0.5, 9.5);
    cairo_close_path (cr);
    cairo_fill (cr);
    PangoLayout *layout = pango_cairo_create_layout (cr);
    PangoFontDescription *font = pango_font_description_from_string ("Roboto Bold 6.5px");
    pango_layout_set_font_description (layout, font);
    pango_layout_set_text (layout, "10", -1);
    int tw, th;
    pango_layout_get_pixel_size (layout, &tw, &th);
    cairo_move_to (cr, 12 - tw / 2.0, 13.3 - th / 2.0);
    pango_cairo_show_layout (cr, layout);
    pango_font_description_free (font);
    g_object_unref (layout);
    break;
  }
  }
}

static GtkWidget *
icon_button (Player *p, Icon icon, int dp, const char *tooltip, GCallback clicked)
{
  GtkWidget *art = gtk_drawing_area_new ();
  gtk_widget_set_size_request (art, dp, dp);
  g_object_set_data (G_OBJECT (art), "player", p);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (art), draw_icon, GINT_TO_POINTER (icon), NULL);
  GtkWidget *button = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (button), art);
  gtk_widget_add_css_class (button, "video-icon");
  gtk_widget_set_tooltip_text (button, tooltip);
  gtk_widget_set_focus_on_click (button, FALSE);
  g_signal_connect_swapped (button, "clicked", clicked, p);
  return button;
}

/* ---- mpv ---- */

static void
command (Player *p, const char *const *args)
{
  if (p->mpv)
    mpv_command_async (p->mpv, 0, (const char **) args);
}

static void
format_time (char *buf, gsize size, double t)
{
  long s = t > 0 && isfinite (t) ? (long) t : 0;
  if (s >= 3600)
    g_snprintf (buf, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
  else
    g_snprintf (buf, size, "%ld:%02ld", s / 60, s % 60);
}

static void show_controls (Player *p, gboolean show);
static void set_controls (Player *p, gboolean show, gboolean asked);

/* the screen stays on while a video plays */
static void
sync_inhibit (Player *p)
{
  gboolean playing = p->have_file && !p->paused && !p->ended;
  if (playing && !p->inhibit)
    p->inhibit = gtk_application_inhibit (p->app, GTK_WINDOW (p->win), GTK_APPLICATION_INHIBIT_IDLE,
                                          _("Playing a video"));
  else if (!playing && p->inhibit) {
    gtk_application_uninhibit (p->app, p->inhibit);
    p->inhibit = 0;
  }
}

static void
sync_time (Player *p)
{
  char buf[32];
  format_time (buf, sizeof buf, p->time);
  gtk_label_set_text (GTK_LABEL (p->position), buf);
  format_time (buf, sizeof buf, p->duration);
  gtk_label_set_text (GTK_LABEL (p->length), buf);
  if (!p->dragging && p->duration > 0) {
    p->setting_seek = TRUE;
    gtk_range_set_range (GTK_RANGE (p->seek), 0, p->duration);
    gtk_range_set_value (GTK_RANGE (p->seek), CLAMP (p->time, 0, p->duration));
    p->setting_seek = FALSE;
  }
}

/* Full screen, a film shot the way the screen is turned is cropped to fill it (mpv's pan-and-scan, all the way: the
 * picture scaled until no black bar is left, its overflow cut evenly from both sides); else it is fitted, whole */
static void
sync_crop (Player *p, gboolean again)
{
  double film = p->aspect > 0 && p->rotate % 180 == 90 ? 1 / p->aspect : p->aspect;
  double screen = p->height > 0 ? (double) p->width / p->height : 0;
  gboolean crop = p->fullscreen && film > 0 && screen > 0 && (film > 1) == (screen > 1) && film != 1 && screen != 1;
  if (!p->mpv || (crop == p->cropped && !again))
    return;
  p->cropped = crop;
  const char *args[] = {"set", "panscan", crop ? "1" : "0", NULL};
  command (p, args);
}

/* ---- the sound ---- */

static const char *node_string (const mpv_node *map, const char *key);
static GArray *tracks_of (Player *p, const char *type);

/* The film plays without sound (the output mpv opened is closed, and ao "null" takes the sound at the film's pace), or
 * with it again, on the outputs mpv chooses itself */
static void
set_silent (Player *p, gboolean silent)
{
  if (silent == p->silent || !p->mpv)
    return;
  p->silent = silent;
  const char *args[] = {"set", "ao", silent ? "null" : "", NULL};
  command (p, args);
  gtk_widget_set_visible (p->notice, silent);
}

/* mpv's audio-device-list: an output of PipeWire's ("pipewire/NAME") that is not its dummy one (auto_null, "Dummy
 * Output") is a real one. With PipeWire not running there is no telling (mpv then opens another server or ALSA) */
static void
sync_outputs (Player *p, const mpv_node *list)
{
  g_autofree char *socket = g_build_filename (g_get_user_runtime_dir (), "pipewire-0", NULL);
  if (!list || list->format != MPV_FORMAT_NODE_ARRAY || !g_file_test (socket, G_FILE_TEST_EXISTS)) {
    p->outputs = -1;
    return;
  }
  p->outputs = 0;
  for (int i = 0; i < list->u.list->num && !p->outputs; i++) {
    const char *name = node_string (&list->u.list->values[i], "name");
    const char *description = node_string (&list->u.list->values[i], "description");
    if (name && g_str_has_prefix (name, "pipewire/") && !strstr (name, "auto_null") &&
        g_strcmp0 (description, "Dummy Output"))
      p->outputs = 1;
  }
}

static gboolean
check_stall (gpointer data)
{
  Player *p = data;
  p->stall_id = 0;
  if (!p->watching || !p->have_file || p->paused || p->ended || p->buffering || p->seeking || p->silent)
    return G_SOURCE_REMOVE;
  gint64 still = (g_get_monotonic_time () - p->moved_at) / 1000;
  if (still < STALL_MS) {
    p->stall_id = g_timeout_add (STALL_MS - still, check_stall, p);
    return G_SOURCE_REMOVE;
  }
  g_message ("the film has not moved for %d ms: playing it without sound", STALL_MS);
  p->watching = FALSE;
  set_silent (p, TRUE);
  return G_SOURCE_REMOVE;
}

/* Once the film plays: one with sound tracks that has no sound output (none would open, and mpv let go of the sound)
 * plays on without sound, its sound to nothing, and the controls say so */
static void
check_output (Player *p)
{
  if (p->silent || !p->mpv)
    return;
  char *ao = mpv_get_property_string (p->mpv, "current-ao");
  mpv_free (ao);
  if (ao)
    return;
  g_autoptr (GArray) sound = tracks_of (p, "audio");
  if (sound->len == 0)
    return;
  g_message ("no sound output would open: playing without sound");
  set_silent (p, TRUE);
  const char *again[] = {"set", "aid", "auto", NULL};
  command (p, again);
}

/* A film watched from when it opens or is played again, until it has moved a second: one that stands still for
 * STALL_MS (not paused, buffering or seeking) plays on without sound */
static void
watch_stall (Player *p)
{
  p->watching = TRUE;
  p->watch_from = p->time;
  p->moved_at = g_get_monotonic_time ();
  g_clear_handle_id (&p->stall_id, g_source_remove);
  p->stall_id = g_timeout_add (STALL_MS, check_stall, p);
}

static void
on_property (Player *p, mpv_event_property *prop)
{
  if (g_str_equal (prop->name, "time-pos")) {
    double time = prop->format == MPV_FORMAT_DOUBLE ? *(double *) prop->data : 0;
    if (time != p->time)
      p->moved_at = g_get_monotonic_time ();
    p->time = time;
    if (p->watching && fabs (p->time - p->watch_from) >= 1)
      p->watching = FALSE;
    sync_time (p);
  } else if (g_str_equal (prop->name, "duration")) {
    p->duration = prop->format == MPV_FORMAT_DOUBLE ? *(double *) prop->data : 0;
    sync_time (p);
  } else if (g_str_equal (prop->name, "pause")) {
    /* (mpv tells a property again now and then when it has not changed: only a change counts) */
    gboolean paused = prop->format == MPV_FORMAT_FLAG && *(int *) prop->data;
    if (paused == p->paused)
      return;
    p->paused = paused;
    gtk_widget_queue_draw (gtk_button_get_child (GTK_BUTTON (p->play)));
    sync_inhibit (p);
    if (!paused && p->have_file)
      watch_stall (p);
    /* paused, the controls come; played again, they go a little later */
    show_controls (p, TRUE);
  } else if (g_str_equal (prop->name, "eof-reached")) {
    gboolean ended = prop->format == MPV_FORMAT_FLAG && *(int *) prop->data;
    if (ended == p->ended)
      return;
    p->ended = ended;
    gtk_widget_queue_draw (gtk_button_get_child (GTK_BUTTON (p->play)));
    sync_inhibit (p);
    if (p->ended)
      show_controls (p, TRUE);
  } else if (g_str_equal (prop->name, "paused-for-cache") || g_str_equal (prop->name, "seeking")) {
    gboolean on = prop->format == MPV_FORMAT_FLAG && *(int *) prop->data;
    if (g_str_equal (prop->name, "paused-for-cache"))
      p->buffering = on;
    else
      p->seeking = on;
    /* the time it stood still buffering or seeking does not count */
    if (!on)
      p->moved_at = g_get_monotonic_time ();
    gtk_widget_set_visible (p->spinner, p->buffering);
    gtk_spinner_set_spinning (GTK_SPINNER (p->spinner), p->buffering);
  } else if (g_str_equal (prop->name, "video-params/aspect")) {
    p->aspect = prop->format == MPV_FORMAT_DOUBLE ? *(double *) prop->data : 0;
    sync_crop (p, FALSE);
  } else if (g_str_equal (prop->name, "video-params/rotate")) {
    p->rotate = prop->format == MPV_FORMAT_INT64 ? *(gint64 *) prop->data : 0;
    sync_crop (p, FALSE);
  } else if (g_str_equal (prop->name, "audio-device-list")) {
    sync_outputs (p, prop->format == MPV_FORMAT_NODE ? prop->data : NULL);
  } else if (g_str_equal (prop->name, "media-title") && prop->format == MPV_FORMAT_STRING && !p->named) {
    /* a web stream's own title (a file keeps its name: a film's title tag is often another film's) */
    const char *title = *(char **) prop->data;
    gtk_label_set_text (GTK_LABEL (p->title), title);
    gtk_window_set_title (GTK_WINDOW (p->win), title);
  }
}

static gboolean
drain_events (gpointer data)
{
  Player *p = data;
  p->events_id = 0;
  while (p->mpv) {
    mpv_event *ev = mpv_wait_event (p->mpv, 0);
    if (ev->event_id == MPV_EVENT_NONE)
      break;
    switch (ev->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE:
      on_property (p, ev->data);
      break;
    case MPV_EVENT_FILE_LOADED:
      p->ended = FALSE;
      gtk_widget_set_visible (p->message, FALSE);
      watch_stall (p);
      /* the crop is the player's, whatever a film's watch-later file says */
      sync_crop (p, TRUE);
      break;
    case MPV_EVENT_PLAYBACK_RESTART:
      check_output (p);
      break;
    case MPV_EVENT_END_FILE: {
      mpv_event_end_file *end = ev->data;
      /* nothing but the sound to play, and no output for it: again where it was, without sound */
      if (end->reason == MPV_END_FILE_REASON_ERROR && end->error == MPV_ERROR_AO_INIT_FAILED && !p->silent &&
          p->playing) {
        g_message ("no sound output would open: playing again without sound");
        set_silent (p, TRUE);
        char start[64];
        g_snprintf (start, sizeof start, "start=%.3f", p->time);
        const char *again[] = {"loadfile", p->playing, "replace", "-1", start, NULL};
        command (p, again);
        break;
      }
      if (end->reason == MPV_END_FILE_REASON_ERROR) {
        g_warning ("mpv: %s", mpv_error_string (end->error));
        /* nothing plays: the controls stay */
        p->have_file = FALSE;
        sync_inhibit (p);
        gtk_label_set_text (GTK_LABEL (p->message), _("Can’t play this video"));
        gtk_widget_set_visible (p->message, TRUE);
        show_controls (p, TRUE);
      }
      break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
      mpv_event_log_message *msg = ev->data;
      g_message ("mpv [%s] %s: %s", msg->level, msg->prefix, g_strchomp ((char *) msg->text));
      break;
    }
    case MPV_EVENT_SHUTDOWN:
      gtk_window_close (GTK_WINDOW (p->win));
      return G_SOURCE_REMOVE;
    default:
      break;
    }
  }
  mpris_sync (p);
  return G_SOURCE_REMOVE;
}

static gboolean
queue_events (gpointer data)
{
  Player *p = data;
  if (!p->events_id)
    p->events_id = g_idle_add (drain_events, p);
  return G_SOURCE_REMOVE;
}

/* from mpv's own threads: its events are read on the main loop */
static void
wakeup (void *data)
{
  g_main_context_invoke (NULL, queue_events, data);
}

static gboolean
queue_render (gpointer data)
{
  gtk_gl_area_queue_render (GTK_GL_AREA (data));
  return G_SOURCE_REMOVE;
}

static void
on_render_update (void *data)
{
  g_main_context_invoke (NULL, queue_render, data);
}

static void *
get_proc_address (void *ctx, const char *name)
{
  return (void *) eglGetProcAddress (name);
}

static void
on_realize (GtkGLArea *area, Player *p)
{
  gtk_gl_area_make_current (area);
  if (gtk_gl_area_get_error (area) || !p->mpv)
    return;
  mpv_opengl_init_params gl = {.get_proc_address = get_proc_address};
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE, (void *) MPV_RENDER_API_TYPE_OPENGL},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
    {0},
  };
  if (mpv_render_context_create (&p->render, p->mpv, params) < 0) {
    g_warning ("mpv's OpenGL renderer did not start");
    p->render = NULL;
    return;
  }
  mpv_render_context_set_update_callback (p->render, on_render_update, area);
}

static void
on_unrealize (GtkGLArea *area, Player *p)
{
  gtk_gl_area_make_current (area);
  g_clear_pointer (&p->render, mpv_render_context_free);
}

static gboolean
on_render (GtkGLArea *area, GdkGLContext *context, Player *p)
{
  glClearColor (0, 0, 0, 1);
  glClear (GL_COLOR_BUFFER_BIT);
  if (!p->render)
    return TRUE;
  int fbo = 0;
  glGetIntegerv (GL_FRAMEBUFFER_BINDING, &fbo);
  int scale = gtk_widget_get_scale_factor (GTK_WIDGET (area));
  mpv_opengl_fbo target = {
    .fbo = fbo,
    .w = gtk_widget_get_width (GTK_WIDGET (area)) * scale,
    .h = gtk_widget_get_height (GTK_WIDGET (area)) * scale,
  };
  int flip = 1;
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_OPENGL_FBO, &target},
    {MPV_RENDER_PARAM_FLIP_Y, &flip},
    {0},
  };
  mpv_render_context_render (p->render, params);
  return TRUE;
}

static gboolean
start_mpv (Player *p)
{
  /* mpv reads numbers as C does (GTK set the user's locale) */
  setlocale (LC_NUMERIC, "C");
  p->mpv = mpv_create ();
  if (!p->mpv)
    return FALSE;
  g_autofree char *later = g_build_filename (g_get_user_state_dir (), "vibeos", "video", "watch-later", NULL);
  g_mkdir_with_parents (later, 0700);
  const char *const options[][2] = {
    {"vo", "libmpv"},
    /* the device's V4L2 decoder, its frames copied out for the OpenGL renderer (mpv has no way to draw them in place),
     * else what mpv knows to be safe (VA-API on a PC), else FFmpeg's own decoders; a codec the decoder lacks goes
     * on to the next */
    {"hwdec", "v4l2m2m-copy,auto-safe"},
    /* at the end the last frame stays, with the controls, as Android's player leaves it */
    {"keep-open", "yes"},
    {"save-position-on-quit", "yes"},
    {"watch-later-dir", later},
    /* (a film left full screen would come back cropped in the window) */
    {"watch-later-options-remove", "panscan"},
    /* a share's file is read ahead: Wi-Fi that stalls for a moment does not stop the film */
    {"cache", "yes"},
    {"demuxer-readahead-secs", "30"},
    {"demuxer-max-bytes", "200MiB"},
    {"sub-auto", "fuzzy"},
    {"input-default-bindings", "no"},
    {"input-vo-keyboard", "no"},
    {"osc", "no"},
    {"ytdl", "no"},
    {"terminal", "no"},
    /* the name the sound settings list it under */
    {"audio-client-name", _("Video Player")},
  };
  for (gsize i = 0; i < G_N_ELEMENTS (options); i++)
    mpv_set_option_string (p->mpv, options[i][0], options[i][1]);
  /* the codecs the device's decoder may take (see the top) */
  g_autoptr (GKeyFile) device = g_key_file_new ();
  g_autofree char *codecs = NULL;
  if (g_key_file_load_from_file (device, DEVICE_VIDEO_CONF, G_KEY_FILE_NONE, NULL))
    codecs = g_key_file_get_string (device, "Video", "hwdec-codecs", NULL);
  mpv_set_option_string (p->mpv, "hwdec-codecs", codecs && *codecs ? codecs : HWDEC_CODECS);
  /* for a test: mpv's options, NAME=VALUE separated by spaces, over the ones above (hwdec=no, ao=null) */
  g_auto (GStrv) extra = g_strsplit (g_getenv ("VIBEOS_VIDEO_MPV_OPTIONS") ? g_getenv ("VIBEOS_VIDEO_MPV_OPTIONS") : "", " ", -1);
  for (int i = 0; extra[i]; i++) {
    g_auto (GStrv) pair = g_strsplit (extra[i], "=", 2);
    if (pair[0] && pair[1])
      mpv_set_option_string (p->mpv, pair[0], pair[1]);
  }
  if (mpv_initialize (p->mpv) < 0) {
    mpv_destroy (p->mpv);
    p->mpv = NULL;
    return FALSE;
  }
  mpv_observe_property (p->mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
  mpv_observe_property (p->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
  mpv_observe_property (p->mpv, 0, "pause", MPV_FORMAT_FLAG);
  mpv_observe_property (p->mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
  mpv_observe_property (p->mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
  mpv_observe_property (p->mpv, 0, "seeking", MPV_FORMAT_FLAG);
  /* the outputs there are, as they come and go */
  mpv_observe_property (p->mpv, 0, "audio-device-list", MPV_FORMAT_NODE);
  mpv_observe_property (p->mpv, 0, "media-title", MPV_FORMAT_STRING);
  mpv_observe_property (p->mpv, 0, "video-params/aspect", MPV_FORMAT_DOUBLE);
  mpv_observe_property (p->mpv, 0, "video-params/rotate", MPV_FORMAT_INT64);
  /* mpv's errors in the journal (VIBEOS_VIDEO_LOG=v for all it says) */
  mpv_request_log_messages (p->mpv, g_getenv ("VIBEOS_VIDEO_LOG") ? g_getenv ("VIBEOS_VIDEO_LOG") : "error");
  mpv_set_wakeup_callback (p->mpv, wakeup, p);
  return TRUE;
}

/* ---- the controls ---- */

static gboolean
hide_later (gpointer data)
{
  Player *p = data;
  p->hide_id = 0;
  show_controls (p, FALSE);
  return G_SOURCE_REMOVE;
}

/* Shown, they go by themselves after a few seconds of playing (not while paused, at the end or with nothing open);
 * a tap (@asked) shows or hides them whenever */
static void
set_controls (Player *p, gboolean show, gboolean asked)
{
  g_clear_handle_id (&p->hide_id, g_source_remove);
  if (!show && !asked && (p->paused || p->ended || !p->have_file || p->dragging))
    show = TRUE;
  if (show)
    gtk_widget_remove_css_class (p->controls, "hidden");
  else
    gtk_widget_add_css_class (p->controls, "hidden");
  gtk_widget_set_can_target (p->controls, show);
  if (show && !p->paused && !p->ended && p->have_file)
    p->hide_id = g_timeout_add (CONTROLS_HIDE_MS, hide_later, p);
}

static void
show_controls (Player *p, gboolean show)
{
  set_controls (p, show, FALSE);
}

static void
toggle_pause (Player *p)
{
  if (p->ended) {
    const char *again[] = {"seek", "0", "absolute", NULL};
    command (p, again);
    const char *play[] = {"set", "pause", "no", NULL};
    command (p, play);
    return;
  }
  const char *args[] = {"cycle", "pause", NULL};
  command (p, args);
}

static void
skip (Player *p, double by)
{
  char amount[32];
  g_ascii_dtostr (amount, sizeof amount, by);
  const char *args[] = {"seek", amount, "relative+exact", NULL};
  command (p, args);
  show_controls (p, TRUE);
}

static void
replay (Player *p)
{
  skip (p, -SKIP_S);
}

static void
forward (Player *p)
{
  skip (p, SKIP_S);
}

static void
go_back (Player *p)
{
  gtk_window_close (GTK_WINDOW (p->win));
}

/* ---- full screen ---- */

static void
toggle_fullscreen (Player *p)
{
  if (gtk_window_is_fullscreen (GTK_WINDOW (p->win)))
    gtk_window_unfullscreen (GTK_WINDOW (p->win));
  else
    gtk_window_fullscreen (GTK_WINDOW (p->win));
  show_controls (p, TRUE);
}

static void
on_fullscreen (GtkWindow *win, GParamSpec *pspec, Player *p)
{
  p->fullscreen = gtk_window_is_fullscreen (win);
  gtk_widget_set_tooltip_text (p->full, p->fullscreen ? _("Exit full screen") : _("Full screen"));
  gtk_widget_queue_draw (gtk_button_get_child (GTK_BUTTON (p->full)));
  sync_crop (p, FALSE);
}

/* The screen the window is on (before the window is up, the only screen there is) is wider than tall: 1, or 0; -1
 * when that is not known. The screen's, not the window's: the window's own shape changes with full screen (and on a
 * computer as it is dragged), and what counts is how the phone is held. */
static int
screen_landscape (Player *p)
{
  GdkDisplay *display = gtk_widget_get_display (p->win);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (p->win));
  g_autoptr (GdkMonitor) monitor = NULL;
  if (surface && gtk_widget_get_mapped (p->win))
    g_set_object (&monitor, gdk_display_get_monitor_at_surface (display, surface));
  else if (g_list_model_get_n_items (gdk_display_get_monitors (display)) == 1)
    monitor = g_list_model_get_item (gdk_display_get_monitors (display), 0);
  if (!monitor)
    return -1;
  GdkRectangle r;
  gdk_monitor_get_geometry (monitor, &r);
  if (r.width <= 0 || r.height <= 0)
    return -1;
  return r.width > r.height;
}

/* The screen turned (or the window went to another): sideways the player goes full screen, upright it comes out */
static gboolean
check_shape (gpointer data)
{
  Player *p = data;
  p->shape_id = 0;
  int landscape = screen_landscape (p);
  if (landscape < 0 || landscape == p->landscape)
    return G_SOURCE_REMOVE;
  p->landscape = landscape;
  if (landscape)
    gtk_window_fullscreen (GTK_WINDOW (p->win));
  else
    gtk_window_unfullscreen (GTK_WINDOW (p->win));
  return G_SOURCE_REMOVE;
}

static void
queue_shape (Player *p)
{
  /* (not in the surface's layout: the window's state is not changed under its own layout) */
  if (!p->shape_id)
    p->shape_id = g_idle_add (check_shape, p);
}

static void
on_layout (GdkSurface *surface, int width, int height, Player *p)
{
  if (width == p->width && height == p->height)
    return;
  p->width = width;
  p->height = height;
  sync_crop (p, FALSE);
  queue_shape (p);
}

static void
on_window_realize (GtkWidget *win, Player *p)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (win));
  g_signal_connect (surface, "layout", G_CALLBACK (on_layout), p);
  g_signal_connect_swapped (surface, "enter-monitor", G_CALLBACK (queue_shape), p);
}

static void
on_window_unrealize (GtkWidget *win, Player *p)
{
  g_clear_handle_id (&p->shape_id, g_source_remove);
}

/* ---- the ⋮ menu: the audio track, the subtitles, About ---- */

static const VibeosAboutSection video_legal[] = {
  /* TRANSLATORS: keep the links' markup as it is */
  { "mpv", N_("Videos are played by <a href=\"https://mpv.io/\">mpv</a> (libmpv), under the GNU GPL 2.0 or later.") },
  /* TRANSLATORS: keep the links' markup as it is */
  { "FFmpeg", N_("<a href=\"https://ffmpeg.org/\">FFmpeg</a> reads and decodes them, under the GNU LGPL 2.1 or later "
                 "(the GPL 2.0 or later as Debian builds it).") },
  { NULL }
};

static const VibeosAboutInfo video_about = { .name = N_("Video Player"), .icon_name = APP_ID, .legal = video_legal };

/* A track's language, its ISO 639 code (-1 or -2, as films name them: "de", "deu", "ger"), by its name in the screen's
 * language, from ICU: "Deutsch" on a German screen, "German" on an English one. Its first letter is a capital, as the
 * name stands alone in the list (CLDR writes most in lower case: "allemand"). NULL for none and "und" (undetermined);
 * a code ICU does not know as it is. */
static char *
language_name (const char *code)
{
  if (!code || !*code || !g_ascii_strcasecmp (code, "und"))
    return NULL;
  UErrorCode status = U_ZERO_ERROR;
  UChar name[128];
  int32_t length = uloc_getDisplayLanguage (code, NULL, name, G_N_ELEMENTS (name), &status);
  if (U_FAILURE (status) || status == U_USING_DEFAULT_WARNING || status == U_STRING_NOT_TERMINATED_WARNING ||
      length <= 0)
    return g_strdup (code);
  char text[512];
  u_strToUTF8 (text, sizeof text, NULL, name, length, &status);
  if (U_FAILURE (status) || status == U_STRING_NOT_TERMINATED_WARNING)
    return g_strdup (code);
  UCaseMap *map = ucasemap_open (NULL, U_TITLECASE_WHOLE_STRING | U_TITLECASE_NO_LOWERCASE, &status);
  char title[512];
  int32_t n = U_SUCCESS (status) ? ucasemap_utf8ToTitle (map, title, sizeof title, text, -1, &status) : 0;
  ucasemap_close (map);
  if (U_FAILURE (status) || status == U_STRING_NOT_TERMINATED_WARNING || n <= 0)
    return g_strdup (text);
  return g_strndup (title, n);
}

typedef struct {
  gint64 id;
  char *name, *details;
  gboolean selected;
} Track;

static void
track_clear (gpointer data)
{
  Track *t = data;
  g_free (t->name);
  g_free (t->details);
}

static const mpv_node *
node_get (const mpv_node *map, const char *key)
{
  if (map->format != MPV_FORMAT_NODE_MAP)
    return NULL;
  for (int i = 0; i < map->u.list->num; i++)
    if (g_str_equal (map->u.list->keys[i], key))
      return &map->u.list->values[i];
  return NULL;
}

static const char *
node_string (const mpv_node *map, const char *key)
{
  const mpv_node *n = node_get (map, key);
  return n && n->format == MPV_FORMAT_STRING ? n->u.string : NULL;
}

static gint64
node_int (const mpv_node *map, const char *key)
{
  const mpv_node *n = node_get (map, key);
  return n && n->format == MPV_FORMAT_INT64 ? n->u.int64 : 0;
}

static gboolean
node_flag (const mpv_node *map, const char *key)
{
  const mpv_node *n = node_get (map, key);
  return n && n->format == MPV_FORMAT_FLAG && n->u.flag;
}

/* mpv's codec name as players show it: the usual name of the common subtitle and sound formats, else in capitals */
static char *
codec_label (const char *codec)
{
  static const struct { const char *mpv, *label; } names[] = {
    { "subrip", "SRT" }, { "webvtt", "WebVTT" }, { "hdmv_pgs_subtitle", "PGS" }, { "dvd_subtitle", "VobSub" },
    { "dvb_subtitle", "DVB" }, { "mov_text", N_("Timed text") }, { "eac3", "E-AC-3" }, { "ac3", "AC-3" },
    { "truehd", "TrueHD" }, { "vorbis", "Vorbis" }, { "opus", "Opus" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (!g_strcmp0 (codec, names[i].mpv))
      return g_strdup (_(names[i].label));
  return g_ascii_strup (codec, -1);
}

/* The film's tracks of `type` ("audio", "sub"), as the sheet lists them: the track's own title, else its language,
 * else "Track n"; under it the language (when the title is the name), the codec and, for sound, the channels */
static GArray *
tracks_of (Player *p, const char *type)
{
  GArray *tracks = g_array_new (FALSE, TRUE, sizeof (Track));
  g_array_set_clear_func (tracks, track_clear);
  mpv_node list;
  if (!p->mpv || mpv_get_property (p->mpv, "track-list", MPV_FORMAT_NODE, &list) < 0)
    return tracks;
  if (list.format == MPV_FORMAT_NODE_ARRAY) {
    for (int i = 0; i < list.u.list->num; i++) {
      const mpv_node *t = &list.u.list->values[i];
      if (g_strcmp0 (node_string (t, "type"), type))
        continue;
      const char *title = node_string (t, "title");
      g_autofree char *lang = language_name (node_string (t, "lang"));
      const char *codec = node_string (t, "codec");
      gint64 channels = node_int (t, "demux-channel-count");
      GPtrArray *parts = g_ptr_array_new_with_free_func (g_free);
      Track track = { .id = node_int (t, "id"), .selected = node_flag (t, "selected") };
      if (title && *title) {
        track.name = g_strdup (title);
        if (lang)
          g_ptr_array_add (parts, g_strdup (lang));
      } else if (lang) {
        track.name = g_strdup (lang);
      } else {
        /* TRANSLATORS: a track with neither a title nor a language; %u is its number */
        track.name = g_strdup_printf (_("Track %u"), tracks->len + 1);
      }
      if (codec && *codec)
        g_ptr_array_add (parts, codec_label (codec));
      if (channels == 1)
        g_ptr_array_add (parts, g_strdup (_("Mono")));
      else if (channels == 2)
        g_ptr_array_add (parts, g_strdup (_("Stereo")));
      else if (channels > 2)
        g_ptr_array_add (parts, g_strdup_printf ("%" G_GINT64_FORMAT ".1", channels - 1));
      if (node_flag (t, "external"))
        /* TRANSLATORS: a subtitle track read from a file of its own, next to the video */
        g_ptr_array_add (parts, g_strdup (_("From a file")));
      g_ptr_array_add (parts, NULL);
      track.details = g_strjoinv (" · ", (char **) parts->pdata);
      g_ptr_array_unref (parts);
      g_array_append_val (tracks, track);
    }
  }
  mpv_free_node_contents (&list);
  return tracks;
}

typedef struct {
  Player *p;
  const char *property;   /* "aid" or "sid" */
  gint64 id;              /* 0: off */
} Choice;

static void
on_track_chosen (GtkWidget *row, gpointer data)
{
  Choice *choice = data;
  char id[32];
  if (choice->id)
    g_snprintf (id, sizeof id, "%" G_GINT64_FORMAT, choice->id);
  else
    g_strlcpy (id, "no", sizeof id);
  const char *args[] = {"set", choice->property, id, NULL};
  command (choice->p, args);
  GtkWidget *sheet = gtk_widget_get_ancestor (row, VIBEOS_TYPE_SHEET);
  if (sheet)
    vibeos_sheet_close (VIBEOS_SHEET (sheet));
}

static void
add_choice (Player *p, GtkWidget *list, const char *property, gint64 id, const char *name, const char *details,
            gboolean selected)
{
  GtkWidget *row = vibeos_list_row_new (name, details && *details ? details : NULL);
  Choice *choice = g_new0 (Choice, 1);
  *choice = (Choice) { p, property, id };
  g_object_set_data_full (G_OBJECT (row), "choice", choice, g_free);
  if (selected)
    vibeos_list_row_set_end_icon (VIBEOS_LIST_ROW (row), "object-select-symbolic");
  vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), TRUE);
  g_signal_connect (row, "clicked", G_CALLBACK (on_track_chosen), choice);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), row);
}

/* a sheet of the film's tracks of `type`; the subtitles' starts with Off */
static void
choose_track (Player *p, const char *title, const char *type, const char *property)
{
  g_autoptr (GArray) tracks = tracks_of (p, type);
  gboolean sub = g_str_equal (type, "sub"), any = FALSE;
  GtkWidget *list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 0, 16);
  for (guint i = 0; i < tracks->len; i++)
    any |= g_array_index (tracks, Track, i).selected;
  if (sub)
    add_choice (p, list, property, 0, C_("no track", "Off"), NULL, !any);
  for (guint i = 0; i < tracks->len; i++) {
    Track *t = &g_array_index (tracks, Track, i);
    add_choice (p, list, property, t->id, t->name, t->details, t->selected);
  }
  GtkWidget *sheet = vibeos_sheet_new (title);
  GtkWidget *scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  vibeos_sheet_set_content (VIBEOS_SHEET (sheet), scroll);
  vibeos_sheet_present (VIBEOS_SHEET (sheet), p->win);
}

static void
choose_audio (gpointer data)
{
  choose_track (data, _("Audio track"), "audio", "aid");
}

static void
choose_subtitles (gpointer data)
{
  choose_track (data, _("Subtitles"), "sub", "sid");
}

/* ---- Settings: the black bar at the camera ---- */

#define EXTEND_KEY "cutout-extend-apps"

/* the shell's settings, or NULL where the schema (or the key) is missing: the shell then keeps the bar, and the row
 * is left out */
static GSettings *
shell_settings (void)
{
  static GSettings *settings;
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, "vibeos.Shell", TRUE) : NULL;
  if (!settings && schema && g_settings_schema_has_key (schema, EXTEND_KEY))
    settings = g_settings_new_full (schema, NULL, NULL);
  return settings;
}

/* whether the shell has this app's full screen under the camera */
static gboolean
extends_under_camera (GSettings *settings)
{
  g_auto (GStrv) apps = g_settings_get_strv (settings, EXTEND_KEY);
  return g_strv_contains ((const char *const *) apps, APP_ID);
}

static void
on_bar_switched (GObject *toggle, GParamSpec *pspec, gpointer data)
{
  GSettings *settings = data;
  gboolean bar = vibeos_switch_get_active (VIBEOS_SWITCH (toggle));
  if (bar != extends_under_camera (settings))
    return;
  g_auto (GStrv) apps = g_settings_get_strv (settings, EXTEND_KEY);
  g_autoptr (GPtrArray) list = g_ptr_array_new ();
  for (char **a = apps; *a; a++)
    if (g_strcmp0 (*a, APP_ID))
      g_ptr_array_add (list, *a);
  if (!bar)
    g_ptr_array_add (list, (gpointer) APP_ID);
  g_ptr_array_add (list, NULL);
  g_settings_set_strv (settings, EXTEND_KEY, (const char *const *) list->pdata);
}

static void
open_settings (gpointer data)
{
  Player *p = data;
  GSettings *settings = shell_settings ();
  if (!settings)
    return;
  GtkWidget *list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 0, 16);
  GtkWidget *row = vibeos_list_row_new (_("Black bar at the camera"),
                                        _("Keep full screen clear of the camera hole instead of playing under it"));
  GtkWidget *toggle = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);
  vibeos_switch_set_active (VIBEOS_SWITCH (toggle), !extends_under_camera (settings));
  g_signal_connect (toggle, "notify::active", G_CALLBACK (on_bar_switched), settings);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), row);
  GtkWidget *sheet = vibeos_sheet_new (_("Settings"));
  vibeos_sheet_set_content (VIBEOS_SHEET (sheet), list);
  vibeos_sheet_present (VIBEOS_SHEET (sheet), p->win);
}

static void
open_menu (Player *p)
{
  g_autoptr (GArray) audio = tracks_of (p, "audio");
  g_autoptr (GArray) subs = tracks_of (p, "sub");
  VibeosMenu *menu = vibeos_menu_new ();
  if (audio->len > 0)
    vibeos_menu_append (menu, _("Audio track"), choose_audio, p);
  if (subs->len > 0)
    vibeos_menu_append (menu, _("Subtitles"), choose_subtitles, p);
  if (shell_settings ())
    vibeos_menu_append (menu, _("Settings"), open_settings, p);
  vibeos_menu_append_about (menu, &video_about);
  vibeos_menu_present (menu, p->more);
  show_controls (p, TRUE);
}

/* the seek bar moved by the finger: mpv goes there (to the keyframe while dragging, exactly once let go) */
static gboolean
on_seek (GtkRange *range, GtkScrollType scroll, double value, Player *p)
{
  if (p->setting_seek)
    return FALSE;
  char to[32];
  g_ascii_dtostr (to, sizeof to, CLAMP (value, 0, p->duration));
  const char *args[] = {"seek", to, p->dragging ? "absolute+keyframes" : "absolute+exact", NULL};
  command (p, args);
  p->time = value;
  sync_time (p);
  show_controls (p, TRUE);
  return FALSE;
}

/* A finger on the seek bar and off it, watched without taking its events (the bar's own drag takes the touch, and a
 * click gesture of ours was cancelled then, never released: the controls thought the finger was still there and
 * never hid again) */
static gboolean
on_seek_event (GtkEventControllerLegacy *legacy, GdkEvent *event, Player *p)
{
  switch (gdk_event_get_event_type (event)) {
  case GDK_BUTTON_PRESS:
  case GDK_TOUCH_BEGIN:
    p->dragging = TRUE;
    show_controls (p, TRUE);
    break;
  case GDK_BUTTON_RELEASE:
  case GDK_TOUCH_END:
  case GDK_TOUCH_CANCEL:
    if (p->dragging) {
      p->dragging = FALSE;
      /* the exact frame where the finger left it */
      on_seek (GTK_RANGE (p->seek), GTK_SCROLL_JUMP, gtk_range_get_value (GTK_RANGE (p->seek)), p);
    }
    break;
  default:
    break;
  }
  return FALSE;
}

/* A tap shows or hides the controls; a second tap soon after on the left or right third skips back or on */
static gboolean
single_tap (gpointer data)
{
  Player *p = data;
  p->tap_id = 0;
  set_controls (p, gtk_widget_has_css_class (p->controls, "hidden"), TRUE);
  return G_SOURCE_REMOVE;
}

static void
on_video_pressed (GtkGestureClick *click, int n, double x, double y, Player *p)
{
  /* A press on one of the controls' buttons is the button's. A button takes the finger only as it lifts, so the press
   * came here as well, and the tap it began hid the controls a moment after the button had done its work (Full
   * screen, play, skip) */
  GtkWidget *hit = gtk_widget_pick (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (click)), x, y,
                                    GTK_PICK_DEFAULT);
  if (hit && gtk_widget_get_ancestor (hit, GTK_TYPE_BUTTON))
    return;
  int w = gtk_widget_get_width (p->area);
  if (n >= 2 && p->tap_id && (x < w / 3.0 || x > 2 * w / 3.0)) {
    g_clear_handle_id (&p->tap_id, g_source_remove);
    skip (p, x < w / 3.0 ? -SKIP_S : SKIP_S);
    return;
  }
  g_clear_handle_id (&p->tap_id, g_source_remove);
  p->tap_x = x;
  p->tap_id = g_timeout_add (DOUBLE_TAP_MS, single_tap, p);
}

/* ---- sliding a finger up and down: the volume (right half) and the brightness (left half) ---- */

static void
level_show (Player *p, gboolean volume, double v)
{
  p->level_value = CLAMP (v, 0, 1);
  g_object_set_data (G_OBJECT (p->level), "volume", GINT_TO_POINTER (volume));
  gtk_accessible_update_property (GTK_ACCESSIBLE (p->level), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  volume ? _("Volume") : _("Brightness"), -1);
  g_clear_handle_id (&p->level_id, g_source_remove);
  gtk_widget_remove_css_class (p->level, "hidden");
  gtk_widget_queue_draw (p->level);
}

static gboolean
level_hide (gpointer data)
{
  Player *p = data;
  p->level_id = 0;
  gtk_widget_add_css_class (p->level, "hidden");
  return G_SOURCE_REMOVE;
}

/* the bar: a dark panel on the side the finger controls, the speaker or the sun over the level, the percent under it */
static void
draw_level (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Player *p = data;
  gboolean volume = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (area), "volume"));
  double pw = 56, ph = CLAMP (h * 0.6, 160, 280), margin = EDGE_DP + 12;
  double px = volume ? w - margin - pw : margin, py = (h - ph) / 2;
  cairo_set_source_rgba (cr, 0, 0, 0, 0.6);
  cairo_new_sub_path (cr);
  cairo_arc (cr, px + pw - 12, py + 12, 12, -G_PI / 2, 0);
  cairo_arc (cr, px + pw - 12, py + ph - 12, 12, 0, G_PI / 2);
  cairo_arc (cr, px + 12, py + ph - 12, 12, G_PI / 2, G_PI);
  cairo_arc (cr, px + 12, py + 12, 12, G_PI, 3 * G_PI / 2);
  cairo_close_path (cr);
  cairo_fill (cr);

  /* the icon on Material's 24-unit grid, at its natural size */
  cairo_save (cr);
  cairo_translate (cr, px + pw / 2 - 12, py + 10);
  cairo_set_source_rgb (cr, 1, 1, 1);
  if (volume) {
    cairo_move_to (cr, 3, 9); cairo_line_to (cr, 3, 15); cairo_line_to (cr, 7, 15); cairo_line_to (cr, 12, 20);
    cairo_line_to (cr, 12, 4); cairo_line_to (cr, 7, 9); cairo_close_path (cr);
    cairo_fill (cr);
    cairo_set_line_width (cr, 2);
    cairo_arc (cr, 12, 12, 5, -G_PI / 3, G_PI / 3);
    cairo_stroke (cr);
    if (p->level_value > 0.5) {
      cairo_arc (cr, 12, 12, 9, -G_PI / 3, G_PI / 3);
      cairo_stroke (cr);
    }
  } else {
    cairo_arc (cr, 12, 12, 4.5, 0, 2 * G_PI);
    cairo_fill (cr);
    cairo_set_line_width (cr, 2);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 8; i++) {
      double a = i * G_PI / 4;
      cairo_move_to (cr, 12 + 8 * cos (a), 12 + 8 * sin (a));
      cairo_line_to (cr, 12 + 10.5 * cos (a), 12 + 10.5 * sin (a));
      cairo_stroke (cr);
    }
  }
  cairo_restore (cr);

  /* the percent */
  char text[16];
  g_snprintf (text, sizeof text, "%d", (int) round (p->level_value * 100));
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (area), text);
  int tw, th;
  pango_layout_get_pixel_size (layout, &tw, &th);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_move_to (cr, px + (pw - tw) / 2.0, py + ph - th - 8);
  pango_cairo_show_layout (cr, layout);
  g_object_unref (layout);

  /* the level between them */
  double bx = px + pw / 2 - 2, by = py + 44, bh = ph - 44 - th - 20;
  cairo_set_source_rgba (cr, 1, 1, 1, 0.3);
  cairo_rectangle (cr, bx, by, 4, bh);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, bx, by + bh * (1 - p->level_value), 4, bh * p->level_value);
  cairo_fill (cr);
}

static void volume_set_done (pa_context *c, int success, void *data);
static void light_set_done (GObject *source, GAsyncResult *res, gpointer data);

/* -- the volume: the default sink, as the volume keys and the shell's slider have it -- */

static void
volume_apply (Player *p)
{
  if (!p->pa_up || !p->sink_known || p->vol_busy || p->vol_want == p->vol_sent)
    return;
  pa_cvolume v = p->sink_volume;
  pa_cvolume_scale (&v, (pa_volume_t) round (p->vol_want * PA_VOLUME_NORM));
  p->vol_sent = p->vol_want;
  if (p->sink_muted && p->vol_want > 0) {
    pa_operation *o = pa_context_set_sink_mute_by_name (p->pa, "@DEFAULT_SINK@", 0, NULL, NULL);
    if (o)
      pa_operation_unref (o);
    p->sink_muted = FALSE;
  }
  pa_operation *o = pa_context_set_sink_volume_by_name (p->pa, "@DEFAULT_SINK@", &v, volume_set_done, p);
  if (o) {
    p->vol_busy = TRUE;
    pa_operation_unref (o);
  }
}

static void
volume_set_done (pa_context *c, int success, void *data)
{
  Player *p = data;
  p->vol_busy = FALSE;
  volume_apply (p);
}

static void
sink_read (pa_context *c, const pa_sink_info *i, int eol, void *data)
{
  Player *p = data;
  if (eol || !i || p->sink_known || p->drag_state != 1 || !p->drag_volume || i->volume.channels == 0)
    return;
  p->sink_volume = i->volume;
  p->sink_muted = i->mute;
  p->sink_known = TRUE;
  p->drag_base = i->mute ? 0 : pa_cvolume_max (&i->volume) / (double) PA_VOLUME_NORM;
  p->vol_want = p->vol_sent = -1;
  /* the finger's travel counts from where it was as the level was read */
  level_show (p, TRUE, p->drag_base);
}

static void
pa_state_changed (pa_context *c, void *data)
{
  Player *p = data;
  pa_context_state_t s = pa_context_get_state (c);
  p->pa_up = s == PA_CONTEXT_READY;
}

static void
pa_start (Player *p)
{
  p->pa_loop = pa_glib_mainloop_new (NULL);
  pa_proplist *props = pa_proplist_new ();
  pa_proplist_sets (props, PA_PROP_APPLICATION_ID, APP_ID);
  p->pa = pa_context_new_with_proplist (pa_glib_mainloop_get_api (p->pa_loop), _("Video Player"), props);
  pa_proplist_free (props);
  pa_context_set_state_callback (p->pa, pa_state_changed, p);
  /* never waits for the server: without one, the drag does nothing */
  pa_context_connect (p->pa, NULL, PA_CONTEXT_NOFAIL | PA_CONTEXT_NOAUTOSPAWN, NULL);
}

static void
pa_stop (Player *p)
{
  if (p->pa) {
    pa_context_set_state_callback (p->pa, NULL, NULL);
    pa_context_disconnect (p->pa);
    pa_context_unref (p->pa);
    p->pa = NULL;
  }
  if (p->pa_loop) {
    pa_glib_mainloop_free (p->pa_loop);
    p->pa_loop = NULL;
  }
  p->pa_up = FALSE;
}

/* -- the brightness: the shell's, which the shade's slider shows and sets (and keeps) -- */

#define SHELL_NAME "vibeos.Shell"
#define SHELL_PATH "/vibeos/Shell"

static void
light_apply (Player *p)
{
  GDBusConnection *bus = g_application_get_dbus_connection (G_APPLICATION (p->app));
  if (!bus || !p->light_known || p->light_busy || p->light_want == p->light_sent)
    return;
  p->light_sent = p->light_want;
  p->light_busy = TRUE;
  g_dbus_connection_call (bus, SHELL_NAME, SHELL_PATH, "org.freedesktop.DBus.Properties", "Set",
                          g_variant_new ("(ssv)", SHELL_NAME, "Brightness", g_variant_new_double (p->light_want)),
                          NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, light_set_done, p);
}

static void
light_set_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Player *p = data;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, NULL);
  p->light_busy = FALSE;
  light_apply (p);
}

static void
light_read_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Player *p = data;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, NULL);
  if (!r || p->light_known || p->drag_state != 1 || p->drag_volume)
    return;
  g_autoptr (GVariant) v = NULL;
  g_variant_get (r, "(v)", &v);
  double level = g_variant_is_of_type (v, G_VARIANT_TYPE_DOUBLE) ? g_variant_get_double (v) : -1;
  /* below 0: no backlight the shell drives */
  if (level < 0)
    return;
  p->light_known = TRUE;
  p->drag_base = level;
  p->light_want = p->light_sent = -1;
  level_show (p, FALSE, level);
}

/* -- the drag itself -- */

/* The finger went down: where it is the app's own is decided here (the system's edge strips, the buttons and the seek
 * bar are not); whether it is a slide up or down, on the first movement past the slop */
static void
on_level_begin (GtkGestureDrag *drag, double x, double y, Player *p)
{
  GtkWidget *overlay = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (drag));
  int w = gtk_widget_get_width (overlay), h = gtk_widget_get_height (overlay);
  p->drag_state = 0;
  if (p->dragging || x < EDGE_DP || x > w - EDGE_DP || y > h - NAV_DP)
    return;
  GtkWidget *hit = gtk_widget_pick (overlay, x, y, GTK_PICK_DEFAULT);
  if (hit && (gtk_widget_get_ancestor (hit, GTK_TYPE_BUTTON) || gtk_widget_get_ancestor (hit, GTK_TYPE_RANGE)))
    return;
  p->drag_x = x;
  p->drag_y = y;
  p->drag_state = -1;
}

static void
on_level_update (GtkGestureDrag *drag, double dx, double dy, Player *p)
{
  GtkWidget *overlay = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (drag));
  int h = gtk_widget_get_height (overlay);
  if (p->drag_state == 0)
    return;
  if (p->drag_state == -1) {
    int slop = 8;
    g_object_get (gtk_widget_get_settings (overlay), "gtk-dnd-drag-threshold", &slop, NULL);
    if (hypot (dx, dy) <= slop)
      return;
    if (fabs (dy) <= fabs (dx)) {
      p->drag_state = 0;
      return;
    }
    p->drag_state = 1;
    p->drag_volume = p->drag_x > gtk_widget_get_width (overlay) / 2.0;
    p->drag_from = dy;
    p->sink_known = p->light_known = FALSE;
    /* not a tap: the press already began one */
    g_clear_handle_id (&p->tap_id, g_source_remove);
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    p->drag_base = -1;
    /* the level it begins from, read as the slide begins */
    if (p->drag_volume) {
      if (p->pa_up) {
        pa_operation *o = pa_context_get_sink_info_by_name (p->pa, "@DEFAULT_SINK@", sink_read, p);
        if (o)
          pa_operation_unref (o);
      }
    } else {
      GDBusConnection *bus = g_application_get_dbus_connection (G_APPLICATION (p->app));
      if (bus)
        g_dbus_connection_call (bus, SHELL_NAME, SHELL_PATH, "org.freedesktop.DBus.Properties", "Get",
                                g_variant_new ("(ss)", SHELL_NAME, "Brightness"), G_VARIANT_TYPE ("(v)"),
                                G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, light_read_done, p);
    }
    return;
  }
  if (p->drag_base < 0)
    return;
  /* the whole height of the picture is the whole range, up is more */
  double v = p->drag_base + (p->drag_from - dy) / MAX (h, 1);
  if (p->drag_volume) {
    p->vol_want = CLAMP (v, 0, 1);
    level_show (p, TRUE, p->vol_want);
    volume_apply (p);
  } else {
    p->light_want = CLAMP (v, MIN_BRIGHTNESS, 1);
    level_show (p, FALSE, p->light_want);
    light_apply (p);
  }
}

static void
on_level_end (GtkGestureDrag *drag, double dx, double dy, Player *p)
{
  if (p->drag_state == 1) {
    /* the last level, if one is still waiting for its turn */
    if (p->drag_volume)
      volume_apply (p);
    else
      light_apply (p);
    /* the bar stays a moment after the finger is up */
    g_clear_handle_id (&p->level_id, g_source_remove);
    if (!gtk_widget_has_css_class (p->level, "hidden"))
      p->level_id = g_timeout_add (LEVEL_HIDE_MS, level_hide, p);
  }
  p->drag_state = 0;
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, Player *p)
{
  switch (keyval) {
  case GDK_KEY_space:
  case GDK_KEY_k:
    toggle_pause (p);
    return TRUE;
  case GDK_KEY_Left:
    skip (p, -SKIP_S);
    return TRUE;
  case GDK_KEY_Right:
    skip (p, SKIP_S);
    return TRUE;
  case GDK_KEY_f:
  case GDK_KEY_F11:
    toggle_fullscreen (p);
    return TRUE;
  case GDK_KEY_Escape:
    go_back (p);
    return TRUE;
  default:
    return FALSE;
  }
}

/* ---- the window ---- */

static const char CSS[] =
  "window.video, window.video .video-bg { background-color: black; color: white; }"
  ".video-controls { transition: opacity 200ms ease-out; }"
  ".video-controls.hidden { opacity: 0; }"
  ".video-top { background-image: linear-gradient(to bottom, alpha(black, 0.6), transparent); padding: 8dp 0 32dp 4dp; }"
  ".video-bottom { background-image: linear-gradient(to top, alpha(black, 0.6), transparent); padding: 32dp 16dp 12dp 16dp; }"
  ".video-title { font-size: 18sp; font-weight: 500; }"
  ".video-notice { font-size: 14sp; color: alpha(white, 0.8); }"
  ".video-time { font-size: 14sp; font-feature-settings: 'tnum'; }"
  ".video-icon { background: none; border: none; box-shadow: none; padding: 0; min-width: 0; min-height: 0; }"
  ".video-level { font-size: 14sp; transition: opacity 300ms ease-out; }"
  ".video-level.hidden { opacity: 0; }"
  ".video-message { font-size: 16sp; background-color: alpha(black, 0.6); border-radius: 12dp; padding: 12dp 20dp; }"
  ".video-seek trough { min-height: 4dp; border-radius: 2dp; background-color: alpha(white, 0.3); }"
  ".video-seek highlight { background-color: white; border-radius: 2dp; }"
  ".video-seek slider { min-width: 16dp; min-height: 16dp; margin: -6dp; background-color: white; border-radius: 50%; box-shadow: none; border: none; }";

/* Covered by another app (the compositor suspends a window nothing of it shows), the film pauses, as Android's
 * player pauses when it leaves the screen; it stays paused when it comes back */
static void
on_suspended (GtkWindow *win, GParamSpec *pspec, Player *p)
{
  if (gtk_window_is_suspended (win) && p->have_file && !p->paused) {
    const char *args[] = {"set", "pause", "yes", NULL};
    command (p, args);
  }
}

static GtkWidget *
time_label (void)
{
  GtkWidget *label = gtk_label_new ("0:00");
  gtk_widget_add_css_class (label, "video-time");
  return label;
}

static void
build (Player *p)
{
  p->win = vibeos_window_new (p->app);
  gtk_window_set_title (GTK_WINDOW (p->win), _("Video Player"));
  gtk_widget_add_css_class (p->win, "video");

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_widget_add_css_class (overlay, "video-bg");
  p->area = gtk_gl_area_new ();
  gtk_gl_area_set_auto_render (GTK_GL_AREA (p->area), FALSE);
  gtk_widget_set_hexpand (p->area, TRUE);
  gtk_widget_set_vexpand (p->area, TRUE);
  g_signal_connect (p->area, "realize", G_CALLBACK (on_realize), p);
  g_signal_connect (p->area, "unrealize", G_CALLBACK (on_unrealize), p);
  g_signal_connect (p->area, "render", G_CALLBACK (on_render), p);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), p->area);

  GtkGesture *slide = gtk_gesture_drag_new ();
  g_signal_connect (slide, "drag-begin", G_CALLBACK (on_level_begin), p);
  g_signal_connect (slide, "drag-update", G_CALLBACK (on_level_update), p);
  g_signal_connect (slide, "drag-end", G_CALLBACK (on_level_end), p);
  gtk_widget_add_controller (overlay, GTK_EVENT_CONTROLLER (slide));

  GtkGesture *tap = gtk_gesture_click_new ();
  g_signal_connect (tap, "pressed", G_CALLBACK (on_video_pressed), p);
  gtk_widget_add_controller (overlay, GTK_EVENT_CONTROLLER (tap));

  /* the level bar of a volume or brightness slide: over everything, never taking a touch, hidden until a slide */
  p->level = gtk_drawing_area_new ();
  gtk_widget_add_css_class (p->level, "video-level");
  gtk_widget_add_css_class (p->level, "hidden");
  gtk_widget_set_can_target (p->level, FALSE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (p->level), draw_level, p, NULL);

  /* the controls: a column of the top bar, the middle row and the bottom bar */
  p->controls = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->controls, "video-controls");

  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (top, "video-top");
  gtk_box_append (GTK_BOX (top), icon_button (p, ICON_BACK, TOP_BUTTON_DP, _("Back"), G_CALLBACK (go_back)));
  p->title = gtk_label_new ("");
  gtk_widget_add_css_class (p->title, "video-title");
  gtk_label_set_ellipsize (GTK_LABEL (p->title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (p->title), 0);
  gtk_widget_set_hexpand (p->title, TRUE);
  gtk_box_append (GTK_BOX (top), p->title);
  /* TRANSLATORS: shown in the video player's top bar while a film plays without sound because the device has no sound
     output (no speaker, headphones or Bluetooth device that takes it) */
  p->notice = gtk_label_new (_("No audio output"));
  gtk_widget_add_css_class (p->notice, "video-notice");
  gtk_widget_set_visible (p->notice, FALSE);
  gtk_box_append (GTK_BOX (top), p->notice);
  p->full = icon_button (p, ICON_FULLSCREEN, TOP_BUTTON_DP, _("Full screen"), G_CALLBACK (toggle_fullscreen));
  gtk_box_append (GTK_BOX (top), p->full);
  p->more = icon_button (p, ICON_MORE, TOP_BUTTON_DP, _("More options"), G_CALLBACK (open_menu));
  /* where every top bar has its ⋮ (vibeos-top-bar.h) */
  gtk_widget_add_css_class (p->more, "vibeos-top-bar-end");
  gtk_box_append (GTK_BOX (top), p->more);
  gtk_box_append (GTK_BOX (p->controls), top);

  GtkWidget *middle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 48);
  gtk_widget_set_halign (middle, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (middle, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (middle, TRUE);
  GtkWidget *replay_button = icon_button (p, ICON_REPLAY, SIDE_BUTTON_DP, _("Back 10 seconds"), G_CALLBACK (replay));
  gtk_widget_set_valign (replay_button, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (middle), replay_button);
  p->play = icon_button (p, ICON_PLAY, CENTER_BUTTON_DP, _("Play or pause"), G_CALLBACK (toggle_pause));
  gtk_box_append (GTK_BOX (middle), p->play);
  GtkWidget *forward_button = icon_button (p, ICON_FORWARD, SIDE_BUTTON_DP, _("On 10 seconds"), G_CALLBACK (forward));
  gtk_widget_set_valign (forward_button, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (middle), forward_button);
  gtk_box_append (GTK_BOX (p->controls), middle);

  GtkWidget *bottom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (bottom, "video-bottom");
  p->position = time_label ();
  p->length = time_label ();
  p->seek = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
  gtk_scale_set_draw_value (GTK_SCALE (p->seek), FALSE);
  gtk_widget_add_css_class (p->seek, "video-seek");
  gtk_widget_set_hexpand (p->seek, TRUE);
  g_signal_connect (p->seek, "change-value", G_CALLBACK (on_seek), p);
  GtkEventController *finger = gtk_event_controller_legacy_new ();
  gtk_event_controller_set_propagation_phase (finger, GTK_PHASE_CAPTURE);
  g_signal_connect (finger, "event", G_CALLBACK (on_seek_event), p);
  gtk_widget_add_controller (p->seek, finger);
  gtk_box_append (GTK_BOX (bottom), p->position);
  gtk_box_append (GTK_BOX (bottom), p->seek);
  gtk_box_append (GTK_BOX (bottom), p->length);
  gtk_box_append (GTK_BOX (p->controls), bottom);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), p->controls);

  p->spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (p->spinner, 48, 48);
  gtk_widget_set_halign (p->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (p->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target (p->spinner, FALSE);
  gtk_widget_set_visible (p->spinner, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), p->spinner);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), p->level);

  p->message = gtk_label_new (_("Open a video from Files"));
  gtk_widget_add_css_class (p->message, "video-message");
  /* a message wider than the screen (Russian's, at a large font size) goes on to more lines, centred */
  gtk_label_set_wrap (GTK_LABEL (p->message), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (p->message), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_justify (GTK_LABEL (p->message), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_start (p->message, 24);
  gtk_widget_set_margin_end (p->message, 24);
  gtk_widget_set_halign (p->message, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (p->message, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (p->message, 120);
  gtk_widget_set_can_target (p->message, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), p->message);

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), p);
  gtk_widget_add_controller (p->win, keys);

  vibeos_window_set_content (GTK_WINDOW (p->win), overlay);
  g_signal_connect (p->win, "notify::suspended", G_CALLBACK (on_suspended), p);
  g_signal_connect (p->win, "notify::fullscreened", G_CALLBACK (on_fullscreen), p);
  g_signal_connect (p->win, "realize", G_CALLBACK (on_window_realize), p);
  g_signal_connect (p->win, "unrealize", G_CALLBACK (on_window_unrealize), p);
  show_controls (p, TRUE);
}

static void play_file (Player *p, GFile *file, gboolean mounted);

static void
on_mounted (GObject *source, GAsyncResult *result, gpointer data)
{
  Player *p = data;
  g_autoptr (GError) error = NULL;
  if (!g_file_mount_enclosing_volume_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED))
    g_warning ("%s", error->message);
  if (p->win)
    play_file (p, G_FILE (source), TRUE);
  g_object_unref (p->app);
}

/* A file to play: a share's through GVfs's FUSE path (the share mounted first where it is not yet: after a restart
 * nothing has opened it), a web address as it is */
static void
play_file (Player *p, GFile *file, gboolean mounted)
{
  g_autofree char *path = g_file_get_path (file);
  g_autofree char *uri = g_file_get_uri (file);
  const char *what = path ? path : uri;
  gboolean web = g_str_has_prefix (uri, "http://") || g_str_has_prefix (uri, "https://");
  if (!path && !web && !mounted) {
    g_autoptr (GMountOperation) op = gtk_mount_operation_new (GTK_WINDOW (p->win));
    gtk_label_set_text (GTK_LABEL (p->message), _("Opening…"));
    gtk_widget_set_visible (p->message, TRUE);
    g_file_mount_enclosing_volume (file, G_MOUNT_MOUNT_NONE, op, NULL, on_mounted, g_object_ref (p->app));
    return;
  }
  if (!path && !web) {
    gtk_label_set_text (GTK_LABEL (p->message), _("Can’t reach this video"));
    gtk_widget_set_visible (p->message, TRUE);
    return;
  }
  p->have_file = TRUE;
  p->ended = FALSE;
  g_autofree char *name = g_file_get_basename (file);
  p->named = !web;
  gtk_label_set_text (GTK_LABEL (p->title), name);
  gtk_window_set_title (GTK_WINDOW (p->win), name);
  gtk_widget_set_visible (p->message, FALSE);
  /* no output: silent from the start; one there again: sound again */
  if (p->outputs == 0)
    set_silent (p, TRUE);
  else if (p->outputs == 1)
    set_silent (p, FALSE);
  g_free (p->playing);
  p->playing = g_strdup (what);
  const char *args[] = {"loadfile", what, NULL};
  command (p, args);
  show_controls (p, TRUE);
  mpris_sync (p);
}

static void
ensure_window (Player *p)
{
  if (p->win)
    return;
  build (p);
  if (!start_mpv (p)) {
    gtk_label_set_text (GTK_LABEL (p->message), _("The player could not start"));
    gtk_widget_set_visible (p->message, TRUE);
  }
  /* on a screen wider than tall it opens full screen (asked before it shows, so that it shows so at once) */
  p->landscape = screen_landscape (p);
  if (p->landscape == 1)
    gtk_window_fullscreen (GTK_WINDOW (p->win));
  gtk_window_present (GTK_WINDOW (p->win));
}

/* ---- MPRIS: org.mpris.MediaPlayer2 and its Player on /org/mpris/MediaPlayer2 ---- */

#define MPRIS_NAME "org.mpris.MediaPlayer2.vibeos.Video"
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_IFACE "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define MPRIS_TRACK "/vibeos/Video/Track"
#define MPRIS_NO_TRACK "/org/mpris/MediaPlayer2/TrackList/NoTrack"

static const char MPRIS_XML[] =
  "<node>"
  " <interface name='org.mpris.MediaPlayer2'>"
  "  <method name='Raise'/>"
  "  <method name='Quit'/>"
  "  <property name='CanQuit' type='b' access='read'/>"
  "  <property name='CanRaise' type='b' access='read'/>"
  "  <property name='HasTrackList' type='b' access='read'/>"
  "  <property name='Identity' type='s' access='read'/>"
  "  <property name='DesktopEntry' type='s' access='read'/>"
  "  <property name='SupportedUriSchemes' type='as' access='read'/>"
  "  <property name='SupportedMimeTypes' type='as' access='read'/>"
  " </interface>"
  " <interface name='org.mpris.MediaPlayer2.Player'>"
  "  <method name='Next'/>"
  "  <method name='Previous'/>"
  "  <method name='Pause'/>"
  "  <method name='PlayPause'/>"
  "  <method name='Stop'/>"
  "  <method name='Play'/>"
  "  <method name='Seek'><arg name='Offset' type='x' direction='in'/></method>"
  "  <method name='SetPosition'>"
  "   <arg name='TrackId' type='o' direction='in'/><arg name='Position' type='x' direction='in'/>"
  "  </method>"
  "  <method name='OpenUri'><arg name='Uri' type='s' direction='in'/></method>"
  "  <signal name='Seeked'><arg name='Position' type='x'/></signal>"
  "  <property name='PlaybackStatus' type='s' access='read'/>"
  "  <property name='Rate' type='d' access='read'/>"
  "  <property name='Metadata' type='a{sv}' access='read'/>"
  "  <property name='Volume' type='d' access='read'/>"
  "  <property name='Position' type='x' access='read'/>"
  "  <property name='MinimumRate' type='d' access='read'/>"
  "  <property name='MaximumRate' type='d' access='read'/>"
  "  <property name='CanGoNext' type='b' access='read'/>"
  "  <property name='CanGoPrevious' type='b' access='read'/>"
  "  <property name='CanPlay' type='b' access='read'/>"
  "  <property name='CanPause' type='b' access='read'/>"
  "  <property name='CanSeek' type='b' access='read'/>"
  "  <property name='CanControl' type='b' access='read'/>"
  " </interface>"
  "</node>";

static const char *
mpris_status (Player *p)
{
  if (!p->have_file)
    return "Stopped";
  return p->paused || p->ended ? "Paused" : "Playing";
}

static const char *
mpris_title (Player *p)
{
  return p->have_file && p->title ? gtk_label_get_text (GTK_LABEL (p->title)) : "";
}

static gint64
mpris_length (Player *p)
{
  return p->have_file && p->duration > 0 ? (gint64) (p->duration * G_USEC_PER_SEC) : 0;
}

static GVariant *
mpris_metadata (Player *p)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&b, "{sv}", "mpris:trackid",
                         g_variant_new_object_path (p->have_file ? MPRIS_TRACK : MPRIS_NO_TRACK));
  const char *title = mpris_title (p);
  if (*title)
    g_variant_builder_add (&b, "{sv}", "xesam:title", g_variant_new_string (title));
  if (mpris_length (p) > 0)
    g_variant_builder_add (&b, "{sv}", "mpris:length", g_variant_new_int64 (mpris_length (p)));
  return g_variant_builder_end (&b);
}

static GVariant *
mpris_get (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *name,
           GError **error, gpointer data)
{
  Player *p = data;
  static const char *const none[] = {NULL};
  static const char *const schemes[] = {"file", "http", "https", "smb", NULL};
  if (g_str_equal (iface, MPRIS_IFACE)) {
    if (g_str_equal (name, "CanQuit") || g_str_equal (name, "HasTrackList"))
      return g_variant_new_boolean (FALSE);
    if (g_str_equal (name, "CanRaise"))
      return g_variant_new_boolean (TRUE);
    if (g_str_equal (name, "Identity"))
      return g_variant_new_string ("Video Player");
    if (g_str_equal (name, "DesktopEntry"))
      return g_variant_new_string (APP_ID);
    if (g_str_equal (name, "SupportedUriSchemes"))
      return g_variant_new_strv (schemes, -1);
    if (g_str_equal (name, "SupportedMimeTypes"))
      return g_variant_new_strv (none, -1);
  } else {
    if (g_str_equal (name, "PlaybackStatus"))
      return g_variant_new_string (mpris_status (p));
    if (g_str_equal (name, "Metadata"))
      return mpris_metadata (p);
    if (g_str_equal (name, "Position"))
      return g_variant_new_int64 (p->have_file && p->time > 0 ? (gint64) (p->time * G_USEC_PER_SEC) : 0);
    if (g_str_equal (name, "Rate") || g_str_equal (name, "Volume") || g_str_equal (name, "MinimumRate") ||
        g_str_equal (name, "MaximumRate"))
      return g_variant_new_double (1.0);
    if (g_str_equal (name, "CanGoNext") || g_str_equal (name, "CanGoPrevious"))
      return g_variant_new_boolean (FALSE);
    if (g_str_equal (name, "CanControl"))
      return g_variant_new_boolean (TRUE);
    if (g_str_equal (name, "CanPlay") || g_str_equal (name, "CanPause") || g_str_equal (name, "CanSeek"))
      return g_variant_new_boolean (p->have_file);
  }
  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "No property %s", name);
  return NULL;
}

static void
mpris_seek_to (Player *p, double seconds, const char *how)
{
  char at[32];
  g_ascii_dtostr (at, sizeof at, seconds);
  const char *args[] = {"seek", at, how, NULL};
  command (p, args);
}

static void
mpris_call (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *method,
            GVariant *params, GDBusMethodInvocation *invocation, gpointer data)
{
  Player *p = data;
  if (g_str_equal (method, "Raise")) {
    ensure_window (p);
    gtk_window_present (GTK_WINDOW (p->win));
  } else if (g_str_equal (method, "Pause") || g_str_equal (method, "Stop")) {
    if (p->have_file && !p->paused) {
      const char *args[] = {"set", "pause", "yes", NULL};
      command (p, args);
    }
  } else if (g_str_equal (method, "Play")) {
    if (p->have_file && (p->paused || p->ended))
      toggle_pause (p);
  } else if (g_str_equal (method, "PlayPause")) {
    if (p->have_file)
      toggle_pause (p);
  } else if (g_str_equal (method, "Seek")) {
    gint64 offset;
    g_variant_get (params, "(x)", &offset);
    if (p->have_file)
      mpris_seek_to (p, (double) offset / G_USEC_PER_SEC, "relative+exact");
  } else if (g_str_equal (method, "SetPosition")) {
    const char *track;
    gint64 at;
    g_variant_get (params, "(&ox)", &track, &at);
    if (p->have_file && g_str_equal (track, MPRIS_TRACK) && at >= 0 && (p->duration <= 0 || at <= mpris_length (p)))
      mpris_seek_to (p, (double) at / G_USEC_PER_SEC, "absolute+exact");
  } else if (g_str_equal (method, "OpenUri")) {
    const char *uri;
    g_variant_get (params, "(&s)", &uri);
    g_autoptr (GFile) file = g_file_new_for_uri (uri);
    g_application_open (G_APPLICATION (p->app), &file, 1, "");
  }
  /* Next, Previous, Quit: nothing (CanGoNext, CanGoPrevious and CanQuit say so) */
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable mpris_vtable = {mpris_call, mpris_get, NULL};

/* what has changed since the last time, to the bus */
static void
mpris_sync (Player *p)
{
  if (!p->mpris_player)
    return;
  const char *status = mpris_status (p);
  const char *title = mpris_title (p);
  gint64 length = mpris_length (p);
  gboolean status_changed = g_strcmp0 (status, p->mpris_status) != 0;
  gboolean meta_changed = g_strcmp0 (title, p->mpris_title) != 0 || length != p->mpris_length;
  if (!status_changed && !meta_changed)
    return;
  GVariantBuilder changed;
  g_variant_builder_init (&changed, G_VARIANT_TYPE_VARDICT);
  if (status_changed) {
    p->mpris_status = status;
    g_variant_builder_add (&changed, "{sv}", "PlaybackStatus", g_variant_new_string (status));
    g_variant_builder_add (&changed, "{sv}", "CanPlay", g_variant_new_boolean (p->have_file));
    g_variant_builder_add (&changed, "{sv}", "CanPause", g_variant_new_boolean (p->have_file));
    g_variant_builder_add (&changed, "{sv}", "CanSeek", g_variant_new_boolean (p->have_file));
  }
  if (meta_changed) {
    g_free (p->mpris_title);
    p->mpris_title = g_strdup (title);
    p->mpris_length = length;
    g_variant_builder_add (&changed, "{sv}", "Metadata", mpris_metadata (p));
  }
  g_dbus_connection_emit_signal (p->bus, NULL, MPRIS_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 g_variant_new ("(sa{sv}@as)", MPRIS_PLAYER_IFACE, &changed,
                                                g_variant_new_strv (NULL, 0)), NULL);
}

static void
mpris_start (Player *p)
{
  GDBusConnection *bus = g_application_get_dbus_connection (G_APPLICATION (p->app));
  if (!bus)
    return;
  g_autoptr (GError) error = NULL;
  p->mpris_info = g_dbus_node_info_new_for_xml (MPRIS_XML, &error);
  if (!p->mpris_info) {
    g_warning ("MPRIS: %s", error->message);
    return;
  }
  p->bus = g_object_ref (bus);
  p->mpris_root = g_dbus_connection_register_object (bus, MPRIS_PATH, p->mpris_info->interfaces[0], &mpris_vtable,
                                                     p, NULL, &error);
  if (p->mpris_root)
    p->mpris_player = g_dbus_connection_register_object (bus, MPRIS_PATH, p->mpris_info->interfaces[1],
                                                         &mpris_vtable, p, NULL, &error);
  if (!p->mpris_player) {
    g_warning ("MPRIS: %s", error ? error->message : "not registered");
    return;
  }
  p->mpris_status = mpris_status (p);
  p->mpris_name = g_bus_own_name_on_connection (bus, MPRIS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
}

static void
mpris_stop (Player *p)
{
  if (p->mpris_name)
    g_bus_unown_name (p->mpris_name);
  p->mpris_name = 0;
  if (p->mpris_player)
    g_dbus_connection_unregister_object (p->bus, p->mpris_player);
  if (p->mpris_root)
    g_dbus_connection_unregister_object (p->bus, p->mpris_root);
  p->mpris_player = p->mpris_root = 0;
  g_clear_pointer (&p->mpris_info, g_dbus_node_info_unref);
  g_clear_pointer (&p->mpris_title, g_free);
  g_clear_object (&p->bus);
}

static void
on_open (GApplication *app, GFile **files, int n, const char *hint, Player *p)
{
  ensure_window (p);
  if (n > 0)
    play_file (p, files[0], FALSE);
  gtk_window_present (GTK_WINDOW (p->win));
}

static void
on_activate (GApplication *app, Player *p)
{
  ensure_window (p);
  gtk_window_present (GTK_WINDOW (p->win));
}

static void
on_startup (GApplication *app, Player *p)
{
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  vibeos_css_load_string (CSS, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  mpris_start (p);
  pa_start (p);
}

static void
on_shutdown (GApplication *app, Player *p)
{
  mpris_stop (p);
  pa_stop (p);
  g_clear_handle_id (&p->level_id, g_source_remove);
  if (p->inhibit)
    gtk_application_uninhibit (p->app, p->inhibit);
  p->inhibit = 0;
  g_clear_handle_id (&p->hide_id, g_source_remove);
  g_clear_handle_id (&p->tap_id, g_source_remove);
  g_clear_handle_id (&p->events_id, g_source_remove);
  g_clear_handle_id (&p->shape_id, g_source_remove);
  g_clear_handle_id (&p->stall_id, g_source_remove);
  g_clear_pointer (&p->playing, g_free);
  if (p->mpv) {
    if (g_getenv ("VIBEOS_VIDEO_LOG")) {
      /* (mpv's strings, freed by mpv) */
      char *hwdec = mpv_get_property_string (p->mpv, "hwdec-current");
      char *vo = mpv_get_property_string (p->mpv, "frame-drop-count");
      char *decoder = mpv_get_property_string (p->mpv, "decoder-frame-drop-count");
      g_message ("decoding: %s; frames dropped: %s by the output, %s by the decoder", hwdec, vo, decoder);
      mpv_free (hwdec);
      mpv_free (vo);
      mpv_free (decoder);
    }
    /* where the film was left, for next time */
    if (p->have_file) {
      const char *save[] = {"write-watch-later-config", NULL};
      mpv_command (p->mpv, save);
    }
    mpv_terminate_destroy (p->mpv);
    p->mpv = NULL;
  }
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  Player p = {.paused = FALSE, .landscape = -1, .outputs = -1};
  g_autoptr (GtkApplication) app = gtk_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);
  p.app = app;
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), &p);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), &p);
  g_signal_connect (app, "open", G_CALLBACK (on_open), &p);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), &p);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
