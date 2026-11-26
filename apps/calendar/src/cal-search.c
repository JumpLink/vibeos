/* Search (cal-search.h): the search field and the search page.
 *
 *  - "Search" in the overflow menu opens a search field in the main toolbar: the collapse arrow takes the
 *    navigation button's place, the field takes the title's, Today and ⋮ stay. Focused, it lists the searches made
 *    before (the "search-history" key), newest first, those containing the text typed.
 *  - Submitting, or tapping a suggestion, closes it and opens the search page: the same field with the query and
 *    unfocused, Today, ⋮ with Settings, over an Agenda of the matching events from the shown calendars. Its
 *    collapse arrow returns home.
 *  - The match: the query cut into words (or "quoted phrases"), every word found in the title, the description,
 *    the place, or a guest's address or name. */
#include "cal-search.h"
#include "agenda-view.h"
#include "cal-colors.h"
#include "cal-model.h"
#include "cal-page.h"
#include "cal-popup.h"
#include "cal-settings.h"
#include "cal-store.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define MAX_HISTORY 250         /* the searches kept */
#define ROW_HEIGHT 58.0         /* a suggestion's row */
#define ICON_OFFSET 40.0        /* the suggestions' icon column and text padding */
#define MENU_WIDTH 196.0        /* the overflow menu's least width, as cal-window.c's */
#define EDGE 8.0

/* --- the icons: the magnifier (the hint), a history clock (a suggestion) and a suggestion's refine arrow, tinted by
 * the CSS colour */

typedef enum { ICON_SEARCH, ICON_HISTORY, ICON_COMMIT } IconKind;

typedef struct {
  GtkWidget parent;
  IconKind kind;
  double size;
  double x, y;          /* where it is drawn, or centred when < 0 */
} CalSearchIcon;

typedef struct {
  GtkWidgetClass parent_class;
} CalSearchIconClass;

G_DEFINE_TYPE (CalSearchIcon, cal_search_icon, GTK_TYPE_WIDGET)

/* the vector paths, on a 24 unit grid (a search and a history icon) */
static const char *search_path =
  "M15.5 14h-.79l-.28-.27C15.41 12.59 16 11.11 16 9.5 16 5.91 13.09 3 9.5 3S3 5.91 3 9.5 5.91 16 9.5 16"
  "c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 "
  "9.5 11.99 14 9.5 14z";
static const char *history_path =
  "M13 3c-4.97 0-9 4.03-9 9H1l3.89 3.89.07.14L9 12H6c0-3.87 3.13-7 7-7s7 3.13 7 7-3.13 7-7 7c-1.93 0-3.68-.79-4.94"
  "-2.06l-1.42 1.42C8.27 19.99 10.51 21 13 21c4.97 0 9-4.03 9-9s-4.03-9-9-9zm-1 5v5l4.28 2.54.72-1.21-3.5-2.08V8H12z";
/* the refine arrow, on a 32 unit grid: an L at the top left and the diagonal out of its corner */
static const char *commit_path = "M 11.8 19 V 10.7 H 20 M 12.3 11.2 L 21.7 20.6";

static void
cal_search_icon_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalSearchIcon *self = (CalSearchIcon *) widget;
  double grid = self->kind == ICON_COMMIT ? 32 : 24;
  g_autoptr(GskPath) path = gsk_path_parse (self->kind == ICON_SEARCH ? search_path
                                            : self->kind == ICON_HISTORY ? history_path : commit_path);
  GdkRGBA color;

  if (!path)
    return;
  gtk_widget_get_color (widget, &color);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot,
                          &GRAPHENE_POINT_INIT (self->x >= 0 ? self->x : (gtk_widget_get_width (widget) - self->size) / 2,
                                                self->y >= 0 ? self->y : (gtk_widget_get_height (widget) - self->size) / 2));
  gtk_snapshot_scale (snapshot, self->size / grid, self->size / grid);
  if (self->kind == ICON_COMMIT)
    {
      GskStroke *stroke = gsk_stroke_new (1.5);

      gtk_snapshot_append_stroke (snapshot, path, stroke, &color);
      gsk_stroke_free (stroke);
    }
  else
    gtk_snapshot_append_fill (snapshot, path, GSK_FILL_RULE_WINDING, &color);
  gtk_snapshot_restore (snapshot);
}

