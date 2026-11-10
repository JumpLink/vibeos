/* Settings: see dialer-settings.h. The rows are AndroidX Preference's (preference_material, the switch in
 * widget_switch_compat), measured on still:dialer-settings, dialer-set-display, dialer-set-sounds and
 * dialer-dlg-sortby (spec §9.1–§9.4, §9.8, §9.9).
 *
 * Left out of the top list, and why:
 *   Quick responses      the incoming call's "Reply" sends a text message: the phone has no messages app yet
 *   Phone number lookup  Lineage's lookup providers are web services this phone has no client for
 *   Calling accounts     Telecom's page for SIP and other accounts: calls here only go over the modem
 *   Voicemail            its rows open Telecom's and the carrier's pages; the phone has no visual voicemail
 *   Accessibility        TTY mode: the modem's TTY setting, which ModemManager does not reach
 * In Sounds and vibration, "Enable Do Not Disturb during calls" and Call recording are left out too: the phone has
 * no do-not-disturb to turn on, and nothing records calls. */
#include "dialer-settings.h"
#include "blocked-numbers.h"
#include "calls-client.h"
#include "phone-number.h"
#include "ringer.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>
#include <string.h>

#define BAR_HEIGHT      56
#define UP_SIZE         56
#define TITLE_START     72      /* measured: the title's x */
#define ROW_PAD_X       16      /* listPreferredItemPaddingStart/End */
#define ROW_PAD_Y       16      /* the text block's padding */
#define WIDGET_GAP      16      /* the widget_frame: 16 dp, then the 56 dp switch */
#define CATEGORY_GAP    16      /* measured over a category */
#define CATEGORY_PAD    8
#define SWITCH_WIDTH    56      /* SwitchCompat: max(switchMinWidth 52, 2 × 28 dp thumb) */
#define SWITCH_HEIGHT   48      /* touch_target_min_size */
#define TRACK_HEIGHT    28
#define THUMB           20      /* switch_thumb: a 20 dp dot inset 4 dp in a 28 dp box */
#define THUMB_INSET     4
#define SWITCH_MS       250     /* SwitchCompat's THUMB_ANIMATION_DURATION */
#define RADIO           20      /* the radio's ring, measured */
#define RADIO_STROKE    2
#define RADIO_DOT       10
#define CHOICE_HEIGHT   48      /* select_dialog_singlechoice_material, measured */
#define CHOICE_TEXT     47      /* measured: the text's start past the radio's */
#define CHOICE_RADIO    26      /* measured: the radio's ring in from the dialog window's edge; its text then
                                 * 1 dp nearer than CHOICE_TEXT at 18 sp (ink at 73 dp from the edge) */
#define RINGTONES_DIR   "sounds/android/ringtones"
#define DEFAULT_TITLE   "Orion" /* LineageOS's ro.config.ringtone (ringer.c) */

/* the rendered colours (spec §9.9) */
#define TRACK_ON        0x81b5bc
#define TRACK_OFF       0x889395
#define THUMB_COLOR     0xfaffff
#define RADIO_ON        0xedf9fb
#define RADIO_OFF       0xeff0f2

static GdkRGBA
rgb (guint32 color)
{
  return (GdkRGBA) { ((color >> 16) & 0xff) / 255.0, ((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0, 1 };
}

/* ---- the switch: Dialer.Switch, drawn; its row takes the tap ------------------------------------------------- */

#define SETTINGS_TYPE_SWITCH (settings_switch_get_type ())
G_DECLARE_FINAL_TYPE (SettingsSwitch, settings_switch, SETTINGS, SWITCH, GtkWidget)

struct _SettingsSwitch
{
  GtkWidget parent;
  NeoAnim *anim;
  double position;              /* 0 off … 1 on */
};

G_DEFINE_FINAL_TYPE (SettingsSwitch, settings_switch, GTK_TYPE_WIDGET)

static void
settings_switch_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                         int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? SWITCH_WIDTH : SWITCH_HEIGHT;
}

static void
settings_switch_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SettingsSwitch *self = SETTINGS_SWITCH (widget);
  double top = (gtk_widget_get_height (widget) - TRACK_HEIGHT) / 2.0;
  GdkRGBA off = rgb (TRACK_OFF), on = rgb (TRACK_ON), thumb = rgb (THUMB_COLOR);
  GdkRGBA track = neo_rgba_mix (&off, &on, (float) self->position);
  double x = THUMB_INSET + self->position * (SWITCH_WIDTH - TRACK_HEIGHT);
  GskRoundedRect rect;

  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (0, top, SWITCH_WIDTH, TRACK_HEIGHT),
                                   TRACK_HEIGHT / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_color (snapshot, &track, &rect.bounds);
  gtk_snapshot_pop (snapshot);
  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (x, top + THUMB_INSET, THUMB, THUMB), THUMB / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_color (snapshot, &thumb, &rect.bounds);
  gtk_snapshot_pop (snapshot);
}

