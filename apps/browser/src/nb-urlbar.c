/* nb-urlbar: the URL bar: the URL card, the tab count (the number of open tabs in a rounded square; a tap opens
 * the tab switcher, nb-switcher.c) and the menu button, the search group that replaces them for find in page, the
 * progress line, and the suggestions dropping from the card. */
#include "nb.h"
#include "nb-suggest.h"
#include "nb-widgets.h"

#include <vibeos.h>
#include <string.h>

#define FADE_LENGTH 12          /* the fading edge's length, px */

/* ---- the URL text ---- */

static gboolean
is_https (const char *url)
{
  return url && g_str_has_prefix (url, "https://");
}

/* the end of a URL too long for the card fades out: the card's colour over its last FADE_LENGTH, only
 * while the text is not being edited and does not fit */
static void
draw_fade (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  NbTab *t = data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (t->entry));
  g_autoptr (PangoLayout) layout = gtk_widget_create_pango_layout (t->entry, text);
  GdkRGBA card = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);
  int w;

  pango_layout_get_pixel_size (layout, &w, NULL);
  if (t->editing || w <= gtk_widget_get_width (t->entry))
    return;
  cairo_pattern_t *p = cairo_pattern_create_linear (0, 0, width, 0);
  cairo_pattern_add_color_stop_rgba (p, 0, card.red, card.green, card.blue, 0);
  cairo_pattern_add_color_stop_rgba (p, 1, card.red, card.green, card.blue, 1);
  cairo_set_source (cr, p);
  cairo_paint (cr);
  cairo_pattern_destroy (p);
}

static void
sync_fade (NbTab *t)
{
  gtk_widget_queue_draw (g_object_get_data (G_OBJECT (t->entry), "nb-fade"));
}

static void
set_text (NbTab *t, const char *text)
{
  t->setting_text = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (t->entry), text ? text : "");
  gtk_editable_set_position (GTK_EDITABLE (t->entry), 0);
  t->setting_text = FALSE;
  sync_fade (t);
}

/* the whole URL; the lock only for https */
void
nb_urlbar_sync (NbTab *t)
{
  const char *url = t->view ? webkit_web_view_get_uri (t->view) : t->url;

  if (!t->entry)
    return;
  if (!t->editing)
    set_text (t, url);
  gtk_widget_set_visible (t->incog_icon, t->incognito);
  gtk_widget_set_visible (t->lock, is_https (url));
  /* a little further into the card without the incognito icon before it; a gone lock collapses to a point where
   * it would start, with no margin, and the text keeps its gap after that */
  gtk_widget_set_margin_start (t->lock, t->incognito ? 12 : 14);
  gtk_image_set_from_icon_name (GTK_IMAGE (g_object_get_data (G_OBJECT (t->lock), "nb-image")),
                                t->tls_failed ? "jelly-warning-symbolic" : "jelly-lock-symbolic");
}

/* ---- the progress line ---- */

static void
draw_progress (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  NbTab *t = data;
  double p = t->view ? webkit_web_view_get_estimated_load_progress (t->view) : 0;
  GdkRGBA track = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST), bar = vibeos_color (VIBEOS_PRIMARY);

  gdk_cairo_set_source_rgba (cr, &track);
  cairo_paint (cr);
  gdk_cairo_set_source_rgba (cr, &bar);
  /* whole percents, no animation */
  cairo_rectangle (cr, 0, 0, round (p * 100) / 100 * width, height);
  cairo_fill (cr);
}

void
nb_urlbar_progress (NbTab *t)
{
  gtk_widget_set_visible (t->progress, t->view && webkit_web_view_is_loading (t->view));
  gtk_widget_queue_draw (t->progress);
}

/* ---- suggestions ---- */

static void
hide_dropdown (NbTab *t)
{
  if (t->suggest_cancel) {
    g_cancellable_cancel (t->suggest_cancel);
    g_clear_object (&t->suggest_cancel);
  }
  gtk_widget_set_visible (t->dropdown, FALSE);
}

