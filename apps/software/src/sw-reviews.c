/* Reviews: a review's card, all of an app's reviews, writing one, and reporting one.
 *
 * Reading is public. Writing, marking helpful and reporting need an account, and the store decides the rest (its
 * rules in the server's docs/api.md): writing and marking helpful need a confirmed address, writing needs the app to
 * have been on one of the account's devices. When the store says no, its own sentence shows. */
#include "sw-pages.h"
#include "sw-account.h"
#include "sw-json.h"
#include "sw-menu.h"
#include "sw-widgets.h"
#include "sw-window.h"

#include <spongy-gtk.h>

#define MAX_TEXT 500

static GHashTable *changed_apps;

void
sw_reviews_changed (const char *app_id)
{
  if (!changed_apps)
    changed_apps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (app_id)
    g_hash_table_add (changed_apps, g_strdup (app_id));
}

gboolean
sw_reviews_take_changed (const char *app_id)
{
  return changed_apps && app_id && g_hash_table_remove (changed_apps, app_id);
}

/* ---- the card -------------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget  *card, *helpful, *helpful_label;
  JsonObject *review;
  char       *app_id, *app_name;
  gboolean    busy;
} Card;

static void
card_free (gpointer data)
{
  Card *c = data;

  json_object_unref (c->review);
  g_free (c->app_id);
  g_free (c->app_name);
  g_free (c);
}

static void
helpful_update (Card *c)
{
  gint64 count = sw_json_int (c->review, "helpful_count", 0);
  gboolean mine = sw_json_bool (c->review, "helpful_by_me", FALSE);
  g_autofree char *text = count > 0 ? g_strdup_printf ("Helpful · %" G_GINT64_FORMAT, count) : g_strdup ("Helpful");
  GtkWidget *box = vibeos_pressable_get_child (VIBEOS_PRESSABLE (c->helpful));
  GtkWidget *icon = gtk_widget_get_first_child (box);

  gtk_label_set_text (GTK_LABEL (c->helpful_label), text);
  gtk_image_set_from_icon_name (GTK_IMAGE (icon), mine ? "sw-thumb-up-fill-symbolic" : "sw-thumb-up-symbolic");
  if (mine)
    gtk_widget_add_css_class (c->helpful, "selected");
  else
    gtk_widget_remove_css_class (c->helpful, "selected");
  gtk_accessible_update_state (GTK_ACCESSIBLE (c->helpful), GTK_ACCESSIBLE_STATE_PRESSED,
                               mine ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
}

static void
on_helpful_done (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Card *c = data;
  JsonObject *answer = result ? sw_json_node_object (result->data) : NULL;

  c->busy = FALSE;
  if (error)
    {
      g_autofree char *text = sw_api_error_describe (error);

      sw_window_toast (c->card, text);
      return;
    }
  json_object_set_int_member (c->review, "helpful_count", sw_json_int (answer, "helpful_count", 0));
  json_object_set_boolean_member (c->review, "helpful_by_me", sw_json_bool (answer, "helpful_by_me", FALSE));
  helpful_update (c);
}

static void
on_helpful (GtkWidget *button, Card *c)
{
  g_autofree char *path = NULL;

  if (!sw_account_is_signed_in (sw_account_get_default ()))
    {
      sw_window_toast (button, "Sign in to mark reviews as helpful.");
      return;
    }
  if (c->busy)
    return;
  c->busy = TRUE;
  path = g_strdup_printf ("/reviews/%" G_GINT64_FORMAT "/helpful", sw_json_int (c->review, "id", 0));
  sw_api_send (sw_api_get_default (), sw_json_bool (c->review, "helpful_by_me", FALSE) ? "DELETE" : "POST", path, NULL,
               SW_API_NONE, sw_cancellable_for (c->card), on_helpful_done, c);
}

static void
menu_report (gpointer data)
{
  Card *c = data;

  sw_report_present (c->card, sw_json_int (c->review, "id", 0));
}

static void
menu_edit (gpointer data)
{
  Card *c = data;

  sw_window_push (c->card, sw_review_editor_new (c->app_id, c->app_name, (int) sw_json_int (c->review, "rating", 0),
                                                 c->review));
}

static void
on_card_more (GtkWidget *button, Card *c)
{
  SwMenu *menu = sw_menu_new ();

  if (sw_json_bool (c->review, "mine", FALSE))
    {
      if (c->app_id)
        sw_menu_append (menu, "Edit", menu_edit, c);
    }
  else
    sw_menu_append (menu, "Report", menu_report, c);
  sw_menu_present (menu, button);
}

GtkWidget *
sw_review_card_new (JsonObject *review, const char *app_id, const char *app_name)
{
  Card *c = g_new0 (Card, 1);
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8), *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *who = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2), *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *initial, *more, *bottom, *helpful_box, *thumb;
  JsonObject *author = sw_json_object (review, "author");
  const char *name = sw_json_string (author, "name"), *text = sw_json_string (review, "text");
  g_autofree char *date = sw_format_date (sw_json_string (review, "updated_at"));
  g_autofree char *first = g_utf8_substring (name ? name : "?", 0, 1);
  g_autofree char *upper = g_utf8_strup (first, -1);

  c->review = json_object_ref (review);
  c->app_id = g_strdup (app_id);
  c->app_name = g_strdup (app_name);
  c->card = card;
  gtk_widget_add_css_class (card, "sw-review");
  g_object_set_data (G_OBJECT (card), "sw-review-id", GSIZE_TO_POINTER ((gsize) sw_json_int (review, "id", 0)));
  g_object_set_data_full (G_OBJECT (card), "sw-card", c, card_free);

  initial = gtk_label_new (upper);
  gtk_widget_add_css_class (initial, "sw-review-initial");
  gtk_widget_set_valign (initial, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (top), initial);
  gtk_box_append (GTK_BOX (who), sw_label_new (name ? name : "Someone", "sw-review-author"));
  gtk_box_append (GTK_BOX (line), sw_stars_new (sw_json_int (review, "rating", 0), 12, 1, FALSE));
  if (date)
    {
      g_autofree char *when = sw_json_bool (review, "edited", FALSE) ? g_strdup_printf ("%s · edited", date)
                                                                     : g_strdup (date);

      gtk_box_append (GTK_BOX (line), sw_label_new (when, "sw-caption"));
    }
  gtk_box_append (GTK_BOX (who), line);
  gtk_widget_set_hexpand (who, TRUE);
  gtk_box_append (GTK_BOX (top), who);
  more = sw_icon_button_new ("sw-more-symbolic", "More");
  gtk_widget_set_valign (more, GTK_ALIGN_START);
  g_signal_connect (more, "clicked", G_CALLBACK (on_card_more), c);
  gtk_box_append (GTK_BOX (top), more);
  gtk_box_append (GTK_BOX (card), top);

  if (text && *text)
    gtk_box_append (GTK_BOX (card), sw_label_new (text, "sw-review-text"));
  if (sw_json_string (review, "app_version"))
    {
      g_autofree char *version = g_strdup_printf ("Version %s", sw_json_string (review, "app_version"));

      gtk_box_append (GTK_BOX (card), sw_label_new (version, "sw-caption"));
    }

  bottom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  helpful_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  thumb = gtk_image_new_from_icon_name ("sw-thumb-up-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (thumb), 18);
  gtk_box_append (GTK_BOX (helpful_box), thumb);
  c->helpful_label = gtk_label_new (NULL);
  gtk_box_append (GTK_BOX (helpful_box), c->helpful_label);
  c->helpful = vibeos_pressable_new (helpful_box);
  gtk_widget_add_css_class (c->helpful, "sw-helpful");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (c->helpful), VIBEOS_RIPPLE_BOUNDED, 16);
  gtk_accessible_update_property (GTK_ACCESSIBLE (c->helpful), GTK_ACCESSIBLE_PROPERTY_LABEL, "Helpful", -1);
  /* nobody votes for their own review */
  gtk_widget_set_visible (c->helpful, !sw_json_bool (review, "mine", FALSE));
  g_signal_connect (c->helpful, "clicked", G_CALLBACK (on_helpful), c);
  gtk_box_append (GTK_BOX (bottom), c->helpful);
  gtk_box_append (GTK_BOX (card), bottom);
  helpful_update (c);
  return card;
}

