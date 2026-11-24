/* DictateStage: see dictate-stage.h.
 *
 * The stage is its bar and, under it, a scroller with the earlier exchanges and then the page of the one going on.
 * That page is kept exactly as tall as the scroller (its size request, set as the stage is laid out): at the bottom of
 * the scroll it is all there is on the screen, as in the design, and scrolled up the earlier ones show over it. The
 * scroll stays at the bottom while the user has not moved it up. */
#include "dictate-stage.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <vibeos.h>

#define BUTTON_HEIGHT       56
#define BUTTON_HEIGHT_LARGE 64
#define LONG_HEADLINE       80  /* characters: a headline this long is a size smaller */

struct _DictateStage
{
  GtkWidget parent;
  GtkWidget *header, *start, *end, *title, *settings, *call;
  GtkWidget *scroller, *column, *past, *page;
  GtkWidget *mine, *middle, *orb, *card, *text, *who, *headline, *sub, *written, *progress, *buttons, *hint;
  gboolean text_first, large_buttons;
  gboolean pinned;              /* the scroll is at the bottom: it stays there as the page grows */
  gboolean held;                /* the press became a long press: its release is no tap */
  int page_height;
};

enum { SIGNAL_TAPPED, SIGNAL_LONG_PRESSED, SIGNAL_SETTINGS, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (DictateStage, dictate_stage, GTK_TYPE_WIDGET)

/* --- the scroll at the bottom */

static gboolean
follow_idle (gpointer data)
{
  DictateStage *self = data;
  GtkAdjustment *adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller));

  if (self->pinned)
    gtk_adjustment_set_value (adjustment, gtk_adjustment_get_upper (adjustment) -
                                          gtk_adjustment_get_page_size (adjustment));
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

static void
follow (DictateStage *self)
{
  g_idle_add_full (G_PRIORITY_LOW, follow_idle, g_object_ref (self), NULL);
}

static void
on_scrolled (GtkAdjustment *adjustment, DictateStage *self)
{
  self->pinned = gtk_adjustment_get_value (adjustment) >= gtk_adjustment_get_upper (adjustment) -
                                                         gtk_adjustment_get_page_size (adjustment) - 2;
}

/* --- layout */

static void
dictate_stage_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                       int *minimum_baseline, int *natural_baseline)
{
  DictateStage *self = DICTATE_STAGE (widget);
  int header_min, header_nat, scroll_min, scroll_nat;

  gtk_widget_measure (self->header, orientation, for_size, &header_min, &header_nat, NULL, NULL);
  gtk_widget_measure (self->scroller, orientation, -1, &scroll_min, &scroll_nat, NULL, NULL);
  if (orientation == GTK_ORIENTATION_VERTICAL)
    {
      *minimum = header_min + scroll_min;
      *natural = header_nat + scroll_nat;
    }
  else
    {
      *minimum = MAX (header_min, scroll_min);
      *natural = MAX (header_nat, scroll_nat);
    }
}

static void
dictate_stage_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  DictateStage *self = DICTATE_STAGE (widget);
  int header = 0, page;

  gtk_widget_measure (self->header, GTK_ORIENTATION_VERTICAL, width, NULL, &header, NULL, NULL);
  page = MAX (0, height - header);
  /* the page of the exchange going on, as tall as what shows of the scroller: laid out again in this frame */
  if (page != self->page_height)
    {
      self->page_height = page;
      gtk_widget_set_size_request (self->page, -1, page);
      follow (self);
    }
  gtk_widget_size_allocate (self->header, &(GtkAllocation) { 0, 0, width, header }, -1);
  gtk_widget_size_allocate (self->scroller, &(GtkAllocation) { 0, header, width, page }, -1);
}

/* --- taps */

/* the press is on a button (or a row) of its own, which takes it */
static gboolean
on_tappable (DictateStage *self, double x, double y)
{
  GtkWidget *picked = gtk_widget_pick (GTK_WIDGET (self), x, y, GTK_PICK_DEFAULT);

  for (GtkWidget *w = picked; w && w != GTK_WIDGET (self); w = gtk_widget_get_parent (w))
    if (VIBEOS_IS_PRESSABLE (w))
      return TRUE;
  return FALSE;
}