/* the query's matches in bold */
static char *
bold_matches (const char *text, const char *query)
{
  g_autofree char *lower = g_utf8_strdown (text, -1);
  gsize qlen = strlen (query);
  GString *out = g_string_new (NULL);
  const char *p = text, *l = lower;

  /* the offsets in the lower-case copy are the text's only where lower-casing kept the byte lengths */
  if (!qlen || strlen (lower) != strlen (text)) {
    g_free (g_string_free (out, TRUE));
    return g_markup_escape_text (text, -1);
  }
  for (;;) {
    const char *hit = strstr (l, query);
    if (!hit) {
      g_autofree char *rest = g_markup_escape_text (p, -1);
      g_string_append (out, rest);
      break;
    }
    g_autofree char *before = g_markup_escape_text (p, hit - l);
    g_autofree char *match = g_markup_escape_text (p + (hit - l), qlen);
    g_string_append_printf (out, "%s<span weight=\"900\">%s</span>", before, match);
    p += hit - l + qlen;
    l = hit + qlen;
  }
  return g_string_free (out, FALSE);
}

static void
on_suggestion (VibeosPressable *row, NbTab *t)
{
  const char *target = g_object_get_data (G_OBJECT (row), "nb-target");
  g_autofree char *text = g_strdup (target);

  /* the keyboard goes, the field lets go of the focus, then the page loads */
  nb_urlbar_stop_editing (t);
  nb_tab_load (t, text);
}

static GtkWidget *
suggestion_row (NbTab *t, NbSuggestion *s, const char *query)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *icon = nb_icon ("jelly-search-symbolic", 24);
  g_autofree char *markup = bold_matches (s->title ? s->title : "", query);
  GtkWidget *title = nb_label (NULL, 18, "nb-suggest-title", TRUE);

  gtk_label_set_markup (GTK_LABEL (title), markup);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_widget_add_css_class (icon, "nb-suggest-icon");
  gtk_box_append (GTK_BOX (line), icon);
  gtk_box_append (GTK_BOX (line), title);
  /* the lines centred in the row, together */
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (lines, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (lines, TRUE);
  gtk_box_append (GTK_BOX (lines), line);
  if (s->url) {
    g_autofree char *url_markup = bold_matches (s->url, query);
    GtkWidget *url = nb_label (NULL, 14, "nb-suggest-url", TRUE);
    gtk_label_set_markup (GTK_LABEL (url), url_markup);
    gtk_box_append (GTK_BOX (lines), url);
  }
  gtk_box_append (GTK_BOX (box), lines);
  gtk_widget_add_css_class (box, "nb-suggest-row");
  GtkWidget *row = vibeos_pressable_new (box);
  /* the URL, else the title */
  g_object_set_data_full (G_OBJECT (row), "nb-target", g_strdup (s->url ? s->url : s->title), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_suggestion), t);
  return row;
}

/* The host the text would go to (the web address typed, or the search engine's), looked up in DNS while the user
 * types, so the navigation does not wait for it. Not in an incognito tab: a host typed and not visited stays unasked. */
static void
prefetch_host (NbTab *t, const char *text)
{
  static char *last;
  if (t->incognito || !text || !*text)
    return;
  g_autofree char *url = nb_smart_url (text);
  g_autoptr (GUri) uri = url ? g_uri_parse (url, G_URI_FLAGS_NONE, NULL) : NULL;
  const char *host = uri ? g_uri_get_host (uri) : NULL;
  const char *scheme = uri ? g_uri_get_scheme (uri) : NULL;
  if (!host || !*host || !scheme || (g_ascii_strcasecmp (scheme, "https") && g_ascii_strcasecmp (scheme, "http")) ||
      !g_strcmp0 (host, last))
    return;
  g_free (last);
  last = g_strdup (host);
  webkit_network_session_prefetch_dns (nb_session_for (FALSE), host);
}

