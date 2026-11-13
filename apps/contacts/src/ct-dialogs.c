/* The Contacts dialogs (ct-dialogs.h). */
#include "ct-dialogs.h"
#include "ct-ui.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

typedef struct {
  CtDialogDone done;
  CtTextDone text_done;
  CtTextCheck check;
  gpointer data;
  GtkWidget *entry, *error, *ok;
} Dialog;

static void
dialog_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

static void
on_response (NeoDialog *neo, int response, Dialog *dialog)
{
  if (dialog->text_done)
    {
      g_autofree char *text = NULL;

      if (response == CT_DIALOG_OK)
        text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (dialog->entry))));
      dialog->text_done (text, dialog->data);
      return;
    }
  if (dialog->done)
    dialog->done (response == NEO_RESPONSE_CANCEL ? -1 : response, dialog->data);
}

static void
on_button (NeoPressable *button, NeoDialog *neo)
{
  neo_dialog_respond (neo, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "ct-response")));
}

/* the card: the content box the dialog's parts go in, as wide as the dialog kind says */
static GtkWidget *
dialog_card (GtkWidget *parent, NeoDialog *neo, CtDialogWidth width, const char *title)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  int screen = gtk_widget_get_width (GTK_WIDGET (gtk_widget_get_root (parent)));

  neo_dialog_set_flush (neo, TRUE);
  gtk_widget_add_css_class (box, "ct-dialog");
  gtk_widget_set_size_request (box, width == CT_DIALOG_FRAMEWORK ? (int) CT_DIALOG_W_DP
                                                                 : (int) (CT_DIALOG_MIN_W_FRAC * (screen > 0 ? screen : 360)),
                               -1);
  if (title)
    {
      GtkWidget *label = ct_label (title, "ct-dialog-title", CT_DIALOG_TITLE_SP);

      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_box_append (GTK_BOX (box), label);
    }
  neo_dialog_set_content (neo, box);
  return box;
}

/* the button bar: right-aligned, negative before positive */
static GtkWidget *
add_button (GtkWidget *box, NeoDialog *neo, const char *label, int response)
{
  GtkWidget *bar = g_object_get_data (G_OBJECT (box), "ct-buttons");
  GtkWidget *button = ct_text_button (label, "ct-dialog-button");

  if (!bar)
    {
      bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_halign (bar, GTK_ALIGN_END);
      gtk_widget_add_css_class (bar, "ct-dialog-buttons");
      gtk_box_append (GTK_BOX (box), bar);
      g_object_set_data (G_OBJECT (box), "ct-buttons", bar);
    }
  g_object_set_data (G_OBJECT (button), "ct-response", GINT_TO_POINTER (response));
  g_signal_connect (button, "clicked", G_CALLBACK (on_button), neo);
  gtk_box_append (GTK_BOX (bar), button);
  return button;
}

static Dialog *
watch_dialog (NeoDialog *neo, CtDialogDone done, gpointer data)
{
  Dialog *dialog = g_new0 (Dialog, 1);

  dialog->done = done;
  dialog->data = data;
  g_signal_connect_data (neo, "response", G_CALLBACK (on_response), dialog, dialog_free, 0);
  return dialog;
}

void
ct_confirm_dialog (GtkWidget *parent, const char *title, const char *message, const char *ok, CtDialogDone done,
                   gpointer data)
{
  NeoDialog *neo = neo_dialog_new (NULL);
  GtkWidget *box = dialog_card (parent, neo, CT_DIALOG_APPCOMPAT, title);
  GtkWidget *text = ct_label (message, "ct-dialog-message", 16);

  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  /* without a title the message keeps the room above it that the title would take */
  if (!title)
    gtk_widget_add_css_class (text, "no-title");
  gtk_box_append (GTK_BOX (box), text);
  add_button (box, neo, _("Cancel"), NEO_RESPONSE_CANCEL);
  add_button (box, neo, ok, CT_DIALOG_OK);
  watch_dialog (neo, done, data);
  neo_dialog_present (neo, parent);
}