static void
on_pressed (GtkGestureClick *gesture, int n, double x, double y, DictateStage *self)
{
  self->held = FALSE;
}

static void
on_released (GtkGestureClick *gesture, int n, double x, double y, DictateStage *self)
{
  if (n == 1 && !self->held && !on_tappable (self, x, y))
    g_signal_emit (self, signals[SIGNAL_TAPPED], 0);
}

static void
on_long_press (GtkGestureLongPress *gesture, double x, double y, DictateStage *self)
{
  if (on_tappable (self, x, y))
    return;
  self->held = TRUE;
  g_signal_emit (self, signals[SIGNAL_LONG_PRESSED], 0);
}

static void
on_settings (VibeosPressable *button, DictateStage *self)
{
  g_signal_emit (self, signals[SIGNAL_SETTINGS], 0);
}

/* --- the bar */

static void
set_slot (GtkWidget *slot, GtkWidget *widget, GtkWidget *usual)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (slot)))
    gtk_box_remove (GTK_BOX (slot), child);
  gtk_box_append (GTK_BOX (slot), widget ? widget : usual);
}

void
dictate_stage_set_title (DictateStage *self, const char *title)
{
  gtk_label_set_text (GTK_LABEL (self->title), title);
}

void
dictate_stage_set_header_start (DictateStage *self, GtkWidget *widget)
{
  set_slot (self->start, widget, self->title);
}

void
dictate_stage_set_header_end (DictateStage *self, GtkWidget *widget)
{
  set_slot (self->end, widget, self->settings);
}

void
dictate_stage_set_call_label (DictateStage *self, const char *label)
{
  gtk_label_set_text (GTK_LABEL (self->call), label ? label : "");
  gtk_widget_set_visible (self->call, label != NULL);
  gtk_widget_set_visible (self->start, label == NULL);
  gtk_widget_set_visible (self->end, label == NULL);
}

/* --- the exchange going on */

static GtkWidget *
label_new (const char *css, gboolean wrap)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_widget_add_css_class (label, css);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  if (wrap)
    {
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
    }
  return label;
}

static void
set_label (GtkWidget *label, const char *text)
{
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
}

static GtkWidget *
bubble_new (const char *text)
{
  GtkWidget *bubble = label_new ("dictate-stage-mine", TRUE);

  /* as wide as its text, up to about three quarters of a phone's column */
  gtk_label_set_max_width_chars (GTK_LABEL (bubble), 30);
  gtk_widget_set_halign (bubble, GTK_ALIGN_END);
  set_label (bubble, text);
  return bubble;
}