static void
cal_search_icon_class_init (CalSearchIconClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = cal_search_icon_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "image");
}

static void
cal_search_icon_init (CalSearchIcon *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

static GtkWidget *
search_icon_new (IconKind kind, double size, int width, int height)
{
  CalSearchIcon *self = g_object_new (cal_search_icon_get_type (), NULL);

  self->kind = kind;
  self->size = size;
  self->x = self->y = -1;
  gtk_widget_set_size_request (GTK_WIDGET (self), width, height);
  return GTK_WIDGET (self);
}

/* --- the Today button (the date in a calendar icon, as the main toolbar draws it) */

typedef struct {
  GtkWidget parent;
} CalSearchToday;

typedef struct {
  GtkWidgetClass parent_class;
} CalSearchTodayClass;

G_DEFINE_TYPE (CalSearchToday, cal_search_today, GTK_TYPE_WIDGET)

static void
cal_search_today_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  GdkRGBA color = cal_argb (CAL_ON_SURFACE_VARIANT);
  double size = 28;
  double x = (gtk_widget_get_width (widget) - size) / 2, y = (gtk_widget_get_height (widget) - size) / 2 - 2;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *number = g_strdup_printf ("%d", g_date_time_get_day_of_month (now));
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, number);
  PangoFontDescription *font = pango_font_description_from_string ("VibeOS Roboto, Roboto");
  int w, h;

  vibeos_snapshot_icon (snapshot, widget, "cal-today-symbolic", x, y, size, &color);
  pango_font_description_set_absolute_size (font, 12 * PANGO_SCALE);
  pango_font_description_set_weight (font, PANGO_WEIGHT_BOLD);
  pango_layout_set_font_description (layout, font);
  pango_layout_get_pixel_size (layout, &w, &h);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x + (size - w) / 2.0,
                                                          y + (size + 12 * 0.711 + 1) / 1.75
                                                            - pango_layout_get_baseline (layout) / (double) PANGO_SCALE
                                                            + 2));
  gtk_snapshot_append_layout (snapshot, layout, &color);
  gtk_snapshot_restore (snapshot);
  pango_font_description_free (font);
  g_object_unref (layout);
}

static void
cal_search_today_class_init (CalSearchTodayClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = cal_search_today_snapshot;
}

static void
cal_search_today_init (CalSearchToday *self)
{
  gtk_widget_set_size_request (GTK_WIDGET (self), 48, 48);
}

/* --- the history: newest first, each query once, 250 at most */

static void
history_save (GSettings *settings, const char *query)
{
  g_auto(GStrv) old = g_settings_get_strv (settings, "search-history");
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  g_auto(GStrv) history = NULL;
  int n = 1;

  g_strv_builder_add (builder, query);
  for (int i = 0; old[i] && n < MAX_HISTORY; i++)
    if (g_strcmp0 (old[i], query) != 0)
      {
        g_strv_builder_add (builder, old[i]);
        n++;
      }
  history = g_strv_builder_end (builder);
  g_settings_set_strv (settings, "search-history", (const char *const *) history);
}

/* --- the match: the query cut into words, each looked for in the event */

static GPtrArray *
tokenize (const char *query)
{
  static GRegex *pattern;
  GPtrArray *tokens = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GMatchInfo) match = NULL;

  if (!pattern)
    pattern = g_regex_new ("[^\\s\"'.?!,]+|\"([^\"]*)\"", G_REGEX_OPTIMIZE, 0, NULL);
  g_regex_match (pattern, query, 0, &match);
  for (; g_match_info_matches (match); g_match_info_next (match, NULL))
    {
      g_autofree char *quoted = g_match_info_fetch (match, 1);
      g_autofree char *token = quoted && *quoted ? g_steal_pointer (&quoted) : g_match_info_fetch (match, 0);

      /* a quoted "" is a token of nothing, which every event matches (LIKE '%%') */
      if (token[0] == '"')
        token[0] = '\0';
      g_ptr_array_add (tokens, g_utf8_casefold (token, -1));
    }
  return tokens;
}

static gboolean
contains (const char *haystack, const char *token)
{
  g_autofree char *folded = NULL;

  if (!haystack)
    return !*token;
  folded = g_utf8_casefold (haystack, -1);
  return strstr (folded, token) != NULL;
}

