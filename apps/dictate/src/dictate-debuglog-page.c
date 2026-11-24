/* dictate-debuglog-page: see dictate-debuglog-page.h. Nothing of a turn is ever written to the journal from here. */
#include "dictate-debuglog-page.h"
#include "dictate-debuglog.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

#define STEP_TEXT 120           /* characters of a step's text in its row; its page has all of it */
#define SAID_TEXT 60            /* of what was said, in the list of turns */

/* --- what the pages share */

/* a page as Settings' are: the bar with Back, and under it `content` in a column on a tablet */
static GtkWidget *
page_new (const char *title, GtkWidget *content, GtkWidget **bar_out, GtkWidget **snackbar_out)
{
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = vibeos_top_bar_new (title), *scroll = spongy_scroll_view_new ();

  gtk_widget_add_css_class (overlay, "dictate-settings");
  vibeos_top_bar_set_compact (VIBEOS_TOP_BAR (bar), TRUE);
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  gtk_box_append (GTK_BOX (page), bar);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), vibeos_clamp_new (content, VIBEOS_CLAMP_SETTINGS));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), page);
  if (snackbar_out)
    {
      *snackbar_out = vibeos_snackbar_new ();
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), *snackbar_out);
    }
  *bar_out = bar;
  return overlay;
}

static GtkWidget *
action_new (GtkWidget *bar, const char *icon, const char *name)
{
  GtkWidget *button = vibeos_top_bar_add_action (VIBEOS_TOP_BAR (bar), icon);

  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return button;
}

static void
on_pop (VibeosTopBar *bar, VibeosPageStack *pages)
{
  vibeos_page_stack_pop (pages);
}

/* how long something took: "812 ms", "1.8 s" */
static char *
duration (double ms)
{
  if (ms < 1000)
    /* TRANSLATORS: a time in the debug log; %d is milliseconds */
    return g_strdup_printf (_("%d ms"), (int) ms);
  /* TRANSLATORS: a time in the debug log; %.1f is seconds */
  return g_strdup_printf (_("%.1f s"), ms / 1000);
}

static const char *
outcome_name (const char *outcome)
{
  if (!g_strcmp0 (outcome, "answered"))
    return _("Answered");
  if (!g_strcmp0 (outcome, "offline"))
    return _("Offline");
  if (!g_strcmp0 (outcome, "error"))
    return _("Error");
  if (!g_strcmp0 (outcome, "stopped"))
    return _("Stopped");
  return outcome ? outcome : "";
}

/* `text` cut to `length` characters, on one line, with the key and the phone's id cut short */
static char *
short_text (const char *text, glong length)
{
  g_autofree char *redacted = dictate_debuglog_redact (text ? text : "");
  g_autofree char *line = g_strdelimit (g_strdup (redacted), "\n\r\t", ' ');

  if (g_utf8_strlen (line, -1) <= length)
    return g_strstrip (g_steal_pointer (&line));
  {
    g_autofree char *cut = g_utf8_substring (line, 0, length);

    return g_strconcat (g_strstrip (cut), "…", NULL);
  }
}

static const char *
member_string (JsonObject *o, const char *name)
{
  JsonNode *node = o ? json_object_get_member (o, name) : NULL;

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
         ? json_node_get_string (node) : NULL;
}

static double
member_number (JsonObject *o, const char *name)
{
  JsonNode *node = o ? json_object_get_member (o, name) : NULL;

  return node && JSON_NODE_HOLDS_VALUE (node) ? json_node_get_double (node) : 0;
}

static gboolean
member_true (JsonObject *o, const char *name)
{
  JsonNode *node = o ? json_object_get_member (o, name) : NULL;

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_BOOLEAN
         && json_node_get_boolean (node);
}

/* `node` as the log has it, indented, with the key and the phone's id cut short */
static char *
pretty (JsonNode *node)
{
  g_autofree char *text = json_to_string (node, TRUE);

  return dictate_debuglog_redact (text);
}

/* --- a step's page: all of it */

static GtkWidget *
step_page_new (VibeosPageStack *pages, const char *title, JsonNode *step)
{
  GtkWidget *view = gtk_text_view_new ();
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  g_autofree char *text = pretty (step);
  GtkWidget *bar, *page;

  gtk_widget_add_css_class (view, "dictate-debuglog-json");
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  /* read only: selectable, nothing to type, so no keyboard */
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 24);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 24);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 8);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (view), 24);
  gtk_text_buffer_set_text (buffer, text, -1);
  page = page_new (title, view, &bar, NULL);
  g_signal_connect (bar, "back", G_CALLBACK (on_pop), pages);
  return page;
}