static void
on_suggestions (GPtrArray *items, gpointer data)
{
  NbTab *t = data;
  GtkWidget *child;
  g_autofree char *query = g_utf8_strdown (gtk_editable_get_text (GTK_EDITABLE (t->entry)), -1);

  g_strstrip (query);
  g_clear_object (&t->suggest_cancel);
  while ((child = gtk_widget_get_first_child (t->dropdown_rows)))
    gtk_box_remove (GTK_BOX (t->dropdown_rows), child);
  for (guint i = 0; i < items->len; i++)
    gtk_box_append (GTK_BOX (t->dropdown_rows), suggestion_row (t, items->pdata[i], query));
  gtk_widget_set_visible (t->dropdown, t->editing && items->len > 0);
  /* the top suggestion is the likeliest tap */
  if (items->len > 0) {
    NbSuggestion *top = items->pdata[0];
    prefetch_host (t, top->url ? top->url : top->title);
  }
}

static void
on_entry_changed (GtkEditable *e, NbTab *t)
{
  if (t->setting_text || !t->editing)
    return;
  sync_fade (t);
  const char *text = gtk_editable_get_text (e);
  if (t->suggest_cancel)
    g_cancellable_cancel (t->suggest_cancel);
  g_clear_object (&t->suggest_cancel);
  /* from the first character; a blank text gives nothing */
  g_autofree char *stripped = g_strstrip (g_strdup (text));
  if (!*stripped) {
    hide_dropdown (t);
    return;
  }
  prefetch_host (t, stripped);
  t->suggest_cancel = g_cancellable_new ();
  nb_suggest_fetch (nb->store, nb_store_get_string (nb->store, "suggestionProvider", "GOOGLE"), text,
                    t->suggest_cancel, on_suggestions, t);
}

/* ---- editing ---- */

static gboolean
select_all (gpointer data)
{
  NbTab *t = data;
  if (t->editing)
    gtk_editable_select_region (GTK_EDITABLE (t->entry), 0, -1);
  return G_SOURCE_REMOVE;
}

static void
on_url_focus (GtkEventControllerFocus *focus, GParamSpec *pspec, NbTab *t)
{
  gboolean in = gtk_event_controller_focus_contains_focus (focus);

  if (in == t->editing)
    return;
  t->editing = in;
  if (in) {
    /* the whole text is selected when the field takes the focus */
    g_idle_add (select_all, t);
    sync_fade (t);
  } else {
    hide_dropdown (t);
    nb_urlbar_sync (t);
  }
  nb_tab_sync_back (t);
}

void
nb_urlbar_stop_editing (NbTab *t)
{
  hide_dropdown (t);
  if (t->editing) {
    t->editing = FALSE;
    gtk_widget_grab_focus (t->view ? GTK_WIDGET (t->view) : t->view_holder);
    gtk_window_set_focus (GTK_WINDOW (t->win), t->view ? GTK_WIDGET (t->view) : NULL);
  }
  nb_urlbar_sync (t);
  nb_tab_sync_back (t);
}

static void
on_url_activate (GtkEntry *entry, NbTab *t)
{
  g_autofree char *text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry)));

  nb_urlbar_stop_editing (t);
  nb_tab_load (t, text);
}

/* ---- find in page ---- */

static void
sync_find (NbTab *t)
{
  gboolean has_input = *gtk_editable_get_text (GTK_EDITABLE (t->find_entry)) != '\0';
  gboolean results = t->find_n > 0;
  g_autofree char *count = g_strdup_printf ("%d/%u", results ? t->find_i + 1 : 0, t->find_n);

  gtk_label_set_text (GTK_LABEL (t->find_count), count);
  gtk_widget_set_visible (t->find_count, has_input);
  gtk_widget_set_visible (t->find_clear, has_input);
  gtk_widget_set_sensitive (t->find_prev, results && t->find_i > 0);
  gtk_widget_set_sensitive (t->find_next, results && t->find_i + 1 < (int) t->find_n);
}

void
nb_urlbar_find_counted (NbTab *t, guint n, gboolean failed)
{
  t->find_n = failed ? 0 : n;
  if (t->find_i >= (int) t->find_n)
    t->find_i = 0;
  sync_find (t);
}