static void
on_row (NeoPressable *row, NeoDialog *neo)
{
  neo_dialog_respond (neo, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-response")));
}

static GtkWidget *
dialog_row (NeoDialog *neo, GtkWidget *child, int index, const char *css_class)
{
  GtkWidget *row = neo_pressable_new (child);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

  gtk_widget_add_css_class (row, css_class);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  g_object_set_data (G_OBJECT (row), "ct-response", GINT_TO_POINTER (index));
  g_signal_connect (row, "clicked", G_CALLBACK (on_row), neo);
  return row;
}

void
ct_list_dialog (GtkWidget *parent, CtDialogWidth width, const char *title, const char *const *labels, int n_labels,
                gboolean cancel, CtDialogDone done, gpointer data)
{
  NeoDialog *neo = neo_dialog_new (NULL);
  GtkWidget *box = dialog_card (parent, neo, width, title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* a gap under the title */
  gtk_widget_add_css_class (list, "ct-dialog-list");
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *text = ct_label (labels[i], "ct-dialog-item-text", 16);

      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (list), dialog_row (neo, text, i, "ct-dialog-item"));
    }
  gtk_box_append (GTK_BOX (box), list);
  if (cancel)
    add_button (box, neo, _("Cancel"), NEO_RESPONSE_CANCEL);
  else
    gtk_widget_add_css_class (box, "no-buttons");
  watch_dialog (neo, done, data);
  neo_dialog_present (neo, parent);
}

void
ct_radio_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                 CtDialogDone done, gpointer data)
{
  NeoDialog *neo = neo_dialog_new (NULL);
  GtkWidget *box = dialog_card (parent, neo, CT_DIALOG_APPCOMPAT, title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();

  gtk_widget_add_css_class (list, "ct-dialog-list");
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *radio = gtk_image_new_from_icon_name (i == selected ? "ct-radio-on-symbolic"
                                                                     : "ct-radio-off-symbolic");
      GtkWidget *text = ct_label (labels[i], "ct-dialog-choice-text", 18);

      /* the radio icon has a margin round its circle */
      gtk_image_set_pixel_size (GTK_IMAGE (radio), 24);
      gtk_widget_add_css_class (radio, i == selected ? "ct-radio-on" : "ct-radio-off");
      gtk_widget_set_valign (radio, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_label_set_wrap (GTK_LABEL (text), TRUE);
      gtk_widget_set_hexpand (text, TRUE);
      gtk_box_append (GTK_BOX (line), radio);
      gtk_box_append (GTK_BOX (line), text);
      gtk_box_append (GTK_BOX (list), dialog_row (neo, line, i, "ct-dialog-choice"));
    }
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  gtk_box_append (GTK_BOX (box), scroll);
  gtk_widget_add_css_class (box, "no-buttons");
  watch_dialog (neo, done, data);
  neo_dialog_present (neo, parent);
}

static void
on_text_changed (GtkEditable *entry, Dialog *dialog)
{
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (entry)));
  const char *error = *text && dialog->check ? dialog->check (text, dialog->data) : NULL;

  gtk_label_set_label (GTK_LABEL (dialog->error), error ? error : "");
  gtk_widget_set_visible (dialog->error, error != NULL);
  gtk_widget_set_sensitive (dialog->ok, *text && !error);
}

static void
on_text_activate (GtkText *entry, NeoDialog *neo)
{
  Dialog *dialog = g_object_get_data (G_OBJECT (entry), "ct-dialog");

  if (gtk_widget_get_sensitive (dialog->ok))
    neo_dialog_respond (neo, CT_DIALOG_OK);
}

void
ct_text_dialog (GtkWidget *parent, const char *title, const char *text, const char *hint, int max_length,
                CtTextCheck check, CtTextDone done, gpointer data)
{
  NeoDialog *neo = neo_dialog_new (NULL);
  GtkWidget *box = dialog_card (parent, neo, CT_DIALOG_APPCOMPAT, title);
  GtkWidget *field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  Dialog *dialog = watch_dialog (neo, NULL, data);

  dialog->text_done = done;
  dialog->check = check;
  dialog->entry = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (dialog->entry), hint);
  if (max_length > 0)
    gtk_text_set_max_length (GTK_TEXT (dialog->entry), max_length);
  gtk_editable_set_text (GTK_EDITABLE (dialog->entry), text ? text : "");
  gtk_editable_set_position (GTK_EDITABLE (dialog->entry), -1);
  gtk_widget_add_css_class (dialog->entry, "ct-dialog-entry");
  g_object_set_data (G_OBJECT (dialog->entry), "ct-dialog", dialog);
  g_signal_connect (dialog->entry, "changed", G_CALLBACK (on_text_changed), dialog);
  g_signal_connect (dialog->entry, "activate", G_CALLBACK (on_text_activate), neo);
  /* the field, then its error line in the error colour */
  dialog->error = ct_label ("", "ct-dialog-error", 12);
  gtk_widget_set_visible (dialog->error, FALSE);
  gtk_widget_add_css_class (field, "ct-dialog-field");
  gtk_box_append (GTK_BOX (field), dialog->entry);
  gtk_box_append (GTK_BOX (field), dialog->error);
  gtk_box_append (GTK_BOX (box), field);
  add_button (box, neo, _("Cancel"), NEO_RESPONSE_CANCEL);
  dialog->ok = add_button (box, neo, _("OK"), CT_DIALOG_OK);
  on_text_changed (GTK_EDITABLE (dialog->entry), dialog);
  neo_dialog_present (neo, parent);
}