/* --- a turn's page: how it went, and its steps */

typedef struct {
  GtkWidget *window, *snackbar;
  VibeosPageStack *pages;
  JsonNode *root;
} Turn;

static void
turn_free (gpointer data)
{
  Turn *t = data;

  json_node_unref (t->root);
  g_free (t);
}

/* a step's kind as its row says it */
static const char *
kind_name (const char *kind)
{
  static const struct { const char *kind, *name; } KINDS[] = {
    { "request", N_("Request") }, { "http", N_("HTTP") }, { "response", N_("Response") }, { "tool", N_("Tool") },
    { "error", N_("Error") }, { "offline", N_("Offline") }, { "retry", N_("Retry") }, { "cut-off", N_("Cut off") },
  };

  for (guint i = 0; i < G_N_ELEMENTS (KINDS); i++)
    if (!g_strcmp0 (kind, KINDS[i].kind))
      return _(KINDS[i].name);
  return kind ? kind : _("Step");
}

/* what a step did, in a line */
static char *
step_line (JsonObject *step)
{
  const char *kind = member_string (step, "kind");

  if (!g_strcmp0 (kind, "http"))
    {
      guint status = (guint) member_number (step, "status");
      g_autofree char *took = duration (member_number (step, "ms"));
      g_autofree char *code = status ? g_strdup_printf ("%u", status) : g_strdup (_("no answer"));
      g_autofree char *line = g_strdup_printf ("%s %s · %s · %s", member_string (step, "method") ?: "",
                                               member_string (step, "url") ?: "", code, took);

      return short_text (line, STEP_TEXT);
    }
  if (!g_strcmp0 (kind, "tool"))
    {
      g_autofree char *took = duration (member_number (step, "ms"));
      g_autofree char *line = g_strdup_printf ("%s · %s · %s", member_string (step, "name") ?: "",
                                               member_true (step, "ok") ? _("worked") : _("failed"), took);

      return short_text (line, STEP_TEXT);
    }
  if (!g_strcmp0 (kind, "response"))
    {
      const char *content = member_string (step, "content");
      JsonNode *calls = json_object_get_member (step, "tool_calls");
      g_autoptr (GString) names = g_string_new (NULL);

      if (content && *content)
        return short_text (content, STEP_TEXT);
      for (guint i = 0; calls && JSON_NODE_HOLDS_ARRAY (calls) && i < json_array_get_length (json_node_get_array (calls));
           i++)
        {
          JsonObject *call = json_array_get_object_element (json_node_get_array (calls), i);
          const char *name = member_string (call, "name");

          if (name)
            g_string_append_printf (names, "%s%s", names->len ? ", " : "", name);
        }
      if (names->len)
        {
          /* TRANSLATORS: a model's answer in the debug log that only calls tools; %s is their names */
          g_autofree char *line = g_strdup_printf (_("Calls %s"), names->str);

          return short_text (line, STEP_TEXT);
        }
      return g_strdup ("");
    }
  if (!g_strcmp0 (kind, "request"))
    {
      JsonNode *body = json_object_get_member (step, "body");
      JsonObject *o = body && JSON_NODE_HOLDS_OBJECT (body) ? json_node_get_object (body) : NULL;
      JsonNode *messages = o ? json_object_get_member (o, "messages") : NULL;
      guint n = messages && JSON_NODE_HOLDS_ARRAY (messages) ? json_array_get_length (json_node_get_array (messages))
                                                             : 0;

      /* TRANSLATORS: a request to the model in the debug log; %u is how many messages of the conversation it sent */
      return g_strdup_printf (ngettext ("%u message", "%u messages", n), n);
    }
  return short_text (member_string (step, "text"), STEP_TEXT);
}

