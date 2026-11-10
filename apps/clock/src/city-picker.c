/* CityPicker: see city-picker.h. Measured on still:clock-cities: the bar y 116…308 px with Up 0…168, the search icon
 * 816…960 and ⋮ 960…1080; rows of 168 px, 0…1056 wide; the index 0…222, the check box 222…366 (an 18 dp square, 2
 * dp outline), the name from 366, the time ending at 1032; the fast-scroll track at 1056…1080. */
#include "city-picker.h"
#include "cities.h"
#include "clock-bar.h"
#include "clock-popup-menu.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

#define ROW_HEIGHT    56        /* cities_list_item_height */
#define INDEX_WIDTH   74        /* the index TextView: 74 dip */
#define CHECK_SIZE    48        /* the CheckBox's touch box */
#define SCROLLBAR     8         /* the fast-scroll track, inside the list (scrollbarStyle insideInset) */
#define THUMB         48        /* the fast-scroll thumb's height */
#define SHADOW_MS     200       /* DropShadowController */

typedef struct
{
  City *city;
  GtkWidget *widget, *index, *check, *mark, *time;
} Row;

struct _CityPicker
{
  GtkWidget parent;
  GSettings *settings;
  GtkWidget *box, *bar, *search_button, *search, *entry, *clear;
  GtkWidget *scroller, *list, *header, *shadow, *track, *thumb;
  NeoAnim *shadow_anim;
  gboolean shadow_shown;
  GHashTable *rows;             /* City * → Row * */
  GPtrArray *original;          /* the cities chosen when the page opened, by name (getSelectedCities at onResume) */
  GPtrArray *checked;           /* the choice as it is being made, in the order it was made */
  char *query;                  /* the search, upper-cased without " -.'"; "" when not searching */
  guint tick;
};

G_DEFINE_FINAL_TYPE (CityPicker, city_picker, GTK_TYPE_WIDGET)

enum { CLOSED, SHOW_SETTINGS, N_SIGNALS };
static guint signals[N_SIGNALS];

static CitySort
get_sort (CityPicker *self)
{
  return g_settings_get_enum (self->settings, "city-sort") ? CITY_SORT_OFFSET : CITY_SORT_NAME;
}

static gboolean
filtering (CityPicker *self)
{
  return self->query[0] != '\0';
}

/* --- the rows */