/* ---- all reviews ----------------------------------------------------------------------------------------------- */

static const struct {
  const char *label, *sort;
} sorts[] = {
  { "Most helpful", "helpful" },
  { "Newest", "newest" },
  { "Highest", "rating_high" },
  { "Lowest", "rating_low" },
};

typedef struct {
  GtkWidget  *page, *status, *scroll, *rows, *footer, *chips[G_N_ELEMENTS (sorts)];
  char       *app_id, *app_name, *cursor;
  int         sort;
  gboolean    loading;
  GHashTable *seen;
  GCancellable *cancellable;   /* this sort's requests */
} Reviews;

static void reviews_load (Reviews *r, gboolean first);

static void
reviews_free (gpointer data)
{
  Reviews *r = data;

  g_free (r->app_id);
  g_free (r->app_name);
  g_free (r->cursor);
  g_hash_table_unref (r->seen);
  g_clear_object (&r->cancellable);
  g_free (r);
}

static void
reviews_retry (Reviews *r)
{
  reviews_load (r, TRUE);
}

static void
on_reviews (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Reviews *r = data;
  JsonArray *list;
  GtkWidget *child;
  gboolean first = !r->cursor;

  if (result && result->stale)
    return;
  r->loading = FALSE;
  while ((child = gtk_widget_get_first_child (r->footer)))
    gtk_box_remove (GTK_BOX (r->footer), child);
  if (error)
    {
      if (first)
        sw_status_error (SW_STATUS (r->status), error, (SwRetryFunc) reviews_retry, r);
      else
        {
          g_autofree char *text = sw_api_error_describe (error);

          gtk_box_append (GTK_BOX (r->footer), sw_label_new (text, "sw-list-error"));
        }
      return;
    }
  list = sw_json_node_array (result->data);
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *review = sw_json_array_object (list, i);
      gint64 id = sw_json_int (review, "id", 0);

      if (!review || g_hash_table_contains (r->seen, GINT_TO_POINTER ((int) id)))
        continue;
      g_hash_table_add (r->seen, GINT_TO_POINTER ((int) id));
      gtk_box_append (GTK_BOX (r->rows), sw_review_card_new (review, r->app_id, r->app_name));
    }
  g_free (r->cursor);
  r->cursor = g_strdup (result->next_cursor);
  if (first && !gtk_widget_get_first_child (r->rows))
    sw_status_empty (SW_STATUS (r->status), "sw-star-outline-symbolic", "No reviews yet.", NULL, NULL, NULL);
  else
    sw_status_show (SW_STATUS (r->status));
}

