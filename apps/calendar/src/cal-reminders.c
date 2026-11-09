/* The reminder rows (cal-reminders.h). */
#include "cal-reminders.h"
#include "cal-format.h"
#include "cal-popup.h"

#include <glib/gi18n.h>
#include <neo.h>

/* V/arrays.xml reminder_minutes_values */
static const int minute_values[] = { 0, 1, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 360, 540, 720, 1440, 2880, 10080,
                                     20160, 40320 };

struct _CalReminders {
  GtkWidget parent;
  GtkWidget *box;
};

G_DEFINE_FINAL_TYPE (CalReminders, cal_reminders, GTK_TYPE_WIDGET)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* the values a row offers: the standard ones, and its own if it is not one of them (addMinutesToList) */
static GArray *
values_for (int minutes)
{
  GArray *values = g_array_new (FALSE, FALSE, sizeof (int));
  gboolean added = FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (minute_values); i++)
    {
      if (!added && minutes < minute_values[i])
        {
          g_array_append_val (values, minutes);
          added = TRUE;
        }
      if (minutes == minute_values[i])
        added = TRUE;
      g_array_append_val (values, minute_values[i]);
    }
  if (!added)
    g_array_append_val (values, minutes);
  return values;
}

static void
on_minutes_changed (CalSpinner *spinner, CalReminders *self)
{
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
on_remove (NeoPressable *button, CalReminders *self)
{
  GtkWidget *row = g_object_get_data (G_OBJECT (button), "cal-row");

  gtk_box_remove (GTK_BOX (self->box), row);
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
add_row (CalReminders *self, int minutes, CalMethod method)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *spinner = cal_spinner_new ();
  GtkWidget *remove = neo_icon_button_new ("cal-close-symbolic");
  g_autoptr(GArray) values = values_for (minutes);
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < values->len; i++)
    {
      g_ptr_array_add (labels, cal_format_reminder_minutes (g_array_index (values, int, i)));
      if (g_array_index (values, int, i) == minutes)
        cal_spinner_set_selected (CAL_SPINNER (spinner), (int) i);
    }
  cal_spinner_set_items (CAL_SPINNER (spinner), (const char *const *) labels->pdata, (int) labels->len);
  gtk_widget_set_hexpand (spinner, TRUE);
  gtk_widget_add_css_class (spinner, "cal-reminder-minutes");
  gtk_accessible_update_property (GTK_ACCESSIBLE (spinner), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                  _("Reminder time"), -1);
  g_object_set_data_full (G_OBJECT (row), "cal-values", g_array_ref (values), (GDestroyNotify) g_array_unref);
  g_object_set_data (G_OBJECT (row), "cal-spinner", spinner);
  g_object_set_data (G_OBJECT (row), "cal-method", GINT_TO_POINTER (method));
  g_signal_connect (spinner, "changed", G_CALLBACK (on_minutes_changed), self);
  gtk_accessible_update_property (GTK_ACCESSIBLE (remove), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Remove reminder"), -1);
  gtk_widget_add_css_class (remove, "cal-reminder-remove");
  g_object_set_data (G_OBJECT (remove), "cal-row", row);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_remove), self);
  gtk_widget_add_css_class (row, "cal-reminder-row");
  gtk_box_append (GTK_BOX (row), spinner);
  gtk_box_append (GTK_BOX (row), remove);
  gtk_box_append (GTK_BOX (self->box), row);
}

static void
cal_reminders_dispose (GObject *object)
{
  g_clear_pointer (&CAL_REMINDERS (object)->box, gtk_widget_unparent);
  G_OBJECT_CLASS (cal_reminders_parent_class)->dispose (object);
}

static void
cal_reminders_class_init (CalRemindersClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_reminders_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
cal_reminders_init (CalReminders *self)
{
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
}

GtkWidget *
cal_reminders_new (void)
{
  return g_object_new (CAL_TYPE_REMINDERS, NULL);
}

void
cal_reminders_set (CalReminders *self, GArray *reminders)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->box)))
    gtk_box_remove (GTK_BOX (self->box), child);
  for (guint i = 0; reminders && i < reminders->len && i < CAL_MAX_REMINDERS; i++)
    {
      CalReminder *reminder = &g_array_index (reminders, CalReminder, i);

      add_row (self, reminder->minutes, reminder->method);
    }
}

GArray *
cal_reminders_get (CalReminders *self)
{
  GArray *reminders = g_array_new (FALSE, FALSE, sizeof (CalReminder));

  for (GtkWidget *row = gtk_widget_get_first_child (self->box); row; row = gtk_widget_get_next_sibling (row))
    {
      GArray *values = g_object_get_data (G_OBJECT (row), "cal-values");
      int index = cal_spinner_get_selected (g_object_get_data (G_OBJECT (row), "cal-spinner"));
      CalReminder reminder = { g_array_index (values, int, MAX (index, 0)),
                               GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cal-method")) };

      g_array_append_val (reminders, reminder);
    }
  return reminders;
}

void
cal_reminders_add (CalReminders *self, int minutes)
{
  if (cal_reminders_count (self) >= CAL_MAX_REMINDERS)
    return;
  add_row (self, minutes, CAL_METHOD_NOTIFICATION);
  g_signal_emit (self, signals[CHANGED], 0);
}

guint
cal_reminders_count (CalReminders *self)
{
  guint n = 0;

  for (GtkWidget *row = gtk_widget_get_first_child (self->box); row; row = gtk_widget_get_next_sibling (row))
    n++;
  return n;
}
