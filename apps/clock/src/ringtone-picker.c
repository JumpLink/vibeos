/* RingtonePicker: see ringtone-picker.h. Its bar is Settings'. */
#include "ringtone-picker.h"
#include "alarms-client.h"
#include "clock-bar.h"
#include "clock-format.h"
#include "clock-popup-menu.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

/* the choices, as this page names them; saving turns them into the setting's or the alarm's own way of saying so */
#define DEFAULT "@default"
#define SILENT  "@silent"

#define DIMMED  0.63            /* a sound not selected: its icon and name */

struct _RingtonePicker
{
  GtkWidget parent;
  GSettings *settings;
  guint alarm_id;               /* 0: the timer sound */
  GtkWidget *box, *bar, *list;
  char *selected;               /* DEFAULT, SILENT or a URI */
  gboolean playing;
  GtkFileDialog *chooser;
};

G_DEFINE_FINAL_TYPE (RingtonePicker, ringtone_picker, GTK_TYPE_WIDGET)

enum { CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void show_list (RingtonePicker *self);

/* --- the preview, by the service at the alarm volume */

static void
call_service (const char *method, GVariant *parameters)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());

  if (bus)
    g_dbus_connection_call (bus, "neo.Clock.Alarms", "/neo/Clock/App", "neo.Clock.App", method,
                            parameters, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  else if (parameters)
    g_variant_unref (g_variant_ref_sink (parameters));
}

static void
stop_preview (RingtonePicker *self)
{
  if (self->playing)
    call_service ("StopSound", NULL);
  self->playing = FALSE;
}

/* the selection plays, looping, until it is stopped; Silent plays nothing */
static void
start_preview (RingtonePicker *self)
{
  const char *sound = self->selected;

  if (g_str_equal (sound, SILENT))
    return;
  if (g_str_equal (sound, DEFAULT))
    sound = self->alarm_id ? "" : "timer";
  call_service ("PlaySound", g_variant_new ("(su)", sound, 0));
  self->playing = TRUE;
}

/* --- the sounds */

/* the media store's order: by title, ignoring case */
static int
compare_titles (gconstpointer a, gconstpointer b)
{
  g_autofree char *name_a = clock_sound_name (a), *name_b = clock_sound_name (b);
  g_autofree char *key_a = g_utf8_casefold (name_a, -1), *key_b = g_utf8_casefold (name_b, -1);

  return g_utf8_collate (key_a, key_b);
}

/* every alarm sound of the system: the files in sounds/android/alarms of the data dirs, by title */
static GPtrArray *
system_sounds (void)
{
  GPtrArray *uris = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char *const *dirs = g_get_system_data_dirs ();

  for (guint i = 0; dirs[i]; i++)
    {
      g_autofree char *path = g_build_filename (dirs[i], "sounds", "android", "alarms", NULL);
      g_autoptr(GDir) dir = g_dir_open (path, 0, NULL);
      const char *name;

      while (dir && (name = g_dir_read_name (dir)))
        {
          g_autofree char *file = g_build_filename (path, name, NULL);

          if (!g_hash_table_contains (seen, name) && g_file_test (file, G_FILE_TEST_IS_REGULAR))
            {
              g_hash_table_add (seen, g_strdup (name));
              g_ptr_array_add (uris, g_filename_to_uri (file, NULL, NULL));
            }
        }
    }
  g_ptr_array_sort_values (uris, compare_titles);
  return uris;
}

/* a section's header: "Your sounds" or "Device sounds" */
static void
add_header (RingtonePicker *self, const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "ringtone-header");
  neo_label_set_font_padding (label, 15);
  gtk_box_append (GTK_BOX (self->list), label);
}

static void
on_sound (NeoPressable *row, RingtonePicker *self)
{
  const char *sound = g_object_get_data (G_OBJECT (row), "sound");

  /* the selected one: its preview stops, or starts again */
  if (g_str_equal (sound, self->selected))
    {
      if (self->playing)
        stop_preview (self);
      else
        start_preview (self);
      return;
    }
  stop_preview (self);
  g_free (self->selected);
  self->selected = g_strdup (sound);
  show_list (self);
  start_preview (self);
}

/* --- removing a sound the user added */