static void
reviews_load (Reviews *r, gboolean first)
{
  g_autofree char *id = sw_api_escape (r->app_id);
  g_autofree char *base = g_strdup_printf ("/apps/%s/reviews?sort=%s", id, sorts[r->sort].sort);
  g_autofree char *path = NULL;

  if (first)
    {
      GtkWidget *child;

      if (r->cancellable)
        g_cancellable_cancel (r->cancellable);
      g_set_object (&r->cancellable, g_cancellable_new ());
      g_clear_pointer (&r->cursor, g_free);
      g_hash_table_remove_all (r->seen);
      while ((child = gtk_widget_get_first_child (r->rows)))
        gtk_box_remove (GTK_BOX (r->rows), child);
      sw_status_loading (SW_STATUS (r->status));
      r->loading = FALSE;
    }
  if (r->loading)
    return;
  r->loading = TRUE;
  path = sw_api_path_with_cursor (base, r->cursor);
  sw_api_get (sw_api_get_default (), path, first ? SW_API_CACHED : SW_API_NONE, r->cancellable, on_reviews, r);
}

static void
on_reviews_scrolled (GtkAdjustment *adj, Reviews *r)
{
  if (!r->loading && r->cursor
      && gtk_adjustment_get_value (adj) + gtk_adjustment_get_page_size (adj) + 600 >= gtk_adjustment_get_upper (adj))
    reviews_load (r, FALSE);
}

static void
on_sort (GtkWidget *chip, Reviews *r)
{
  for (guint i = 0; i < G_N_ELEMENTS (sorts); i++)
    {
      sw_filter_chip_set_selected (r->chips[i], r->chips[i] == chip);
      if (r->chips[i] == chip)
        r->sort = i;
    }
  reviews_load (r, TRUE);
}

static void
on_reviews_destroy (GtkWidget *page, Reviews *r)
{
  if (r->cancellable)
    g_cancellable_cancel (r->cancellable);
}