/* ===== the date dialog ========================================================================================= */

/* CtWheel: one number wheel, drawn: three rows of values with the chosen one in the middle between two
 * dividers; a vertical drag turns it (snapping to a value when let go), a tap above or below the
 * middle steps it by one. The month and the day wrap round; the year does not. */
#define CT_TYPE_WHEEL (ct_wheel_get_type ())
G_DECLARE_FINAL_TYPE (CtWheel, ct_wheel, CT, WHEEL, GtkWidget)

typedef char *(*CtWheelFormat) (int value);

struct _CtWheel {
  GtkWidget parent;
  int min, max, value;
  gboolean wrap;
  CtWheelFormat format;
  double drag;                  /* the drag's offset in px, between values */
  int drag_from;
  void (*changed) (CtWheel *wheel, gpointer data);
  gpointer data;
};

G_DEFINE_FINAL_TYPE (CtWheel, ct_wheel, GTK_TYPE_WIDGET)

#define WHEEL_ROW 60.0          /* a third of the picker's height */
#define WHEEL_DIVIDERS 48.0     /* the distance between the two dividers */

static int
wheel_step (CtWheel *self, int value, int by)
{
  int span = self->max - self->min + 1;

  value += by;
  if (self->wrap)
    return self->min + ((value - self->min) % span + span) % span;
  return CLAMP (value, self->min, self->max);
}

static void
wheel_set (CtWheel *self, int value)
{
  value = CLAMP (value, self->min, self->max);
  if (value == self->value)
    return;
  self->value = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->changed)
    self->changed (self, self->data);
}

static void
ct_wheel_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? (int) (3 * WHEEL_ROW) : 64;
}

static void
ct_wheel_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtWheel *self = CT_WHEEL (widget);
  double width = gtk_widget_get_width (widget), middle = gtk_widget_get_height (widget) / 2;
  TextStyle style = CT_TEXT (18, CT_WEIGHT_REGULAR);
  GdkRGBA text = ct_rgba (CT_C_DIALOG_TEXT), divider = ct_rgba (CT_C_LIST_SECONDARY);

  for (int row = -2; row <= 2; row++)
    {
      int value = wheel_step (self, self->value, row);
      double y = middle + row * WHEEL_ROW + self->drag;
      g_autofree char *label = NULL;
      g_autoptr(PangoLayout) layout = NULL;
      double distance = fabs (y - middle) / WHEEL_ROW;
      GdkRGBA color = text;

      /* the year stops at its ends; the rows past them stay empty */
      if (!self->wrap && (self->value + row < self->min || self->value + row > self->max))
        continue;
      if (distance > 1.5)
        continue;
      label = self->format (value);
      layout = text_layout (widget, style, label, 0);
      /* the fading edges: the rows away from the middle dim */
      color.alpha *= (float) MAX (1 - 0.6 * MIN (distance, 1), 0);
      text_draw (snapshot, layout, style, (width - text_width (layout)) / 2, y - text_box_height (layout, style) / 2,
                 &color);
    }
  gtk_snapshot_append_color (snapshot, &divider,
                             &GRAPHENE_RECT_INIT (0, (float) (middle - WHEEL_DIVIDERS / 2 - 1), (float) width, 2));
  gtk_snapshot_append_color (snapshot, &divider,
                             &GRAPHENE_RECT_INIT (0, (float) (middle + WHEEL_DIVIDERS / 2 - 1), (float) width, 2));
}

