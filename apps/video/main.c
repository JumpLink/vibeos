/* vibeos-video: the video player. mpv plays the video (libmpv: its demuxers, decoders and seeking, which play every
 * file the owner tried, over a Samba share too, and skip anywhere at once), drawn by its OpenGL renderer into a
 * GtkGLArea; the controls over it are an Android player's: a back arrow, the title and ⋮ at the top, replay 10 s,
 * play/pause and forward 10 s in the middle, the time, the seek bar and the length at the bottom. A tap shows or hides
 * them (they hide by themselves after a few seconds of playing); a double tap on the left or right third skips 10 s.
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
 * shot the other way (a wide film on an upright phone) stays whole: filled, most of it would be cut away. */
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
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

typedef enum { ICON_BACK, ICON_PLAY, ICON_PAUSE, ICON_REPLAY, ICON_FORWARD, ICON_MORE, ICON_FULLSCREEN } Icon;

typedef struct {
  GtkApplication *app;
  GtkWidget *win, *area, *controls, *title, *full, *more, *play, *position, *length, *seek, *spinner, *message;
  mpv_handle *mpv;
  mpv_render_context *render;
  gboolean paused, buffering, ended, have_file, dragging, setting_seek, named, fullscreen, cropped;
  double time, duration;
  double aspect;          /* the film's picture as it is shown, width / height (0: no picture) */
  gint64 rotate;          /* and the turn it is shown at (a phone's video filmed upright: 90) */
  int width, height;      /* the window's size */
  int landscape;          /* the screen the window is on is wider than tall: 1, or 0; -1 not known yet */
  guint hide_id, tap_id, events_id, shape_id;
  guint inhibit;
  double tap_x;
} Player;

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

static void
on_property (Player *p, mpv_event_property *prop)
{
  if (g_str_equal (prop->name, "time-pos")) {
    p->time = prop->format == MPV_FORMAT_DOUBLE ? *(double *) prop->data : 0;
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
    gtk_widget_set_visible (p->spinner, p->buffering);
    gtk_spinner_set_spinning (GTK_SPINNER (p->spinner), p->buffering);
  } else if (g_str_equal (prop->name, "video-params/aspect")) {
    p->aspect = prop->format == MPV_FORMAT_DOUBLE ? *(double *) prop->data : 0;
    sync_crop (p, FALSE);
  } else if (g_str_equal (prop->name, "video-params/rotate")) {
    p->rotate = prop->format == MPV_FORMAT_INT64 ? *(gint64 *) prop->data : 0;
    sync_crop (p, FALSE);
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
      /* the crop is the player's, whatever a film's watch-later file says */
      sync_crop (p, TRUE);
      break;
    case MPV_EVENT_END_FILE: {
      mpv_event_end_file *end = ev->data;
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
    {"hwdec", "auto-safe"},
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
  ".video-top { background-image: linear-gradient(to bottom, alpha(black, 0.6), transparent); padding: 8dp 8dp 32dp 4dp; }"
  ".video-bottom { background-image: linear-gradient(to top, alpha(black, 0.6), transparent); padding: 32dp 16dp 12dp 16dp; }"
  ".video-title { font-size: 18sp; font-weight: 500; }"
  ".video-time { font-size: 14sp; font-feature-settings: 'tnum'; }"
  ".video-icon { background: none; border: none; box-shadow: none; padding: 0; min-width: 0; min-height: 0; }"
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

  GtkGesture *tap = gtk_gesture_click_new ();
  g_signal_connect (tap, "pressed", G_CALLBACK (on_video_pressed), p);
  gtk_widget_add_controller (overlay, GTK_EVENT_CONTROLLER (tap));

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
  p->full = icon_button (p, ICON_FULLSCREEN, TOP_BUTTON_DP, _("Full screen"), G_CALLBACK (toggle_fullscreen));
  gtk_box_append (GTK_BOX (top), p->full);
  p->more = icon_button (p, ICON_MORE, TOP_BUTTON_DP, _("More options"), G_CALLBACK (open_menu));
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
  const char *args[] = {"loadfile", what, NULL};
  command (p, args);
  show_controls (p, TRUE);
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
on_startup (GApplication *app, gpointer data)
{
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  vibeos_css_load_string (CSS, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

static void
on_shutdown (GApplication *app, Player *p)
{
  if (p->inhibit)
    gtk_application_uninhibit (p->app, p->inhibit);
  p->inhibit = 0;
  g_clear_handle_id (&p->hide_id, g_source_remove);
  g_clear_handle_id (&p->tap_id, g_source_remove);
  g_clear_handle_id (&p->events_id, g_source_remove);
  g_clear_handle_id (&p->shape_id, g_source_remove);
  if (p->mpv) {
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
  Player p = {.paused = FALSE, .landscape = -1};
  g_autoptr (GtkApplication) app = gtk_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);
  p.app = app;
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), &p);
  g_signal_connect (app, "open", G_CALLBACK (on_open), &p);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), &p);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