/* every token in a column: title, description, place, or a guest's address or name (the organizer is one of
 * them) */
static gboolean
event_matches (const CalEvent *event, GPtrArray *tokens)
{
  for (guint t = 0; t < tokens->len; t++)
    {
      const char *token = tokens->pdata[t];
      gboolean found = contains (event->title, token) || contains (event->description, token)
                       || contains (event->location, token);

      for (guint i = 0; !found && event->attendees && i < event->attendees->len; i++)
        {
          const CalAttendee *attendee = event->attendees->pdata[i];

          found = contains (attendee->email, token) || contains (attendee->name, token);
        }
      if (!found)
        return FALSE;
    }
  return TRUE;
}

/* --- a search field */

typedef struct _Search Search;

struct _Search {
  GtkWidget *window;
  GSettings *settings;
  GtkWidget *view, *frame, *plate, *text, *hint, *clear;
  GtkWidget *dropdown, *list;           /* the suggestions, while they show */
  GtkGesture *outside;                  /* a press elsewhere closes them */
  gboolean quiet;                       /* setting the text: no suggestions */
  void (*submit) (Search *search, const char *query);

  /* in the main toolbar */
  GtkWidget *bar, *collapse;
  GtkEventController *escape;

  guint tick;                           /* waiting for the field's first allocation */

  /* on the search page */
  GtkWidget *page, *agenda;
  GCancellable *query;
  char *query_text;
  int day;
  CalStore *store;
};

static void dropdown_update (Search *search);

/* a controller of the window, taken off it perhaps while it runs: its last reference goes once the event is done */
static void
remove_window_controller (GtkWidget *window, GtkEventController *controller)
{
  g_object_ref (controller);
  gtk_widget_remove_controller (window, controller);
  g_idle_add_once ((GSourceOnceFunc) g_object_unref, controller);
}

static void
dropdown_hide (Search *search)
{
  if (search->outside)
    {
      remove_window_controller (search->window, GTK_EVENT_CONTROLLER (search->outside));
      search->outside = NULL;
    }
  if (search->dropdown)
    {
      GtkWidget *parent = gtk_widget_get_parent (search->dropdown);

      if (parent)
        gtk_overlay_remove_overlay (GTK_OVERLAY (parent), search->dropdown);
      search->dropdown = search->list = NULL;
    }
}

static void
set_text (Search *search, const char *text)
{
  search->quiet = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (search->text), text);
  gtk_editable_set_position (GTK_EDITABLE (search->text), -1);
  search->quiet = FALSE;
}

static void
on_suggestion (VibeosPressable *row, Search *search)
{
  g_autofree char *query = g_strdup (g_object_get_data (G_OBJECT (row), "cal-query"));

  dropdown_hide (search);
  search->submit (search, query);
}

/* the refine arrow: the suggestion into the field, to change it */
static void
on_refine (VibeosPressable *button, Search *search)
{
  gtk_editable_set_text (GTK_EDITABLE (search->text), g_object_get_data (G_OBJECT (button), "cal-query"));
  gtk_editable_set_position (GTK_EDITABLE (search->text), -1);
  gtk_widget_grab_focus (search->text);
}

static GtkWidget *
suggestion_row (Search *search, const char *query)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new (query);
  GtkWidget *refine = vibeos_pressable_new (search_icon_new (ICON_COMMIT, 32, 48, 48));
  GtkWidget *row;

  gtk_box_append (GTK_BOX (box), search_icon_new (ICON_HISTORY, 24, 32, 48));
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_add_css_class (label, "cal-search-suggestion-text");
  gtk_box_append (GTK_BOX (box), label);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (refine), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_widget_set_valign (refine, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (refine, "cal-search-refine");
  gtk_accessible_update_property (GTK_ACCESSIBLE (refine), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Edit query"), -1);
  g_object_set_data_full (G_OBJECT (refine), "cal-query", g_strdup (query), g_free);
  g_signal_connect (refine, "clicked", G_CALLBACK (on_refine), search);
  gtk_box_append (GTK_BOX (box), refine);
  gtk_widget_add_css_class (box, "cal-search-suggestion");
  row = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, query, -1);
  g_object_set_data_full (G_OBJECT (row), "cal-query", g_strdup (query), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_suggestion), search);
  return row;
}