static void
on_switch_anim (NeoAnim *anim, double value, gpointer data)
{
  SettingsSwitch *self = data;

  self->position = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
settings_switch_set_active (SettingsSwitch *self, gboolean active)
{
  neo_anim_start (self->anim, self->position, active ? 1 : 0, SWITCH_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

static void
settings_switch_dispose (GObject *object)
{
  g_clear_pointer (&SETTINGS_SWITCH (object)->anim, neo_anim_free);
  G_OBJECT_CLASS (settings_switch_parent_class)->dispose (object);
}

static void
settings_switch_class_init (SettingsSwitchClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = settings_switch_dispose;
  widget_class->measure = settings_switch_measure;
  widget_class->snapshot = settings_switch_snapshot;
  gtk_widget_class_set_css_name (widget_class, "settings-switch");
}

static void
settings_switch_init (SettingsSwitch *self)
{
  self->anim = neo_anim_new (GTK_WIDGET (self), on_switch_anim, self);
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

static GtkWidget *
settings_switch_new (gboolean active)
{
  SettingsSwitch *self = g_object_new (SETTINGS_TYPE_SWITCH, NULL);

  self->position = active ? 1 : 0;
  return GTK_WIDGET (self);
}

/* ---- the radio of a single-choice row ------------------------------------------------------------------------- */

#define SETTINGS_TYPE_RADIO (settings_radio_get_type ())
G_DECLARE_FINAL_TYPE (SettingsRadio, settings_radio, SETTINGS, RADIO, GtkWidget)

struct _SettingsRadio
{
  GtkWidget parent;
  gboolean checked;
};

G_DEFINE_FINAL_TYPE (SettingsRadio, settings_radio, GTK_TYPE_WIDGET)

static void
settings_radio_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = RADIO;
}

static void
settings_radio_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SettingsRadio *self = SETTINGS_RADIO (widget);
  GdkRGBA color = rgb (self->checked ? RADIO_ON : RADIO_OFF);
  GdkRGBA colors[4] = { color, color, color, color };
  float widths[4] = { RADIO_STROKE, RADIO_STROKE, RADIO_STROKE, RADIO_STROKE };
  double y = (gtk_widget_get_height (widget) - RADIO) / 2.0;
  GskRoundedRect rect;

  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (0, y, RADIO, RADIO), RADIO / 2.0);
  gtk_snapshot_append_border (snapshot, &rect, widths, colors);
  if (!self->checked)
    return;
  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT ((RADIO - RADIO_DOT) / 2.0,
                                                               y + (RADIO - RADIO_DOT) / 2.0, RADIO_DOT, RADIO_DOT),
                                   RADIO_DOT / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_color (snapshot, &color, &rect.bounds);
  gtk_snapshot_pop (snapshot);
}

static void
settings_radio_class_init (SettingsRadioClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->measure = settings_radio_measure;
  widget_class->snapshot = settings_radio_snapshot;
  gtk_widget_class_set_css_name (widget_class, "settings-radio");
}

static void
settings_radio_init (SettingsRadio *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

static GtkWidget *
settings_radio_new (gboolean checked)
{
  SettingsRadio *self = g_object_new (SETTINGS_TYPE_RADIO, NULL);

  self->checked = checked;
  return GTK_WIDGET (self);
}

/* ---- the screen ---------------------------------------------------------------------------------------------- */

typedef struct
{
  GtkWidget *page;
  char *title;
} Level;

struct _DialerSettings
{
  GtkWidget parent;
  GSettings *settings;
  GtkWidget *box, *title, *stack;
  GPtrArray *levels;            /* Level: the top list, then the pages opened from it */
  GtkWidget *sort_summary, *format_summary, *ringtone_summary;
};

G_DEFINE_FINAL_TYPE (DialerSettings, dialer_settings, GTK_TYPE_WIDGET)

enum { CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
level_free (Level *level)
{
  g_free (level->title);
  g_free (level);
}

/* onPreferenceStartFragment: the page replaces the list (no transition) and the bar takes its title */
static void
push_page (DialerSettings *self, GtkWidget *page, const char *title)
{
  Level *level = g_new0 (Level, 1);

  level->page = page;
  level->title = g_strdup (title);
  g_ptr_array_add (self->levels, level);
  gtk_stack_add_child (GTK_STACK (self->stack), page);
  gtk_stack_set_visible_child (GTK_STACK (self->stack), page);
  gtk_label_set_label (GTK_LABEL (self->title), title);
}

/* Back pops the page; on the top list the screen goes. Android's Up always went to the main screen, but here Up is
 * what the back gesture clicks, so it does what Back does */
static void
go_back (DialerSettings *self)
{
  Level *gone, *shown;

  if (self->levels->len <= 1)
    {
      g_signal_emit (self, signals[CLOSED], 0);
      return;
    }
  gone = g_ptr_array_steal_index (self->levels, self->levels->len - 1);
  shown = self->levels->pdata[self->levels->len - 1];
  gtk_stack_set_visible_child (GTK_STACK (self->stack), shown->page);
  gtk_label_set_label (GTK_LABEL (self->title), shown->title);
  /* the ringtone picker's preview stops with it */
  if (g_object_get_data (G_OBJECT (gone->page), "previews"))
    calls_client_preview_ringtone (calls_client_get (), NULL);
  gtk_stack_remove (GTK_STACK (self->stack), gone->page);
  level_free (gone);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, DialerSettings *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  go_back (self);
  return TRUE;
}

/* ---- rows ---------------------------------------------------------------------------------------------------- */

/* a page: its rows in a list that scrolls (the PreferenceFragment's RecyclerView) */
static GtkWidget *
page_new (void)
{
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = spongy_scroll_view_new ();

  gtk_widget_set_valign (list, GTK_ALIGN_START);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroller), list);
  g_object_set_data (G_OBJECT (scroller), "list", list);
  return scroller;
}

static GtkWidget *
page_list (GtkWidget *page)
{
  return g_object_get_data (G_OBJECT (page), "list");
}

static GtkWidget *
text_label (const char *text, const char *css_class, double size)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (label, css_class);
  neo_label_set_font_padding (label, size);
  return label;
}