/* "Hm" or "hma" in the city's zone (getTimeCharSequence) */
static void
show_time (Row *row)
{
  g_autoptr(GDateTime) now = g_date_time_new_now (row->city->tz);
  g_autofree char *text = NULL;
  int hour = g_date_time_get_hour (now), minute = g_date_time_get_minute (now);

  if (neo_clock_uses_24h ())
    text = g_strdup_printf ("%02d:%02d", hour, minute);
  else
    text = g_strdup_printf ("%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
  gtk_label_set_text (GTK_LABEL (row->time), text);
}

static void
show_checked (CityPicker *self, Row *row)
{
  gboolean checked = g_ptr_array_find (self->checked, row->city, NULL);

  if (checked)
    gtk_widget_add_css_class (row->mark, "checked");
  else
    gtk_widget_remove_css_class (row->mark, "checked");
  gtk_widget_set_visible (gtk_widget_get_first_child (row->mark), checked);
  gtk_accessible_update_state (GTK_ACCESSIBLE (row->widget), GTK_ACCESSIBLE_STATE_CHECKED,
                               checked ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
}

/* the row toggles its check box (CityAdapter.onClick); the box itself is not clickable */
static void
on_row (NeoPressable *widget, CityPicker *self)
{
  Row *row = g_object_get_data (G_OBJECT (widget), "city-row");

  if (!g_ptr_array_remove (self->checked, row->city))
    g_ptr_array_add (self->checked, row->city);
  show_checked (self, row);
}

static void
row_free (gpointer data)
{
  Row *row = data;

  g_object_unref (row->widget);
  g_free (row);
}

static Row *
make_row (CityPicker *self, City *city)
{
  Row *row = g_new0 (Row, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *name = gtk_label_new (city->data->name);
  GtkWidget *tick = gtk_image_new_from_icon_name ("clock-check-symbolic");

  row->city = city;
  /* the index: 74 dp wide, centred both ways, bold white; its text and size are set by the sort. A GMT offset wider
   * than that ("GMT -10:00") wraps onto two lines as the TextView's does: the label asks for no width of its own
   * (max-width-chars 1), so the 74 dp it is given is the width it wraps to */
  row->index = gtk_label_new (NULL);
  gtk_widget_set_size_request (row->index, INDEX_WIDTH, -1);
  gtk_label_set_wrap (GTK_LABEL (row->index), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (row->index), 1);
  gtk_label_set_justify (GTK_LABEL (row->index), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (row->index, "city-index");
  gtk_box_append (GTK_BOX (box), row->index);

  /* the check box: an 18 dp square in its 48 dp box, outlined in colorControlNormal, or filled with colorPrimary
   * round a check in colorOnPrimary (MaterialCheckBox) */
  row->mark = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (row->mark, "city-check");
  gtk_widget_set_halign (row->mark, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (row->mark, GTK_ALIGN_CENTER);
  gtk_image_set_pixel_size (GTK_IMAGE (tick), 14);
  gtk_box_append (GTK_BOX (row->mark), tick);
  /* the 48 dp box keeps its width: a child that expands would make it expand too (GTK passes hexpand up), and the
   * row's spare width would then go to it as well as to the name */
  row->check = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (row->check, CHECK_SIZE, CHECK_SIZE);
  gtk_widget_set_valign (row->check, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (row->check, FALSE);
  gtk_widget_set_hexpand (row->mark, TRUE);
  gtk_box_append (GTK_BOX (row->check), row->mark);
  gtk_box_append (GTK_BOX (box), row->check);

  /* the name: PrimaryLabelTextAppearance, 16 dp in, 10 dp before the time, one line */
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (name), TRUE);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_widget_set_valign (name, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (name, 16);
  gtk_widget_set_margin_end (name, 10);
  gtk_widget_add_css_class (name, "primary-label");
  neo_label_set_font_padding (name, 14);
  gtk_box_append (GTK_BOX (box), name);

  /* the time: SecondaryLabelTextAppearance, 8 dp from the end */
  row->time = gtk_label_new (NULL);
  gtk_widget_set_valign (row->time, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (row->time, 8);
  gtk_widget_add_css_class (row->time, "secondary-label");
  neo_label_set_font_padding (row->time, 14);
  gtk_box_append (GTK_BOX (box), row->time);

  row->widget = g_object_ref_sink (neo_pressable_new (box));
  gtk_widget_set_size_request (row->widget, -1, ROW_HEIGHT);
  neo_pressable_set_ripple (NEO_PRESSABLE (row->widget), NEO_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row->widget), GTK_ACCESSIBLE_PROPERTY_LABEL, city->data->name,
                                  -1);
  g_object_set_data (G_OBJECT (row->widget), "city-row", row);
  g_signal_connect (row->widget, "clicked", G_CALLBACK (on_row), self);
  show_time (row);
  return row;
}

static Row *
get_row (CityPicker *self, City *city)
{
  Row *row = g_hash_table_lookup (self->rows, city);

  if (!row)
    {
      row = make_row (self, city);
      g_hash_table_insert (self->rows, city, row);
    }
  return row;
}

/* --- the list (CityAdapter) */

/* the index of a row (getView): the section's letter at 24 sp by name, its GMT offset at 14 sp by time */
static void
show_index (Row *row, CitySort sort, gboolean shown)
{
  g_autofree char *text = NULL;

  gtk_widget_remove_css_class (row->index, "letter");
  gtk_widget_remove_css_class (row->index, "offset");
  if (!shown)
    {
      /* INVISIBLE: it keeps its width */
      gtk_label_set_text (GTK_LABEL (row->index), "");
      return;
    }
  if (sort == CITY_SORT_NAME)
    text = g_strdup (row->city->data->index);
  else
    text = clock_zone_gmt_label (row->city->tz, g_get_real_time () / G_USEC_PER_SEC);
  gtk_label_set_text (GTK_LABEL (row->index), text);
  gtk_widget_add_css_class (row->index, sort == CITY_SORT_NAME ? "letter" : "offset");
  /* Android centres the text with its font padding (the font's yMax and yMin), which sits it lower than GTK's
   * ascent-and-descent box does: 2 dp at 24 sp on still:clock-cities, so about 1 dp at 14 sp */
  gtk_widget_set_margin_top (row->index, sort == CITY_SORT_NAME ? 4 : 2);
}

/* refresh and filter: the rows in their order now. Not searching: "Selected Cities" and the cities chosen when the
 * page opened, then every other city, each section's first row with its index. Searching: only the cities that were
 * not chosen when it opened, whose names start with the query, with no indexes. */
static void
show_list (CityPicker *self)
{
  GPtrArray *all = cities_get_all ();
  g_autoptr(GPtrArray) others = g_ptr_array_new ();
  CitySort sort = get_sort (self);
  GtkWidget *child;
  City *prior = NULL;

  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);

  for (guint i = 0; i < all->len; i++)
    if (!g_ptr_array_find (self->original, all->pdata[i], NULL)
        && (!filtering (self) || cities_matches (all->pdata[i], self->query)))
      g_ptr_array_add (others, all->pdata[i]);
  cities_sort (others, sort);

  if (!filtering (self))
    {
      if (self->original->len)
        gtk_box_append (GTK_BOX (self->list), self->header);
      for (guint i = 0; i < self->original->len; i++)
        {
          Row *row = get_row (self, self->original->pdata[i]);

          show_index (row, sort, FALSE);
          show_checked (self, row);
          gtk_box_append (GTK_BOX (self->list), row->widget);
        }
    }
  for (guint i = 0; i < others->len; i++)
    {
      Row *row = get_row (self, others->pdata[i]);
      /* getShowIndex: the first one after the chosen ones, then each that sorts apart from the one before it */
      gboolean index = !filtering (self) && (!prior || cities_compare_index (prior, row->city, sort) != 0);

      show_index (row, sort, index);
      show_checked (self, row);
      gtk_box_append (GTK_BOX (self->list), row->widget);
      prior = row->city;
    }
  /* updateFastScrolling: no fast scroll while searching */
  gtk_widget_set_visible (self->track, !filtering (self));
}

/* onResume: the choice as saved, and the list from it */
static void
refresh (CityPicker *self)
{
  g_auto(GStrv) ids = g_settings_get_strv (self->settings, "selected-cities");
  g_autoptr(GPtrArray) selected = cities_get_selected ((const char *const *) ids);

  g_ptr_array_set_size (self->original, 0);
  g_ptr_array_set_size (self->checked, 0);
  for (guint i = 0; i < selected->len; i++)
    {
      g_ptr_array_add (self->original, selected->pdata[i]);
      g_ptr_array_add (self->checked, selected->pdata[i]);
    }
  /* CityModel.getAllCities: the chosen ones by name */
  cities_sort (self->original, CITY_SORT_NAME);
  show_list (self);
}

/* onPause: the choice is saved, in the order it was made */
static void
save (CityPicker *self)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  g_auto(GStrv) ids = NULL, old = g_settings_get_strv (self->settings, "selected-cities");

  for (guint i = 0; i < self->checked->len; i++)
    g_strv_builder_add (builder, ((City *) self->checked->pdata[i])->data->id);
  ids = g_strv_builder_end (builder);
  if (!g_strv_equal ((const char *const *) ids, (const char *const *) old))
    g_settings_set_strv (self->settings, "selected-cities", (const char *const *) ids);
}

/* --- the times, once a minute */

static gboolean on_tick (gpointer data);

static void
schedule_tick (CityPicker *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();

  g_clear_handle_id (&self->tick, g_source_remove);
  self->tick = g_timeout_add ((60 - g_date_time_get_second (now)) * 1000 - g_date_time_get_microsecond (now) / 1000,
                              on_tick, self);
}

static gboolean
on_tick (gpointer data)
{
  CityPicker *self = data;
  GHashTableIter iter;
  Row *row;

  self->tick = 0;
  g_hash_table_iter_init (&iter, self->rows);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &row))
    show_time (row);
  schedule_tick (self);
  return G_SOURCE_REMOVE;
}

/* --- searching (SearchMenuItemController: a SearchView in the bar) */

static void
on_query (GtkEditable *entry, CityPicker *self)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (gtk_editable_get_text (entry)));

  /* isFiltering: the trimmed query; filter: the query upper-cased without " -.'" */
  g_free (self->query);
  self->query = *trimmed ? cities_search_key (gtk_editable_get_text (entry)) : g_strdup ("");
  gtk_widget_set_visible (self->clear, *gtk_editable_get_text (entry) != '\0');
  show_list (self);
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroller), 0);
}