static gboolean
is_inside (GtkWidget *widget, GtkWidget *ancestor)
{
  return widget && ancestor && (widget == ancestor || gtk_widget_is_ancestor (widget, ancestor));
}

/* a touch outside the list closes it, and goes on to what is under it */
static void
on_outside (GtkGestureClick *gesture, int n, double x, double y, Search *search)
{
  GtkWidget *picked = gtk_widget_pick (search->window, x, y, GTK_PICK_DEFAULT);

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
  if (!is_inside (picked, search->dropdown) && !is_inside (picked, search->view))
    dropdown_hide (search);
}

/* the list under the edit frame, reaching ICON_OFFSET further to the start so the queries line up with the field's
 * text; as tall as its rows, within the window */
static void
dropdown_update (Search *search)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (search->window);
  g_auto(GStrv) history = g_settings_get_strv (search->settings, "search-history");
  g_autofree char *needle = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (search->text)), -1);
  graphene_rect_t frame;
  double width, height, top, window_height;
  GtkWidget *child, *scroll;
  int n = 0;

  if (search->quiet)
    return;
  if (!overlay || !gtk_widget_has_focus (search->text) || !gtk_widget_get_mapped (search->frame)
      || !gtk_widget_compute_bounds (search->frame, GTK_WIDGET (overlay), &frame))
    {
      dropdown_hide (search);
      return;
    }
  if (!search->dropdown)
    {
      search->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      scroll = spongy_scroll_view_new ();
      spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), search->list);
      gtk_widget_set_vexpand (scroll, TRUE);
      search->dropdown = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class (search->dropdown, "cal-menu");
      gtk_widget_add_css_class (search->dropdown, "cal-search-dropdown");
      gtk_widget_set_overflow (search->dropdown, GTK_OVERFLOW_HIDDEN);
      gtk_widget_set_halign (search->dropdown, GTK_ALIGN_START);
      gtk_widget_set_valign (search->dropdown, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (search->dropdown), scroll);
      gtk_overlay_add_overlay (overlay, search->dropdown);
      search->outside = gtk_gesture_click_new ();
      gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (search->outside), GTK_PHASE_CAPTURE);
      g_signal_connect (search->outside, "pressed", G_CALLBACK (on_outside), search);
      gtk_widget_add_controller (search->window, GTK_EVENT_CONTROLLER (search->outside));
    }
  while ((child = gtk_widget_get_first_child (search->list)))
    gtk_box_remove (GTK_BOX (search->list), child);
  /* the searches containing the text, newest first; all of them for no text */
  for (int i = 0; history[i]; i++)
    if (!*needle || contains (history[i], needle))
      {
        gtk_box_append (GTK_BOX (search->list), suggestion_row (search, history[i]));
        n++;
      }
  if (!n)
    {
      dropdown_hide (search);
      return;
    }
  window_height = gtk_widget_get_height (GTK_WIDGET (overlay));
  width = frame.size.width + ICON_OFFSET;
  top = frame.origin.y + frame.size.height;
  height = MIN (n * ROW_HEIGHT + 16, window_height - top - EDGE);
  gtk_widget_set_size_request (search->dropdown, (int) width, (int) MAX (height, ROW_HEIGHT));
  gtk_widget_set_margin_start (search->dropdown, (int) MAX (0, frame.origin.x - ICON_OFFSET));
  gtk_widget_set_margin_top (search->dropdown, (int) top);
}

/* the suggestions once the field has its place (it gets the focus as it is added to the toolbar) */
static gboolean
on_frame_tick (GtkWidget *frame, GdkFrameClock *clock, gpointer data)
{
  Search *search = data;

  if (gtk_widget_get_width (frame) <= 0)
    return G_SOURCE_CONTINUE;
  search->tick = 0;
  dropdown_update (search);
  return G_SOURCE_REMOVE;
}

/* the callback goes with the field, before the Search does (it belongs to the view or the page holding the field) */
static void
on_tick_gone (gpointer data)
{
  ((Search *) data)->tick = 0;
}

static void
dropdown_queue (Search *search)
{
  if (!search->tick)
    search->tick = gtk_widget_add_tick_callback (search->frame, on_frame_tick, search, on_tick_gone);
}

