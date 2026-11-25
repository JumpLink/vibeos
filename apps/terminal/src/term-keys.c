/* TermKeys: see term-keys.h */
#include "term-keys.h"
#include "term-input.h"

#include <glib/gi18n.h>

#define LOCK_MS          500   /* a modifier held this long locks */
#define REPEAT_START_MS  400   /* a cursor key held repeats after this long (the keyboard's delete key's), */
#define REPEAT_MS        50    /* then this often */

enum { KIND_KEY, KIND_MOD };

static const struct {
  const char *label;     /* NULL: the symbol's */
  const char *symbol;
  int         kind;
  int         value;     /* a TermKey, or a TermMods bit */
} key_defs[] = {
  { NC_("extra key", "Esc"),  NULL, KIND_KEY, TERM_KEY_ESC },
  { NC_("extra key", "Tab"),  NULL, KIND_KEY, TERM_KEY_TAB },
  { NC_("extra key", "Ctrl"), NULL, KIND_MOD, TERM_MOD_CTRL },
  { NC_("extra key", "Alt"),  NULL, KIND_MOD, TERM_MOD_ALT },
  { NULL, "←", KIND_KEY, TERM_KEY_LEFT },
  { NULL, "↑", KIND_KEY, TERM_KEY_UP },
  { NULL, "↓", KIND_KEY, TERM_KEY_DOWN },
  { NULL, "→", KIND_KEY, TERM_KEY_RIGHT },
};
#define N_KEYS G_N_ELEMENTS (key_defs)

struct _TermKeys {
  GtkWidget         parent;
  GtkWidget        *box;
  GtkWidget        *keys[N_KEYS];
  TermSession      *session;
  TermMods          on;          /* put on for the next key */
  TermMods          locked;      /* locked on */
  /* the key held, by the finger (or the mouse) on it */
  int               held;        /* -1: none */
  GdkEventSequence *sequence;
  TermMods          held_mods;   /* the modifiers its press took, for its repeats */
  guint             timer;       /* its repeat, or a modifier's lock */
  gboolean          lock_fired;
};

G_DEFINE_FINAL_TYPE (TermKeys, term_keys, GTK_TYPE_WIDGET)