/* a Preference: the title, the summary under it (returned in @summary_out when asked for), and a widget at the end */
static GtkWidget *
add_row (GtkWidget *page, const char *title, const char *summary, GtkWidget *widget, GtkWidget **summary_out)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *pressable;

  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (text, ROW_PAD_Y);
  gtk_widget_set_margin_bottom (text, ROW_PAD_Y);
  gtk_box_append (GTK_BOX (text), text_label (title, "settings-title", 16));
  if (summary || summary_out)
    {
      GtkWidget *label = text_label (summary ? summary : "", "settings-summary", 14);

      gtk_box_append (GTK_BOX (text), label);
      if (summary_out)
        *summary_out = label;
    }
  gtk_widget_set_margin_start (row, ROW_PAD_X);
  gtk_widget_set_margin_end (row, ROW_PAD_X);
  gtk_box_append (GTK_BOX (row), text);
  if (widget)
    {
      gtk_widget_set_margin_start (widget, WIDGET_GAP);
      gtk_widget_set_valign (widget, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (row), widget);
    }
  /* selectableItemBackground over the whole row */
  pressable = neo_pressable_new (row);
  neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (page_list (page)), pressable);
  return pressable;
}

/* a PreferenceCategory: the accent title over its rows, 16 dp under the rows before */
static void
add_category (GtkWidget *page, const char *title)
{
  GtkWidget *label = text_label (title, "settings-category", 14);

  gtk_widget_set_margin_start (label, ROW_PAD_X);
  gtk_widget_set_margin_end (label, ROW_PAD_X);
  gtk_widget_set_margin_top (label, CATEGORY_GAP + CATEGORY_PAD + gtk_widget_get_margin_top (label));
  gtk_widget_set_margin_bottom (label, CATEGORY_PAD + gtk_widget_get_margin_bottom (label));
  gtk_accessible_update_property (GTK_ACCESSIBLE (label), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (page_list (page)), label);
}

/* a SwitchPreferenceCompat on a boolean key: a tap anywhere on the row flips it */
static void
on_switch_row_clicked (GtkWidget *row, DialerSettings *self)
{
  const char *key = g_object_get_data (G_OBJECT (row), "key");
  gboolean active = !g_settings_get_boolean (self->settings, key);

  g_settings_set_boolean (self->settings, key, active);
  settings_switch_set_active (g_object_get_data (G_OBJECT (row), "switch"), active);
}