static void
on_search (NeoPressable *button, CityPicker *self)
{
  gtk_widget_set_visible (self->search_button, FALSE);
  gtk_widget_set_visible (self->search, TRUE);
  gtk_widget_grab_focus (self->entry);
}

/* the SearchView's close button: clears the query, or with none closes the field */
static void
on_clear (NeoPressable *button, CityPicker *self)
{
  if (*gtk_editable_get_text (GTK_EDITABLE (self->entry)))
    {
      gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
      gtk_widget_grab_focus (self->entry);
      return;
    }
  gtk_widget_set_visible (self->search, FALSE);
  gtk_widget_set_visible (self->search_button, TRUE);
}

/* --- the menu */

static void
on_sort (gpointer data)
{
  CityPicker *self = data;

  /* SortOrderMenuItemController: the other order, remembered */
  g_settings_set_enum (self->settings, "city-sort", get_sort (self) == CITY_SORT_NAME ? 1 : 0);
  show_list (self);
}

static void
on_settings (gpointer data)
{
  g_signal_emit (data, signals[SHOW_SETTINGS], 0);
}

static void
on_overflow (NeoPressable *button, CityPicker *self)
{
  ClockPopupMenu *menu = clock_popup_menu_new ();

  /* onPrepareOptionsItem: the item names the order it switches to */
  clock_popup_menu_append (menu, get_sort (self) == CITY_SORT_NAME ? _("Sort by time") : _("Sort by name"), on_sort,
                           self);
  clock_popup_menu_append (menu, _("Settings"), on_settings, self);
  clock_popup_menu_present (menu, GTK_WIDGET (button));
}

