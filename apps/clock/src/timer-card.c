/* TimerCard: see timer-card.h. The label and the close button on top; under them the ring with the time in it,
 * and "+ 1:00" over play/pause to its right. On the expired screen it has no label and no close button. */
#include "timer-card.h"
#include "clock-draw.h"
#include "timers-client.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>

#define PADDING         16      /* the card's padding */
#define CORNER          28      /* as round as an alarm's card */
#define LABEL_MARGIN    8       /* the label's margin, and its padding on either side */
#define LABEL_HEIGHT    27      /* one line of the label, with the font's padding */
#define ICON_BUTTON     48      /* close and reset */
#define ICON_SIZE       20      /* ... and their icons */
#define STROKE          6       /* the ring, and twice the dot's radius */
#define MAX_CIRCLE      240
#define CIRCLE_MARGIN   8       /* the ring's box: this much under the label, twice that before "+ 1:00" */
#define SPACE           8       /* the space over and under "+ 1:00" */
#define INSET           4       /* over and under each filled button, inside its box */
#define ADD_HEIGHT      75      /* "+ 1:00": its line and the padding over and under it */
#define PLAY_HEIGHT     67      /* play/pause: its icon and the padding */
#define BUTTON_PADDING  24      /* both buttons' padding at the sides */
#define TIME_SIZE       40      /* at most: it shrinks to fit */
#define TIME_PADDING    24      /* ... and its padding either side */
#define BOX_HEIGHT      160     /* a card without the ring (a tablet's third column): the time's box, this tall */

#define HOUR (60 * 60 * 1000)

struct _TimerCard
{
  GtkWidget parent;
  guint id;
  gboolean ringing;

  TimerState state;             /* as last shown; 0 before the first update */
  double elapsed;               /* how much of the length has gone, 0..1, for the ring */
  gboolean ring_hidden;         /* an expired timer's ring blinking off */
  char *label;                  /* the timer's, as shown */

  GtkWidget *label_button, *label_text, *close, *time, *reset, *add, *play, *play_icon;
  graphene_rect_t ring;         /* the ring's box (circle_container) */
  gboolean no_ring;             /* the time in a box as wide as the ring's would be, BOX_HEIGHT tall, no ring */
  double fitted;                /* the width the time was last fitted to */
  guint refit;
};

G_DEFINE_FINAL_TYPE (TimerCard, timer_card, GTK_TYPE_WIDGET)

guint
timer_card_get_id (TimerCard *self)
{
  return self->id;
}

/* --- the time */

/* the time left rounded up to the next second, "1:02:03", "2:03" or "3"; in overtime counting on behind a minus
 * sign, rounded down */
static char *
time_text (gint64 remaining)
{
  gboolean negative = remaining < 0;
  int hours, minutes, seconds, rest;
  g_autofree char *time = NULL;

  if (negative)
    remaining = -remaining;
  hours = remaining / HOUR;
  minutes = remaining % HOUR / 60000;
  seconds = remaining % 60000 / 1000;
  rest = remaining % 1000;
  if (!negative && rest)
    {
      if (++seconds == 60)
        {
          seconds = 0;
          if (++minutes == 60)
            {
              minutes = 0;
              hours++;
            }
        }
    }
  if (hours)
    time = g_strdup_printf ("%d:%02d:%02d", hours, minutes, seconds);
  else if (minutes)
    time = g_strdup_printf ("%d:%02d", minutes, seconds);
  else
    time = g_strdup_printf ("%d", seconds);
  return negative && (hours || minutes || seconds) ? g_strconcat ("−", time, NULL) : g_steal_pointer (&time);
}

static void
fit_time (TimerCard *self)
{
  clock_label_fit (self->time, TIME_SIZE, self->ring.size.width - 2 * TIME_PADDING);
}

static gboolean
on_refit (gpointer data)
{
  TimerCard *self = data;

  self->refit = 0;
  fit_time (self);
  return G_SOURCE_REMOVE;
}

/* --- the buttons */