void
dictate_stage_set_said (DictateStage *self, const char *said)
{
  set_label (self->mine, said);
  dictate_orb_set_small (DICTATE_ORB (self->orb), said && *said);
  if (said && *said)
    {
      /* TRANSLATORS: a bubble's name for screen readers; %s is what the user said */
      g_autofree char *name = g_strdup_printf (_("You: %s"), said);

      gtk_accessible_update_property (GTK_ACCESSIBLE (self->mine), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    }
  follow (self);
}

DictateOrb *
dictate_stage_get_orb (DictateStage *self)
{
  return DICTATE_ORB (self->orb);
}

void
dictate_stage_set_middle (DictateStage *self, GtkWidget *widget)
{
  if (self->card)
    gtk_box_remove (GTK_BOX (self->middle), self->card);
  self->card = widget;
  gtk_widget_set_visible (self->orb, widget == NULL);
  if (widget)
    {
      gtk_widget_set_valign (widget, GTK_ALIGN_CENTER);
      gtk_widget_set_vexpand (widget, TRUE);
      gtk_box_append (GTK_BOX (self->middle), widget);
    }
  follow (self);
}

void
dictate_stage_set_text_first (DictateStage *self, gboolean first)
{
  if (self->text_first == first)
    return;
  self->text_first = first;
  if (first)
    gtk_box_reorder_child_after (GTK_BOX (self->page), self->text, self->mine);
  else
    gtk_box_reorder_child_after (GTK_BOX (self->page), self->text, self->middle);
  gtk_widget_set_margin_top (self->text, first ? 12 : 0);
  /* the question's text is what it needs; under the orb it keeps its room, so the orb does not move */
  gtk_widget_set_size_request (self->text, -1, first ? -1 : 150);
}

void
dictate_stage_set_who (DictateStage *self, const char *who, gboolean error)
{
  set_label (self->who, who);
  if (error)
    gtk_widget_add_css_class (self->who, "dictate-stage-error");
  else
    gtk_widget_remove_css_class (self->who, "dictate-stage-error");
}

static void
set_headline (DictateStage *self, const char *headline)
{
  set_label (self->headline, headline);
  if (headline && g_utf8_strlen (headline, -1) > LONG_HEADLINE)
    gtk_widget_add_css_class (self->headline, "dictate-stage-headline-long");
  else
    gtk_widget_remove_css_class (self->headline, "dictate-stage-headline-long");
}

void
dictate_stage_set_text (DictateStage *self, const char *headline, const char *sub)
{
  gtk_label_set_use_markup (GTK_LABEL (self->headline), FALSE);
  set_headline (self, headline);
  set_label (self->sub, sub);
  follow (self);
}

void
dictate_stage_set_heard (DictateStage *self, const char *finished, const char *partial)
{
  GdkRGBA faint = vibeos_color (VIBEOS_OUTLINE);
  g_autofree char *color = gdk_rgba_to_string (&faint);
  g_autofree char *done = g_markup_escape_text (finished ? finished : "", -1);
  g_autofree char *going = g_markup_escape_text (partial ? partial : "", -1);
  g_autofree char *markup = *going ? g_strdup_printf ("%s%s<span foreground=\"%s\">%s…</span>", done,
                                                       *done ? " " : "", color, going)
                                   : g_strdup (done);

  gtk_label_set_markup (GTK_LABEL (self->headline), markup);
  gtk_widget_set_visible (self->headline, *markup);
  gtk_widget_remove_css_class (self->headline, "dictate-stage-headline-long");
  set_label (self->sub, NULL);
  follow (self);
}

static gboolean
closes (gunichar c)
{
  return c == '"' || c == '\'' || c == ')' || c == ']' || c == 0x201D || c == 0x2019 || c == 0x00BB;
}

/* the end of the first sentence: a full stop, question or exclamation mark (or an ellipsis, or a line's end) with a
 * space after it, and any closing quote or bracket before the space */
void
dictate_stage_split_reply (const char *reply, char **headline, char **rest)
{
  const char *p, *end = NULL;

  reply = reply ? reply : "";
  for (p = reply; *p && !end; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      const char *next = g_utf8_next_char (p);

      if (c == '\n')
        end = p;
      else if (c == '.' || c == '!' || c == '?' || c == 0x2026 || c == 0x3002 || c == 0xFF01 || c == 0xFF1F)
        {
          while (*next && closes (g_utf8_get_char (next)))
            next = g_utf8_next_char (next);
          if (*next && g_unichar_isspace (g_utf8_get_char (next)))
            end = next;
        }
    }
  if (!end)
    {
      *headline = g_strstrip (g_strdup (reply));
      *rest = g_strdup ("");
      return;
    }
  *headline = g_strstrip (g_strndup (reply, end - reply));
  *rest = g_strstrip (g_strdup (end));
}

void
dictate_stage_set_reply (DictateStage *self, const char *reply)
{
  g_autofree char *headline = NULL, *rest = NULL;

  dictate_stage_split_reply (reply, &headline, &rest);
  dictate_stage_set_text (self, headline, rest);
  if (reply && *reply)
    {
      /* TRANSLATORS: a reply's name for screen readers; %s is what Vibe answered */
      g_autofree char *name = g_strdup_printf (_("Vibe: %s"), reply);

      gtk_accessible_update_property (GTK_ACCESSIBLE (self->text), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    }
}

void
dictate_stage_set_written (DictateStage *self, const char *const *lines)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->written)))
    gtk_box_remove (GTK_BOX (self->written), child);
  for (guint i = 0; lines && lines[i]; i++)
    {
      GtkWidget *line = label_new ("dictate-stage-written-line", TRUE);

      set_label (line, lines[i]);
      gtk_box_append (GTK_BOX (self->written), line);
    }
  gtk_widget_set_visible (self->written, lines && lines[0]);
  follow (self);
}