static void
update_decorations (Search *search)
{
  gboolean empty = !*gtk_editable_get_text (GTK_EDITABLE (search->text));

  /* the hint (a magnifier, the query hint being empty) without text; the X with text */
  gtk_widget_set_visible (search->hint, empty);
  gtk_widget_set_visible (search->clear, !empty);
}

static void
on_text_changed (GtkEditable *editable, Search *search)
{
  update_decorations (search);
  dropdown_update (search);
}

static void
on_activate (GtkText *text, Search *search)
{
  g_autofree char *copy = g_strdup (gtk_editable_get_text (GTK_EDITABLE (text)));
  g_autofree char *trimmed = g_strstrip (g_strdup (copy));

  /* only a query with more than spaces */
  if (!*trimmed)
    return;
  dropdown_hide (search);
  search->submit (search, copy);
}

static void
on_clear (VibeosPressable *button, Search *search)
{
  /* the X with text: empty it, focus, the keyboard up */
  gtk_editable_set_text (GTK_EDITABLE (search->text), "");
  gtk_widget_set_focusable (search->text, TRUE);
  gtk_widget_grab_focus (search->text);
}

static void
on_focus_enter (GtkEventControllerFocus *focus, Search *search)
{
  gtk_widget_add_css_class (search->plate, "focused");
  if (gtk_widget_get_width (search->frame) > 0)
    dropdown_update (search);
  else
    dropdown_queue (search);
}

static void
on_focus_leave (GtkEventControllerFocus *focus, Search *search)
{
  gtk_widget_remove_css_class (search->plate, "focused");
  dropdown_hide (search);
}

/* a tap on the field shows the suggestions again; on the search page, where the field is kept out of the focus
 * chain until then, it takes the focus */
static void
on_text_pressed (GtkGestureClick *gesture, int n, double x, double y, Search *search)
{
  if (!gtk_widget_get_focusable (search->text))
    {
      gtk_widget_set_focusable (search->text, TRUE);
      gtk_widget_grab_focus (search->text);
      gtk_editable_set_position (GTK_EDITABLE (search->text), -1);
    }
  if (gtk_widget_has_focus (search->text))
    dropdown_update (search);
}

/* the search view: the edit frame; the plate, its underline, holding the text and the X; the hint a magnifier
 * 1.25 × the text size after a space */