GtkWidget *
sw_reviews_page_new (const char *app_id, const char *app_name)
{
  Reviews *r = g_new0 (Reviews, 1);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *chips, *strip;
  GtkAdjustment *adj;

  r->app_id = g_strdup (app_id);
  r->app_name = g_strdup (app_name);
  r->seen = g_hash_table_new (NULL, NULL);
  r->page = sw_page_new (app_name ? app_name : "Reviews");
  g_object_set_data_full (G_OBJECT (r->page), "sw-reviews", r, reviews_free);
  g_signal_connect (r->page, "destroy", G_CALLBACK (on_reviews_destroy), r);

  strip = sw_strip_new (&chips);
  gtk_widget_add_css_class (chips, "sw-chips");
  for (guint i = 0; i < G_N_ELEMENTS (sorts); i++)
    {
      r->chips[i] = sw_filter_chip_new (sorts[i].label);
      sw_filter_chip_set_selected (r->chips[i], i == 0);
      g_signal_connect (r->chips[i], "clicked", G_CALLBACK (on_sort), r);
      gtk_box_append (GTK_BOX (chips), r->chips[i]);
    }
  gtk_box_append (GTK_BOX (r->page), strip);

  r->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (r->rows, "sw-reviews");
  r->footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (r->footer, "sw-list-footer");
  gtk_box_append (GTK_BOX (column), r->rows);
  gtk_box_append (GTK_BOX (column), r->footer);
  r->scroll = sw_scroll_new (column);
  adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (r->scroll));
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_reviews_scrolled), r);
  r->status = sw_status_new (r->scroll);
  sw_page_set_body (r->page, r->status);
  reviews_load (r, TRUE);
  return r->page;
}