static void
add_switch_row (DialerSettings *self, GtkWidget *page, const char *title, const char *summary, const char *key)
{
  GtkWidget *sw = settings_switch_new (g_settings_get_boolean (self->settings, key));
  GtkWidget *row = add_row (page, title, summary, sw, NULL);

  g_object_set_data_full (G_OBJECT (row), "key", g_strdup (key), g_free);
  g_object_set_data (G_OBJECT (row), "switch", sw);
  g_signal_connect (row, "clicked", G_CALLBACK (on_switch_row_clicked), self);
}

/* ---- a ListPreference's dialog: its entries with a radio each, and Cancel (§9.8) ----------------------------- */

typedef void (*ChoiceFunc) (DialerSettings *self, int index);

typedef struct
{
  DialerSettings *self;
  NeoDialog *dialog;
  ChoiceFunc chosen;
} Choice;

static void
on_choice_clicked (GtkWidget *row, Choice *choice)
{
  /* selecting an entry applies it and closes the dialog */
  choice->chosen (choice->self, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "index")));
  neo_dialog_respond (choice->dialog, NEO_RESPONSE_OK);
}

static void
show_choice (DialerSettings *self, const char *title, const char * const *entries, int selected, ChoiceFunc chosen)
{
  NeoDialog *dialog = neo_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  Choice *choice = g_new0 (Choice, 1);
  g_autofree char *cancel = g_utf8_strup (_("Cancel"), -1);

  choice->self = self;
  choice->dialog = dialog;
  choice->chosen = chosen;
  g_object_set_data_full (G_OBJECT (dialog), "choice", choice, g_free);
  for (int i = 0; entries[i]; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *radio = settings_radio_new (i == selected);
      GtkWidget *label = gtk_label_new (entries[i]);
      GtkWidget *pressable;

      gtk_widget_set_size_request (row, -1, CHOICE_HEIGHT);
      gtk_widget_set_valign (radio, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_start (radio, CHOICE_RADIO);
      gtk_box_append (GTK_BOX (row), radio);
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_widget_set_margin_start (label, CHOICE_TEXT - 1 - RADIO);
      gtk_widget_set_margin_end (label, CHOICE_RADIO);
      gtk_widget_add_css_class (label, "settings-choice");
      gtk_box_append (GTK_BOX (row), label);
      pressable = neo_pressable_new (row);
      neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (pressable), "index", GINT_TO_POINTER (i));
      g_signal_connect (pressable, "clicked", G_CALLBACK (on_choice_clicked), choice);
      gtk_box_append (GTK_BOX (list), pressable);
    }
  /* AppCompat's AlertDialog (dialer.css .settings-dialog): the rows edge to edge, its button in capitals */
  gtk_widget_add_css_class (GTK_WIDGET (dialog), "settings-dialog");
  neo_dialog_set_content (dialog, list);
  neo_dialog_add_button (dialog, cancel, NEO_RESPONSE_CANCEL);
  neo_dialog_present (dialog, GTK_WIDGET (self));
}

/* ---- Display options (§9.3) ---------------------------------------------------------------------------------- */

static void
sync_display_summaries (DialerSettings *self)
{
  /* the summary is the chosen entry (%s) */
  gtk_label_set_label (GTK_LABEL (self->sort_summary), g_settings_get_enum (self->settings, "sort-order")
                                                       ? _("Last name") : _("First name"));
  gtk_label_set_label (GTK_LABEL (self->format_summary), g_settings_get_enum (self->settings, "display-order")
                                                         ? _("Last name first") : _("First name first"));
}

static void
on_sort_chosen (DialerSettings *self, int index)
{
  g_settings_set_enum (self->settings, "sort-order", index);
  sync_display_summaries (self);
}

static void
on_format_chosen (DialerSettings *self, int index)
{
  g_settings_set_enum (self->settings, "display-order", index);
  sync_display_summaries (self);
}

static void
on_sort_clicked (DialerSettings *self)
{
  const char *entries[] = { _("First name"), _("Last name"), NULL };

  show_choice (self, _("Sort by"), entries, g_settings_get_enum (self->settings, "sort-order"), on_sort_chosen);
}

static void
on_format_clicked (DialerSettings *self)
{
  const char *entries[] = { _("First name first"), _("Last name first"), NULL };

  show_choice (self, _("Name format"), entries, g_settings_get_enum (self->settings, "display-order"),
               on_format_chosen);
}