static void
on_remove_answer (NeoDialog *dialog, int response, gpointer data)
{
  RingtonePicker *self = g_object_get_data (G_OBJECT (dialog), "picker");
  const char *uri = data;
  g_auto(GStrv) custom = g_settings_get_strv (self->settings, "custom-ringtones");
  g_autoptr(GStrvBuilder) rest = g_strv_builder_new ();
  g_auto(GStrv) left = NULL;
  GPtrArray *alarms = alarms_client_get_alarms (alarms_client_get ());

  if (response != NEO_RESPONSE_OK)
    return;
  for (guint i = 0; custom[i]; i++)
    if (!g_str_equal (custom[i], uri))
      g_strv_builder_add (rest, custom[i]);
  left = g_strv_builder_end (rest);
  g_settings_set_strv (self->settings, "custom-ringtones", (const char *const *) left);
  /* "Alarms and timers using this sound will play the default sound instead." */
  for (guint i = 0; alarms && i < alarms->len; i++)
    {
      ClockAlarm *alarm = alarms->pdata[i];

      if (alarm->ringtone && g_str_equal (alarm->ringtone, uri))
        alarms_client_update (alarms_client_get (), alarm->id, g_variant_new_parsed ("{'ringtone': <'default'>}"));
    }
  for (guint i = 0; i < 2; i++)
    {
      const char *key = i ? "timer-ringtone" : "default-alarm-ringtone";
      g_autofree char *value = g_settings_get_string (self->settings, key);

      if (g_str_equal (value, uri))
        g_settings_set_string (self->settings, key, "");
    }
  if (g_str_equal (self->selected, uri))
    {
      stop_preview (self);
      g_free (self->selected);
      self->selected = g_strdup (DEFAULT);
    }
  show_list (self);
}

typedef struct
{
  RingtonePicker *picker;
  char *uri;
} Removal;

static void
on_remove (gpointer data)
{
  Removal *removal = data;
  NeoDialog *dialog = neo_dialog_new (NULL);

  neo_dialog_set_message (dialog, _("Alarms and timers using this sound will play the default sound instead."));
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("Remove"), NEO_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "picker", removal->picker);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_remove_answer), g_steal_pointer (&removal->uri),
                         (GClosureNotify) (void (*) (void)) g_free, 0);
  neo_dialog_present (dialog, GTK_WIDGET (removal->picker));
}

static void
on_overflow (NeoPressable *button, RingtonePicker *self)
{
  ClockPopupMenu *menu = clock_popup_menu_new ();
  Removal *removal = g_new0 (Removal, 1);

  removal->picker = self;
  removal->uri = g_strdup (g_object_get_data (G_OBJECT (button), "sound"));
  /* the removal lives as long as the menu */
  g_object_set_data_full (G_OBJECT (menu), "removal", removal, g_free);
  clock_popup_menu_append (menu, _("Remove"), on_remove, removal);
  clock_popup_menu_present (menu, GTK_WIDGET (button));
}

/* a sound's row: the icon, the name, then the check (selected) and a ⋮ (the user's own sounds) */
static void
add_sound (RingtonePicker *self, const char *sound, const char *name, const char *icon_name, gboolean custom)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *label = gtk_label_new (name);
  GtkWidget *check = gtk_image_new_from_icon_name ("clock-selector-checkbox-checked-symbolic");
  gboolean selected = g_str_equal (sound, self->selected);
  GtkWidget *row;

  /* end_guide_very_thin: the row ends at 98 % of the width */
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 7);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_size_request (icon, 48, 48);
  gtk_widget_add_css_class (icon, "ringtone-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_margin_start (label, 16);
  gtk_widget_set_margin_end (label, 16);
  gtk_widget_add_css_class (label, "ringtone-name");
  neo_label_set_font_padding (label, 16);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), label);
  if (!selected)
    {
      gtk_widget_set_opacity (icon, DIMMED);
      gtk_widget_set_opacity (label, DIMMED);
    }
  /* the check: shown on the selected row only */
  gtk_image_set_pixel_size (GTK_IMAGE (check), 24);
  gtk_widget_set_size_request (check, 48, 48);
  gtk_widget_add_css_class (check, "ringtone-check");
  gtk_widget_set_visible (check, selected);
  gtk_box_append (GTK_BOX (box), check);
  if (custom)
    {
      GtkWidget *more = gtk_image_new_from_icon_name ("clock-more-symbolic");
      GtkWidget *button;

      gtk_image_set_pixel_size (GTK_IMAGE (more), 24);
      button = neo_pressable_new (more);
      gtk_widget_set_size_request (button, 48, 48);
      gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
      neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_CIRCLE, 20);
      gtk_widget_add_css_class (button, "ringtone-more");
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
      g_object_set_data_full (G_OBJECT (button), "sound", g_strdup (sound), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_overflow), self);
      gtk_box_append (GTK_BOX (box), button);
    }

  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  gtk_widget_add_css_class (row, "ringtone-row");
  /* the selected row: white at 8 % over it */
  if (selected)
    gtk_widget_add_css_class (row, "selected");
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  gtk_accessible_update_state (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_STATE_SELECTED, selected, -1);
  g_object_set_data_full (G_OBJECT (row), "sound", g_strdup (sound), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_sound), self);
  gtk_box_append (GTK_BOX (self->list), row);
}

