/* The in-call screen: see incall-view.h.
 *
 * The contact grid and the hold banner are drawn here; the buttons, the end-call button and the keypad are
 * children, in that order, so the keypad covers the buttons and the end-call button stays in front of it. */
#include "incall-view.h"
#include "avatar.h"
#include "dialpad.h"
#include "incall-button.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>

/* the contact grid: the status line, the photo and name, the label or timer under them */
#define GRID_TOP        12
#define GRID_SIDE       24
#define AVATAR          88
#define AVATAR_GAP      8
#define STATUS_SIZE     18      /* the status and bottom lines, and the hold banner */
#define NAME_SIZE       36      /* the name, shrunk as far as NAME_MIN_SIZE to fit */
#define NAME_MIN_SIZE   28
#define NAME_GAP        4
#define BOTTOM_GAP      2       /* under the label or the timer */

/* the button grid and the end-call button */
#define GRID_MARGIN     24
#define ROW_GAP         32
#define END_SIZE        64
#define END_MARGIN      36
#define END_ICON        48      /* the handset, scaled up to fill the button less its padding */
#define DIALPAD_GAP     8       /* between the contact grid and the keypad under it */
#define DIALPAD_MS      400

/* the hold banner over the screen: an icon, the held caller's name and "On hold" */
#define BANNER_PAD_X    24
#define BANNER_PAD_Y    16
#define BANNER_ICON     18
#define BANNER_GAP      8
#define BANNER_NAME_END 24

#define ROBOTO_PAD_TOP(size)    ((size) * 263 / 2048.0)
#define ROBOTO_PAD_BOTTOM(size) ((size) * 55 / 2048.0)
#define GSF_PAD_TOP(size)       ((size) * 278 / 2000.0)
#define GSF_PAD_BOTTOM(size)    ((size) * 33 / 2000.0)

#define BACKGROUND      0x181c1d
#define TEXT_SECONDARY  (0xdd / 255.0)  /* the status and bottom lines: white at this alpha */
#define END_COLOR       0xe46962
#define BANNER_COLOR    0x212121        /* at 0xCC */

/* the buttons this phone has; the GSM chooser's others (Record, which Germany forbids, Merge, Manage, Video call,
 * Change SIM) need what the modem or this phone does not have, so they are never allowed */
typedef enum
{
  BUTTON_MUTE,
  BUTTON_KEYPAD,
  BUTTON_SPEAKER,
  BUTTON_SWAP,                  /* to the held call */
  BUTTON_ADD_CALL,
  BUTTON_HOLD,
  N_BUTTONS,
  NO_BUTTON = -1,
} ButtonId;

#define GRID_POSITIONS 6        /* 2 rows × 3 */

struct _InCallView
{
  GtkWidget parent;

  /* the calls, as last set */
  guint call;
  CallState state;
  CallEnd end;
  gint64 connected;
  gboolean secondary;
  char *identifier, *letter_from;
  GdkTexture *photo;
  gboolean locked;

  PangoLayout *status, *name, *bottom, *banner_name, *banner_label;
  double name_size;
  char *label;                  /* the bottom row's text when no timer shows */
  guint timer;

  GtkWidget *buttons[N_BUTTONS];
  ButtonId placed[GRID_POSITIONS];
  GtkWidget *end_button;
  GtkWidget *dialpad;
  gboolean dialpad_shown;
  VibeosAnim *slide;
  double dialpad_progress;      /* 0 hidden … 1 up */

  double grid_bottom;           /* where the contact grid ends: the button grid and the keypad go under it */
};

G_DEFINE_FINAL_TYPE (InCallView, incall_view, GTK_TYPE_WIDGET)

enum { ADD_CALL, NOTICE, N_SIGNALS };
static guint signals[N_SIGNALS];

static GdkRGBA
rgb (guint32 value, double alpha)
{
  return (GdkRGBA) { ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0, (value & 0xff) / 255.0, alpha };
}

static gboolean
is_live (CallState state)
{
  return state != CALL_STATE_DISCONNECTED;
}

/* ---- text ----------------------------------------------------------------------------------------------------- */

static PangoLayout *
new_layout (InCallView *self, const char *family, double size)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), NULL);
  g_autoptr(PangoFontDescription) font = pango_font_description_from_string (family);

  /* 400, which Bold text makes 700 */
  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_font_description_set_weight (font, 700);
  pango_layout_set_font_description (layout, font);
  pango_layout_set_single_paragraph_mode (layout, TRUE);
  pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
  return layout;
}