static void
on_display_options (DialerSettings *self)
{
  GtkWidget *page = page_new ();

  g_signal_connect_swapped (add_row (page, _("Sort by"), NULL, NULL, &self->sort_summary), "clicked",
                            G_CALLBACK (on_sort_clicked), self);
  g_signal_connect_swapped (add_row (page, _("Name format"), NULL, NULL, &self->format_summary), "clicked",
                            G_CALLBACK (on_format_clicked), self);
  sync_display_summaries (self);
  push_page (self, page, _("Display options"));
}

/* ---- the ringtone picker: the phone's ringtones, "None" first (RingtonePickerActivity, EXTRA_RINGTONE_SHOW_SILENT;
 * the default is not offered, as this row sets it) ----------------------------------------------------------- */

typedef struct
{
  char *uri;
  char *title;
} Ringtone;

static void
ringtone_free (Ringtone *ringtone)
{
  g_free (ringtone->uri);
  g_free (ringtone->title);
  g_free (ringtone);
}

static int
by_title (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate ((*(Ringtone **) a)->title, (*(Ringtone **) b)->title);
}

/* a sound's title: its file's name without the extension (the ringtones' TITLE tags are the same) */
static char *
title_of (const char *uri)
{
  g_autofree char *path = g_filename_from_uri (uri, NULL, NULL);
  g_autofree char *base = g_path_get_basename (path ? path : uri);
  char *dot = strrchr (base, '.');

  if (dot && dot != base)
    *dot = 0;
  return g_strdup (base);
}

/* every ringtone in the data dirs, by title; a name in two dirs is the first dir's */
static GPtrArray *
list_ringtones (void)
{
  GPtrArray *ringtones = g_ptr_array_new_with_free_func ((GDestroyNotify) ringtone_free);
  g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char * const *dirs = g_get_system_data_dirs ();

  for (guint i = 0; dirs[i]; i++)
    {
      g_autofree char *path = g_build_filename (dirs[i], RINGTONES_DIR, NULL);
      g_autoptr(GDir) dir = g_dir_open (path, 0, NULL);
      const char *name;

      while (dir && (name = g_dir_read_name (dir)))
        {
          g_autofree char *file = g_build_filename (path, name, NULL);
          Ringtone *ringtone;

          if (!g_str_has_suffix (name, ".ogg") && !g_str_has_suffix (name, ".wav") && !g_str_has_suffix (name, ".mp3")
              && !g_str_has_suffix (name, ".flac"))
            continue;
          ringtone = g_new0 (Ringtone, 1);
          ringtone->uri = g_filename_to_uri (file, NULL, NULL);
          ringtone->title = title_of (ringtone->uri);
          if (!ringtone->uri || g_hash_table_contains (seen, ringtone->title))
            {
              ringtone_free (ringtone);
              continue;
            }
          g_hash_table_add (seen, g_strdup (ringtone->title));
          g_ptr_array_add (ringtones, ringtone);
        }
    }
  g_ptr_array_sort (ringtones, by_title);
  return ringtones;
}

/* what the "Phone ringtone" row says: the ringtone's title, or "None" */
static char *
ringtone_title (DialerSettings *self)
{
  g_autofree char *setting = g_settings_get_string (self->settings, "ringtone");

  if (g_str_equal (setting, RINGER_SILENT))
    return g_strdup (_("None"));
  if (!*setting || g_str_equal (setting, RINGER_DEFAULT))
    return g_strdup (DEFAULT_TITLE);
  return title_of (setting);
}

/* whether @uri (NULL: None) is the ringtone set; an unset one is the default's file */
static gboolean
is_current (DialerSettings *self, const char *uri)
{
  g_autofree char *setting = g_settings_get_string (self->settings, "ringtone");
  g_autofree char *title = NULL;

  if (!uri)
    return g_str_equal (setting, RINGER_SILENT);
  if (!*setting || g_str_equal (setting, RINGER_DEFAULT))
    {
      title = title_of (uri);
      return g_str_equal (title, DEFAULT_TITLE);
    }
  return g_str_equal (setting, uri);
}

static void
on_ringtone_clicked (GtkWidget *row, DialerSettings *self)
{
  const char *uri = g_object_get_data (G_OBJECT (row), "uri");
  GtkWidget *list = gtk_widget_get_parent (row);
  g_autofree char *title = NULL;

  /* the tapped one is checked and plays (none for None); the choice is kept at once */
  for (GtkWidget *r = gtk_widget_get_first_child (list); r; r = gtk_widget_get_next_sibling (r))
    {
      SettingsRadio *radio = g_object_get_data (G_OBJECT (r), "radio");

      radio->checked = r == row;
      gtk_widget_queue_draw (GTK_WIDGET (radio));
    }
  g_settings_set_string (self->settings, "ringtone", uri ? uri : RINGER_SILENT);
  calls_client_preview_ringtone (calls_client_get (), uri);
  title = ringtone_title (self);
  gtk_label_set_label (GTK_LABEL (self->ringtone_summary), title);
}

