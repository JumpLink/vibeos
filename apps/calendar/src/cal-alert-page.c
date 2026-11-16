/* The reminder screens (cal-alert-page.h): the reminders page, the snooze delay and Email guests.
 *
 * The reminders page is an old-style dark dialog, whatever the app's theme: a blue window title ("Calendar
 * notifications") with a blue line under it, on a dark dialog 95 % as wide as a portrait screen (65 % of a
 * landscape one), over the 60 % dim. The list (dividers between its rows) is over a divider and a button bar with
 * one borderless button, "Dismiss all". A row: a square in the calendar's display colour before the title (two
 * lines at most); under it, the time (one line, as cal_alert_when makes it) with the repeat icon after it for an
 * event with a repeat rule, and the place (one line, none when there is none). The title is white and the rest
 * grey, all three dimmed once the event has ended.
 *
 * The list is the reminders the service holds as fired (its GetFired()), by start then title, scrolled to the last
 * one. A tap on a row dismisses that reminder, opens its event and closes the page; "Dismiss all" dismisses them
 * all (their notifications go too) and closes it. The page follows the service's FiredChanged(). An emptied list
 * closes the page, and the page does not open when nothing is fired (a notification swiped before the app came
 * up).
 *
 * The dialog background and the dividers are flat colours; a row's press is the app's ripple; the repeat icon is
 * the app's own.
 *
 * The snooze delay: a time picker titled "Set snooze delay", at the default snooze delay as hours and minutes; OK
 * snoozes the reminder that long through the service's Snooze(), Cancel does nothing. The picker is libvibeos's
 * VibeosTimePicker (not the editor's CalTimePicker), always 24-hour.
 *
 * Email guests: a dialog in the app's dialog style, "Choose quick response" over the quick responses sorted and
 * "Write your own…"; a tap opens the mail app (the guests but the calendar's owner, "Re: <title>", the response as
 * the body). No message is shown when there is no mail app. */
#include "cal-alert-page.h"
#include "cal-alerts.h"
#include "cal-dialogs.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>
#include <string.h>

#define SERVICE         "vibeos.Calendar.Reminders"
#define SERVICE_PATH    "/vibeos/Calendar/Reminders"

/* --- the reminder service */

static GDBusConnection *
session (void)
{
  GApplication *app = g_application_get_default ();

  return app ? g_application_get_dbus_connection (app) : NULL;
}

static void
service_call (const char *method, GVariant *parameters)
{
  GDBusConnection *bus = session ();

  if (bus)
    g_dbus_connection_call (bus, SERVICE, SERVICE_PATH, SERVICE, method, parameters, NULL, G_DBUS_CALL_FLAGS_NONE,
                            -1, NULL, NULL, NULL);
  else if (parameters)
    g_variant_unref (g_variant_ref_sink (parameters));
}

/* --- the reminders page */

typedef struct {
  char *occurrence, *calendar, *uid, *rid;
  gint64 start;
} Item;

typedef struct {
  GtkWidget *window;
  VibeosDialog *dialog;            /* NULL until the first list has come */
  GtkWidget *scroll, *list, *dismiss_all;
  GDBusConnection *bus;
  guint changed;                /* the FiredChanged subscription */
  GCancellable *cancellable;    /* the GetFired call in flight */
} Page;

static Page *page;              /* one at a time */

static void refresh (Page *self);

static void
item_free (gpointer data)
{
  Item *item = data;

  g_free (item->occurrence);
  g_free (item->calendar);
  g_free (item->uid);
  g_free (item->rid);
  g_free (item);
}

static void
page_free (Page *self)
{
  if (self->changed)
    g_dbus_connection_signal_unsubscribe (self->bus, self->changed);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->bus);
  g_object_unref (self->window);
  if (page == self)
    page = NULL;
  g_free (self);
}

/* 95 % of a portrait screen's width, 65 % of a landscape one's: the dialog window's
 * width, its content's ellipsizing within it */
static void
holo_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
              int *minimum_baseline, int *natural_baseline)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (widget));

  gtk_widget_measure (child, orientation, for_size, minimum, natural, NULL, NULL);
  if (orientation == GTK_ORIENTATION_HORIZONTAL && root)
    {
      int width = gtk_widget_get_width (root), height = gtk_widget_get_height (root);

      *natural = MAX (*minimum, width * (width > height ? 65 : 95) / 100);
    }
}