static void
on_label_response (VibeosDialog *dialog, int response, TimerCard *self)
{
  GtkWidget *entry = g_object_get_data (G_OBJECT (dialog), "entry");

  if (response == VIBEOS_RESPONSE_OK)
    timers_client_set_label (timers_client_get (), self->id, gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_label_activate (GtkEntry *entry, VibeosDialog *dialog)
{
  /* the keyboard's Done is OK */
  vibeos_dialog_respond (dialog, VIBEOS_RESPONSE_OK);
}

/* the label dialog, as for an alarm: "Label" over one line of text, Cancel and OK; the text starts selected */
static void
on_label (VibeosPressable *button, TimerCard *self)
{
  VibeosDialog *dialog = vibeos_dialog_new (NULL);
  GtkWidget *entry = gtk_entry_new ();

  vibeos_dialog_set_message (dialog, _("Label"));
  gtk_editable_set_text (GTK_EDITABLE (entry), self->label ? self->label : "");
  gtk_entry_set_input_hints (GTK_ENTRY (entry), GTK_INPUT_HINT_UPPERCASE_SENTENCES);
  gtk_widget_add_css_class (entry, "vibeos-entry");
  gtk_widget_add_css_class (entry, "label-entry");
  g_signal_connect (entry, "activate", G_CALLBACK (on_label_activate), dialog);
  vibeos_dialog_set_content (dialog, entry);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "entry", entry);
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_label_response), self, 0);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
  gtk_widget_grab_focus (entry);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
}

static void
on_close (VibeosPressable *button, TimerCard *self)
{
  timers_client_delete (timers_client_get (), self->id);
}

/* reset: the service deletes a timer another app asked to have gone once it rang */
static void
on_reset (VibeosPressable *button, TimerCard *self)
{
  timers_client_reset (timers_client_get (), self->id);
}

static void
on_add (VibeosPressable *button, TimerCard *self)
{
  timers_client_add_minute (timers_client_get (), self->id);
}

/* start a paused or reset one, pause a running one, stop one that rang */
static void
on_play (VibeosPressable *button, TimerCard *self)
{
  TimersClient *client = timers_client_get ();

  switch (self->state)
    {
    case TIMER_PAUSED:
    case TIMER_RESET:
      timers_client_start (client, self->id);
      break;
    case TIMER_RUNNING:
      timers_client_pause (client, self->id);
      break;
    case TIMER_EXPIRED:
    case TIMER_MISSED:
      timers_client_reset (client, self->id);
      break;
    }
}

static void
describe (GtkWidget *widget, const char *description)
{
  gtk_accessible_update_property (GTK_ACCESSIBLE (widget), GTK_ACCESSIBLE_PROPERTY_LABEL, description, -1);
}

/* --- updating */

static void
show_label (TimerCard *self, const char *label)
{
  gboolean empty = !label || !*label;

  if (!g_strcmp0 (label, self->label) && self->state)
    return;
  g_free (self->label);
  self->label = g_strdup (label);
  /* the hint "Label" until it has one; on the expired screen no hint, and no label at all without one */
  gtk_label_set_text (GTK_LABEL (self->label_text), empty ? _("Label") : label);
  if (empty)
    gtk_widget_add_css_class (self->label_text, "hint");
  else
    gtk_widget_remove_css_class (self->label_text, "hint");
  if (self->ringing)
    gtk_widget_set_visible (self->label_button, !empty);
}