static double
layout_height (PangoLayout *layout)
{
  int width, height;

  pango_layout_get_size (layout, &width, &height);
  return height / (double) PANGO_SCALE;
}

static double
layout_width (PangoLayout *layout)
{
  int width, height;

  pango_layout_get_size (layout, &width, &height);
  return width / (double) PANGO_SCALE;
}

static double
roboto_box (PangoLayout *layout)
{
  return ROBOTO_PAD_TOP (STATUS_SIZE) + layout_height (layout) + ROBOTO_PAD_BOTTOM (STATUS_SIZE);
}

/* the largest size from NAME_SIZE down to NAME_MIN_SIZE at which the name fits; below that it is ellipsized */
static void
fit_name (InCallView *self, double width)
{
  g_autoptr(PangoFontDescription) font = pango_font_description_copy (pango_layout_get_font_description (self->name));
  double size;

  pango_layout_set_width (self->name, -1);
  for (size = NAME_SIZE; size > NAME_MIN_SIZE; size--)
    {
      pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
      pango_layout_set_font_description (self->name, font);
      if (layout_width (self->name) <= width)
        break;
    }
  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_layout_set_font_description (self->name, font);
  pango_layout_set_width (self->name, (int) (width * PANGO_SCALE));
  self->name_size = size;
}

static void
draw_layout (GtkSnapshot *snapshot, PangoLayout *layout, double x, double y, double alpha)
{
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, y));
  gtk_snapshot_append_layout (snapshot, layout, &(GdkRGBA) { 1, 1, 1, alpha });
  gtk_snapshot_restore (snapshot);
}

static void
draw_centered (GtkSnapshot *snapshot, PangoLayout *layout, double width, double y, double alpha)
{
  PangoRectangle logical;

  /* the layout is as wide as its row and centres its line; its ink starts where the line does */
  pango_layout_get_pixel_extents (layout, NULL, &logical);
  draw_layout (snapshot, layout, round ((width - logical.width) / 2) - logical.x, y, alpha);
}

/* ---- the contact grid ----------------------------------------------------------------------------------------- */

/* the time since the call came up: "MM:SS", from an hour "H:MM:SS" */
static char *
format_elapsed (gint64 seconds)
{
  if (seconds >= 3600)
    return g_strdup_printf ("%" G_GINT64_FORMAT ":%02d:%02d", seconds / 3600, (int) (seconds / 60 % 60),
                            (int) (seconds % 60));
  return g_strdup_printf ("%02d:%02d", (int) (seconds / 60), (int) (seconds % 60));
}

static gboolean on_timer (gpointer data);

/* the bottom row: while the call is up the timer takes the label's place (it counts from when it was first up, so a
 * call that was held keeps its time) */