static void
holo_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (gtk_widget_get_first_child (widget), width, height, baseline, NULL);
}

static void
on_response (VibeosDialog *dialog, int response, Page *self)
{
  page_free (self);
}

static void
on_row (VibeosPressable *row, Page *self)
{
  Item *item = g_object_get_data (G_OBJECT (row), "cal-item");
  const char *occurrences[] = { item->occurrence, NULL };

  /* dismissed, its event's info over the views, the page closed */
  service_call ("Dismiss", g_variant_new ("(^as)", occurrences));
  cal_window_open_reminder (self->window, item->calendar, item->uid, item->rid, item->start);
  vibeos_dialog_respond (self->dialog, VIBEOS_RESPONSE_CANCEL);
}

static void
on_dismiss_all (VibeosPressable *button, Page *self)
{
  /* every notification and fired reminder dismissed, the page closed */
  service_call ("DismissAll", NULL);
  vibeos_dialog_respond (self->dialog, VIBEOS_RESPONSE_CANCEL);
}

static GtkWidget *
text (const char *string, const char *css_class, int lines)
{
  GtkWidget *label = gtk_label_new (string);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  if (lines > 1)
    {
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_lines (GTK_LABEL (label), lines);
    }
  else
    gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

static void
draw_square (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  guint32 rgb = GPOINTER_TO_UINT (data);

  cairo_set_source_rgb (cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
  cairo_paint (cr);
}


/* a reminder's row */
static GtkWidget *
alert_row (Page *self, const char *occurrence, GVariant *fields)
{
  GSettings *settings = cal_window_get_settings (self->window);
  const char *calendar = "", *uid = "", *rid = "", *title = "", *location = "";
  gint64 start = 0, end = 0;
  gboolean all_day = FALSE, recurring = FALSE;
  guint32 color = 0;
  g_autofree char *when = NULL;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *square = gtk_drawing_area_new ();
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *row;
  Item *item = g_new0 (Item, 1);

  g_variant_lookup (fields, "calendar", "&s", &calendar);
  g_variant_lookup (fields, "uid", "&s", &uid);
  g_variant_lookup (fields, "rid", "&s", &rid);
  g_variant_lookup (fields, "title", "&s", &title);
  g_variant_lookup (fields, "location", "&s", &location);
  g_variant_lookup (fields, "start", "x", &start);
  g_variant_lookup (fields, "end", "x", &end);
  g_variant_lookup (fields, "all-day", "b", &all_day);
  g_variant_lookup (fields, "recurring", "b", &recurring);
  g_variant_lookup (fields, "color", "u", &color);

  /* the colour square: the calendar's colour as events show it */
  color = cal_display_color (color, g_settings_get_boolean (settings, "real-event-colors"));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (square), draw_square, GUINT_TO_POINTER (color), NULL);
  /* widget margins, not CSS ones: a size request in GTK 4 includes the CSS ones */
  gtk_widget_set_size_request (square, 12, 12);
  gtk_widget_set_margin_top (square, 6);
  gtk_widget_set_margin_start (square, 16);
  gtk_widget_set_margin_end (square, 16);
  gtk_widget_set_valign (square, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (head), square);
  gtk_box_append (GTK_BOX (head), text (*title ? title : _("(No title)"), "cal-alert-title", 2));
  gtk_box_append (GTK_BOX (box), head);

  when = cal_alert_when (start, end, all_day, g_get_real_time () / G_USEC_PER_SEC);
  gtk_box_append (GTK_BOX (line), text (when, "cal-alert-when", 1));
  if (recurring)
    {
      GtkWidget *repeat = gtk_image_new_from_icon_name ("cal-repeat-symbolic");

      gtk_image_set_pixel_size (GTK_IMAGE (repeat), 14);
      gtk_widget_add_css_class (repeat, "cal-alert-repeat");
      gtk_box_append (GTK_BOX (line), repeat);
    }
  gtk_widget_add_css_class (line, "cal-alert-line");
  gtk_box_append (GTK_BOX (box), line);
  if (*location)
    gtk_box_append (GTK_BOX (box), text (location, "cal-alert-where", 1));

  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  row = vibeos_pressable_new (box);
  gtk_widget_add_css_class (row, "cal-alert-row");
  /* an event that has ended: all three dimmed */
  if (end < g_get_real_time () / G_USEC_PER_SEC)
    gtk_widget_add_css_class (row, "past");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  item->occurrence = g_strdup (occurrence);
  item->calendar = g_strdup (calendar);
  item->uid = g_strdup (uid);
  item->rid = g_strdup (rid);
  item->start = start;
  g_object_set_data_full (G_OBJECT (row), "cal-item", item, item_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row), self);
  return row;
}