static void
button_shape (DictateStage *self, GtkWidget *button)
{
  int height = self->large_buttons ? BUTTON_HEIGHT_LARGE : BUTTON_HEIGHT;

  gtk_widget_set_size_request (button, -1, height);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, height / 2.0);
  if (self->large_buttons)
    gtk_widget_add_css_class (button, "dictate-stage-button-large");
  else
    gtk_widget_remove_css_class (button, "dictate-stage-button-large");
}

void
dictate_stage_clear_buttons (DictateStage *self)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->buttons)))
    gtk_box_remove (GTK_BOX (self->buttons), child);
  gtk_widget_set_visible (self->buttons, FALSE);
}

GtkWidget *
dictate_stage_add_button (DictateStage *self, const char *label, DictateStageButtonKind kind)
{
  static const struct { VibeosColor fill; const char *css; } KINDS[] = {
    [DICTATE_STAGE_BUTTON] = { VIBEOS_SURFACE_CONTAINER_HIGH, NULL },
    [DICTATE_STAGE_BUTTON_PRIMARY] = { VIBEOS_PRIMARY, "dictate-stage-button-primary" },
    [DICTATE_STAGE_BUTTON_TONAL] = { VIBEOS_SECONDARY_CONTAINER, "dictate-stage-button-tonal" },
    [DICTATE_STAGE_BUTTON_DANGER] = { VIBEOS_ERROR_CONTAINER, "dictate-stage-button-danger" },
  };
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *button = vibeos_pressable_new (text);

  gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (button, "dictate-stage-button");
  if (KINDS[kind].css)
    gtk_widget_add_css_class (button, KINDS[kind].css);
  vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (button), KINDS[kind].fill, FALSE);
  gtk_widget_set_hexpand (button, TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  button_shape (self, button);
  gtk_box_append (GTK_BOX (self->buttons), button);
  gtk_widget_set_visible (self->buttons, TRUE);
  follow (self);
  return button;
}

void
dictate_stage_set_large_buttons (DictateStage *self, gboolean large)
{
  self->large_buttons = large;
  for (GtkWidget *b = gtk_widget_get_first_child (self->buttons); b; b = gtk_widget_get_next_sibling (b))
    button_shape (self, b);
}

void
dictate_stage_set_progress (DictateStage *self, double fraction)
{
  gtk_widget_set_visible (self->progress, fraction >= 0);
  if (fraction >= 0)
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->progress), CLAMP (fraction, 0, 1));
}

void
dictate_stage_set_hint (DictateStage *self, const char *hint)
{
  set_label (self->hint, hint);
}

void
dictate_stage_reset (DictateStage *self)
{
  dictate_stage_set_said (self, NULL);
  dictate_stage_set_middle (self, NULL);
  dictate_orb_set_look (DICTATE_ORB (self->orb), DICTATE_ORB_REST, NULL);
  dictate_stage_set_who (self, NULL, FALSE);
  dictate_stage_set_text (self, NULL, NULL);
  dictate_stage_set_written (self, NULL);
  dictate_stage_set_progress (self, -1);
  dictate_stage_clear_buttons (self);
  dictate_stage_set_large_buttons (self, FALSE);
  dictate_stage_set_hint (self, NULL);
  dictate_stage_set_call_label (self, NULL);
  gtk_accessible_reset_property (GTK_ACCESSIBLE (self->text), GTK_ACCESSIBLE_PROPERTY_LABEL);
}

/* --- the earlier exchanges */