enum { SENT, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
show_mods (TermKeys *self)
{
  for (guint i = 0; i < N_KEYS; i++) {
    if (key_defs[i].kind != KIND_MOD)
      continue;
    gtk_widget_remove_css_class (self->keys[i], "on");
    gtk_widget_remove_css_class (self->keys[i], "locked");
    if (self->locked & key_defs[i].value)
      gtk_widget_add_css_class (self->keys[i], "locked");
    else if (self->on & key_defs[i].value)
      gtk_widget_add_css_class (self->keys[i], "on");
  }
}

static void
send_key (TermKeys *self, TermKey key, TermMods mods)
{
  const TermModes *modes = self->session ? term_session_get_modes (self->session) : NULL;
  char buf[32];
  gsize n = term_input_key (key, mods, modes && term_modes_has (modes, 1), buf, sizeof buf);

  if (n && self->session)
    term_session_write (self->session, buf, n);
  g_signal_emit (self, signals[SENT], 0);
}

/* the key just sent took the modifiers that were on for it */
static void
mods_used (TermKeys *self)
{
  if (!self->on)
    return;
  self->on = 0;
  show_mods (self);
}

static gboolean
on_repeat (gpointer data)
{
  TermKeys *self = data;

  send_key (self, key_defs[self->held].value, self->held_mods);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_repeat_start (gpointer data)
{
  TermKeys *self = data;

  on_repeat (self);
  self->timer = g_timeout_add (REPEAT_MS, on_repeat, self);
  return G_SOURCE_REMOVE;
}

static gboolean
on_lock (gpointer data)
{
  TermKeys *self = data;
  int mod = key_defs[self->held].value;

  self->timer = 0;
  self->lock_fired = TRUE;
  self->locked |= mod;
  self->on &= ~mod;
  show_mods (self);
  return G_SOURCE_REMOVE;
}

static void
press (TermKeys *self, int i, GdkEventSequence *sequence)
{
  g_clear_handle_id (&self->timer, g_source_remove);
  if (self->held >= 0)
    gtk_widget_remove_css_class (self->keys[self->held], "pressed");
  self->held = i;
  self->sequence = sequence;
  self->lock_fired = FALSE;
  gtk_widget_add_css_class (self->keys[i], "pressed");
  if (key_defs[i].kind == KIND_MOD) {
    self->timer = g_timeout_add (LOCK_MS, on_lock, self);
    return;
  }
  self->held_mods = self->on | self->locked;
  send_key (self, key_defs[i].value, self->held_mods);
  mods_used (self);
  if (key_defs[i].value != TERM_KEY_ESC && key_defs[i].value != TERM_KEY_TAB)
    self->timer = g_timeout_add (REPEAT_START_MS, on_repeat_start, self);
}

/* the finger lifted (`tapped`), or the touch was cancelled */
static void
release (TermKeys *self, gboolean tapped)
{
  int i = self->held;

  if (i < 0)
    return;
  g_clear_handle_id (&self->timer, g_source_remove);
  self->held = -1;
  self->sequence = NULL;
  gtk_widget_remove_css_class (self->keys[i], "pressed");
  if (key_defs[i].kind == KIND_MOD && tapped && !self->lock_fired) {
    int mod = key_defs[i].value;
    if ((self->locked | self->on) & mod) {
      self->locked &= ~mod;
      self->on &= ~mod;
    } else {
      self->on |= mod;
    }
  }
  show_mods (self);
}

static int
key_at (TermKeys *self, double x, double y)
{
  for (guint i = 0; i < N_KEYS; i++) {
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds (self->keys[i], GTK_WIDGET (self), &bounds)
        && graphene_rect_contains_point (&bounds, &GRAPHENE_POINT_INIT (x, y)))
      return (int) i;
  }
  return -1;
}

/* touches and the mouse's first button, taken here rather than by gestures: a key is pressed when the finger comes
 * down and held until it lifts, however long, for the repeat and the lock */
static gboolean
on_event (GtkEventControllerLegacy *legacy, GdkEvent *event, TermKeys *self)
{
  GdkEventType type = gdk_event_get_event_type (event);
  GdkEventSequence *sequence = gdk_event_get_event_sequence (event);
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  double ex, ey, sx, sy;
  graphene_point_t p;
  int i;

  switch ((int) type) {
  case GDK_TOUCH_BEGIN:
  case GDK_BUTTON_PRESS:
    if (type == GDK_BUTTON_PRESS && gdk_button_event_get_button (event) != GDK_BUTTON_PRIMARY)
      return GDK_EVENT_PROPAGATE;
    gdk_event_get_position (event, &ex, &ey);
    gtk_native_get_surface_transform (native, &sx, &sy);
    if (!gtk_widget_compute_point (GTK_WIDGET (native), GTK_WIDGET (self), &GRAPHENE_POINT_INIT (ex - sx, ey - sy), &p)
        || (i = key_at (self, p.x, p.y)) < 0)
      return GDK_EVENT_STOP;
    press (self, i, sequence);
    return GDK_EVENT_STOP;
  case GDK_TOUCH_END:
  case GDK_BUTTON_RELEASE:
    if (self->held >= 0 && self->sequence == sequence)
      release (self, TRUE);
    return GDK_EVENT_STOP;
  case GDK_TOUCH_CANCEL:
    if (self->held >= 0 && self->sequence == sequence)
      release (self, FALSE);
    return GDK_EVENT_STOP;
  case GDK_TOUCH_UPDATE:
    return GDK_EVENT_STOP;
  default:
    return GDK_EVENT_PROPAGATE;
  }
}

static void
on_unmap (GtkWidget *widget, TermKeys *self)
{
  release (self, FALSE);
}

gboolean
term_keys_filter (TermKeys *self, const char *text, gsize length)
{
  TermMods mods = self->on | self->locked;
  char buf[32];
  gsize n;

  g_return_val_if_fail (TERM_IS_KEYS (self), FALSE);

  if (!mods || length == 0 || text[0] == '\033')
    return FALSE;
  n = term_input_char (text, length, mods, buf, sizeof buf);
  mods_used (self);
  if (!n)
    return FALSE;
  if (self->session)
    term_session_write (self->session, buf, n);
  return TRUE;
}

void
term_keys_set_session (TermKeys *self, TermSession *session)
{
  g_return_if_fail (TERM_IS_KEYS (self));

  g_set_object (&self->session, session);
}

static void
term_keys_dispose (GObject *object)
{
  TermKeys *self = TERM_KEYS (object);

  g_clear_handle_id (&self->timer, g_source_remove);
  g_clear_object (&self->session);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (term_keys_parent_class)->dispose (object);
}

static void
term_keys_class_init (TermKeysClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = term_keys_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "term-keys");
  signals[SENT] = g_signal_new ("sent", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                G_TYPE_NONE, 0);
}

static void
term_keys_init (TermKeys *self)
{
  GtkEventController *legacy = gtk_event_controller_legacy_new ();

  self->held = -1;
  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (self->box), TRUE);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  for (guint i = 0; i < N_KEYS; i++) {
    const char *text = key_defs[i].label ? g_dpgettext2 (NULL, "extra key", key_defs[i].label) : key_defs[i].symbol;
    self->keys[i] = gtk_label_new (text);
    gtk_widget_add_css_class (self->keys[i], "term-key");
    gtk_widget_set_hexpand (self->keys[i], TRUE);
    gtk_box_append (GTK_BOX (self->box), self->keys[i]);
  }
  gtk_widget_set_focusable (GTK_WIDGET (self), FALSE);
  g_signal_connect (legacy, "event", G_CALLBACK (on_event), self);
  gtk_widget_add_controller (GTK_WIDGET (self), legacy);
  g_signal_connect (self, "unmap", G_CALLBACK (on_unmap), self);
}

TermKeys *
term_keys_new (void)
{
  return g_object_new (TERM_TYPE_KEYS, NULL);
}