/* the last one in sight, once the list is laid out */
static gboolean
to_last (GtkWidget *scroll, GdkFrameClock *clock, gpointer data)
{
  GtkAdjustment *adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (scroll));

  if (gtk_widget_get_height (scroll) <= 0)
    return G_SOURCE_CONTINUE;
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (scroll), MAX (0, gtk_adjustment_get_upper (adjustment)
                                                                     - gtk_adjustment_get_page_size (adjustment)));
  return G_SOURCE_REMOVE;
}

static GtkWidget *
divider (const char *css_class)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (line, css_class);
  return line;
}

/* the dialog around the list: the window title, the list, the button bar */
static void
build (Page *self)
{
  GtkWidget *wrapper = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = text (_("Calendar notifications"), "cal-alerts-title", 1);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new (_("Dismiss all"));

  self->dialog = vibeos_dialog_new (NULL);
  gtk_widget_add_css_class (GTK_WIDGET (self->dialog), "cal-alerts");
  vibeos_dialog_set_flush (self->dialog, TRUE);

  gtk_box_append (GTK_BOX (content), title);
  gtk_box_append (GTK_BOX (content), divider ("cal-alerts-title-divider"));
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->list);
  gtk_widget_set_vexpand (self->scroll, TRUE);
  gtk_box_append (GTK_BOX (content), self->scroll);
  gtk_box_append (GTK_BOX (content), divider ("cal-alerts-divider"));
  self->dismiss_all = vibeos_pressable_new (label);
  gtk_widget_add_css_class (self->dismiss_all, "cal-alerts-button");
  gtk_widget_set_hexpand (self->dismiss_all, TRUE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->dismiss_all), VIBEOS_RIPPLE_BOUNDED, 0);
  g_signal_connect (self->dismiss_all, "clicked", G_CALLBACK (on_dismiss_all), self);
  gtk_box_append (GTK_BOX (bar), self->dismiss_all);
  gtk_box_append (GTK_BOX (content), bar);
  gtk_widget_add_css_class (content, "cal-alerts-content");

  gtk_box_append (GTK_BOX (wrapper), content);
  gtk_widget_set_layout_manager (wrapper, gtk_custom_layout_new (NULL, holo_measure, holo_allocate));
  vibeos_dialog_set_content (self->dialog, wrapper);
  g_signal_connect (self->dialog, "response", G_CALLBACK (on_response), self);
}

static void
on_fired (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autoptr(GVariant) list = NULL;
  GVariantIter iter;
  const char *occurrence;
  GVariant *fields;
  GtkWidget *child;
  Page *self = data;

  /* the page went (or a newer call took over) before the answer: self is not to be touched */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&self->cancellable);
  if (reply)
    list = g_variant_get_child_value (reply, 0);
  else
    g_warning ("%s: %s", SERVICE, error->message);
  if (!list || g_variant_n_children (list) == 0)
    {
      if (self->dialog)
        vibeos_dialog_respond (self->dialog, VIBEOS_RESPONSE_CANCEL);
      else
        page_free (self);
      return;
    }
  if (!self->dialog)
    build (self);
  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);
  g_variant_iter_init (&iter, list);
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &occurrence, &fields))
    {
      if (gtk_widget_get_first_child (self->list))
        gtk_box_append (GTK_BOX (self->list), divider ("cal-alerts-divider"));
      gtk_box_append (GTK_BOX (self->list), alert_row (self, occurrence, fields));
      g_variant_unref (fields);
    }
  if (!gtk_widget_get_parent (GTK_WIDGET (self->dialog)))
    vibeos_dialog_present (self->dialog, self->window);
  gtk_widget_add_tick_callback (self->scroll, to_last, NULL, NULL);
}

static void
refresh (Page *self)
{
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
  g_dbus_connection_call (self->bus, SERVICE, SERVICE_PATH, SERVICE, "GetFired", NULL, G_VARIANT_TYPE ("(a(sa{sv}))"),
                          G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable, on_fired, self);
}