static void
add_ringtone_row (DialerSettings *self, GtkWidget *page, const char *title, const char *uri)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *radio = settings_radio_new (is_current (self, uri));
  GtkWidget *label = gtk_label_new (title);
  GtkWidget *pressable;

  gtk_widget_set_size_request (row, -1, CHOICE_HEIGHT);
  gtk_widget_set_margin_start (row, ROW_PAD_X);
  gtk_widget_set_margin_end (row, ROW_PAD_X);
  gtk_widget_set_valign (radio, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (row), radio);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_margin_start (label, CHOICE_TEXT - RADIO);
  gtk_widget_add_css_class (label, "settings-choice");
  gtk_box_append (GTK_BOX (row), label);
  pressable = neo_pressable_new (row);
  neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_BOUNDED, 0);
  g_object_set_data_full (G_OBJECT (pressable), "uri", g_strdup (uri), g_free);
  g_object_set_data (G_OBJECT (pressable), "radio", radio);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_ringtone_clicked), self);
  gtk_box_append (GTK_BOX (page_list (page)), pressable);
}

static void
on_ringtone (DialerSettings *self)
{
  GtkWidget *page = page_new ();
  g_autoptr(GPtrArray) ringtones = list_ringtones ();

  add_ringtone_row (self, page, _("None"), NULL);
  for (guint i = 0; i < ringtones->len; i++)
    {
      Ringtone *ringtone = ringtones->pdata[i];

      add_ringtone_row (self, page, ringtone->title, ringtone->uri);
    }
  g_object_set_data (G_OBJECT (page), "previews", GINT_TO_POINTER (TRUE));
  push_page (self, page, _("Phone ringtone"));
}

/* ---- Sounds and vibration (§9.4) ----------------------------------------------------------------------------- */

static void
on_sounds (DialerSettings *self)
{
  GtkWidget *page = page_new ();
  g_autofree char *title = ringtone_title (self);

  g_signal_connect_swapped (add_row (page, _("Phone ringtone"), title, NULL, &self->ringtone_summary), "clicked",
                            G_CALLBACK (on_ringtone), self);
  /* the vibrator-gated rows: the phone has one (feedbackd) */
  add_switch_row (self, page, _("Also vibrate for calls"), NULL, "vibrate-when-ringing");
  add_switch_row (self, page, _("Keypad tones"), NULL, "dtmf-tone-when-dialing");
  add_category (page, _("In-call"));
  add_switch_row (self, page, _("Vibrate on answer"), NULL, "incall-vibrate-outgoing");
  add_switch_row (self, page, _("Vibrate on call waiting"), NULL, "incall-vibrate-call-waiting");
  add_switch_row (self, page, _("Vibrate on hang up"), NULL, "incall-vibrate-hangup");
  add_switch_row (self, page, _("Vibrate every minute"),
                  _("Vibrates at the 45 second mark of every minute during outgoing calls"), "incall-vibrate-45secs");
  push_page (self, page, _("Sounds and vibration"));
}

/* ---- Blocked numbers (§9.7): Telecom's BlockedNumbersActivity, another app with its own theme ----------------
 *
 * Measured on still:dialer-set-blocked (an empty list): the explanation, then "Add a number"; each blocked number is
 * a row with an X that unblocks it (blocked_number_item). Left out: the "Unknown / Block calls from unidentified
 * callers" switch. A caller's number can come a moment after the call starts ringing, so "no number yet" is not
 * "unidentified", and blocking on it would turn away callers who are not withheld at all. */

#define BLOCKED_ROW_HEIGHT  56
#define BLOCKED_BODY_PAD    16
#define BLOCKED_ADD_HEIGHT  52      /* measured: 51.67 dp */
#define BLOCKED_X_SIZE      48

static const char *
blocked_country (void)
{
  const char *country = calls_client_get_country (calls_client_get ());

  return country && *country ? country : phone_number_locale_country ();
}