/* --- adding a sound (the document picker, for audio files) */

static void
on_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  RingtonePicker *self = data;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, NULL);
  g_auto(GStrv) custom = NULL;
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  g_auto(GStrv) all = NULL;
  g_autofree char *uri = NULL;

  if (file)
    {
      uri = g_file_get_uri (file);
      custom = g_settings_get_strv (self->settings, "custom-ringtones");
      if (!g_strv_contains ((const char *const *) custom, uri))
        {
          g_strv_builder_addv (builder, (const char **) custom);
          g_strv_builder_add (builder, uri);
          all = g_strv_builder_end (builder);
          g_settings_set_strv (self->settings, "custom-ringtones", (const char *const *) all);
        }
      /* the new sound, selected and playing */
      g_free (self->selected);
      self->selected = g_strdup (uri);
      show_list (self);
      start_preview (self);
    }
  g_object_unref (self);
}

static void
on_add (NeoPressable *row, RingtonePicker *self)
{
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (self)));
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr(GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  stop_preview (self);
  gtk_file_filter_add_mime_type (filter, "audio/*");
  gtk_file_filter_set_name (filter, _("Audio"));
  g_list_store_append (filters, filter);
  g_clear_object (&self->chooser);
  self->chooser = gtk_file_dialog_new ();
  gtk_file_dialog_set_filters (self->chooser, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (self->chooser, filter);
  gtk_file_dialog_open (self->chooser, GTK_WINDOW (root), NULL, on_chosen, g_object_ref (self));
}

/* "Add new": a + on a filled circle, its name dimmed, no check */
static void
add_add_new (RingtonePicker *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("clock-add-symbolic");
  GtkWidget *label = gtk_label_new (_("Add new"));
  GtkWidget *row;

  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_size_request (icon, 48, 48);
  gtk_widget_add_css_class (icon, "ringtone-add");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_margin_start (label, 16);
  gtk_widget_set_margin_end (label, 16);
  gtk_widget_set_opacity (label, DIMMED);
  gtk_widget_add_css_class (label, "ringtone-name");
  neo_label_set_font_padding (label, 16);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), label);
  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  gtk_widget_add_css_class (row, "ringtone-row");
  g_signal_connect (row, "clicked", G_CALLBACK (on_add), self);
  gtk_box_append (GTK_BOX (self->list), row);
}

/* the rows, in order */
static void
show_list (RingtonePicker *self)
{
  g_auto(GStrv) custom = g_settings_get_strv (self->settings, "custom-ringtones");
  g_autoptr(GPtrArray) sounds = system_sounds ();
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);

  add_header (self, _("Your sounds"));
  for (guint i = 0; custom[i]; i++)
    {
      g_autofree char *name = clock_sound_name (custom[i]);

      add_sound (self, custom[i], name, "clock-ringtone-symbolic", TRUE);
    }
  add_add_new (self);
  add_header (self, _("Device sounds"));
  add_sound (self, SILENT, _("Silent"), "clock-ringtone-silent-symbolic", FALSE);
  add_sound (self, DEFAULT, self->alarm_id ? _("Default alarm sound") : _("Timer Expired"), "clock-ringtone-symbolic",
             FALSE);
  for (guint i = 0; i < sounds->len; i++)
    {
      g_autofree char *name = clock_sound_name (sounds->pdata[i]);

      add_sound (self, sounds->pdata[i], name, "clock-ringtone-symbolic", FALSE);
    }
}

/* --- load and save */

static void
load (RingtonePicker *self)
{
  g_autofree char *value = NULL;

  g_free (self->selected);
  if (self->alarm_id)
    {
      /* the alarm's own: NULL the default, "" silent */
      ClockAlarm *alarm = alarms_client_find (alarms_client_get (), self->alarm_id);
      const char *ringtone = alarm ? alarm->ringtone : NULL;

      self->selected = g_strdup (!ringtone ? DEFAULT : !*ringtone ? SILENT : ringtone);
      return;
    }
  /* the setting's: "" the default, CLOCK_SILENT none */
  value = g_settings_get_string (self->settings, "timer-ringtone");
  self->selected = g_strdup (!*value ? DEFAULT : g_str_equal (value, CLOCK_SILENT) ? SILENT : value);
}