static void
update_bottom (InCallView *self)
{
  g_clear_handle_id (&self->timer, g_source_remove);
  if (self->state == CALL_STATE_ACTIVE && self->connected > 0)
    {
      gint64 elapsed = MAX (0, g_get_real_time () / 1000 - self->connected);
      gint64 seconds = (elapsed + 500) / 1000;
      g_autofree char *text = format_elapsed (seconds);

      pango_layout_set_text (self->bottom, text, -1);
      /* the next change, where the rounding goes over to the next second */
      if (gtk_widget_get_mapped (GTK_WIDGET (self)))
        self->timer = g_timeout_add ((guint) ((seconds * 1000 + 500) - elapsed + 1), on_timer, self);
    }
  else
    pango_layout_set_text (self->bottom, self->label ? self->label : "", -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_timer (gpointer data)
{
  InCallView *self = data;

  self->timer = 0;
  update_bottom (self);
  return G_SOURCE_REMOVE;
}

/* the top row: "Calling…" while the call is being made; otherwise the connection label, which a single-SIM phone off
 * Wi-Fi does not have (the row stays, empty) */
static const char *
status_text (InCallView *self)
{
  switch (self->state)
    {
    case CALL_STATE_CONNECTING:
    case CALL_STATE_DIALING:
      return _("Calling…");
    case CALL_STATE_INCOMING:
    case CALL_STATE_CALL_WAITING:
      return _("Call from");
    default:
      return "";
    }
}

/* the bottom row: "Hanging up", what ended it, or the kind of number and the number */
static char *
bottom_label (InCallView *self, const Caller *caller)
{
  switch (self->state)
    {
    case CALL_STATE_DISCONNECTING:
      return g_strdup (_("Hanging up"));
    case CALL_STATE_DISCONNECTED:
      /* what ended the call: busy is named; the others end with "Call ended" */
      return g_strdup (self->end == CALL_END_BUSY ? _("Line busy") : _("Call ended"));
    default:
      return caller_label_and_number (caller);
    }
}

/* ---- the buttons ---------------------------------------------------------------------------------------------- */

/* the button slots for a GSM call, each with its buttons in order of preference; the Record slot and the buttons
 * never allowed here are left out */
static const ButtonId slots[][2] = {
  { BUTTON_MUTE, NO_BUTTON },
  { BUTTON_KEYPAD, NO_BUTTON },
  { BUTTON_SPEAKER, NO_BUTTON },
  { BUTTON_SWAP, BUTTON_ADD_CALL },
  { BUTTON_HOLD, NO_BUTTON },
};

/* each slot takes its first allowed button, in order and without gaps; the
 * buttons it passed over then fill what is left, if they are allowed and enabled */
static void
choose_buttons (InCallView *self, const gboolean allowed[N_BUTTONS], const gboolean enabled[N_BUTTONS])
{
  ButtonId conflicts[N_BUTTONS];
  guint n = 0, n_conflicts = 0;

  for (guint i = 0; i < G_N_ELEMENTS (slots) && n < GRID_POSITIONS; i++)
    for (guint j = 0; j < G_N_ELEMENTS (slots[i]) && slots[i][j] != NO_BUTTON; j++)
      if (allowed[slots[i][j]])
        {
          self->placed[n++] = slots[i][j];
          for (j++; j < G_N_ELEMENTS (slots[i]) && slots[i][j] != NO_BUTTON; j++)
            conflicts[n_conflicts++] = slots[i][j];
          break;
        }
  for (guint i = 0; i < n_conflicts && n < GRID_POSITIONS; i++)
    if (allowed[conflicts[i]] && enabled[conflicts[i]])
      self->placed[n++] = conflicts[i];
  for (; n < GRID_POSITIONS; n++)
    self->placed[n] = NO_BUTTON;
}

/* which buttons are allowed and on, for a GSM call through ModemManager */
static void
update_buttons (InCallView *self)
{
  CallsClient *client = calls_client_get ();
  gboolean live = self->call && is_live (self->state);
  gboolean can_hold = calls_client_has_feature (client, "hold");
  gboolean allowed[N_BUTTONS], enabled[N_BUTTONS];

  /* Mute, Keypad and Speaker are always there; the keypad needs a modem that sends tones */
  allowed[BUTTON_MUTE] = allowed[BUTTON_KEYPAD] = allowed[BUTTON_SPEAKER] = TRUE;
  enabled[BUTTON_MUTE] = enabled[BUTTON_SPEAKER] = live;
  enabled[BUTTON_KEYPAD] = live && calls_client_has_feature (client, "dtmf");
  /* Swap with a second call, while this one is up; Hold only on a modem that holds */
  allowed[BUTTON_SWAP] = self->secondary && can_hold;
  enabled[BUTTON_SWAP] = live && self->state == CALL_STATE_ACTIVE;
  allowed[BUTTON_HOLD] = can_hold;
  enabled[BUTTON_HOLD] = live;
  /* Add call is always shown, and on only when a second call can be made: the first one has to go on hold, there
   * is not one already, and the Phone app it opens can show (not over the lock screen) */
  allowed[BUTTON_ADD_CALL] = TRUE;
  enabled[BUTTON_ADD_CALL] = live && can_hold && !self->secondary && !self->locked;

  choose_buttons (self, allowed, enabled);
  for (int i = 0; i < N_BUTTONS; i++)
    {
      gboolean shown = FALSE;

      for (int p = 0; p < GRID_POSITIONS; p++)
        shown |= self->placed[p] == i;
      gtk_widget_set_visible (self->buttons[i], shown);
      gtk_widget_set_sensitive (self->buttons[i], enabled[i]);
    }
  incall_button_set_checked (INCALL_BUTTON (self->buttons[BUTTON_MUTE]), calls_client_get_muted (client));
  incall_button_set_checked (INCALL_BUTTON (self->buttons[BUTTON_SPEAKER]), calls_client_get_speaker (client));
  incall_button_set_checked (INCALL_BUTTON (self->buttons[BUTTON_HOLD]), self->state == CALL_STATE_ONHOLD);
  incall_button_set_checked (INCALL_BUTTON (self->buttons[BUTTON_KEYPAD]), self->dialpad_shown);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
on_mute (InCallView *self)
{
  CallsClient *client = calls_client_get ();

  calls_client_set_muted (client, !calls_client_get_muted (client));
}

static void
on_speaker (InCallView *self)
{
  CallsClient *client = calls_client_get ();

  calls_client_set_speaker (client, !calls_client_get_speaker (client));
}

static void
on_keypad (InCallView *self)
{
  incall_view_show_dialpad (self, !self->dialpad_shown);
}

/* swap: holding the call that is up brings back the held one (GSM swaps them) */
static void
on_swap (InCallView *self)
{
  calls_client_set_held (calls_client_get (), self->call, TRUE);
}

static void
on_add_call (InCallView *self)
{
  g_signal_emit (self, signals[ADD_CALL], 0);
}

static void
on_hold (InCallView *self)
{
  calls_client_set_held (calls_client_get (), self->call, self->state != CALL_STATE_ONHOLD);
}

static void
on_end (InCallView *self)
{
  /* a call that ended already shows "Call ended" a moment: the button is there, and does nothing */
  if (self->call && is_live (self->state))
    calls_client_hangup (calls_client_get (), self->call);
}

/* ---- the keypad ----------------------------------------------------------------------------------------------- */

static void
on_slide (VibeosAnim *anim, double value, gpointer data)
{
  InCallView *self = data;

  self->dialpad_progress = value;
  if (!vibeos_anim_is_running (anim) && value == 0)
    gtk_widget_set_visible (self->dialpad, FALSE);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

gboolean
incall_view_get_dialpad_shown (InCallView *self)
{
  return self->dialpad_shown;
}

/* the keypad slides up from below over 400 ms and back down; it starts empty */
void
incall_view_show_dialpad (InCallView *self, gboolean shown)
{
  if (shown == self->dialpad_shown || (shown && !gtk_widget_get_sensitive (self->buttons[BUTTON_KEYPAD])))
    return;
  self->dialpad_shown = shown;
  if (shown)
    {
      dialer_dialpad_set_digits (DIALER_DIALPAD (self->dialpad), "");
      gtk_widget_set_visible (self->dialpad, TRUE);
      vibeos_anim_start_cubic (self->slide, self->dialpad_progress, 1, DIALPAD_MS, 0, 0, 0, 0.2, 1);
    }
  else
    vibeos_anim_start_cubic (self->slide, self->dialpad_progress, 0, DIALPAD_MS, 0, 0.4, 0, 1, 1);
  incall_button_set_checked (INCALL_BUTTON (self->buttons[BUTTON_KEYPAD]), shown);
}

/* a key: it shows in the field and its tone goes to the call. The modem plays each tone
 * for its own fixed time, so a key held down does not make a longer one */
static void
on_dialpad_key (DialerDialpad *dialpad, int c, InCallView *self)
{
  char tone[2] = { (char) c, '\0' };

  if (!self->call || !is_live (self->state))
    return;
  dialer_dialpad_append (dialpad, (char) c);
  calls_client_send_dtmf (calls_client_get (), self->call, tone);
}

static void
on_dialpad_close (InCallView *self)
{
  incall_view_show_dialpad (self, FALSE);
}

/* ---- the service ---------------------------------------------------------------------------------------------- */

static void
on_audio_changed (CallsClient *client, InCallView *self)
{
  update_buttons (self);
}

/* the modem may show it cannot send tones only when it is first asked to: the keypad then closes, and says why */
static void
on_status_changed (CallsClient *client, InCallView *self)
{
  gboolean closing = self->dialpad_shown && !calls_client_has_feature (client, "dtmf");

  if (closing)
    {
      /* closed before the button goes insensitive (show_dialpad would refuse to, after) */
      incall_view_show_dialpad (self, FALSE);
      g_signal_emit (self, signals[NOTICE], 0, _("This phone can't send keypad tones"));
    }
  update_buttons (self);
}

void
incall_view_set_calls (InCallView *self, const CallInfo *primary, const Caller *caller, const CallInfo *secondary,
                       const Caller *secondary_caller)
{
  g_autofree char *accessible = NULL;

  self->call = primary->id;
  self->state = primary->state;
  self->end = primary->end;
  self->connected = primary->connected;
  self->secondary = secondary != NULL;

  /* the avatar: the contact's photo or letter, else the person glyph */
  g_free (self->identifier);
  self->identifier = g_strdup (caller->identifier);
  g_free (self->letter_from);
  self->letter_from = caller->is_contact ? g_strdup (caller->name) : NULL;
  g_clear_object (&self->photo);
  self->photo = avatar_texture_from_bytes (caller->photo);

  pango_layout_set_text (self->status, status_text (self), -1);
  pango_layout_set_text (self->name, caller->name, -1);
  g_free (self->label);
  self->label = bottom_label (self, caller);
  update_bottom (self);

  /* the held call's banner: the other call's name */
  pango_layout_set_text (self->banner_name, secondary_caller ? secondary_caller->name : "", -1);

  update_buttons (self);
  accessible = g_strjoin (", ", caller->name, pango_layout_get_text (self->status),
                          pango_layout_get_text (self->bottom), NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
incall_view_set_locked (InCallView *self, gboolean locked)
{
  if (self->locked == locked)
    return;
  self->locked = locked;
  update_buttons (self);
}

/* ---- layout and drawing --------------------------------------------------------------------------------------- */

static double
banner_height (InCallView *self)
{
  return BANNER_PAD_Y + MAX (BANNER_ICON, roboto_box (self->banner_label)) + BANNER_PAD_Y;
}

static void
incall_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  InCallView *self = INCALL_VIEW (widget);
  double column = (width - 2.0 * GRID_MARGIN) / 3, button_height = 0, top, bottom, y;
  double dialpad_top, end_top = height - END_MARGIN - END_SIZE;
  int min, nat;

  pango_layout_set_width (self->status, (int) ((width - 2 * GRID_SIDE) * PANGO_SCALE));
  pango_layout_set_width (self->bottom, (int) ((width - 2 * GRID_SIDE) * PANGO_SCALE));
  pango_layout_set_alignment (self->status, PANGO_ALIGN_CENTER);
  pango_layout_set_alignment (self->bottom, PANGO_ALIGN_CENTER);
  pango_layout_set_alignment (self->name, PANGO_ALIGN_CENTER);
  fit_name (self, width - 2 * GRID_SIDE);

  /* the contact grid, top to bottom */
  y = GRID_TOP + AVATAR + AVATAR_GAP + roboto_box (self->status);
  y += GSF_PAD_TOP (self->name_size) + layout_height (self->name) + GSF_PAD_BOTTOM (self->name_size) + NAME_GAP;
  y += roboto_box (self->bottom) + BOTTOM_GAP;
  self->grid_bottom = y;

  /* the button grid: two rows of three, centred between the contact grid and the end-call button's margin */
  for (int i = 0; i < N_BUTTONS; i++)
    {
      gtk_widget_measure (self->buttons[i], GTK_ORIENTATION_VERTICAL, (int) column, &min, &nat, NULL, NULL);
      button_height = MAX (button_height, nat);
    }
  top = self->grid_bottom;
  bottom = end_top - END_MARGIN;
  y = round (top + (bottom - top - (2 * button_height + ROW_GAP)) / 2);
  for (int p = 0; p < GRID_POSITIONS; p++)
    if (self->placed[p] != NO_BUTTON)
      {
        double x = round (GRID_MARGIN + (p % 3) * column);

        gtk_widget_size_allocate (self->buttons[self->placed[p]],
                                  &(GtkAllocation) { (int) x, (int) (y + (p / 3) * (button_height + ROW_GAP)),
                                                     (int) (round (GRID_MARGIN + (p % 3 + 1) * column) - x),
                                                     (int) button_height }, -1);
      }

  /* the keypad from under the contact grid to the foot, slid down by its own height while it comes and goes */
  dialpad_top = round (self->grid_bottom + DIALPAD_GAP);
  if (gtk_widget_get_visible (self->dialpad))
    {
      double h = height - dialpad_top;
      GskTransform *t = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, dialpad_top
                                                                            + round ((1 - self->dialpad_progress) * h)));

      gtk_widget_allocate (self->dialpad, width, (int) h, -1, t);
    }

  gtk_widget_size_allocate (self->end_button, &(GtkAllocation) { (width - END_SIZE) / 2, (int) end_top, END_SIZE,
                                                                 END_SIZE }, -1);

  /* the banner's name between the icon and "On hold" */
  pango_layout_set_width (self->banner_name, (int) ((width - 2 * BANNER_PAD_X - BANNER_ICON - BANNER_GAP
                                                     - BANNER_NAME_END - layout_width (self->banner_label))
                                                    * PANGO_SCALE));
}

static void
draw_grid (InCallView *self, GtkSnapshot *snapshot, double width)
{
  double y = GRID_TOP;

  avatar_snapshot (snapshot, GTK_WIDGET (self), width / 2, y + AVATAR / 2.0, AVATAR, self->identifier,
                   self->letter_from, self->photo);
  y += AVATAR + AVATAR_GAP;
  draw_centered (snapshot, self->status, width, y + ROBOTO_PAD_TOP (STATUS_SIZE), TEXT_SECONDARY);
  y += roboto_box (self->status);
  draw_centered (snapshot, self->name, width, y + GSF_PAD_TOP (self->name_size), 1);
  y += GSF_PAD_TOP (self->name_size) + layout_height (self->name) + GSF_PAD_BOTTOM (self->name_size) + NAME_GAP;
  draw_centered (snapshot, self->bottom, width, y + ROBOTO_PAD_TOP (STATUS_SIZE), TEXT_SECONDARY);
}

/* the held call's banner: across the top, over the contact grid */
static void
draw_banner (InCallView *self, GtkSnapshot *snapshot, double width)
{
  double height = banner_height (self), x = BANNER_PAD_X;
  double text_y = round ((height - roboto_box (self->banner_label)) / 2) + ROBOTO_PAD_TOP (STATUS_SIZE);
  GdkRGBA background = rgb (BANNER_COLOR, 0xcc / 255.0), icon = { 1, 1, 1, TEXT_SECONDARY };

  gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, 0, width, height));
  vibeos_snapshot_icon (snapshot, GTK_WIDGET (self), "dialer-phone-paused-symbolic", x,
                     round ((height - BANNER_ICON) / 2), BANNER_ICON, &icon);
  x += BANNER_ICON + BANNER_GAP;
  draw_layout (snapshot, self->banner_name, x, text_y, 1);
  draw_layout (snapshot, self->banner_label, width - BANNER_PAD_X - layout_width (self->banner_label), text_y, 1);
}

static void
incall_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  InCallView *self = INCALL_VIEW (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  GdkRGBA background = rgb (BACKGROUND, 1);

  gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, 0, width, height));
  if (self->call)
    draw_grid (self, snapshot, width);
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    gtk_widget_snapshot_child (widget, child, snapshot);
  if (self->secondary)
    draw_banner (self, snapshot, width);
}