/* the buttons for the state, changed only when it changes */
static void
show_state (TimerCard *self, TimerState state)
{
  const char *icon = "clock-pause-play-symbolic", *description = _("Start");

  if (state == self->state)
    return;
  self->state = state;
  gtk_widget_set_visible (self->reset, !self->ringing && state != TIMER_RESET && state != TIMER_EXPIRED
                                         && state != TIMER_MISSED);
  /* a reset timer keeps the room of "+ 1:00" (INVISIBLE) */
  gtk_widget_set_child_visible (self->add, state != TIMER_RESET);
  if (state == TIMER_RUNNING)
    {
      icon = "clock-play-pause-symbolic";
      description = _("Pause");
    }
  else if (state == TIMER_EXPIRED || state == TIMER_MISSED)
    {
      icon = "clock-stop-play-symbolic";
      description = _("Stop");
    }
  gtk_image_set_from_icon_name (GTK_IMAGE (self->play_icon), icon);
  describe (self->play, description);
  /* the ring's colours go with the state */
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
timer_card_update (TimerCard *self, const Timer *timer, gint64 now)
{
  g_autofree char *text = time_text (timer_remaining (timer, now));
  const char *shown = gtk_label_get_text (GTK_LABEL (self->time));
  gint64 total = timer_total_length (timer);
  gboolean blink_off = now % 1000 < 500, hidden;
  double elapsed;

  if (!g_str_equal (text, shown))
    {
      gboolean longer = g_utf8_strlen (text, -1) != g_utf8_strlen (shown, -1);

      gtk_label_set_text (GTK_LABEL (self->time), text);
      if (longer)
        fit_time (self);
    }
  show_label (self, timer->label);
  show_state (self, timer->state);

  /* the ring: how much has gone, and an expired or missed one's ring off for the first half of each second;
   * drawn again only when that changed, which a paused timer's never does */
  hidden = (timer->state == TIMER_EXPIRED || timer->state == TIMER_MISSED) && blink_off;
  elapsed = total > 0 ? MIN (1, (double) timer_elapsed (timer, now) / total) : 0;
  if (hidden != self->ring_hidden || elapsed != self->elapsed)
    {
      self->ring_hidden = hidden;
      self->elapsed = elapsed;
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

/* --- layout */

typedef struct
{
  graphene_rect_t label, close, add, play, ring;
  double height;
} Layout;

/* the layout of a card `width` wide. The right column is anchored at the bottom: play/pause,
 * a Space, "+ 1:00", a Space, the close button. The ring's box is square, as wide as the room left of "+ 1:00"
 * (at most MAX_CIRCLE), and centred in the height under the label. The card is as tall as the taller column. */
static void
compute_layout (TimerCard *self, int width, Layout *layout)
{
  double x = PADDING, right = width - PADDING, top = PADDING;
  gboolean label = gtk_widget_get_visible (self->label_button), close = gtk_widget_get_visible (self->close);
  double label_bottom = label ? top + LABEL_MARGIN + LABEL_HEIGHT : top;
  int pill;
  double size, column, inner;

  gtk_widget_measure (self->add, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &pill, NULL, NULL);
  size = CLAMP (right - pill - 2 * CIRCLE_MARGIN - x, 0, MAX_CIRCLE);
  column = (close ? ICON_BUTTON : 0) + SPACE + ADD_HEIGHT + 2 * INSET + SPACE + PLAY_HEIGHT + 2 * INSET;
  inner = MAX (column, label_bottom - top + CIRCLE_MARGIN + (self->no_ring ? BOX_HEIGHT : size));
  layout->height = inner + 2 * PADDING;

  layout->close = GRAPHENE_RECT_INIT (right - ICON_BUTTON, top, ICON_BUTTON, ICON_BUTTON);
  layout->label = GRAPHENE_RECT_INIT (x + LABEL_MARGIN, top + LABEL_MARGIN,
                                      (close ? right - ICON_BUTTON : right) - LABEL_MARGIN - x - LABEL_MARGIN,
                                      LABEL_HEIGHT);
  layout->play = GRAPHENE_RECT_INIT (right - pill, top + inner - INSET - PLAY_HEIGHT, pill, PLAY_HEIGHT);
  layout->add = GRAPHENE_RECT_INIT (right - pill, layout->play.origin.y - INSET - SPACE - INSET - ADD_HEIGHT, pill,
                                    ADD_HEIGHT);
  {
    double space_top = label_bottom + CIRCLE_MARGIN;

    double box = self->no_ring ? BOX_HEIGHT : size;

    layout->ring = GRAPHENE_RECT_INIT (x, space_top + (top + inner - space_top - box) / 2,
                                       self->no_ring ? right - pill - 2 * CIRCLE_MARGIN - x : size, box);
  }
}

static void
place (GtkWidget *child, const graphene_rect_t *rect)
{
  GskTransform *transform = gsk_transform_translate (NULL, &rect->origin);

  gtk_widget_allocate (child, (int) round (rect->size.width), (int) round (rect->size.height), -1, transform);
}

double
timer_card_get_ring_height (TimerCard *self, int width)
{
  Layout layout;

  compute_layout (self, width, &layout);
  return layout.ring.size.height;
}

static GtkSizeRequestMode
timer_card_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
timer_card_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                    int *minimum_baseline, int *natural_baseline)
{
  Layout layout;

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *minimum = *natural = 0;
      return;
    }
  compute_layout (TIMER_CARD (widget), for_size < 0 ? 328 : for_size, &layout);
  *minimum = *natural = (int) ceil (layout.height);
}

static void
timer_card_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  TimerCard *self = TIMER_CARD (widget);
  Layout layout;
  int time_width, time_height;

  compute_layout (self, width, &layout);
  self->ring = layout.ring;
  if (gtk_widget_get_visible (self->label_button))
    place (self->label_button, &layout.label);
  if (gtk_widget_get_visible (self->close))
    place (self->close, &layout.close);
  place (self->add, &layout.add);
  place (self->play, &layout.play);

  /* the time in the middle of the ring (without font padding: the label's own line), the reset button at its foot, an
   * eighth of the ring's inner diameter up */
  if (layout.ring.size.width != self->fitted && !self->refit)
    self->refit = g_idle_add (on_refit, self);
  self->fitted = layout.ring.size.width;
  gtk_widget_measure (self->time, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &time_width, NULL, NULL);
  gtk_widget_measure (self->time, GTK_ORIENTATION_VERTICAL, time_width, NULL, &time_height, NULL, NULL);
  place (self->time, &GRAPHENE_RECT_INIT (layout.ring.origin.x + (layout.ring.size.width - time_width) / 2,
                                          layout.ring.origin.y + (layout.ring.size.height - time_height) / 2,
                                          time_width, time_height));
  if (gtk_widget_get_visible (self->reset))
    place (self->reset, &GRAPHENE_RECT_INIT (layout.ring.origin.x + (layout.ring.size.width - ICON_BUTTON) / 2,
                                             layout.ring.origin.y + layout.ring.size.height - ICON_BUTTON
                                               - (self->no_ring ? 0 : floor ((layout.ring.size.width - 2 * STROKE) / 8)),
                                             ICON_BUTTON, ICON_BUTTON));
}