static void
clear_search (NbTab *t)
{
  gtk_editable_set_text (GTK_EDITABLE (t->find_entry), "");
  t->find_n = 0;
  t->find_i = 0;
  if (t->view)
    webkit_find_controller_search_finish (webkit_web_view_get_find_controller (t->view));
  sync_find (t);
}

static void
on_find_activate (GtkEntry *entry, NbTab *t)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (!*text || !t->view) {
    clear_search (t);
    return;
  }
  /* every match counted, the first one current; no wrapping */
  WebKitFindController *fc = webkit_web_view_get_find_controller (t->view);
  t->find_i = 0;
  t->find_n = 0;
  webkit_find_controller_search (fc, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE, G_MAXUINT);
  webkit_find_controller_count_matches (fc, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE, G_MAXUINT);
}

static void
on_find_step (GtkWidget *button, NbTab *t)
{
  gboolean next = button == t->find_next;

  if (!t->view || !t->find_n)
    return;
  WebKitFindController *fc = webkit_web_view_get_find_controller (t->view);
  if (next && t->find_i + 1 < (int) t->find_n) {
    webkit_find_controller_search_next (fc);
    t->find_i++;
  } else if (!next && t->find_i > 0) {
    webkit_find_controller_search_previous (fc);
    t->find_i--;
  }
  sync_find (t);
}

static void
on_find_changed (GtkEditable *e, NbTab *t)
{
  sync_find (t);
}

void
nb_urlbar_set_finding (NbTab *t, gboolean finding)
{
  t->finding = finding;
  gtk_stack_set_visible_child_name (GTK_STACK (t->bar_stack), finding ? "find" : "url");
  if (finding) {
    hide_dropdown (t);
    gtk_widget_grab_focus (t->find_entry);
  } else {
    clear_search (t);
    if (t->view)
      gtk_widget_grab_focus (GTK_WIDGET (t->view));
  }
  nb_tab_sync_back (t);
}

static void
on_find_cancel (NbTab *t)
{
  nb_urlbar_set_finding (t, FALSE);
}

/* ---- the tab count ---- */