void
dictate_stage_add_past (DictateStage *self, const char *said, const char *reply, const char *note)
{
  GtkWidget *entry = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  g_autofree char *headline = NULL, *rest = NULL;

  gtk_widget_add_css_class (entry, "dictate-stage-past");
  if (said && *said)
    gtk_box_append (GTK_BOX (entry), bubble_new (said));
  if (reply && *reply)
    {
      GtkWidget *who = label_new ("dictate-stage-who", FALSE), *line = label_new ("dictate-stage-past-headline", TRUE);
      GtkWidget *sub = label_new ("dictate-stage-sub", TRUE);

      dictate_stage_split_reply (reply, &headline, &rest);
      set_label (who, _("Vibe says"));
      set_label (line, headline);
      set_label (sub, rest);
      gtk_box_append (GTK_BOX (entry), who);
      gtk_box_append (GTK_BOX (entry), line);
      gtk_box_append (GTK_BOX (entry), sub);
    }
  if (note && *note)
    {
      GtkWidget *what = label_new ("dictate-stage-past-note", TRUE);

      set_label (what, note);
      gtk_box_append (GTK_BOX (entry), what);
    }
  gtk_box_append (GTK_BOX (self->past), entry);
  gtk_widget_set_visible (self->past, TRUE);
  follow (self);
}

void
dictate_stage_clear_past (DictateStage *self)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->past)))
    gtk_box_remove (GTK_BOX (self->past), child);
  gtk_widget_set_visible (self->past, FALSE);
  self->pinned = TRUE;
  follow (self);
}

gboolean
dictate_stage_has_past (DictateStage *self)
{
  return gtk_widget_get_first_child (self->past) != NULL;
}

/* --- a series' dots */

GtkWidget *
dictate_dots_new (guint n)
{
  GtkWidget *dots = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

  gtk_widget_add_css_class (dots, "dictate-dots");
  gtk_widget_set_valign (dots, GTK_ALIGN_CENTER);
  for (guint i = 0; i < n; i++)
    {
      GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

      gtk_widget_add_css_class (dot, "dictate-dot");
      gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (dots), dot);
    }
  dictate_dots_set (dots, 0);
  return dots;
}

