/* The event's info screen (event-info.h; spec/calendar.md §6). */
#include "event-info.h"
#include "cal-colors.h"
#include "cal-dialogs.h"
#include "cal-format.h"
#include "cal-page.h"
#include "cal-reminders.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <string.h>
#include <spongy-gtk.h>

typedef struct {
  GtkWidget *page, *window;
  CalStore *store;
  CalEvent *event;
  GArray *saved_reminders;      /* the reminders as the calendar has them */
  GCancellable *cancellable;

  GtkWidget *edit, *delete;
  GtkWidget *headline, *title, *when, *repeat, *where;
  GtkWidget *calendar_row, *calendar_name, *description, *url;
  GtkWidget *reminders_header, *reminders, *add_reminder;
} Info;

static void
info_free (gpointer data)
{
  Info *info = data;

  g_cancellable_cancel (info->cancellable);
  g_clear_object (&info->cancellable);
  g_signal_handlers_disconnect_by_data (info->store, info);
  g_clear_pointer (&info->event, cal_event_unref);
  g_clear_pointer (&info->saved_reminders, g_array_unref);
  g_object_unref (info->store);
  g_free (info);
}

/* --- the headline's colour: a box that paints itself in the event's display colour */

#define CAL_TYPE_COLOR_BOX (cal_color_box_get_type ())
G_DECLARE_FINAL_TYPE (CalColorBox, cal_color_box, CAL, COLOR_BOX, GtkBox)

struct _CalColorBox {
  GtkBox parent;
  GdkRGBA color;
};

G_DEFINE_FINAL_TYPE (CalColorBox, cal_color_box, GTK_TYPE_BOX)

static void
cal_color_box_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalColorBox *self = CAL_COLOR_BOX (widget);

  gtk_snapshot_append_color (snapshot, &self->color,
                             &GRAPHENE_RECT_INIT (0, 0, (float) gtk_widget_get_width (widget),
                                                  (float) gtk_widget_get_height (widget)));
  GTK_WIDGET_CLASS (cal_color_box_parent_class)->snapshot (widget, snapshot);
}

static void
cal_color_box_class_init (CalColorBoxClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = cal_color_box_snapshot;
}

static void
cal_color_box_init (CalColorBox *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
}

/* --- filling it in */

static gboolean
reminders_equal (GArray *a, GArray *b)
{
  if (!a || !b || a->len != b->len)
    return a == b || (!a && b && !b->len) || (!b && a && !a->len);
  for (guint i = 0; i < a->len; i++)
    if (g_array_index (a, CalReminder, i).minutes != g_array_index (b, CalReminder, i).minutes
        || g_array_index (a, CalReminder, i).method != g_array_index (b, CalReminder, i).method)
      return FALSE;
  return TRUE;
}