void
nb_urlbar_sync_count (NbTab *t)
{
  if (!t->count_label)
    return;
  guint n = nb->tabs->len;
  /* the number up to 99, then a smile */
  g_autofree char *text = n > 99 ? g_strdup (":D") : g_strdup_printf ("%u", n);
  g_autofree char *name = g_strdup_printf (g_dngettext (NULL, "Switch tabs, %u open tab", "Switch tabs, %u open tabs",
                                                       n), n);
  nb_tab_count_set_text (NB_TAB_COUNT (t->count_label), text);
  gtk_accessible_update_property (GTK_ACCESSIBLE (t->count), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

static void
on_count (NbTab *t)
{
  nb_urlbar_stop_editing (t);
  nb_switcher_open ();
}

static GtkWidget *
count_button (NbTab *t)
{
  t->count_label = nb_tab_count_new ();
  gtk_widget_set_halign (t->count_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (t->count_label, GTK_ALIGN_CENTER);
  t->count = vibeos_pressable_new (t->count_label);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (t->count), VIBEOS_RIPPLE_CIRCLE, 20);
  gtk_widget_add_css_class (t->count, "nb-bar-icon");
  gtk_widget_set_size_request (t->count, 48, 48);
  gtk_widget_set_valign (t->count, GTK_ALIGN_CENTER);
  g_signal_connect_swapped (t->count, "clicked", G_CALLBACK (on_count), t);
  return t->count;
}

/* ---- building ---- */

static GtkWidget *
bar_icon_button (const char *icon, const char *name, const char *css)
{
  GtkWidget *b = vibeos_icon_button_new (icon);

  gtk_widget_add_css_class (b, css);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return b;
}

static GtkWidget *
url_group (NbTab *t)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_overlay_new ();
  GtkWidget *fade = gtk_drawing_area_new ();

  gtk_widget_add_css_class (card, "nb-url-card");
  gtk_widget_set_hexpand (card, TRUE);
  /* the pill clips: the fading edge is a rectangle that would otherwise square off its round end */
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);

  t->incog_icon = nb_icon ("jelly-incognito-symbolic", 20);
  gtk_widget_add_css_class (t->incog_icon, "nb-card-icon");
  gtk_widget_set_margin_start (t->incog_icon, 14);
  gtk_widget_set_valign (t->incog_icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (card), t->incog_icon);

  /* the lock: a small button with a borderless ripple; a tap shows the certificate */
  GtkWidget *lock_image = nb_icon ("jelly-lock-symbolic", 20);
  t->lock = vibeos_pressable_new (lock_image);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (t->lock), VIBEOS_RIPPLE_CIRCLE, 20);
  g_object_set_data (G_OBJECT (t->lock), "nb-image", lock_image);
  gtk_widget_add_css_class (t->lock, "nb-card-icon");
  gtk_widget_set_size_request (t->lock, 20, 20);
  gtk_widget_set_valign (t->lock, GTK_ALIGN_CENTER);
  g_signal_connect_swapped (t->lock, "clicked", G_CALLBACK (nb_tab_show_certificate), t);
  gtk_box_append (GTK_BOX (card), t->lock);

  t->entry = gtk_entry_new ();
  gtk_widget_add_css_class (t->entry, "nb-url-entry");
  gtk_entry_set_placeholder_text (GTK_ENTRY (t->entry), _("Search or type URL"));
  gtk_entry_set_input_purpose (GTK_ENTRY (t->entry), GTK_INPUT_PURPOSE_URL);
  gtk_entry_set_input_hints (GTK_ENTRY (t->entry), GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_NO_EMOJI |
                             (t->incognito ? GTK_INPUT_HINT_PRIVATE : 0));
  gtk_widget_set_hexpand (t->entry, TRUE);
  gtk_widget_set_margin_start (t->entry, 12);
  gtk_widget_set_margin_end (t->entry, 12);
  g_signal_connect (t->entry, "activate", G_CALLBACK (on_url_activate), t);
  g_signal_connect (t->entry, "changed", G_CALLBACK (on_entry_changed), t);
  GtkEventController *focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "notify::contains-focus", G_CALLBACK (on_url_focus), t);
  gtk_widget_add_controller (t->entry, focus);
  gtk_overlay_set_child (GTK_OVERLAY (text), t->entry);
  gtk_widget_set_hexpand (text, TRUE);
  /* the fading edge: the card's colour over the end of a URL that does not fit */
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (fade), draw_fade, t, NULL);
  gtk_widget_set_size_request (fade, FADE_LENGTH, -1);
  gtk_widget_set_halign (fade, GTK_ALIGN_END);
  gtk_widget_set_margin_end (fade, 12);
  gtk_widget_set_can_target (fade, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (text), fade);
  g_object_set_data (G_OBJECT (t->entry), "nb-fade", fade);
  gtk_box_append (GTK_BOX (card), text);
  gtk_box_append (GTK_BOX (row), card);

  gtk_box_append (GTK_BOX (row), count_button (t));
  t->more = bar_icon_button ("jelly-menu-symbolic", _("More"), "nb-bar-icon");
  gtk_widget_set_margin_end (t->more, 4);
  g_signal_connect_swapped (t->more, "clicked", G_CALLBACK (nb_menu_open), t);
  gtk_box_append (GTK_BOX (row), t->more);
  return row;
}