/* ---- writing --------------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *page, *stars, *view, *counter, *send, *error;
  char      *app_id;
  gboolean   had_review, busy;
} Editor;

static void
editor_free (gpointer data)
{
  Editor *e = data;

  g_free (e->app_id);
  g_free (e);
}

static char *
editor_text (Editor *e)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (e->view));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buffer, &start, &end);
  return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
}

static void
editor_update (Editor *e)
{
  g_autofree char *text = editor_text (e);
  glong length = sw_text_length (text);
  g_autofree char *count = g_strdup_printf ("%ld/%d", length, MAX_TEXT);

  gtk_label_set_text (GTK_LABEL (e->counter), count);
  if (length > MAX_TEXT)
    gtk_widget_add_css_class (e->counter, "error");
  else
    gtk_widget_remove_css_class (e->counter, "error");
  gtk_widget_set_sensitive (e->send, !e->busy && sw_stars_get_rating (SW_STARS (e->stars)) >= 1 && length <= MAX_TEXT);
}

static void
on_editor_changed (gpointer source, Editor *e)
{
  editor_update (e);
}

static void
on_sent (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Editor *e = data;

  e->busy = FALSE;
  editor_update (e);
  if (error)
    {
      g_autofree char *text = sw_api_error_describe (error);
      const char *field = sw_api_error_field (error, "text");

      gtk_label_set_text (GTK_LABEL (e->error), field ? field : text);
      gtk_widget_set_visible (e->error, TRUE);
      return;
    }
  sw_reviews_changed (e->app_id);
  sw_window_toast (e->page, e->had_review ? "Your review is changed" : "Thank you for your review");
  sw_window_pop (e->page);
}

static void
on_send (GtkWidget *button, Editor *e)
{
  g_autofree char *text = editor_text (e);
  g_autofree char *id = sw_api_escape (e->app_id);
  g_autofree char *path = g_strdup_printf ("/apps/%s/my-review", id);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) body = NULL;

  g_strstrip (text);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "rating");
  json_builder_add_int_value (b, (int) sw_stars_get_rating (SW_STARS (e->stars)));
  json_builder_set_member_name (b, "text");
  if (*text)
    json_builder_add_string_value (b, text);
  else
    json_builder_add_null_value (b);
  json_builder_end_object (b);
  body = json_builder_get_root (b);
  e->busy = TRUE;
  gtk_widget_set_visible (e->error, FALSE);
  editor_update (e);
  sw_api_send (sw_api_get_default (), "PUT", path, body, SW_API_NONE, sw_cancellable_for (e->page), on_sent, e);
}

static void
on_deleted (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Editor *e = data;

  if (error && error->status != 404)
    {
      g_autofree char *text = sw_api_error_describe (error);

      sw_window_toast (e->page, text);
      return;
    }
  sw_reviews_changed (e->app_id);
  sw_window_toast (e->page, "Your review is deleted");
  sw_window_pop (e->page);
}

static void
on_delete_response (VibeosDialog *dialog, int response, Editor *e)
{
  g_autofree char *id = NULL, *path = NULL;

  if (response != VIBEOS_RESPONSE_OK)
    return;
  id = sw_api_escape (e->app_id);
  path = g_strdup_printf ("/apps/%s/my-review", id);
  sw_api_send (sw_api_get_default (), "DELETE", path, NULL, SW_API_NONE, sw_cancellable_for (e->page), on_deleted, e);
}

static void
on_delete (GtkWidget *button, Editor *e)
{
  VibeosDialog *dialog = vibeos_dialog_new ("Delete your review?");

  vibeos_dialog_set_message (dialog, "Your stars and text go, and the app's rating no longer counts them.");
  vibeos_dialog_add_button (dialog, "Cancel", VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, "Delete", VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), e);
  vibeos_dialog_present (dialog, e->page);
}

GtkWidget *
sw_review_editor_new (const char *app_id, const char *app_name, int rating, JsonObject *review)
{
  Editor *e = g_new0 (Editor, 1);
  g_autofree char *title = app_name ? g_strdup_printf ("Rate %s", app_name) : g_strdup ("Rate the app");
  GtkWidget *column = sw_column_new (16), *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *scroll;
  GtkWidget *label_row, *label;
  GtkTextBuffer *buffer;

  e->app_id = g_strdup (app_id);
  e->had_review = review != NULL;
  e->page = sw_page_new (title);
  g_object_set_data_full (G_OBJECT (e->page), "sw-editor", e, editor_free);
  gtk_widget_add_css_class (column, "sw-form");

  gtk_box_append (GTK_BOX (column), sw_label_new ("Your rating", "sw-subtitle"));
  e->stars = sw_stars_new (rating, 40, 16, TRUE);
  gtk_widget_set_halign (e->stars, GTK_ALIGN_START);
  g_signal_connect (e->stars, "changed", G_CALLBACK (on_editor_changed), e);
  gtk_box_append (GTK_BOX (column), e->stars);

  /* the label, and how much of the 500 characters is used, over the field */
  label_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  label = sw_label_new ("Your review (if you want to say more)", "sw-field-label");
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (label_row), label);
  e->counter = gtk_label_new (NULL);
  gtk_widget_set_valign (e->counter, GTK_ALIGN_END);
  gtk_widget_add_css_class (e->counter, "sw-counter");
  gtk_box_append (GTK_BOX (label_row), e->counter);
  gtk_box_append (GTK_BOX (column), label_row);
  e->view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (e->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (e->view), FALSE);
  gtk_text_view_set_input_hints (GTK_TEXT_VIEW (e->view),
                                 GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_UPPERCASE_SENTENCES);
  gtk_accessible_update_property (GTK_ACCESSIBLE (e->view), GTK_ACCESSIBLE_PROPERTY_LABEL, "Your review", -1);
  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (e->view));
  if (sw_json_string (review, "text"))
    gtk_text_buffer_set_text (buffer, sw_json_string (review, "text"), -1);
  g_signal_connect (buffer, "changed", G_CALLBACK (on_editor_changed), e);
  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), e->view);
  gtk_widget_set_size_request (scroll, -1, 160);
  gtk_widget_add_css_class (frame, "sw-text-area");
  gtk_box_append (GTK_BOX (frame), scroll);
  gtk_box_append (GTK_BOX (column), frame);
  gtk_box_append (GTK_BOX (column),
                  sw_label_new ("Your review shows with your account's name. Be kind, and say what you think of the "
                                "app.", "sw-caption"));
  e->error = sw_label_new (NULL, "sw-form-error");
  gtk_widget_set_visible (e->error, FALSE);
  gtk_box_append (GTK_BOX (column), e->error);

  e->send = sw_button_new (review ? "Save" : "Send", SW_BUTTON_FILLED);
  gtk_widget_add_css_class (e->send, "large");
  g_signal_connect (e->send, "clicked", G_CALLBACK (on_send), e);
  gtk_box_append (GTK_BOX (column), e->send);
  if (review)
    {
      GtkWidget *remove = sw_button_new ("Delete my review", SW_BUTTON_TEXT);

      gtk_widget_set_halign (remove, GTK_ALIGN_CENTER);
      g_signal_connect (remove, "clicked", G_CALLBACK (on_delete), e);
      gtk_box_append (GTK_BOX (column), remove);
    }
  sw_page_set_body (e->page, sw_scroll_new (column));
  editor_update (e);
  return e->page;
}