static void
search_view_build (Search *search)
{
  GtkWidget *area = gtk_overlay_new ();
  GtkEventController *focus = gtk_event_controller_focus_new ();
  GtkGesture *press = gtk_gesture_click_new ();

  search->text = gtk_text_new ();
  gtk_text_set_input_hints (GTK_TEXT (search->text), GTK_INPUT_HINT_NO_SPELLCHECK);
  gtk_widget_set_hexpand (search->text, TRUE);
  gtk_widget_set_valign (search->text, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (search->text, "cal-search-text");
  gtk_accessible_update_property (GTK_ACCESSIBLE (search->text), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Search query"),
                                  -1);
  g_signal_connect (search->text, "changed", G_CALLBACK (on_text_changed), search);
  g_signal_connect (search->text, "activate", G_CALLBACK (on_activate), search);
  g_signal_connect (focus, "enter", G_CALLBACK (on_focus_enter), search);
  g_signal_connect (focus, "leave", G_CALLBACK (on_focus_leave), search);
  gtk_widget_add_controller (search->text, focus);
  g_signal_connect (press, "released", G_CALLBACK (on_text_pressed), search);
  gtk_widget_add_controller (search->text, GTK_EVENT_CONTROLLER (press));
  gtk_overlay_set_child (GTK_OVERLAY (area), search->text);

  /* "   " with the icon on the second space, its bottom on the line's bottom: x after the 8 px padding and a space
   * (4.46 px); y with the line (the icon's 22.5 px over the font's 4.9 px descent) centred in the 36 px field, which
   * is centred in the 48 px plate */
  search->hint = search_icon_new (ICON_SEARCH, 22.5, -1, -1);
  ((CalSearchIcon *) search->hint)->x = 8 + 4.46;
  ((CalSearchIcon *) search->hint)->y = (48 - 36) / 2.0 + (36 - (22.5 + 4.9)) / 2 + (22.5 + 4.9) - 22.5;
  gtk_widget_add_css_class (search->hint, "cal-search-hint");
  gtk_overlay_add_overlay (GTK_OVERLAY (area), search->hint);
  gtk_widget_set_hexpand (area, TRUE);

  search->clear = vibeos_icon_button_new ("cal-close-symbolic");
  gtk_widget_add_css_class (search->clear, "cal-search-close");
  gtk_accessible_update_property (GTK_ACCESSIBLE (search->clear), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Clear query"),
                                  -1);
  g_signal_connect (search->clear, "clicked", G_CALLBACK (on_clear), search);

  search->plate = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (search->plate, "cal-search-plate");
  gtk_widget_set_hexpand (search->plate, TRUE);
  gtk_box_append (GTK_BOX (search->plate), area);
  gtk_box_append (GTK_BOX (search->plate), search->clear);
  search->frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (search->frame, "cal-search-frame");
  gtk_widget_set_hexpand (search->frame, TRUE);
  gtk_box_append (GTK_BOX (search->frame), search->plate);
  search->view = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (search->view, "cal-search-view");
  gtk_widget_set_hexpand (search->view, TRUE);
  gtk_widget_set_valign (search->view, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (search->view), search->frame);
  update_decorations (search);
}

static void
search_free (gpointer data)
{
  Search *search = data;

  dropdown_hide (search);
  if (search->escape)
    remove_window_controller (search->window, search->escape);
  if (search->store)
    {
      g_signal_handlers_disconnect_by_data (search->store, search);
      g_object_unref (search->store);
    }
  if (search->query)
    {
      g_cancellable_cancel (search->query);
      g_object_unref (search->query);
    }
  g_free (search->query_text);
  g_free (search);
}

/* --- the search page */

static void page_search (Search *search, const char *query);

static void
on_results (GPtrArray *events, gpointer data)
{
  Search *search = data;
  g_autoptr(GPtrArray) tokens = NULL;
  gboolean hide_declined;

  if (!events)
    return;             /* cancelled (perhaps with the page gone): a newer query is on its way */
  tokens = tokenize (search->query_text);
  hide_declined = g_settings_get_boolean (search->settings, "hide-declined");
  for (guint i = events->len; i-- > 0;)
    {
      CalEvent *event = events->pdata[i];

      if ((hide_declined && event->self_status == CAL_STATUS_DECLINED) || !event_matches (event, tokens))
        g_ptr_array_remove_index (events, i);
    }
  cal_agenda_view_set_events (CAL_AGENDA_VIEW (search->agenda), events);
}

/* the search over the days the agenda holds, the shown calendars only */
static void
run_query (Search *search)
{
  int first, last;

  cal_agenda_view_get_range (CAL_AGENDA_VIEW (search->agenda), &first, &last);
  if (search->query)
    {
      g_cancellable_cancel (search->query);
      g_object_unref (search->query);
    }
  search->query = g_cancellable_new ();
  cal_store_query (search->store, cal_day_start (first), cal_day_start (last + 1), search->query, on_results, search);
}

static void
on_range_changed (CalAgendaView *agenda, Search *search)
{
  run_query (search);
}

static void
on_store_changed (CalStore *store, Search *search)
{
  run_query (search);
}

static void
on_event_activated (CalAgendaView *agenda, CalEvent *event, Search *search)
{
  cal_window_show_event (search->window, event);
}

static int
today (void)
{
  return cal_day_number (g_get_real_time () / G_USEC_PER_SEC);
}

static void
on_page_today (VibeosPressable *button, Search *search)
{
  /* to today */
  search->day = today ();
  cal_agenda_view_go_to (CAL_AGENDA_VIEW (search->agenda), search->day);
  run_query (search);
}

static void
on_page_menu (int index, gpointer data)
{
  Search *search = data;

  /* ⋮ ▸ Settings */
  cal_window_push (search->window, cal_settings_page_new (search->window));
}

static void
on_page_more (VibeosPressable *button, Search *search)
{
  const char *labels[] = { _("Settings") };

  cal_popup_show (GTK_WIDGET (button), labels, 1, -1, MENU_WIDTH, TRUE, on_page_menu, search);
}

/* Back: the suggestions first, then home */
static gboolean
on_page_back (CalPage *page, gpointer data)
{
  Search *search = data;

  if (search->dropdown)
    {
      dropdown_hide (search);
      return TRUE;
    }
  return FALSE;
}

/* a search: the query into the history, into the field (unfocused), and the agenda searched from the day
 * it was last sent to */
static void
page_search (Search *search, const char *query)
{
  history_save (search->settings, query);
  g_free (search->query_text);
  search->query_text = g_strdup (query);
  set_text (search, query);
  /* unfocused, and out of the focus chain until tapped, or GtkStack hands the focus to it with the page */
  if (gtk_widget_has_focus (search->text))
    gtk_window_set_focus (GTK_WINDOW (search->window), NULL);
  gtk_widget_set_focusable (search->text, FALSE);
  cal_agenda_view_go_to (CAL_AGENDA_VIEW (search->agenda), search->day);
  run_query (search);
}

static void
on_page_submit (Search *search, const char *query)
{
  page_search (search, query);
}

static void
push_search_page (GtkWidget *window, const char *query)
{
  Search *search = g_new0 (Search, 1);
  GtkWidget *page = cal_page_new (window), *actions, *today_button, *more, *bar;

  search->window = window;
  search->settings = cal_window_get_settings (window);
  search->store = g_object_ref (cal_window_get_store (window));
  search->submit = on_page_submit;
  search->page = page;
  search->day = today ();
  search_view_build (search);

  /* the bar: the search field, Today, ⋮ (Settings) */
  bar = cal_page_toolbar (CAL_PAGE (page), NULL, &actions);
  gtk_widget_add_css_class (bar, "cal-search-bar");
  gtk_widget_set_halign (actions, GTK_ALIGN_FILL);
  gtk_box_append (GTK_BOX (actions), search->view);
  today_button = vibeos_pressable_new (g_object_new (cal_search_today_get_type (), NULL));
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (today_button), VIBEOS_RIPPLE_CIRCLE, 20);
  gtk_accessible_update_property (GTK_ACCESSIBLE (today_button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Today"), -1);
  g_signal_connect (today_button, "clicked", G_CALLBACK (on_page_today), search);
  gtk_box_append (GTK_BOX (actions), today_button);
  more = vibeos_icon_button_new ("cal-more-symbolic");
  gtk_widget_add_css_class (more, "cal-toolbar-more");
  gtk_widget_add_css_class (more, "vibeos-top-bar-end");
  gtk_accessible_update_property (GTK_ACCESSIBLE (more), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  g_signal_connect (more, "clicked", G_CALLBACK (on_page_more), search);
  gtk_box_append (GTK_BOX (actions), more);
  gtk_box_append (GTK_BOX (page), bar);

  /* the body: an Agenda of the results */
  search->agenda = cal_agenda_view_new ();
  gtk_widget_set_vexpand (search->agenda, TRUE);
  g_signal_connect (search->agenda, "range-changed", G_CALLBACK (on_range_changed), search);
  g_signal_connect (search->agenda, "event-activated", G_CALLBACK (on_event_activated), search);
  gtk_box_append (GTK_BOX (page), search->agenda);

  g_signal_connect (search->store, "changed", G_CALLBACK (on_store_changed), search);
  g_signal_connect (search->store, "calendars-changed", G_CALLBACK (on_store_changed), search);
  cal_page_set_back_handler (CAL_PAGE (page), on_page_back, search);
  g_object_set_data_full (G_OBJECT (page), "cal-search", search, search_free);
  page_search (search, query);
  cal_window_push (window, page);
}

/* --- the main toolbar's search */

static GtkWidget *
find_main_toolbar (GtkWidget *widget)
{
  if (gtk_widget_has_css_class (widget, "cal-toolbar"))
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
      if (gtk_widget_has_css_class (child, "cal-toolbar-nav"))
        return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_main_toolbar (child);

      if (found)
        return found;
    }
  return NULL;
}

static void
main_collapse (Search *search)
{
  GtkWidget *bar = search->bar;

  dropdown_hide (search);
  for (GtkWidget *child = gtk_widget_get_first_child (bar); child; child = gtk_widget_get_next_sibling (child))
    if (gtk_widget_has_css_class (child, "cal-toolbar-nav") || gtk_widget_has_css_class (child, "cal-toolbar-titles"))
      gtk_widget_set_visible (child, TRUE);
  gtk_box_remove (GTK_BOX (bar), search->collapse);
  /* the view holds the last reference to its Search: the struct goes with it */
  g_object_set_data (G_OBJECT (bar), "cal-search", NULL);
  gtk_box_remove (GTK_BOX (bar), search->view);
}

static void
on_collapse (VibeosPressable *button, Search *search)
{
  main_collapse (search);
}

static void
on_main_submit (Search *search, const char *query)
{
  GtkWidget *window = search->window;
  g_autofree char *copy = g_strdup (query);

  /* a query submitted or a suggestion tapped: the field closes, then the search page */
  main_collapse (search);
  push_search_page (window, copy);
}

static gboolean
has_child_with_class (GtkWidget *widget, const char *css_class)
{
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    if (gtk_widget_has_css_class (child, css_class))
      return TRUE;
  return FALSE;
}

/* the main screen is in front: no page over it, no menu, popup or dialog over the window, no drawer open */
static gboolean
main_in_front (Search *search)
{
  GtkWidget *stack = gtk_widget_get_ancestor (search->bar, GTK_TYPE_STACK);
  GtkWidget *visible = stack ? gtk_stack_get_visible_child (GTK_STACK (stack)) : NULL;
  GtkOverlay *overlay = vibeos_window_get_overlay (search->window);
  GtkWidget *content;

  if (stack && (!visible || !gtk_widget_is_ancestor (search->bar, visible)))
    return FALSE;
  if (overlay)
    for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (overlay)); child;
         child = gtk_widget_get_next_sibling (child))
      if (child != gtk_overlay_get_child (overlay) && child != search->dropdown && gtk_widget_get_visible (child))
        return FALSE;
  content = gtk_widget_get_ancestor (search->bar, GTK_TYPE_OVERLAY);
  if (content)
    for (GtkWidget *child = gtk_widget_get_first_child (content); child; child = gtk_widget_get_next_sibling (child))
      if (gtk_widget_get_visible (child) && has_child_with_class (child, "cal-drawer"))
        return FALSE;
  return TRUE;
}