static void
on_step (VibeosListRow *row, Turn *t)
{
  JsonObject *o = JSON_NODE_HOLDS_OBJECT (t->root) ? json_node_get_object (t->root) : NULL;
  JsonNode *steps = o ? json_object_get_member (o, "steps") : NULL;
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "dictate-step"));
  JsonArray *list = steps && JSON_NODE_HOLDS_ARRAY (steps) ? json_node_get_array (steps) : NULL;
  JsonNode *step = list && index < json_array_get_length (list) ? json_array_get_element (list, index) : NULL;
  const char *kind;

  if (!step || !JSON_NODE_HOLDS_OBJECT (step))
    return;
  kind = kind_name (member_string (json_node_get_object (step), "kind"));
  vibeos_page_stack_push_from (t->pages, step_page_new (t->pages, kind, step), GTK_WIDGET (row));
}

static void
on_copy_turn (VibeosPressable *button, Turn *t)
{
  g_autofree char *text = pretty (t->root);

  gdk_clipboard_set_text (gtk_widget_get_clipboard (t->window), text);
  vibeos_snackbar_show (VIBEOS_SNACKBAR (t->snackbar), _("Turn copied"), NULL, NULL, NULL, VIBEOS_SNACKBAR_SHORT);
}

/* a row of the turn's: what it was, and its value under it */
static void
value_row (GtkWidget *cards, const char *title, const char *value)
{
  g_autofree char *redacted = value ? dictate_debuglog_redact (value) : NULL;
  GtkWidget *row;

  if (!redacted || !*redacted)
    return;
  row = vibeos_list_row_new (title, redacted);
  vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), FALSE);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
}

static GtkWidget *
turn_page_new (GtkWidget *window, VibeosPageStack *pages, JsonNode *root)
{
  GtkWidget *cards = vibeos_card_list_new ();
  JsonObject *o = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonNode *steps = o ? json_object_get_member (o, "steps") : NULL;
  JsonArray *list = steps && JSON_NODE_HOLDS_ARRAY (steps) ? json_node_get_array (steps) : NULL;
  Turn *t = g_new0 (Turn, 1);
  GtkWidget *bar, *page, *copy;

  t->window = window;
  t->pages = pages;
  t->root = json_node_ref (root);

  /* the turn: what was said and answered, and how */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Turn"));
  value_row (cards, _("Said"), member_string (o, "said"));
  value_row (cards, _("Reply"), member_string (o, "reply"));
  value_row (cards, _("Outcome"), outcome_name (member_string (o, "outcome")));
  value_row (cards, _("Language"), member_string (o, "language"));
  value_row (cards, _("Model"), member_string (o, "brain"));
  {
    g_autofree char *took = duration (member_number (o, "ms"));

    value_row (cards, _("Time"), took);
  }
  if (member_true (o, "locked"))
    value_row (cards, _("Phone"), _("Locked"));
  if (member_true (o, "private_mode"))
    value_row (cards, _("Private mode"), _("On"));

  /* its steps, in order: each opens on a page of its own */
  if (list && json_array_get_length (list))
    {
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Steps"));
      for (guint i = 0; i < json_array_get_length (list); i++)
        {
          JsonObject *step = json_array_get_object_element (list, i);
          g_autofree char *at = step ? duration (member_number (step, "at_ms")) : NULL;
          g_autofree char *line = step ? step_line (step) : NULL;
          g_autofree char *summary = NULL;
          GtkWidget *row;

          if (!step)
            continue;
          /* when it came in the turn, and what it did: "+1.2 s · POST … · 200 · 812 ms" */
          summary = *line ? g_strdup_printf ("+%s · %s", at, line) : g_strdup_printf ("+%s", at);
          row = vibeos_list_row_new (kind_name (member_string (step, "kind")), summary);
          vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
          g_object_set_data (G_OBJECT (row), "dictate-step", GUINT_TO_POINTER (i));
          g_signal_connect (row, "clicked", G_CALLBACK (on_step), t);
          vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
        }
    }
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 8, 24);

  page = page_new (_("Turn"), cards, &bar, &t->snackbar);
  g_signal_connect (bar, "back", G_CALLBACK (on_pop), pages);
  copy = action_new (bar, "dictate-copy-symbolic", _("Copy turn"));
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_turn), t);
  g_object_set_data_full (G_OBJECT (page), "dictate-debuglog-turn", t, turn_free);
  return page;
}

/* --- the list of turns */

typedef struct {
  GtkWidget *window, *page, *cards;
  VibeosPageStack *pages;
  void (*back) (gpointer data);
  gpointer data;
} List;

static void refresh (List *l);

static gboolean
refresh_later (gpointer data)
{
  List *l = g_object_get_data (G_OBJECT (data), "dictate-debuglog");

  if (l)
    refresh (l);
  return G_SOURCE_REMOVE;
}