void
dictate_dots_set (GtkWidget *dots, guint current)
{
  guint i = 0;
  g_autofree char *name = NULL;

  for (GtkWidget *dot = gtk_widget_get_first_child (dots); dot; dot = gtk_widget_get_next_sibling (dot), i++)
    {
      gtk_widget_remove_css_class (dot, "dictate-dot-done");
      gtk_widget_remove_css_class (dot, "dictate-dot-now");
      if (i < current)
        gtk_widget_add_css_class (dot, "dictate-dot-done");
      else if (i == current)
        gtk_widget_add_css_class (dot, "dictate-dot-now");
    }
  /* TRANSLATORS: the steps' dots for screen readers: step 2 of 6 */
  name = g_strdup_printf (_("Step %u of %u"), current + 1, i);
  gtk_accessible_update_property (GTK_ACCESSIBLE (dots), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

/* --- the widget */

static void
dictate_stage_dispose (GObject *object)
{
  DictateStage *self = DICTATE_STAGE (object);

  g_clear_object (&self->title);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->header, gtk_widget_unparent);
  g_clear_pointer (&self->scroller, gtk_widget_unparent);
  G_OBJECT_CLASS (dictate_stage_parent_class)->dispose (object);
}

static void
dictate_stage_class_init (DictateStageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dictate_stage_dispose;
  widget_class->measure = dictate_stage_measure;
  widget_class->size_allocate = dictate_stage_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "dictate-stage");
  signals[SIGNAL_TAPPED] = g_signal_new ("tapped", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
  signals[SIGNAL_LONG_PRESSED] = g_signal_new ("long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                               NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_SETTINGS] = g_signal_new ("settings", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 0);
}

static void
dictate_stage_init (DictateStage *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkGesture *click = gtk_gesture_click_new (), *hold = gtk_gesture_long_press_new ();
  GtkAdjustment *adjustment;

  gtk_widget_add_css_class (widget, "dictate-stage");
  self->pinned = TRUE;

  /* the bar: the app's name, and Settings */
  self->header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->header, "dictate-stage-header");
  self->start = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (self->start, TRUE);
  gtk_widget_set_valign (self->start, GTK_ALIGN_CENTER);
  self->title = g_object_ref_sink (label_new ("dictate-stage-title", FALSE));
  gtk_label_set_ellipsize (GTK_LABEL (self->title), PANGO_ELLIPSIZE_END);
  gtk_label_set_text (GTK_LABEL (self->title), _("Vibe"));
  gtk_box_append (GTK_BOX (self->start), self->title);
  gtk_box_append (GTK_BOX (self->header), self->start);
  self->call = label_new ("dictate-stage-call", FALSE);
  gtk_widget_set_hexpand (self->call, TRUE);
  gtk_widget_set_valign (self->call, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->call, FALSE);
  gtk_box_append (GTK_BOX (self->header), self->call);
  self->end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (self->end, GTK_ALIGN_CENTER);
  self->settings = g_object_ref_sink (vibeos_text_button_new (_("Settings")));
  g_signal_connect (self->settings, "clicked", G_CALLBACK (on_settings), self);
  gtk_box_append (GTK_BOX (self->end), self->settings);
  gtk_box_append (GTK_BOX (self->header), self->end);
  gtk_widget_set_parent (self->header, widget);

  /* the page of the exchange going on */
  self->page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->page, "dictate-stage-page");
  self->mine = bubble_new (NULL);
  gtk_widget_set_margin_top (self->mine, 8);
  gtk_box_append (GTK_BOX (self->page), self->mine);
  self->middle = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->middle, "dictate-stage-middle");
  gtk_widget_set_vexpand (self->middle, TRUE);
  self->orb = dictate_orb_new (NULL);
  gtk_widget_set_vexpand (self->orb, TRUE);
  gtk_widget_set_valign (self->orb, GTK_ALIGN_FILL);
  gtk_box_append (GTK_BOX (self->middle), self->orb);
  gtk_box_append (GTK_BOX (self->page), self->middle);
  self->text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_size_request (self->text, -1, 150);
  self->who = label_new ("dictate-stage-who", TRUE);
  self->headline = label_new ("dictate-stage-headline", TRUE);
  self->sub = label_new ("dictate-stage-sub", TRUE);
  gtk_box_append (GTK_BOX (self->text), self->who);
  gtk_box_append (GTK_BOX (self->text), self->headline);
  gtk_box_append (GTK_BOX (self->text), self->sub);
  gtk_box_append (GTK_BOX (self->page), self->text);
  self->written = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->written, "dictate-stage-written");
  gtk_box_append (GTK_BOX (self->page), self->written);
  self->progress = gtk_progress_bar_new ();
  gtk_widget_add_css_class (self->progress, "dictate-stage-progress");
  gtk_widget_set_visible (self->progress, FALSE);
  gtk_box_append (GTK_BOX (self->page), self->progress);
  self->buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_set_homogeneous (GTK_BOX (self->buttons), TRUE);
  gtk_widget_add_css_class (self->buttons, "dictate-stage-buttons");
  gtk_box_append (GTK_BOX (self->page), self->buttons);
  self->hint = label_new ("dictate-stage-hint", TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->hint), 0.5);
  gtk_label_set_justify (GTK_LABEL (self->hint), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (self->page), self->hint);

  /* the earlier exchanges over it, in a scroller */
  self->past = gtk_box_new (GTK_ORIENTATION_VERTICAL, 32);
  gtk_widget_add_css_class (self->past, "dictate-stage-pasts");
  self->column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (self->column), self->past);
  gtk_box_append (GTK_BOX (self->column), self->page);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->column);
  gtk_widget_set_parent (self->scroller, widget);
  adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller));
  g_signal_connect (adjustment, "value-changed", G_CALLBACK (on_scrolled), self);

  /* a tap anywhere but on a button: in the capture phase and never claimed, so the buttons still get theirs; a
   * scroll moves past the click's threshold and is no tap */
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (click));
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (hold), GTK_PHASE_CAPTURE);
  g_signal_connect (hold, "pressed", G_CALLBACK (on_long_press), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (hold));

  dictate_stage_reset (self);
  gtk_widget_set_visible (self->past, FALSE);
}

GtkWidget *
dictate_stage_new (DictateEngine *engine)
{
  DictateStage *self = g_object_new (DICTATE_TYPE_STAGE, NULL);

  /* the orb that listens: its halo follows the engine's loudness */
  dictate_orb_set_engine (DICTATE_ORB (self->orb), engine);
  return GTK_WIDGET (self);
}