static void
on_up (NeoPressable *button, CityPicker *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

/* --- the drop shadow and the fast-scroll thumb follow the scrolling */

static void
on_shadow (NeoAnim *anim, double value, gpointer data)
{
  CityPicker *self = data;

  gtk_widget_set_opacity (self->shadow, value);
}

static void
on_scrolled (GtkAdjustment *adjustment, CityPicker *self)
{
  double value = gtk_adjustment_get_value (adjustment);
  double range = gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment);
  gboolean shown = value > 0;
  int height = gtk_widget_get_height (self->track);

  /* DropShadowController: in once the list leaves its top, out when it is back */
  if (shown != self->shadow_shown)
    {
      self->shadow_shown = shown;
      neo_anim_start (self->shadow_anim, gtk_widget_get_opacity (self->shadow), shown, SHADOW_MS, 0,
                      SPONGY_ACCELERATE_DECELERATE);
    }
  gtk_widget_set_margin_top (self->thumb, range > 0 ? (int) round (CLAMP (value / range, 0, 1) * MAX (height - THUMB, 0))
                                                     : 0);
}

/* fast scroll: a finger on the track takes the list to the same fraction of its length */
static void
drag_to (CityPicker *self, double y)
{
  GtkAdjustment *adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller));
  double range = gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment);
  int height = gtk_widget_get_height (self->track);

  if (height > THUMB && range > 0)
    spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroller),
                                  CLAMP ((y - THUMB / 2.0) / (height - THUMB), 0, 1) * range);
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, CityPicker *self)
{
  gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  drag_to (self, y);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CityPicker *self)
{
  double x, y;

  gtk_gesture_drag_get_start_point (drag, &x, &y);
  drag_to (self, y + dy);
}

/* --- the widget */

static void
city_picker_map (GtkWidget *widget)
{
  CityPicker *self = CLOCK_CITY_PICKER (widget);

  GTK_WIDGET_CLASS (city_picker_parent_class)->map (widget);
  /* onResume */
  refresh (self);
  schedule_tick (self);
}

static void
city_picker_unmap (GtkWidget *widget)
{
  CityPicker *self = CLOCK_CITY_PICKER (widget);

  /* onPause: covered by Settings, closed, or the app gone */
  g_clear_handle_id (&self->tick, g_source_remove);
  save (self);
  GTK_WIDGET_CLASS (city_picker_parent_class)->unmap (widget);
}

static void
city_picker_dispose (GObject *object)
{
  CityPicker *self = CLOCK_CITY_PICKER (object);

  g_clear_handle_id (&self->tick, g_source_remove);
  g_clear_pointer (&self->shadow_anim, neo_anim_free);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->rows, g_hash_table_unref);
  g_clear_object (&self->header);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (city_picker_parent_class)->dispose (object);
}

static void
city_picker_finalize (GObject *object)
{
  CityPicker *self = CLOCK_CITY_PICKER (object);

  g_ptr_array_unref (self->original);
  g_ptr_array_unref (self->checked);
  g_free (self->query);
  G_OBJECT_CLASS (city_picker_parent_class)->finalize (object);
}

static void
city_picker_class_init (CityPickerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = city_picker_dispose;
  G_OBJECT_CLASS (klass)->finalize = city_picker_finalize;
  widget_class->map = city_picker_map;
  widget_class->unmap = city_picker_unmap;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "city-picker");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[SHOW_SETTINGS] = g_signal_new ("show-settings", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
}