static void
incall_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

/* ---- GObject -------------------------------------------------------------------------------------------------- */

static void
incall_view_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (incall_view_parent_class)->map (widget);
  update_bottom (INCALL_VIEW (widget));
}

static void
incall_view_unmap (GtkWidget *widget)
{
  g_clear_handle_id (&INCALL_VIEW (widget)->timer, g_source_remove);
  GTK_WIDGET_CLASS (incall_view_parent_class)->unmap (widget);
}

static void
incall_view_dispose (GObject *object)
{
  InCallView *self = INCALL_VIEW (object);
  GtkWidget *child;

  g_signal_handlers_disconnect_by_data (calls_client_get (), self);
  g_clear_handle_id (&self->timer, g_source_remove);
  g_clear_pointer (&self->slide, vibeos_anim_free);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  g_clear_object (&self->status);
  g_clear_object (&self->name);
  g_clear_object (&self->bottom);
  g_clear_object (&self->banner_name);
  g_clear_object (&self->banner_label);
  g_clear_object (&self->photo);
  g_clear_pointer (&self->identifier, g_free);
  g_clear_pointer (&self->letter_from, g_free);
  g_clear_pointer (&self->label, g_free);
  G_OBJECT_CLASS (incall_view_parent_class)->dispose (object);
}