static void
on_changed (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
            GVariant *parameters, gpointer data)
{
  refresh (data);
}

void
cal_alert_page_show (GtkWidget *window)
{
  GDBusConnection *bus = session ();
  Page *self;

  if (page)
    {
      refresh (page);
      return;
    }
  if (!bus)
    return;
  self = page = g_new0 (Page, 1);
  self->window = g_object_ref (window);
  self->bus = g_object_ref (bus);
  self->changed = g_dbus_connection_signal_subscribe (bus, NULL, SERVICE, "FiredChanged", SERVICE_PATH, NULL,
                                                      G_DBUS_SIGNAL_FLAGS_NONE, on_changed, self, NULL);
  refresh (self);
}

/* --- the snooze delay */

static char *snoozing;          /* the reminder the picker is open for (one at a time) */

static void
on_snooze_delay (VibeosDialog *dialog, int response, VibeosTimePicker *picker)
{
  int hour, minute;

  /* OK: snoozed with that delay; a cancel leaves the reminder as it was */
  vibeos_time_picker_get_time (picker, &hour, &minute);
  if (response == VIBEOS_RESPONSE_OK && snoozing)
    service_call ("Snooze", g_variant_new ("(si)", snoozing, hour * 60 + minute));
  g_clear_pointer (&snoozing, g_free);
}

void
cal_alert_snooze_delay (GtkWidget *window, const char *occurrence, int minutes)
{
  VibeosDialog *dialog;
  GtkWidget *picker;

  /* the default delay, 5 minutes when it is none */
  if (minutes < 0)
    minutes = 5;
  g_free (snoozing);
  snoozing = g_strdup (occurrence);
  /* libvibeos's time picker (not the editor's), titled, its clock always 24-hour: hours and minutes of delay, not a
   * time of day */
  dialog = vibeos_dialog_new (_("Set snooze delay"));
  vibeos_dialog_set_flush (dialog, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (dialog), "cal-snooze-dialog");
  picker = vibeos_time_picker_new (minutes / 60 % 24, minutes % 60, TRUE);
  vibeos_dialog_set_content (dialog, picker);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_snooze_delay), picker);
  vibeos_dialog_present (dialog, window);
}

/* --- Email guests */

typedef struct {
  GtkWidget *window;
  CalEvent *event;
  char *me;
  char **responses;
} Mailing;

static void
on_quick_response (int index, gpointer data)
{
  Mailing *mailing = data;

  if (index >= 0)
    {
      /* the last row, "Write your own…", has no body */
      const char *body = index < (int) g_strv_length (mailing->responses) ? mailing->responses[index] : NULL;
      g_autofree char *uri = cal_event_mail_uri (mailing->event, mailing->me, body);
      g_autoptr(GtkUriLauncher) launcher = uri ? gtk_uri_launcher_new (uri) : NULL;

      if (launcher)
        gtk_uri_launcher_launch (launcher, GTK_WINDOW (gtk_widget_get_root (mailing->window)), NULL, NULL, NULL);
    }
  g_object_unref (mailing->window);
  cal_event_unref (mailing->event);
  g_free (mailing->me);
  g_strfreev (mailing->responses);
  g_free (mailing);
}

static int
compare_responses (gconstpointer a, gconstpointer b)
{
  /* by UTF-16 code unit (UTF-8 bytes order the same but for the astral planes) */
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

void
cal_alert_email_guests (GtkWidget *window, GSettings *settings, const CalEvent *event, const char *me)
{
  Mailing *mailing = g_new0 (Mailing, 1);
  g_autoptr(GPtrArray) labels = g_ptr_array_new ();
  guint n;

  mailing->window = g_object_ref (window);
  mailing->event = cal_event_copy (event);
  mailing->me = g_strdup (me);
  mailing->responses = g_settings_get_strv (settings, "quick-responses");
  n = g_strv_length (mailing->responses);
  qsort (mailing->responses, n, sizeof (char *), compare_responses);
  for (guint i = 0; i < n; i++)
    g_ptr_array_add (labels, mailing->responses[i]);
  g_ptr_array_add (labels, (gpointer) _("Write your own…"));
  cal_choice_dialog (window, _("Choose quick response"), (const char *const *) labels->pdata, labels->len,
                     on_quick_response, mailing);
}