static void
on_wheel_drag_begin (GtkGestureDrag *gesture, double x, double y, CtWheel *self)
{
  self->drag_from = self->value;
  self->drag = 0;
}

static void
on_wheel_drag_update (GtkGestureDrag *gesture, double dx, double dy, CtWheel *self)
{
  int rows = (int) floor ((dy + WHEEL_ROW / 2) / WHEEL_ROW);
  int value = wheel_step (self, self->drag_from, -rows);

  if (fabs (dy) < CT_TOUCH_SLOP_DP)
    return;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  /* the wheel stops at a non-wrapping end */
  if (!self->wrap && value == wheel_step (self, self->drag_from, -rows + (rows > 0 ? 1 : -1)))
    rows = self->drag_from - value;
  self->drag = dy - rows * WHEEL_ROW;
  if (!self->wrap && (value == self->min || value == self->max))
    self->drag = CLAMP (self->drag, value == self->max ? 0 : -WHEEL_ROW / 2, value == self->min ? 0 : WHEEL_ROW / 2);
  wheel_set (self, value);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_wheel_drag_end (GtkGestureDrag *gesture, double dx, double dy, CtWheel *self)
{
  self->drag = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_wheel_tap (GtkGestureClick *gesture, int n, double x, double y, CtWheel *self)
{
  double middle = gtk_widget_get_height (GTK_WIDGET (self)) / 2;

  if (y < middle - WHEEL_DIVIDERS / 2)
    wheel_set (self, wheel_step (self, self->value, -1));
  else if (y > middle + WHEEL_DIVIDERS / 2)
    wheel_set (self, wheel_step (self, self->value, 1));
}

static void
ct_wheel_class_init (CtWheelClass *klass)
{
  GTK_WIDGET_CLASS (klass)->measure = ct_wheel_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = ct_wheel_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-wheel");
}

static void
ct_wheel_init (CtWheel *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkGesture *tap = gtk_gesture_click_new ();

  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_wheel_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_wheel_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_wheel_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  g_signal_connect (tap, "released", G_CALLBACK (on_wheel_tap), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (tap));
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

static GtkWidget *
wheel_new (int min, int max, int value, gboolean wrap, CtWheelFormat format, double width)
{
  CtWheel *self = g_object_new (CT_TYPE_WHEEL, NULL);

  self->min = min;
  self->max = max;
  self->value = CLAMP (value, min, max);
  self->wrap = wrap;
  self->format = format;
  gtk_widget_set_size_request (GTK_WIDGET (self), (int) width, -1);
  return GTK_WIDGET (self);
}

/* the month's short name */
static char *
format_month (int month)
{
  g_autoptr(GDateTime) date = g_date_time_new_local (2000, month, 1, 0, 0, 0);

  return g_date_time_format (date, "%b");
}

static char *
format_day (int day)
{
  return g_strdup_printf ("%02d", day);
}

static char *
format_year (int year)
{
  return g_strdup_printf ("%d", year);
}

typedef struct {
  CtDateDone done;
  gpointer data;
  GtkWidget *title, *month, *day, *year, *include;
  gboolean include_year;
} DateDialog;

static int
days_in_month (int year, int month)
{
  return g_date_get_days_in_month ((GDateMonth) month, (GDateYear) (year > 0 ? year : 2000));
}

/* the title: the full date ("Saturday, November 7, 2026"), or the date without its year ("November 7") */
static void
date_update (DateDialog *dialog)
{
  int year = dialog->include_year ? CT_WHEEL (dialog->year)->value : 2000;
  int month = CT_WHEEL (dialog->month)->value;
  CtWheel *day = CT_WHEEL (dialog->day);
  g_autoptr(GDateTime) date = NULL;
  g_autofree char *text = NULL;

  /* the days of this month (February of a leap year, when there is no year) */
  day->max = days_in_month (year, month);
  if (day->value > day->max)
    day->value = day->max;
  gtk_widget_queue_draw (dialog->day);
  date = g_date_time_new_local (year, month, day->value, 0, 0, 0);
  /* Translators: the date picker's title, the full date (strftime; %-d is the day without a leading zero) */
  text = g_date_time_format (date, dialog->include_year ? _("%A, %B %-d, %Y") : _("%B %-d"));
  gtk_label_set_label (GTK_LABEL (dialog->title), text);
}

static void
on_wheel_changed (CtWheel *wheel, gpointer data)
{
  date_update (data);
}

/* unchecking "Include year" takes the year's wheel away (GONE) */
static void
on_include_year (NeoPressable *row, DateDialog *dialog)
{
  dialog->include_year = !dialog->include_year;
  gtk_widget_set_visible (dialog->year, dialog->include_year);
  ct_check_set_active (dialog->include, dialog->include_year);
  date_update (dialog);
}

static void
on_date_response (NeoDialog *neo, int response, DateDialog *dialog)
{
  if (response == CT_DIALOG_OK && dialog->done)
    dialog->done (dialog->include_year ? CT_WHEEL (dialog->year)->value : 0, CT_WHEEL (dialog->month)->value,
                  CT_WHEEL (dialog->day)->value, dialog->data);
}

static void
date_dialog_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

void
ct_date_dialog (GtkWidget *parent, int year, int month, int day, gboolean year_optional, CtDateDone done,
                gpointer data)
{
  NeoDialog *neo = neo_dialog_new (NULL);
  GtkWidget *box = dialog_card (parent, neo, CT_DIALOG_APPCOMPAT, " ");
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *wheels = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  DateDialog *dialog = g_new0 (DateDialog, 1);
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  /* Translators: the order of the date picker's wheels: M month, D day, Y year (German: DMY) */
  const char *order = _("MDY");

  dialog->done = done;
  dialog->data = data;
  dialog->include_year = year > 0 || !year_optional;
  /* an empty field opens on today */
  if (month < 1 || day < 1)
    {
      year = g_date_time_get_year (now);
      month = g_date_time_get_month (now);
      day = g_date_time_get_day_of_month (now);
      dialog->include_year = TRUE;
    }
  dialog->title = gtk_widget_get_first_child (box);
  dialog->month = wheel_new (1, 12, month, TRUE, format_month, 80);
  dialog->day = wheel_new (1, days_in_month (year, month), day, TRUE, format_day, 80);
  dialog->year = wheel_new (1900, 2100, year > 0 ? year : g_date_time_get_year (now), FALSE, format_year, 95);
  for (CtWheel *w = CT_WHEEL (dialog->month); w; w = w == CT_WHEEL (dialog->month) ? CT_WHEEL (dialog->day)
                                                    : w == CT_WHEEL (dialog->day) ? CT_WHEEL (dialog->year) : NULL)
    {
      w->changed = on_wheel_changed;
      w->data = dialog;
      gtk_widget_set_margin_start (GTK_WIDGET (w), 1);
      gtk_widget_set_margin_end (GTK_WIDGET (w), 1);
    }
  for (const char *c = order; *c; c++)
    gtk_box_append (GTK_BOX (wheels), *c == 'M' ? dialog->month : *c == 'D' ? dialog->day : dialog->year);
  gtk_widget_set_halign (wheels, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (dialog->year, dialog->include_year);
  gtk_widget_add_css_class (column, "ct-date-picker");
  gtk_widget_set_size_request (column, 270, -1);
  gtk_widget_set_halign (column, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (column), wheels);
  if (year_optional)
    {
      /* the "Include year" check box */
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *check = ct_check_new (CT_C_CHECK_ON, CT_C_CHECK_OFF);
      GtkWidget *label = ct_label (_("Include year"), "ct-date-include", 22);
      GtkWidget *row = neo_pressable_new (line);
      GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (line), check);
      gtk_box_append (GTK_BOX (line), label);
      gtk_widget_add_css_class (row, "ct-date-include-row");
      neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
      neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
      g_signal_connect (row, "clicked", G_CALLBACK (on_include_year), dialog);
      dialog->include = check;
      ct_check_set_active (check, dialog->include_year);
      gtk_box_append (GTK_BOX (column), row);
    }
  gtk_box_append (GTK_BOX (box), column);
  add_button (box, neo, _("Cancel"), NEO_RESPONSE_CANCEL);
  add_button (box, neo, _("Set"), CT_DIALOG_OK);
  g_signal_connect_data (neo, "response", G_CALLBACK (on_date_response), dialog, date_dialog_free, 0);
  date_update (dialog);
  neo_dialog_present (neo, parent);
}