/* saved: an alarm's sound and the default for new alarms; or the timer sound */
static void
save (RingtonePicker *self)
{
  gboolean is_default = g_str_equal (self->selected, DEFAULT), is_silent = g_str_equal (self->selected, SILENT);
  const char *setting = is_default ? "" : is_silent ? CLOCK_SILENT : self->selected;

  if (!self->alarm_id)
    {
      g_autofree char *old = g_settings_get_string (self->settings, "timer-ringtone");

      if (!g_str_equal (old, setting))
        g_settings_set_string (self->settings, "timer-ringtone", setting);
      return;
    }
  if (alarms_client_find (alarms_client_get (), self->alarm_id))
    {
      ClockAlarm *alarm = alarms_client_find (alarms_client_get (), self->alarm_id);
      const char *ringtone = is_default ? "default" : is_silent ? "" : self->selected;
      const char *old = !alarm->ringtone ? "default" : alarm->ringtone;

      if (!g_str_equal (old, ringtone))
        alarms_client_update (alarms_client_get (), self->alarm_id,
                              g_variant_new_parsed ("{'ringtone': <%s>}", ringtone));
    }
  /* the default for future alarms: what was picked, except that "Default" is the default */
  if (!is_default)
    g_settings_set_string (self->settings, "default-alarm-ringtone", setting);
}

/* --- the widget */

static void
on_scrolled (GtkAdjustment *adjustment, RingtonePicker *self)
{
  if (gtk_adjustment_get_value (adjustment) > 0)
    gtk_widget_add_css_class (self->bar, "lifted");
  else
    gtk_widget_remove_css_class (self->bar, "lifted");
}

static void
on_up (NeoPressable *button, RingtonePicker *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
ringtone_picker_unmap (GtkWidget *widget)
{
  RingtonePicker *self = CLOCK_RINGTONE_PICKER (widget);

  /* hidden: the choice saved, the preview stopped */
  save (self);
  stop_preview (self);
  GTK_WIDGET_CLASS (ringtone_picker_parent_class)->unmap (widget);
}

static void
ringtone_picker_dispose (GObject *object)
{
  RingtonePicker *self = CLOCK_RINGTONE_PICKER (object);

  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_object (&self->chooser);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (ringtone_picker_parent_class)->dispose (object);
}

static void
ringtone_picker_finalize (GObject *object)
{
  g_free (CLOCK_RINGTONE_PICKER (object)->selected);
  G_OBJECT_CLASS (ringtone_picker_parent_class)->finalize (object);
}

static void
ringtone_picker_class_init (RingtonePickerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = ringtone_picker_dispose;
  G_OBJECT_CLASS (klass)->finalize = ringtone_picker_finalize;
  widget_class->unmap = ringtone_picker_unmap;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "ringtone-picker");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
ringtone_picker_init (RingtonePicker *self)
{
}

static GtkWidget *
make (GSettings *settings, guint alarm_id)
{
  RingtonePicker *self = g_object_new (CLOCK_TYPE_RINGTONE_PICKER, NULL);
  GtkWidget *scroller = spongy_scroll_view_new ();

  self->settings = g_object_ref (settings);
  self->alarm_id = alarm_id;
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  /* "Alarm sound" or "Timer sound", in Settings' bar */
  self->bar = clock_bar_new (alarm_id ? _("Alarm sound") : _("Timer sound"));
  gtk_widget_add_css_class (self->bar, "settings");
  g_signal_connect (clock_bar_get_up (self->bar), "clicked", G_CALLBACK (on_up), self);
  gtk_box_append (GTK_BOX (self->box), self->bar);
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (scroller, TRUE);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroller), self->list);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (scroller)), "value-changed",
                    G_CALLBACK (on_scrolled), self);
  gtk_box_append (GTK_BOX (self->box), scroller);
  load (self);
  show_list (self);
  return GTK_WIDGET (self);
}

GtkWidget *
ringtone_picker_new_for_alarm (GSettings *settings, guint alarm_id)
{
  return make (settings, alarm_id);
}

GtkWidget *
ringtone_picker_new_for_timer (GSettings *settings)
{
  return make (settings, 0);
}