static void
set_label (GtkWidget *label, const char *text)
{
  gtk_label_set_label (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
}

static void
show_event (Info *info)
{
  CalEvent *event = info->event;
  const CalCalendar *calendar = cal_store_find_calendar (info->store, event->calendar);
  g_autofree char *when = cal_format_event_when (event->start, event->end, event->all_day,
                                                 g_get_real_time () / G_USEC_PER_SEC);
  g_autofree char *repeat = event->recurring ? cal_format_rrule (event->rrule, event->start) : NULL;
  gboolean writable = !event->readonly;

  CAL_COLOR_BOX (info->headline)->color = cal_argb (0xff000000 | cal_event_display_color (event));
  gtk_widget_queue_draw (info->headline);
  set_label (info->title, event->title && *event->title ? event->title : _("(No title)"));
  set_label (info->when, when);
  set_label (info->repeat, repeat);
  set_label (info->where, event->location);

  /* "Calendar: <name>" (calendar_container shows when the calendar is known) */
  gtk_widget_set_visible (info->calendar_row, calendar != NULL);
  if (calendar)
    gtk_label_set_label (GTK_LABEL (info->calendar_name), calendar->name);
  set_label (info->description, event->description);
  set_label (info->url, event->url);

  /* Edit (the organizer may edit: on this phone, whoever can write the calendar) and Delete need a writable one */
  gtk_widget_set_visible (info->edit, writable);
  gtk_widget_set_visible (info->delete, writable);

  /* The reminders: Etar keeps them in its own table, so even a read-only calendar's events have them. Here they are
   * the event's VALARMs, which only a writable calendar can store: a read-only one shows none, and no "Add". */
  g_clear_pointer (&info->saved_reminders, g_array_unref);
  info->saved_reminders = g_array_copy (event->reminders);
  cal_reminders_set (CAL_REMINDERS (info->reminders), event->reminders);
  gtk_widget_set_visible (info->reminders_header, writable);
  gtk_widget_set_visible (info->reminders, writable);
  gtk_widget_set_visible (info->add_reminder, writable && cal_reminders_count (CAL_REMINDERS (info->reminders))
                                                             < CAL_MAX_REMINDERS);
}

/* the store changed: read the event again (it may have been edited, or be gone) */
static void
on_found (GObject *source, GAsyncResult *result, gpointer data)
{
  Info *info = data;
  g_autoptr(GError) error = NULL;
  CalEvent *event = cal_store_find_event_finish (CAL_STORE (source), result, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (!event)
    {
      /* "Event not found.": it went from under us */
      cal_window_pop (info->window);
      return;
    }
  /* the reminders the user is changing stay as they are (and still differ from the calendar's) */
  {
    GArray *changing = cal_reminders_get (CAL_REMINDERS (info->reminders));
    GArray *stored = g_array_copy (event->reminders);
    gboolean changed = !reminders_equal (changing, info->saved_reminders);

    if (changed)
      {
        g_array_unref (event->reminders);
        event->reminders = g_array_ref (changing);
      }
    cal_event_unref (info->event);
    info->event = event;
    show_event (info);
    g_clear_pointer (&info->saved_reminders, g_array_unref);
    info->saved_reminders = stored;
    g_array_unref (changing);
  }
}

static void
on_store_changed (CalStore *store, Info *info)
{
  g_cancellable_cancel (info->cancellable);
  g_clear_object (&info->cancellable);
  info->cancellable = g_cancellable_new ();
  cal_store_find_event (store, info->event->calendar, info->event->uid, info->event->rid, info->event->start,
                        info->cancellable, on_found, info);
}

/* --- the reminders: saved on the way back (EventInfoFragment.saveReminders, onStop) */

static void
on_reminders_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!cal_store_save_finish (CAL_STORE (source), result, &error))
    g_warning ("reminders not saved: %s", error->message);
}

static void
save_reminders (Info *info)
{
  g_autoptr(GArray) now = cal_reminders_get (CAL_REMINDERS (info->reminders));
  CalEvent *edited;

  if (info->event->readonly || reminders_equal (now, info->saved_reminders))
    return;
  /* the reminders are the series' (Etar's Reminders rows belong to the event, not an instance) */
  edited = cal_event_new ();
  edited->calendar = g_strdup (info->event->calendar);
  edited->uid = g_strdup (info->event->uid);
  edited->rid = g_strdup (info->event->rid);
  edited->title = g_strdup (info->event->title);
  edited->location = g_strdup (info->event->location);
  edited->description = g_strdup (info->event->description);
  edited->url = g_strdup (info->event->url);
  edited->start = info->event->start;
  edited->end = info->event->end;
  edited->all_day = info->event->all_day;
  edited->color = info->event->color;
  edited->recurring = info->event->recurring;
  edited->rrule = g_strdup (info->event->rrule);
  edited->tzid = g_strdup (info->event->tzid);
  g_array_unref (edited->reminders);
  edited->reminders = g_array_ref (now);
  cal_store_save (info->store, info->event, edited, CAL_SCOPE_ALL, on_reminders_saved, NULL);
  cal_event_unref (edited);
  g_clear_pointer (&info->saved_reminders, g_array_unref);
  info->saved_reminders = g_array_copy (now);
}

static gboolean
on_back (CalPage *page, gpointer data)
{
  save_reminders (data);
  return FALSE;
}

static void
on_reminders_changed (CalReminders *reminders, Info *info)
{
  gtk_widget_set_visible (info->add_reminder, cal_reminders_count (reminders) < CAL_MAX_REMINDERS);
}

static void
on_add_reminder (NeoPressable *button, Info *info)
{
  /* EventInfoFragment.addReminder: the default reminder time, else 10 minutes */
  int minutes = g_settings_get_int (cal_window_get_settings (info->window), "default-reminder");

  cal_reminders_add (CAL_REMINDERS (info->reminders), minutes >= 0 ? minutes : 10);
}

/* --- the actions */