/* Back while the main toolbar searches: the suggestions first, then the field closes */
static gboolean
on_main_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  Search *search = data;

  if (!main_in_front (search))
    return FALSE;
  if (search->dropdown)
    dropdown_hide (search);
  else
    main_collapse (search);
  return TRUE;
}

void
cal_search_open (GtkWidget *window)
{
  GtkWidget *bar = find_main_toolbar (window);
  Search *search;
  GtkEventController *escape;

  if (!bar)
    return;
  if ((search = g_object_get_data (G_OBJECT (bar), "cal-search")))
    {
      gtk_widget_grab_focus (search->text);
      return;
    }
  search = g_new0 (Search, 1);
  search->window = window;
  search->settings = cal_window_get_settings (window);
  search->bar = bar;
  search->submit = on_main_submit;
  search_view_build (search);
  g_object_set_data_full (G_OBJECT (search->view), "cal-search", search, search_free);
  g_object_set_data (G_OBJECT (bar), "cal-search", search);

  /* the field opens: the collapse button (the Up arrow) where the navigation button
   * was, the field after it; the title goes; the menu (Today, ⋮) stays */
  for (GtkWidget *child = gtk_widget_get_first_child (bar); child; child = gtk_widget_get_next_sibling (child))
    if (gtk_widget_has_css_class (child, "cal-toolbar-nav") || gtk_widget_has_css_class (child, "cal-toolbar-titles"))
      gtk_widget_set_visible (child, FALSE);
  /* "Back" is the name the shell's back gesture presses (extension/a11y.js) */
  search->collapse = cal_page_action ("cal-back-symbolic", _("Back"));
  gtk_widget_add_css_class (search->collapse, "cal-toolbar-back");
  gtk_widget_add_css_class (search->collapse, "cal-search-collapse");
  g_signal_connect (search->collapse, "clicked", G_CALLBACK (on_collapse), search);
  gtk_box_insert_child_after (GTK_BOX (bar), search->collapse, NULL);
  gtk_box_insert_child_after (GTK_BOX (bar), search->view, search->collapse);

  /* before the window's own Escape (which would close the window from the main screen) */
  escape = gtk_shortcut_controller_new ();
  gtk_event_controller_set_propagation_phase (escape, GTK_PHASE_CAPTURE);
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (escape),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_main_escape, search, NULL)));
  gtk_widget_add_controller (window, escape);
  search->escape = escape;
  gtk_widget_grab_focus (search->text);
}