/* --- drawing */

/* the ring. A reset timer: the whole ring in primary. Running or paused: what is left in primary clockwise from 12
 * o'clock, what has gone in the background colour the other way, the dot between them. Expired: the whole ring in the
 * background colour, the dot at the top. */
static void
snapshot_ring (TimerCard *self, GtkSnapshot *snapshot)
{
  int scale = vibeos_device_scale (GTK_WIDGET (self));
  /* the centre in whole device pixels */
  double half = floor (self->ring.size.width * scale / 2) / scale;
  double cx = self->ring.origin.x + half, cy = self->ring.origin.y + half;
  double radius = half - STROKE, gone;
  GdkRGBA left = vibeos_color (VIBEOS_PRIMARY), background = vibeos_color (VIBEOS_SURFACE);

  if (radius <= 0 || self->ring_hidden || !self->state || self->no_ring)
    return;
  gone = self->state == TIMER_RESET ? 0 : self->state == TIMER_EXPIRED ? 1 : self->elapsed;
  clock_snapshot_arcs (snapshot, scale, cx, cy, radius, STROKE,
                       (ClockArc[]) { { 270, (1 - gone) * 360, left }, { 270, -gone * 360, background } }, 2);
  clock_snapshot_dot (snapshot, cx + radius * cos ((270 - gone * 360) * G_PI / 180),
                      cy + radius * sin ((270 - gone * 360) * G_PI / 180), STROKE / 2.0, &left);
}

static void
timer_card_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  TimerCard *self = TIMER_CARD (widget);
  GskRoundedRect card;
  GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);

  gsk_rounded_rect_init_from_rect (&card, &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                               gtk_widget_get_height (widget)), CORNER);
  gtk_snapshot_push_rounded_clip (snapshot, &card);
  gtk_snapshot_append_color (snapshot, &fill, &card.bounds);
  gtk_snapshot_pop (snapshot);
  snapshot_ring (self, snapshot);
  GTK_WIDGET_CLASS (timer_card_parent_class)->snapshot (widget, snapshot);
}

void
timer_card_set_ring (TimerCard *self, gboolean ring)
{
  gboolean no_ring = !ring;

  if (no_ring == self->no_ring)
    return;
  self->no_ring = no_ring;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

/* --- GObject */

static void
timer_card_dispose (GObject *object)
{
  TimerCard *self = TIMER_CARD (object);
  GtkWidget *child;

  g_clear_handle_id (&self->refit, g_source_remove);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (timer_card_parent_class)->dispose (object);
}

static void
timer_card_finalize (GObject *object)
{
  g_free (TIMER_CARD (object)->label);
  G_OBJECT_CLASS (timer_card_parent_class)->finalize (object);
}

static void
timer_card_class_init (TimerCardClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = timer_card_dispose;
  G_OBJECT_CLASS (klass)->finalize = timer_card_finalize;
  widget_class->get_request_mode = timer_card_get_request_mode;
  widget_class->measure = timer_card_measure;
  widget_class->size_allocate = timer_card_size_allocate;
  widget_class->snapshot = timer_card_snapshot;
  gtk_widget_class_set_css_name (widget_class, "timer-card");
}

/* a filled pill: its corners fully round, the ripple bounded by them */
static GtkWidget *
pill_new (GtkWidget *child, VibeosColor fill, int height)
{
  GtkWidget *pill = vibeos_pressable_new (child);

  vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (pill), fill, FALSE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (pill), VIBEOS_RIPPLE_BOUNDED, height / 2.0);
  return pill;
}