static void
on_edit (NeoPressable *button, Info *info)
{
  g_autoptr(CalEvent) event = cal_event_ref (info->event);
  GtkWidget *window = info->window;

  /* doEdit, then the info screen finishes: the editor takes its place */
  save_reminders (info);
  cal_window_pop (window);
  cal_window_edit_event (window, event, FALSE);
}

static void
on_duplicate (NeoPressable *button, Info *info)
{
  g_autoptr(CalEvent) event = cal_event_ref (info->event);
  GtkWidget *window = info->window;

  save_reminders (info);
  cal_window_pop (window);
  cal_window_edit_event (window, event, TRUE);
}

static void
on_deleted (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!cal_store_delete_finish (CAL_STORE (source), result, &error))
    g_warning ("delete: %s", error->message);
}

typedef struct {
  GtkWidget *window;
  CalStore *store;
  CalEvent *event;
} Deleting;

static void
on_delete_answer (int index, gpointer data)
{
  Deleting *deleting = data;
  CalScope scopes[] = { CAL_SCOPE_THIS, CAL_SCOPE_THIS_AND_FUTURE, CAL_SCOPE_ALL };

  if (index >= 0)
    {
      /* a single event: OK is index 0, and its scope is the whole event */
      CalScope scope = deleting->event->recurring ? scopes[index] : CAL_SCOPE_ALL;

      cal_store_delete (deleting->store, deleting->event, scope, on_deleted, NULL);
      /* exitWhenDone: back to the views */
      cal_window_pop (deleting->window);
    }
  g_object_unref (deleting->store);
  cal_event_unref (deleting->event);
  g_free (deleting);
}

static void
on_delete (NeoPressable *button, Info *info)
{
  Deleting *deleting = g_new0 (Deleting, 1);

  deleting->window = info->window;
  deleting->store = g_object_ref (info->store);
  deleting->event = cal_event_ref (info->event);
  if (info->event->recurring)
    {
      static const char *labels[] = { N_("Only this event"), N_("This and future events"), N_("All events") };
      const char *translated[] = { _(labels[0]), _(labels[1]), _(labels[2]) };
      g_autofree char *title = g_strdup_printf (_("Delete %s?"), info->event->title && *info->event->title
                                                                 ? info->event->title : _("(No title)"));

      cal_radio_dialog (info->page, title, translated, 3, on_delete_answer, deleting);
    }
  else
    cal_confirm_dialog (info->page, _("Delete this event?"), _("OK"), on_delete_answer, deleting);
}

static void
on_export_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    g_warning ("export: %s", error->message);
  g_free (data);
}

static void
on_export_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  char *ics = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (!file)
    {
      g_free (ics);
      return;
    }
  g_file_replace_contents_async (file, ics, strlen (ics), NULL, FALSE, G_FILE_CREATE_NONE, NULL, on_export_saved,
                                 ics);
}

static void
on_export (NeoPressable *button, Info *info)
{
  g_autoptr(GError) error = NULL;
  char *ics = cal_store_export_sync (info->store, info->event, &error);
  g_autoptr(GtkFileDialog) dialog = NULL;
  g_autofree char *name = NULL;

  if (!ics)
    {
      g_warning ("export: %s", error->message);
      return;
    }
  /* shareEvent(EXPORT_SAF): the system's "save as", the file named after the event */
  name = g_strdup_printf ("%s.ics", info->event->title && *info->event->title ? info->event->title : "event");
  g_strdelimit (name, "/", '_');
  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Export event"));
  gtk_file_dialog_set_initial_name (dialog, name);
  gtk_file_dialog_save (dialog, GTK_WINDOW (info->window), NULL, on_export_chosen, ics);
}

/* --- building it */

static GtkWidget *
text (const char *css_class, gboolean wrap)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), wrap);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable (GTK_LABEL (label), FALSE);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