static void
on_unblock_response (NeoDialog *dialog, int response, gpointer data)
{
  const char *number = g_object_get_data (G_OBJECT (dialog), "number");
  g_autoptr(GError) error = NULL;

  if (response == NEO_RESPONSE_OK
      && !blocked_numbers_remove (blocked_numbers_get_default (), number, blocked_country (), &error))
    g_warning ("settings: %s", error->message);
}

/* the X on a row: "Unblock %s?", Cancel and Unblock */
static void
on_unblock_clicked (GtkWidget *button, gpointer data)
{
  const char *number = g_object_get_data (G_OBJECT (button), "number");
  g_autofree char *shown = phone_number_format (number, blocked_country ());
  g_autofree char *question = g_strdup_printf (_("Unblock %s?"), shown);
  NeoDialog *dialog = neo_dialog_new (NULL);

  neo_dialog_set_message (dialog, question);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("Unblock"), NEO_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "number", g_strdup (number), g_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_unblock_response), NULL);
  neo_dialog_present (dialog, button);
}

/* the rows under "Add a number": one per blocked number, the last blocked first; made again on every change, from
 * this app or another (the Messaging blocks into the same list) */
static void
fill_blocked (GtkWidget *rows)
{
  g_auto(GStrv) numbers = blocked_numbers_list (blocked_numbers_get_default ());
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (rows)))
    gtk_box_remove (GTK_BOX (rows), child);
  for (guint i = 0; numbers[i]; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      g_autofree char *shown = phone_number_format (numbers[i], blocked_country ());
      GtkWidget *label = text_label (shown, "blocked-number", 16);
      GtkWidget *unblock = neo_icon_button_new ("dialer-close-symbolic");

      gtk_widget_set_size_request (row, -1, BLOCKED_ROW_HEIGHT);
      gtk_widget_set_margin_start (row, ROW_PAD_X);
      /* the X's 24 dp glyph ends at the 16 dp padding; its 48 dp touch area reaches past it */
      gtk_widget_set_margin_end (row, ROW_PAD_X - (BLOCKED_X_SIZE - 24) / 2);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
      gtk_label_set_wrap (GTK_LABEL (label), FALSE);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (row), label);
      gtk_widget_set_size_request (unblock, BLOCKED_X_SIZE, BLOCKED_X_SIZE);
      gtk_widget_set_valign (unblock, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (unblock, "blocked-unblock");
      gtk_accessible_update_property (GTK_ACCESSIBLE (unblock), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Unblock"), -1);
      g_object_set_data_full (G_OBJECT (unblock), "number", g_strdup (numbers[i]), g_free);
      g_signal_connect (unblock, "clicked", G_CALLBACK (on_unblock_clicked), NULL);
      gtk_box_append (GTK_BOX (row), unblock);
      gtk_box_append (GTK_BOX (rows), row);
    }
}

static void
on_add_response (NeoDialog *dialog, int response, gpointer data)
{
  GtkWidget *entry = g_object_get_data (G_OBJECT (dialog), "entry");
  g_autofree char *number = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
  g_autoptr(GError) error = NULL;

  if (response != NEO_RESPONSE_OK || !*number)
    return;
  /* Telecom refuses to block an emergency number */
  if (phone_number_is_emergency (number))
    return;
  if (!blocked_numbers_add (blocked_numbers_get_default (), number, blocked_country (), &error))
    g_warning ("settings: %s", error->message);
}

static void
on_add_activate (GtkEntry *entry, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, NEO_RESPONSE_OK);
}

/* "Add a number": one line for the number (the phone keypad), Cancel and Block */
static void
on_add_clicked (GtkWidget *link, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (NULL);
  GtkWidget *entry = gtk_entry_new ();

  neo_dialog_set_message (dialog, _("Add a number"));
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), _("Phone number"));
  gtk_entry_set_input_purpose (GTK_ENTRY (entry), GTK_INPUT_PURPOSE_PHONE);
  gtk_widget_add_css_class (entry, "neo-entry");
  g_signal_connect (entry, "activate", G_CALLBACK (on_add_activate), dialog);
  neo_dialog_set_content (dialog, entry);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("Block"), NEO_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "entry", entry);
  g_signal_connect (dialog, "response", G_CALLBACK (on_add_response), NULL);
  neo_dialog_present (dialog, link);
  gtk_widget_grab_focus (entry);
}