static void
timer_card_init (TimerCard *self)
{
  GtkWidget *add_text, *close_icon;

  /* the label: cut at the start when long; a tap edits it */
  self->label_text = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->label_text, "timer-label");
  gtk_label_set_ellipsize (GTK_LABEL (self->label_text), PANGO_ELLIPSIZE_START);
  gtk_label_set_single_line_mode (GTK_LABEL (self->label_text), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->label_text), 0);
  vibeos_label_set_font_padding_sp (self->label_text, 20);
  gtk_widget_set_valign (self->label_text, GTK_ALIGN_START);
  gtk_widget_set_margin_start (self->label_text, LABEL_MARGIN);
  gtk_widget_set_margin_end (self->label_text, LABEL_MARGIN);
  self->label_button = vibeos_pressable_new (self->label_text);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->label_button), VIBEOS_RIPPLE_BOUNDED, 0);
  g_signal_connect (self->label_button, "clicked", G_CALLBACK (on_label), self);
  gtk_widget_set_parent (self->label_button, GTK_WIDGET (self));

  /* close: a white cross on a disc */
  close_icon = gtk_image_new_from_icon_name ("clock-close-circle-cross-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (close_icon), ICON_SIZE);
  gtk_widget_set_halign (close_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (close_icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (close_icon, "timer-close");
  self->close = vibeos_pressable_new (close_icon);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->close), VIBEOS_RIPPLE_CIRCLE, ICON_SIZE);
  describe (self->close, _("Delete"));
  g_signal_connect (self->close, "clicked", G_CALLBACK (on_close), self);
  gtk_widget_set_parent (self->close, GTK_WIDGET (self));

  /* the time */
  self->time = gtk_label_new ("");
  gtk_widget_add_css_class (self->time, "timer-time");
  gtk_widget_set_parent (self->time, GTK_WIDGET (self));

  /* reset */
  self->reset = vibeos_icon_button_new ("clock-reset-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->reset))), ICON_SIZE);
  gtk_widget_add_css_class (self->reset, "timer-reset");
  describe (self->reset, _("Reset"));
  g_signal_connect (self->reset, "clicked", G_CALLBACK (on_reset), self);
  gtk_widget_set_parent (self->reset, GTK_WIDGET (self));

  /* "+ 1:00" in the tertiary colour, and play/pause in primary under it, as wide as it */
  add_text = gtk_label_new (_("+ 1:00"));
  gtk_widget_add_css_class (add_text, "timer-add");
  vibeos_label_set_font_padding_sp (add_text, 20);
  gtk_widget_set_valign (add_text, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (add_text, BUTTON_PADDING);
  gtk_widget_set_margin_end (add_text, BUTTON_PADDING);
  self->add = pill_new (add_text, VIBEOS_TERTIARY, ADD_HEIGHT);
  describe (self->add, _("Add 1 Minute"));
  g_signal_connect (self->add, "clicked", G_CALLBACK (on_add), self);
  gtk_widget_set_parent (self->add, GTK_WIDGET (self));

  self->play_icon = gtk_image_new_from_icon_name ("clock-pause-play-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->play_icon), 18);
  gtk_widget_add_css_class (self->play_icon, "timer-play");
  self->play = pill_new (self->play_icon, VIBEOS_PRIMARY, PLAY_HEIGHT);
  g_signal_connect (self->play, "clicked", G_CALLBACK (on_play), self);
  gtk_widget_set_parent (self->play, GTK_WIDGET (self));
}

GtkWidget *
timer_card_new (guint id, gboolean ringing)
{
  TimerCard *self = g_object_new (TIMER_TYPE_CARD, NULL);

  self->id = id;
  self->ringing = ringing;
  if (ringing)
    {
      /* on the expired-timers screen: no close, no reset, no hint */
      gtk_widget_set_visible (self->close, FALSE);
      gtk_widget_set_visible (self->reset, FALSE);
      gtk_widget_set_visible (self->label_button, FALSE);
    }
  return GTK_WIDGET (self);
}
