/* vibeos-video: the video player. mpv plays the video (libmpv: its demuxers, decoders and seeking, which play every
 * file the owner tried, over a Samba share too, and skip anywhere at once), drawn by its OpenGL renderer into a
 * GtkGLArea; the controls over it are an Android player's: a back arrow and the title at the top, replay 10 s,
 * play/pause and forward 10 s in the middle, the time, the seek bar and the length at the bottom. A tap shows or hides
 * them (they hide by themselves after a few seconds of playing); a double tap on the left or right third skips 10 s.
 *
 * A file opened from a share (smb://…) is played from GVfs's FUSE path to it: mpv's FFmpeg cannot open smb:// itself.
 * Where a film was left is remembered (mpv's watch-later files, in the user's state directory), and it carries on
 * from there the next time. */
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <vibeos.h>

#define APP_ID "vibeos.Video"
#define CONTROLS_HIDE_MS 2000
#define DOUBLE_TAP_MS 250
#define SKIP_S 10.0
#define CENTER_BUTTON_DP 72
#define SIDE_BUTTON_DP 56
#define TOP_BUTTON_DP 48

typedef enum { ICON_BACK, ICON_PLAY, ICON_PAUSE, ICON_REPLAY, ICON_FORWARD } Icon;

typedef struct {
  GtkApplication *app;
  GtkWidget *win, *area, *controls, *title, *play, *position, *length, *seek, *spinner, *message;
  mpv_handle *mpv;
  mpv_render_context *render;
  gboolean paused, buffering, ended, have_file, dragging, setting_seek, named;
  double time, duration;
  guint hide_id, tap_id, events_id;
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
  gboolean round = icon != ICON_BACK;

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
                                          "Playing a video");
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
      break;
    case MPV_EVENT_END_FILE: {
      mpv_event_end_file *end = ev->data;
      if (end->reason == MPV_END_FILE_REASON_ERROR) {
        g_warning ("mpv: %s", mpv_error_string (end->error));
        /* nothing plays: the controls stay */
        p->have_file = FALSE;
        sync_inhibit (p);
        gtk_label_set_text (GTK_LABEL (p->message), "Can’t play this video");
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
    {"audio-client-name", "Video Player"},
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
  gtk_window_set_title (GTK_WINDOW (p->win), "Video Player");
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
  gtk_box_append (GTK_BOX (top), icon_button (p, ICON_BACK, TOP_BUTTON_DP, "Back", G_CALLBACK (go_back)));
  p->title = gtk_label_new ("");
  gtk_widget_add_css_class (p->title, "video-title");
  gtk_label_set_ellipsize (GTK_LABEL (p->title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (p->title), 0);
  gtk_widget_set_hexpand (p->title, TRUE);
  gtk_box_append (GTK_BOX (top), p->title);
  gtk_box_append (GTK_BOX (p->controls), top);

  GtkWidget *middle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 48);
  gtk_widget_set_halign (middle, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (middle, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (middle, TRUE);
  GtkWidget *replay_button = icon_button (p, ICON_REPLAY, SIDE_BUTTON_DP, "Back 10 seconds", G_CALLBACK (replay));
  gtk_widget_set_valign (replay_button, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (middle), replay_button);
  p->play = icon_button (p, ICON_PLAY, CENTER_BUTTON_DP, "Play or pause", G_CALLBACK (toggle_pause));
  gtk_box_append (GTK_BOX (middle), p->play);
  GtkWidget *forward_button = icon_button (p, ICON_FORWARD, SIDE_BUTTON_DP, "On 10 seconds", G_CALLBACK (forward));
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

  p->message = gtk_label_new ("Open a video from Files");
  gtk_widget_add_css_class (p->message, "video-message");
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
    gtk_label_set_text (GTK_LABEL (p->message), "Opening…");
    gtk_widget_set_visible (p->message, TRUE);
    g_file_mount_enclosing_volume (file, G_MOUNT_MOUNT_NONE, op, NULL, on_mounted, g_object_ref (p->app));
    return;
  }
  if (!path && !web) {
    gtk_label_set_text (GTK_LABEL (p->message), "Can’t reach this video");
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
    gtk_label_set_text (GTK_LABEL (p->message), "The player could not start");
    gtk_widget_set_visible (p->message, TRUE);
  }
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
  Player p = {.paused = FALSE};
  g_autoptr (GtkApplication) app = gtk_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);
  p.app = app;
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), &p);
  g_signal_connect (app, "open", G_CALLBACK (on_open), &p);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), &p);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