static void
on_blocked (DialerSettings *self)
{
  GtkWidget *page = page_new ();
  GtkWidget *list = page_list (page);
  GtkWidget *body = text_label (_("You won't receive calls or texts from blocked numbers."), "blocked-body", 16);
  GtkWidget *add = text_label (_("Add a number"), "blocked-add", 16);
  GtkWidget *add_pressable, *rows;

  gtk_widget_add_css_class (page, "blocked-numbers");
  gtk_widget_set_margin_start (body, ROW_PAD_X);
  gtk_widget_set_margin_end (body, ROW_PAD_X);
  gtk_widget_set_margin_top (body, BLOCKED_BODY_PAD);
  gtk_widget_set_margin_bottom (body, BLOCKED_BODY_PAD);
  gtk_box_append (GTK_BOX (list), body);

  /* a link, not a full-width row: the measured touch area is the text's (x 16 dp, 52 dp tall). One line: a
   * wrapping label asks for its narrowest width, and a link sized to it broke "Add a number" in two */
  gtk_label_set_wrap (GTK_LABEL (add), FALSE);
  gtk_widget_set_valign (add, GTK_ALIGN_CENTER);
  add_pressable = neo_pressable_new (add);
  neo_pressable_set_ripple (NEO_PRESSABLE (add_pressable), NEO_RIPPLE_BOUNDED, 0);
  gtk_widget_set_halign (add_pressable, GTK_ALIGN_START);
  gtk_widget_set_size_request (add_pressable, -1, BLOCKED_ADD_HEIGHT);
  gtk_widget_set_margin_start (add_pressable, ROW_PAD_X);
  gtk_accessible_update_property (GTK_ACCESSIBLE (add_pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Add a number"),
                                  -1);
  g_signal_connect (add_pressable, "clicked", G_CALLBACK (on_add_clicked), NULL);
  gtk_box_append (GTK_BOX (list), add_pressable);

  rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (list), rows);
  fill_blocked (rows);
  g_signal_connect_object (blocked_numbers_get_default (), "changed", G_CALLBACK (fill_blocked), rows,
                           G_CONNECT_SWAPPED);
  push_page (self, page, _("Blocked numbers"));
}

/* ---- the screen ---------------------------------------------------------------------------------------------- */

static void
dialer_settings_dispose (GObject *object)
{
  DialerSettings *self = DIALER_SETTINGS (object);

  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->levels, g_ptr_array_unref);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (dialer_settings_parent_class)->dispose (object);
}

static void
dialer_settings_class_init (DialerSettingsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dialer_settings_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "dialer-settings");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
dialer_settings_init (DialerSettings *self)
{
}

static GtkWidget *
make_bar (DialerSettings *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *up = neo_icon_button_new ("dialer-back-symbolic");

  gtk_widget_add_css_class (bar, "dialer-history-bar");
  gtk_widget_set_size_request (bar, -1, BAR_HEIGHT);
  gtk_widget_set_size_request (up, UP_SIZE, UP_SIZE);
  gtk_widget_set_valign (up, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (up, "dialer-history-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (up), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Navigate up"), -1);
  g_signal_connect_swapped (up, "clicked", G_CALLBACK (go_back), self);
  gtk_box_append (GTK_BOX (bar), up);
  self->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->title, TRUE);
  gtk_widget_set_margin_start (self->title, TITLE_START - UP_SIZE);
  gtk_widget_set_margin_end (self->title, ROW_PAD_X);
  gtk_widget_add_css_class (self->title, "dialer-history-title");
  gtk_box_append (GTK_BOX (bar), self->title);
  return bar;
}

GtkWidget *
dialer_settings_new (GSettings *settings)
{
  DialerSettings *self = g_object_new (DIALER_TYPE_SETTINGS, NULL);
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkWidget *top;

  self->settings = g_object_ref (settings);
  self->levels = g_ptr_array_new_with_free_func ((GDestroyNotify) level_free);
  gtk_widget_add_css_class (GTK_WIDGET (self), "dialer-settings");
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_box_append (GTK_BOX (self->box), make_bar (self));
  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);
  gtk_box_append (GTK_BOX (self->box), self->stack);

  /* the top list: title-only rows, no icons, no dividers */
  top = page_new ();
  g_signal_connect_swapped (add_row (top, _("Display options"), NULL, NULL, NULL), "clicked",
                            G_CALLBACK (on_display_options), self);
  g_signal_connect_swapped (add_row (top, _("Sounds and vibration"), NULL, NULL, NULL), "clicked",
                            G_CALLBACK (on_sounds), self);
  g_signal_connect_swapped (add_row (top, _("Blocked numbers"), NULL, NULL, NULL), "clicked",
                            G_CALLBACK (on_blocked), self);
  push_page (self, top, _("Settings"));

  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  return GTK_WIDGET (self);
}