/* ---- reporting ------------------------------------------------------------------------------------------------- */

static const struct {
  const char *label, *reason;
} reasons[] = {
  { "Spam or an advert", "spam" },
  { "Offensive", "offensive" },
  { "Not about this app", "off_topic" },
  { "Fake or misleading", "fake" },
  { "Something else", "other" },
};

typedef struct {
  GtkWidget *sheet, *from, *rows[G_N_ELEMENTS (reasons)], *comment, *send, *error;
  gint64     review_id;
  int        reason;
} Report;

static void
on_report_sent (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Report *r = data;

  gtk_widget_set_sensitive (r->send, TRUE);
  if (error)
    {
      g_autofree char *text = sw_api_error_describe (error);

      gtk_label_set_text (GTK_LABEL (r->error), text);
      gtk_widget_set_visible (r->error, TRUE);
      return;
    }
  sw_window_toast (r->from, "Thank you. The store team will look at the review.");
  vibeos_sheet_close (VIBEOS_SHEET (r->sheet));
}

static void
on_report_send (GtkWidget *button, Report *r)
{
  g_autofree char *path = g_strdup_printf ("/reviews/%" G_GINT64_FORMAT "/report", r->review_id);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) body = NULL;
  g_autofree char *comment = g_strstrip (g_strdup (sw_field_get_text (r->comment)));

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "reason");
  json_builder_add_string_value (b, reasons[r->reason].reason);
  if (*comment)
    {
      json_builder_set_member_name (b, "comment");
      json_builder_add_string_value (b, comment);
    }
  json_builder_end_object (b);
  body = json_builder_get_root (b);
  gtk_widget_set_sensitive (r->send, FALSE);
  sw_api_send (sw_api_get_default (), "POST", path, body, SW_API_NONE, sw_cancellable_for (r->sheet), on_report_sent,
               r);
}

static void
on_reason (GtkWidget *row, Report *r)
{
  for (guint i = 0; i < G_N_ELEMENTS (reasons); i++)
    {
      gboolean on = r->rows[i] == row;

      if (on)
        vibeos_list_row_set_end_icon (VIBEOS_LIST_ROW (r->rows[i]), "sw-check-symbolic");
      else
        vibeos_list_row_clear_end (VIBEOS_LIST_ROW (r->rows[i]));
      if (on)
        r->reason = i;
    }
}

void
sw_report_present (GtkWidget *from, gint64 review_id)
{
  Report *r;
  GtkWidget *column, *cards;

  if (!sw_account_is_signed_in (sw_account_get_default ()))
    {
      sw_window_toast (from, "Sign in to report reviews.");
      return;
    }
  r = g_new0 (Report, 1);
  r->from = from;
  r->review_id = review_id;
  r->sheet = vibeos_sheet_new ("Report the review");
  g_object_set_data_full (G_OBJECT (r->sheet), "sw-report", r, g_free);
  column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (column, "sw-sheet-column");
  gtk_box_append (GTK_BOX (column), sw_label_new ("What is wrong with it?", "sw-body-text"));
  cards = vibeos_card_list_new ();
  for (guint i = 0; i < G_N_ELEMENTS (reasons); i++)
    {
      r->rows[i] = vibeos_list_row_new (reasons[i].label, NULL);
      g_signal_connect (r->rows[i], "clicked", G_CALLBACK (on_reason), r);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), r->rows[i]);
    }
  gtk_box_append (GTK_BOX (column), cards);
  on_reason (r->rows[0], r);
  r->comment = sw_field_new ("More about it (if you want)", SW_FIELD_TEXT);
  gtk_text_set_max_length (GTK_TEXT (sw_field_get_text_widget (r->comment)), MAX_TEXT);
  gtk_box_append (GTK_BOX (column), r->comment);
  r->error = sw_label_new (NULL, "sw-form-error");
  gtk_widget_set_visible (r->error, FALSE);
  gtk_box_append (GTK_BOX (column), r->error);
  r->send = sw_button_new ("Send the report", SW_BUTTON_FILLED);
  gtk_widget_add_css_class (r->send, "large");
  g_signal_connect (r->send, "clicked", G_CALLBACK (on_report_send), r);
  gtk_box_append (GTK_BOX (column), r->send);
  vibeos_sheet_set_content (VIBEOS_SHEET (r->sheet), sw_scroll_new (column));
  vibeos_sheet_present (VIBEOS_SHEET (r->sheet), from);
}