GtkWidget *
event_info_new (GtkWidget *window, CalEvent *event)
{
  Info *info = g_new0 (Info, 1);
  GtkWidget *page = cal_page_new (window);
  GtkWidget *actions, *duplicate, *export, *scroll, *content, *body, *label, *add_text;

  info->page = page;
  info->window = window;
  info->store = g_object_ref (cal_window_get_store (window));
  info->event = cal_event_ref (event);
  g_object_set_data_full (G_OBJECT (page), "cal-info", info, info_free);
  cal_page_set_back_handler (CAL_PAGE (page), on_back, info);

  /* MN/event_info_title_bar.xml, in its order. Share is left out: Android hands the .ics to another app through
   * its share sheet, and this phone has no share sheet to hand it to. Change colour is left out as Etar leaves it
   * out for a calendar without an event-colour palette (only Google accounts have one; EDS calendars do not). */
  gtk_box_append (GTK_BOX (page), cal_page_toolbar (CAL_PAGE (page), NULL, &actions));
  info->edit = cal_page_action ("cal-edit-symbolic", _("Edit"));
  g_signal_connect (info->edit, "clicked", G_CALLBACK (on_edit), info);
  gtk_box_append (GTK_BOX (actions), info->edit);
  info->delete = cal_page_action ("cal-delete-symbolic", _("Delete"));
  g_signal_connect (info->delete, "clicked", G_CALLBACK (on_delete), info);
  gtk_box_append (GTK_BOX (actions), info->delete);
  duplicate = cal_page_action ("cal-duplicate-symbolic", _("Duplicate"));
  g_signal_connect (duplicate, "clicked", G_CALLBACK (on_duplicate), info);
  gtk_box_append (GTK_BOX (actions), duplicate);
  export = cal_page_action ("cal-export-symbolic", _("Export event"));
  g_signal_connect (export, "clicked", G_CALLBACK (on_export), info);
  gtk_box_append (GTK_BOX (actions), export);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* L/event_info_headline.xml */
  info->headline = g_object_new (CAL_TYPE_COLOR_BOX, NULL);
  gtk_widget_add_css_class (info->headline, "cal-info-headline");
  info->title = text ("cal-info-title", TRUE);
  info->when = text ("cal-info-when", TRUE);
  info->repeat = text ("cal-info-repeat", TRUE);
  info->where = text ("cal-info-where", TRUE);
  gtk_box_append (GTK_BOX (info->headline), info->title);
  gtk_box_append (GTK_BOX (info->headline), info->when);
  gtk_box_append (GTK_BOX (info->headline), info->repeat);
  gtk_box_append (GTK_BOX (info->headline), info->where);
  gtk_box_append (GTK_BOX (content), info->headline);

  /* L/event_info.xml: the calendar, the description, the URL */
  body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (body, "cal-info-body");
  info->calendar_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  label = text ("cal-info-small", FALSE);
  gtk_label_set_label (GTK_LABEL (label), _("Calendar:"));
  gtk_box_append (GTK_BOX (info->calendar_row), label);
  info->calendar_name = text ("cal-info-small", TRUE);
  gtk_widget_add_css_class (info->calendar_name, "cal-info-calendar-name");
  gtk_widget_set_hexpand (info->calendar_name, TRUE);
  gtk_box_append (GTK_BOX (info->calendar_row), info->calendar_name);
  gtk_box_append (GTK_BOX (body), info->calendar_row);
  info->description = text ("cal-info-small", TRUE);
  gtk_widget_add_css_class (info->description, "cal-info-description");
  gtk_box_append (GTK_BOX (body), info->description);
  info->url = text ("cal-info-small", TRUE);
  gtk_widget_add_css_class (info->url, "cal-info-description");
  gtk_box_append (GTK_BOX (body), info->url);
  gtk_box_append (GTK_BOX (content), body);

  /* REMINDERS (listSeparatorTextViewStyle), the rows, "Add reminder" */
  info->reminders_header = text ("cal-list-separator", FALSE);
  gtk_label_set_label (GTK_LABEL (info->reminders_header), _("REMINDERS"));
  gtk_box_append (GTK_BOX (content), info->reminders_header);
  info->reminders = cal_reminders_new ();
  gtk_widget_add_css_class (info->reminders, "cal-info-reminders");
  g_signal_connect (info->reminders, "changed", G_CALLBACK (on_reminders_changed), info);
  gtk_box_append (GTK_BOX (content), info->reminders);
  add_text = gtk_label_new (_("Add reminder"));
  gtk_label_set_xalign (GTK_LABEL (add_text), 0);
  info->add_reminder = neo_pressable_new (add_text);
  gtk_widget_add_css_class (info->add_reminder, "cal-add-reminder");
  neo_pressable_set_ripple (NEO_PRESSABLE (info->add_reminder), NEO_RIPPLE_BOUNDED, 4);
  g_signal_connect (info->add_reminder, "clicked", G_CALLBACK (on_add_reminder), info);
  gtk_box_append (GTK_BOX (content), info->add_reminder);

  scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), content);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);

  g_signal_connect (info->store, "changed", G_CALLBACK (on_store_changed), info);
  show_event (info);
  return page;
}