static void
city_picker_init (CityPicker *self)
{
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *more, *close;
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->rows = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, row_free);
  self->original = g_ptr_array_new ();
  self->checked = g_ptr_array_new ();
  self->query = g_strdup ("");
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->box, "city-picker");
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));

  /* the action bar: Up, no title, then the search and ⋮ (48 and 40 dp, still:clock-cities) */
  self->bar = clock_bar_new (NULL);
  gtk_widget_add_css_class (self->bar, "cities");
  /* this activity's bar centres its Up arrow on the 64 dp (y 72…119 px on still:clock-cities, where Settings has it
   * at 60…107) */
  gtk_widget_set_valign (clock_bar_get_up (self->bar), GTK_ALIGN_CENTER);
  g_signal_connect (clock_bar_get_up (self->bar), "clicked", G_CALLBACK (on_up), self);
  self->search_button = clock_bar_add_icon (self->bar, "clock-search-symbolic", 48, _("Search"));
  g_signal_connect (self->search_button, "clicked", G_CALLBACK (on_search), self);
  /* the overflow button: 36 dp (abc_action_button_min_width_overflow_material), 4 dp from the edge */
  more = clock_bar_add_icon (self->bar, "clock-more-symbolic", 36, _("More options"));
  gtk_widget_set_margin_end (more, 4);
  g_signal_connect (more, "clicked", G_CALLBACK (on_overflow), self);

  /* the SearchView, expanded: the query field (capital words, as CapWords asks) and its close button */
  self->search = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  self->entry = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (self->entry), _("Search…"));
  gtk_text_set_input_hints (GTK_TEXT (self->entry), GTK_INPUT_HINT_UPPERCASE_WORDS | GTK_INPUT_HINT_NO_SPELLCHECK);
  gtk_widget_set_hexpand (self->entry, TRUE);
  gtk_widget_set_valign (self->entry, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (self->entry, "city-search");
  g_signal_connect (self->entry, "changed", G_CALLBACK (on_query), self);
  gtk_box_append (GTK_BOX (self->search), self->entry);
  close = gtk_image_new_from_icon_name ("clock-close-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (close), 24);
  self->clear = neo_pressable_new (close);
  gtk_widget_set_size_request (self->clear, 48, 48);
  gtk_widget_set_valign (self->clear, GTK_ALIGN_CENTER);
  neo_pressable_set_ripple (NEO_PRESSABLE (self->clear), NEO_RIPPLE_CIRCLE, 20);
  gtk_widget_add_css_class (self->clear, "clock-bar-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->clear), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Clear query"),
                                  -1);
  g_signal_connect (self->clear, "clicked", G_CALLBACK (on_clear), self);
  gtk_box_append (GTK_BOX (self->search), self->clear);
  gtk_widget_set_visible (self->search, FALSE);
  clock_bar_set_custom (self->bar, self->search);
  gtk_box_append (GTK_BOX (self->box), self->bar);

  /* the list, the fast-scroll track at its end, and the drop shadow along its top */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_end (self->list, SCROLLBAR);
  self->header = g_object_ref_sink (gtk_label_new (_("Selected Cities")));
  gtk_label_set_xalign (GTK_LABEL (self->header), 0);
  gtk_widget_add_css_class (self->header, "city-header");
  neo_label_set_font_padding (self->header, 20);
  self->scroller = spongy_scroll_view_new ();
  gtk_widget_set_vexpand (self->scroller, TRUE);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller)), "value-changed",
                    G_CALLBACK (on_scrolled), self);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller)), "changed",
                    G_CALLBACK (on_scrolled), self);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), self->scroller);

  self->track = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (self->track, SCROLLBAR, -1);
  gtk_widget_set_halign (self->track, GTK_ALIGN_END);
  gtk_widget_add_css_class (self->track, "city-fast-track");
  self->thumb = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (self->thumb, SCROLLBAR, THUMB);
  gtk_widget_set_valign (self->thumb, GTK_ALIGN_START);
  gtk_widget_set_vexpand (self->thumb, TRUE);
  gtk_widget_add_css_class (self->thumb, "city-fast-thumb");
  gtk_box_append (GTK_BOX (self->track), self->thumb);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  gtk_widget_add_controller (self->track, GTK_EVENT_CONTROLLER (drag));
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->track);

  self->shadow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->shadow, GTK_ALIGN_START);
  gtk_widget_set_can_target (self->shadow, FALSE);
  gtk_widget_add_css_class (self->shadow, "city-drop-shadow");
  gtk_widget_set_opacity (self->shadow, 0);
  self->shadow_anim = neo_anim_new (self->shadow, on_shadow, self);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->shadow);
  gtk_widget_set_vexpand (overlay, TRUE);
  gtk_box_append (GTK_BOX (self->box), overlay);
}

GtkWidget *
city_picker_new (GSettings *settings)
{
  CityPicker *self = g_object_new (CLOCK_TYPE_CITY_PICKER, NULL);

  self->settings = g_object_ref (settings);
  return GTK_WIDGET (self);
}