static void
incall_view_class_init (InCallViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = incall_view_dispose;
  widget_class->measure = incall_view_measure;
  widget_class->size_allocate = incall_view_size_allocate;
  widget_class->snapshot = incall_view_snapshot;
  widget_class->map = incall_view_map;
  widget_class->unmap = incall_view_unmap;
  gtk_widget_class_set_css_name (widget_class, "incall-view");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_GROUP);
  signals[ADD_CALL] = g_signal_new ("add-call", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);
  signals[NOTICE] = g_signal_new ("notice", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
add_button (InCallView *self, ButtonId id, const char *icon, const char *label, GCallback clicked)
{
  self->buttons[id] = incall_button_new (icon, label);
  gtk_widget_set_parent (self->buttons[id], GTK_WIDGET (self));
  g_signal_connect_swapped (self->buttons[id], "clicked", clicked, self);
}

static void
incall_view_init (InCallView *self)
{
  GtkWidget *icon = gtk_image_new_from_icon_name ("dialer-call-end-symbolic");
  GdkRGBA end = rgb (END_COLOR, 1);
  CallsClient *client = calls_client_get ();

  self->status = new_layout (self, "VibeOS Roboto", STATUS_SIZE);
  self->name = new_layout (self, "Google Sans Flex, VibeOS Roboto", NAME_SIZE);
  self->name_size = NAME_SIZE;
  self->bottom = new_layout (self, "VibeOS Roboto", STATUS_SIZE);
  self->banner_name = new_layout (self, "VibeOS Roboto", STATUS_SIZE);
  self->banner_label = new_layout (self, "VibeOS Roboto", STATUS_SIZE);
  pango_layout_set_text (self->banner_label, _("On hold"), -1);

  /* the grid's buttons, then the keypad over them, then the end-call button over that */
  add_button (self, BUTTON_MUTE, "dialer-mic-off-symbolic", _("Mute"), G_CALLBACK (on_mute));
  add_button (self, BUTTON_KEYPAD, "dialer-dialpad-symbolic", _("Keypad"), G_CALLBACK (on_keypad));
  add_button (self, BUTTON_SPEAKER, "dialer-volume-up-symbolic", _("Speaker"), G_CALLBACK (on_speaker));
  add_button (self, BUTTON_SWAP, "dialer-swap-calls-symbolic", _("Swap"), G_CALLBACK (on_swap));
  add_button (self, BUTTON_ADD_CALL, "dialer-add-call-symbolic", _("Add call"), G_CALLBACK (on_add_call));
  add_button (self, BUTTON_HOLD, "dialer-pause-symbolic", _("Hold"), G_CALLBACK (on_hold));
  for (int p = 0; p < GRID_POSITIONS; p++)
    self->placed[p] = NO_BUTTON;

  self->dialpad = dialer_dialpad_new (DIALPAD_INCALL);
  gtk_widget_set_visible (self->dialpad, FALSE);
  gtk_widget_set_parent (self->dialpad, GTK_WIDGET (self));
  g_signal_connect (self->dialpad, "key-down", G_CALLBACK (on_dialpad_key), self);
  g_signal_connect_swapped (self->dialpad, "close", G_CALLBACK (on_dialpad_close), self);
  self->slide = vibeos_anim_new (GTK_WIDGET (self), on_slide, self);

  /* Incall.Button.End: a red disc at elevation 8 with the black handset in it, and a white ripple */
  gtk_image_set_pixel_size (GTK_IMAGE (icon), END_ICON);
  gtk_widget_add_css_class (icon, "end-call-icon");
  self->end_button = vibeos_pressable_new (icon);
  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (self->end_button), &end, TRUE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->end_button), VIBEOS_RIPPLE_CIRCLE, END_SIZE / 2.0);
  vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (self->end_button), &(GdkRGBA) { 1, 1, 1, 0.25 });
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->end_button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("End call"), -1);
  gtk_widget_set_parent (self->end_button, GTK_WIDGET (self));
  g_signal_connect_swapped (self->end_button, "clicked", G_CALLBACK (on_end), self);

  g_signal_connect (client, "audio-changed", G_CALLBACK (on_audio_changed), self);
  g_signal_connect (client, "status-changed", G_CALLBACK (on_status_changed), self);
}

GtkWidget *
incall_view_new (void)
{
  return g_object_new (INCALL_TYPE_VIEW, NULL);
}