static void
on_turn (VibeosListRow *row, List *l)
{
  const char *path = g_object_get_data (G_OBJECT (row), "dictate-path");
  g_autoptr (JsonNode) root = path ? dictate_debuglog_read (path) : NULL;

  /* gone since the list was made (only the newest are kept): the list as it is now, once the tap is over, since it
   * takes this row away */
  if (!root)
    {
      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, refresh_later, g_object_ref (l->page), g_object_unref);
      return;
    }
  vibeos_page_stack_push_from (l->pages, turn_page_new (l->window, l->pages, root), GTK_WIDGET (row));
}

/* a turn's row: what was said, and when, how it ended, its tools and its time */
static GtkWidget *
turn_row (List *l, DictateDebugEntry *entry)
{
  g_autoptr (GDateTime) started = g_date_time_new_from_unix_local (entry->started / G_USEC_PER_SEC);
  /* TRANSLATORS: when a turn in the debug log began, a g_date_time_format () string: "24 Nov, 08:12".
   * xgettext:no-c-format */
  g_autofree char *when = started ? g_date_time_format (started, _("%e %b, %H:%M")) : NULL;
  /* TRANSLATORS: a turn in the debug log; %u is how many tools it called */
  g_autofree char *tools = g_strdup_printf (ngettext ("%u tool", "%u tools", entry->tools), entry->tools);
  g_autofree char *took = duration (entry->ms);
  g_autofree char *summary = g_strdup_printf ("%s · %s · %s · %s", when ? g_strstrip (when) : "",
                                              outcome_name (entry->outcome), tools, took);
  g_autofree char *said = entry->said && *entry->said ? short_text (entry->said, SAID_TEXT) : g_strdup (_("(nothing said)"));
  GtkWidget *row = vibeos_list_row_new (said, summary);

  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
  g_object_set_data_full (G_OBJECT (row), "dictate-path", g_strdup (entry->path), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_turn), l);
  return row;
}

static void
refresh (List *l)
{
  g_autoptr (GPtrArray) entries = dictate_debuglog_list ();

  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (l->cards));
  if (!entries || !entries->len)
    {
      GtkWidget *row = vibeos_list_row_new (_("No turns recorded"),
                                            _("Turn on Debug log in Settings, then talk to Vibe"));

      vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), FALSE);
      vibeos_card_list_append (VIBEOS_CARD_LIST (l->cards), row);
      return;
    }
  for (guint i = 0; i < entries->len; i++)
    vibeos_card_list_append (VIBEOS_CARD_LIST (l->cards), turn_row (l, entries->pdata[i]));
}

static void
on_clear_response (VibeosDialog *dialog, int response, GtkWidget *page)
{
  List *l = g_object_get_data (G_OBJECT (page), "dictate-debuglog");

  if (response != VIBEOS_RESPONSE_OK || !l)
    return;
  dictate_debuglog_clear ();
  refresh (l);
}

static void
on_clear (VibeosPressable *button, List *l)
{
  VibeosDialog *dialog = vibeos_dialog_new (_("Clear the debug log?"));

  vibeos_dialog_set_message (dialog, _("Every recorded turn is deleted from the phone."));
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("Clear"), VIBEOS_RESPONSE_OK);
  /* the answer goes to the page while it is there */
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_clear_response), l->page, 0);
  vibeos_dialog_present (dialog, l->window);
}

static void
on_back (VibeosTopBar *bar, List *l)
{
  l->back (l->data);
}

GtkWidget *
dictate_debuglog_page_new (GtkWidget *window, VibeosPageStack *pages, void (*back) (gpointer data), gpointer data)
{
  List *l = g_new0 (List, 1);
  GtkWidget *bar, *clear;

  l->window = window;
  l->pages = pages;
  l->back = back;
  l->data = data;
  l->cards = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (l->cards), 8, 24);
  l->page = page_new (_("Debug log"), l->cards, &bar, NULL);
  g_signal_connect (bar, "back", G_CALLBACK (on_back), l);
  clear = action_new (bar, "dictate-delete-symbolic", _("Clear log"));
  g_signal_connect (clear, "clicked", G_CALLBACK (on_clear), l);
  g_object_set_data_full (G_OBJECT (l->page), "dictate-debuglog", l, g_free);
  refresh (l);
  return l->page;
}