static GtkWidget *
find_group (NbTab *t)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *cancel = bar_icon_button ("jelly-cancel-symbolic", _("Cancel search"), "nb-bar-icon");

  gtk_widget_set_margin_start (cancel, 4);
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (on_find_cancel), t);
  gtk_box_append (GTK_BOX (row), cancel);

  gtk_widget_add_css_class (card, "nb-find-card");
  gtk_widget_set_hexpand (card, TRUE);
  t->find_entry = gtk_entry_new ();
  gtk_widget_add_css_class (t->find_entry, "nb-find-entry");
  gtk_entry_set_placeholder_text (GTK_ENTRY (t->find_entry), _("Search"));
  gtk_entry_set_input_hints (GTK_ENTRY (t->find_entry), GTK_INPUT_HINT_NO_SPELLCHECK);
  gtk_widget_set_hexpand (t->find_entry, TRUE);
  gtk_widget_set_margin_start (t->find_entry, 16);
  gtk_widget_set_margin_end (t->find_entry, 18);
  g_signal_connect (t->find_entry, "activate", G_CALLBACK (on_find_activate), t);
  g_signal_connect (t->find_entry, "changed", G_CALLBACK (on_find_changed), t);
  gtk_box_append (GTK_BOX (card), t->find_entry);
  t->find_count = nb_label ("0/0", 14, "nb-find-count", FALSE);
  gtk_widget_set_margin_end (t->find_count, 20);
  gtk_widget_set_valign (t->find_count, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (t->find_count, FALSE);
  gtk_box_append (GTK_BOX (card), t->find_count);
  GtkWidget *clear_image = nb_icon ("jelly-clear-symbolic", 24);
  t->find_clear = vibeos_pressable_new (clear_image);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (t->find_clear), VIBEOS_RIPPLE_CIRCLE, 20);
  gtk_widget_add_css_class (t->find_clear, "nb-card-icon");
  gtk_widget_set_valign (t->find_clear, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (t->find_clear, 12);
  gtk_accessible_update_property (GTK_ACCESSIBLE (t->find_clear), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Clear search"), -1);
  g_signal_connect_swapped (t->find_clear, "clicked", G_CALLBACK (clear_search), t);
  gtk_widget_set_visible (t->find_clear, FALSE);
  gtk_box_append (GTK_BOX (card), t->find_clear);
  gtk_box_append (GTK_BOX (row), card);

  t->find_prev = bar_icon_button ("jelly-prev-symbolic", _("Previous"), "nb-bar-icon");
  t->find_next = bar_icon_button ("jelly-next-symbolic", _("Next"), "nb-bar-icon");
  gtk_widget_set_margin_end (t->find_next, 4);
  g_signal_connect (t->find_prev, "clicked", G_CALLBACK (on_find_step), t);
  g_signal_connect (t->find_next, "clicked", G_CALLBACK (on_find_step), t);
  gtk_box_append (GTK_BOX (row), t->find_prev);
  gtk_box_append (GTK_BOX (row), t->find_next);
  sync_find (t);
  return row;
}

GtkWidget *
nb_urlbar_new (NbTab *t)
{
  GtkWidget *bar = gtk_overlay_new ();

  gtk_widget_add_css_class (bar, "nb-bar");
  t->bar_stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (t->bar_stack), url_group (t), "url");
  gtk_stack_add_named (GTK_STACK (t->bar_stack), find_group (t), "find");
  gtk_overlay_set_child (GTK_OVERLAY (bar), t->bar_stack);

  t->progress = gtk_drawing_area_new ();
  gtk_widget_set_size_request (t->progress, -1, 2);
  gtk_widget_set_valign (t->progress, GTK_ALIGN_END);
  gtk_widget_set_can_target (t->progress, FALSE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (t->progress), draw_progress, t, NULL);
  gtk_widget_set_visible (t->progress, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (bar), t->progress);

  /* the dropdown: as wide as the card, from its bottom edge, flat */
  t->dropdown = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (t->dropdown, "nb-dropdown");
  gtk_widget_set_valign (t->dropdown, GTK_ALIGN_START);
  gtk_widget_set_margin_start (t->dropdown, 16);
  /* the card ends before the tab count and the menu button */
  gtk_widget_set_margin_end (t->dropdown, 104);
  gtk_widget_set_margin_top (t->dropdown, 56);
  t->dropdown_rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (t->dropdown), t->dropdown_rows);
  gtk_widget_set_visible (t->dropdown, FALSE);
  return bar;
}
