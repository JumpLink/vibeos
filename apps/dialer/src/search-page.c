/* The search results: see search-page.h. The rules are SearchContactsCursorLoader's (regular search, CP2's phone
 * filter), SmartDialNameMatcher's and DialerDatabaseHelper's (smart dial), QueryBoldingUtil's (what is bold) and
 * NewSearchFragment's (the action rows); the geometry is search_contact_row.xml's, header_layout.xml's and
 * search_action_layout.xml's, measured on still:dialer-search-ma and still:dialer-dialpad-typed (spec §4). */
#include "search-page.h"
#include "avatar.h"
#include "calls-client.h"
#include "call-types.h"
#include "contacts-app.h"
#include "phone.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <string.h>

#define DEBOUNCE_MS     300     /* NETWORK_SEARCH_DELAY_MILLIS */
#define SMART_DIAL_MAX  20      /* MAX_ENTRIES */
#define HEADER_HEIGHT   48
#define HEADER_SIDE     16
#define HEADER_BOTTOM   2
#define ROW_HEIGHT      56      /* search_row_height */
#define PHOTO           40      /* the 56 dp badge less its 8 dp padding */
#define PHOTO_START     8
#define TEXT_START      64      /* past the badge, and the column's 8 dp margin */
#define ACTION_HEIGHT   48
#define ACTION_ICON_BOX 48
#define ACTION_START    8
#define ACTION_TEXT     72
#define BOLD            1000    /* StyleSpan(BOLD) on GSF 700: capped */

/* ---- text: folding, and the keypad's letters ---------------------------------------------------------------- */

/* one character as matching sees it: no accents, no case ("É" -> "e"); "" for a combining mark */
static char *
fold_char (gunichar c)
{
  char buf[8];
  g_autofree char *decomposed = NULL;
  GString *out = g_string_new (NULL);
  int n = g_unichar_to_utf8 (c, buf);

  buf[n] = 0;
  decomposed = g_utf8_normalize (buf, -1, G_NORMALIZE_NFD);
  for (const char *p = decomposed; p && *p; p = g_utf8_next_char (p))
    if (!g_unichar_ismark (g_utf8_get_char (p)))
      {
        g_autofree char *folded = NULL;
        char one[8];
        int m = g_unichar_to_utf8 (g_utf8_get_char (p), one);

        one[m] = 0;
        folded = g_utf8_casefold (one, -1);
        g_string_append (out, folded);
      }
  return g_string_free (out, FALSE);
}

static char *
fold (const char *text)
{
  GString *out = g_string_new (NULL);

  for (const char *p = text; *p; p = g_utf8_next_char (p))
    {
      g_autofree char *one = fold_char (g_utf8_get_char (p));

      g_string_append (out, one);
    }
  return g_string_free (out, FALSE);
}

/* DialpadCharMappings: the Latin keypad, and the locale's second alphabet for Bulgarian, Russian and Ukrainian */
static gboolean
second_alphabet (void)
{
  const char *language = g_get_language_names ()[0];

  return g_str_has_prefix (language, "ru") || g_str_has_prefix (language, "uk") || g_str_has_prefix (language, "bg");
}

static char
t9 (gunichar c)
{
  static const char *latin[] = { "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" };
  static const char *cyrillic[] = { "абвгґ", "дежзє", "ийклії", "мноп", "рсту", "фхцч", "шщъы", "ьэюя" };
  g_autofree char *folded = fold_char (c);
  gunichar f = g_utf8_get_char (folded);

  if (g_unichar_isdigit (c))
    return '0' + g_unichar_digit_value (c);
  for (int k = 0; k < 8; k++)
    {
      if (f < 0x80 && strchr (latin[k], (int) f))
        return '2' + k;
      if (f >= 0x400 && second_alphabet () && g_utf8_strchr (cyrillic[k], -1, f))
        return '2' + k;
    }
  return 0;
}

static char *
digits_of (const char *text)
{
  GString *out = g_string_new (NULL);

  for (const char *p = text; *p; p++)
    if (g_ascii_isdigit (*p))
      g_string_append_c (out, *p);
  return g_string_free (out, FALSE);
}

/* a span of a text to draw bold, in bytes */
typedef struct
{
  guint start, end;
} Span;

/* ---- regular search ----------------------------------------------------------------------------------------- */

/* the query (folded) matched from @at in @name; the byte where the match ends, or NULL */
static const char *
match_at (const char *at, const char *query)
{
  const char *q = query;

  for (const char *p = at; *p; p = g_utf8_next_char (p))
    {
      g_autofree char *one = fold_char (g_utf8_get_char (p));
      size_t n = strlen (one);

      if (!n)
        continue;
      if (strncmp (q, one, MIN (n, strlen (q))) != 0)
        return NULL;
      q += MIN (n, strlen (q));
      if (!*q)
        return g_utf8_next_char (p);
    }
  return NULL;
}

static gboolean
word_start (const char *text, const char *p)
{
  return p == text || g_unichar_isspace (g_utf8_get_char (g_utf8_prev_char (p)));
}

/* CP2's filter: each word of the query starts a word of the name. Bold: the first match of the whole query at
 * the start of a word (QueryBoldingUtil.getNameWithQueryBolded) */
static gboolean
name_matches (const char *name, const char *query, Span *bold)
{
  g_autofree char *folded = g_utf8_casefold (query, -1);
  g_auto(GStrv) words = g_strsplit_set (g_strstrip (folded), " \t", -1);
  gboolean all = TRUE;

  bold->start = bold->end = 0;
  for (guint i = 0; words[i] && all; i++)
    {
      g_autofree char *word = fold (words[i]);
      gboolean found = FALSE;

      if (!*word)
        continue;
      for (const char *p = name; *p && !found; p = g_utf8_next_char (p))
        found = word_start (name, p) && match_at (p, word);
      all = found;
    }
  if (!all)
    return FALSE;
  {
    g_autofree char *whole = fold (query);

    for (const char *p = name; *p; p = g_utf8_next_char (p))
      {
        const char *end = word_start (name, p) ? match_at (p, whole) : NULL;

        if (end)
          {
            bold->start = p - name;
            bold->end = end - name;
            break;
          }
      }
  }
  return TRUE;
}

/* the digits of @query found in @number: bold those characters of @number (the formatting kept) */
static gboolean
number_matches (const char *number, const char *query_digits, Span *bold)
{
  g_autofree char *digits = digits_of (number);
  const char *found = *query_digits ? strstr (digits, query_digits) : NULL;
  guint index = 0, first, last;

  bold->start = bold->end = 0;
  if (!found)
    return FALSE;
  first = found - digits;
  last = first + strlen (query_digits);
  for (const char *p = number; *p; p++)
    {
      if (!g_ascii_isdigit (*p))
        continue;
      if (index == first)
        bold->start = p - number;
      if (index == last - 1)
        bold->end = p - number + 1;
      index++;
    }
  return TRUE;
}

/* PhoneNumberUtils.isGlobalPhoneNumber: "[\+]?[0-9.-]+" */
static gboolean
is_global_number (const char *text)
{
  const char *p = text;

  if (*p == '+')
    p++;
  if (!*p)
    return FALSE;
  for (; *p; p++)
    if (!g_ascii_isdigit (*p) && *p != '.' && *p != '-')
      return FALSE;
  return TRUE;
}

/* ---- smart dial --------------------------------------------------------------------------------------------- */

typedef struct
{
  const char *start;            /* in the name */
  GArray *chars;                /* gunichar */
} Token;

/* the name's words for smart dial: split on what is not a letter or digit (after accents go) */
static GArray *
tokens_of (const char *name)
{
  GArray *tokens = g_array_new (FALSE, TRUE, sizeof (Token));
  Token *token = NULL;

  for (const char *p = name; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (!t9 (c))
        {
          token = NULL;
          continue;
        }
      if (!token)
        {
          g_array_set_size (tokens, tokens->len + 1);
          token = &g_array_index (tokens, Token, tokens->len - 1);
          token->start = p;
          token->chars = g_array_new (FALSE, FALSE, sizeof (gunichar));
        }
      g_array_append_val (token->chars, c);
    }
  return tokens;
}

static void
tokens_free (GArray *tokens)
{
  for (guint i = 0; i < tokens->len; i++)
    g_array_unref (g_array_index (tokens, Token, i).chars);
  g_array_unref (tokens);
}

/* SmartDialNameMatcher.matchesCombination: the digits from the start of a word, on into the start of a later word
 * once at least one letter of this one matched (so "57" is John Smith's initials, "37337" Fred Smith).
 * @used[t] counts the letters matched in word t */
static gboolean
smart_match (GArray *tokens, guint t, guint c, const char *query, guint *used)
{
  const Token *token;

  if (!*query)
    return TRUE;
  if (t >= tokens->len)
    return FALSE;
  token = &g_array_index (tokens, Token, t);
  if (c < token->chars->len && t9 (g_array_index (token->chars, gunichar, c)) == *query)
    {
      used[t] = c + 1;
      if (smart_match (tokens, t, c + 1, query + 1, used))
        return TRUE;
      used[t] = c;
    }
  if (c > 0)
    for (guint next = t + 1; next < tokens->len; next++)
      if (smart_match (tokens, next, 0, query, used))
        return TRUE;
  return FALSE;
}

/* the name matches the digits; the letters matched are the ones to bold (the run, or the initials) */
static gboolean
smart_name_matches (const char *name, const char *digits, GArray *spans)
{
  GArray *tokens = tokens_of (name);
  g_autofree guint *used = g_new0 (guint, tokens->len + 1);
  gboolean found = FALSE;

  for (guint t = 0; t < tokens->len && !found; t++)
    {
      memset (used, 0, sizeof (guint) * tokens->len);
      found = smart_match (tokens, t, 0, digits, used);
    }
  if (found)
    for (guint t = 0; t < tokens->len; t++)
      if (used[t])
        {
          const Token *token = &g_array_index (tokens, Token, t);
          const char *end = token->start;
          Span span;

          for (guint i = 0; i < used[t]; i++)
            end = g_utf8_next_char (end);
          span.start = token->start - name;
          span.end = end - name;
          g_array_append_val (spans, span);
        }
  tokens_free (tokens);
  return found;
}

/* ---- the results -------------------------------------------------------------------------------------------- */

typedef struct
{
  const Contact *contact;       /* valid until the contacts' next "changed": the rows copy what they show */
  const ContactNumber *number;
  gboolean first;               /* the contact's first row: its name and picture show */
  GArray *name_bold;            /* Span */
  Span number_bold;
} Result;

static void
result_free (Result *result)
{
  g_array_unref (result->name_bold);
  g_free (result);
}

static Result *
result_new (const Contact *contact, const ContactNumber *number)
{
  Result *result = g_new0 (Result, 1);

  result->contact = contact;
  result->number = number;
  result->name_bold = g_array_new (FALSE, FALSE, sizeof (Span));
  return result;
}

struct _SearchPage
{
  GtkWidget parent;
  Contacts *contacts;
  GSettings *settings;
  NeoSnackbar *snackbar;
  GtkWidget *scroller, *list;
  char *query;
  gboolean dialpad;
  guint debounce_id;
};

G_DEFINE_FINAL_TYPE (SearchPage, search_page, GTK_TYPE_WIDGET)

enum { TOUCHED, CALL_PLACED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GPtrArray *
regular_results (SearchPage *self)
{
  GPtrArray *all = contacts_get_all (self->contacts);
  GPtrArray *results = g_ptr_array_new_with_free_func ((GDestroyNotify) result_free);
  g_autofree char *query_digits = digits_of (self->query);
  /* a query with letters in it is a name: only a query of dial characters looks in the numbers */
  gboolean numeric = *query_digits && strspn (self->query, "0123456789+-.() ") == strlen (self->query);

  for (guint i = 0; i < all->len; i++)
    {
      const Contact *contact = all->pdata[i];
      Span name_bold;
      gboolean by_name = contact->name && *contact->name && name_matches (contact->name, self->query, &name_bold);
      gboolean first = TRUE;

      for (guint n = 0; n < contact->numbers->len; n++)
        {
          const ContactNumber *number = contact->numbers->pdata[n];
          Span number_bold = { 0, 0 };
          Result *result;

          if (!by_name && !(numeric && number_matches (number->number, query_digits, &number_bold)))
            continue;
          if (by_name && numeric)
            number_matches (number->number, query_digits, &number_bold);
          result = result_new (contact, number);
          result->first = first;
          if (by_name && name_bold.end > name_bold.start)
            g_array_append_val (result->name_bold, name_bold);
          result->number_bold = number_bold;
          g_ptr_array_add (results, result);
          first = FALSE;
        }
    }
  return results;
}

/* smart dial: one row per contact, its first number that matches (any, when the name does); favourites first,
 * then by name; at most 20 */
static GPtrArray *
smart_dial_results (SearchPage *self)
{
  GPtrArray *all = contacts_get_all (self->contacts);
  g_auto(GStrv) favorites = g_settings_get_strv (self->settings, "favorites");
  g_autofree char *digits = digits_of (self->query);
  const char *country = calls_client_get_country (calls_client_get ());
  GPtrArray *starred = g_ptr_array_new_with_free_func ((GDestroyNotify) result_free);
  GPtrArray *results = g_ptr_array_new_with_free_func ((GDestroyNotify) result_free);

  if (!*digits)
    {
      g_ptr_array_unref (starred);
      return results;
    }
  for (guint i = 0; i < all->len; i++)
    {
      const Contact *contact = all->pdata[i];
      g_autoptr(GArray) spans = g_array_new (FALSE, FALSE, sizeof (Span));
      gboolean by_name = contact->name && smart_name_matches (contact->name, digits, spans);
      Result *result = NULL;

      for (guint n = 0; n < contact->numbers->len && !result; n++)
        {
          const ContactNumber *number = contact->numbers->pdata[n];
          g_autofree char *national = phone_number_format (number->number, country ? country
                                                                                   : phone_number_locale_country ());
          Span bold = { 0, 0 };
          g_autofree char *national_digits = digits_of (national);

          /* the number as stored, or as dialled at home (030 …, without the +49) */
          if (number_matches (number->number, digits, &bold) || strstr (national_digits, digits) || by_name)
            {
              result = result_new (contact, number);
              result->number_bold = bold;
            }
        }
      if (!result)
        continue;
      result->first = TRUE;
      g_array_append_vals (result->name_bold, spans->data, spans->len);
      g_ptr_array_add (g_strv_contains ((const char * const *) favorites, contact->uid) ? starred : results,
                       result);
    }
  /* starred first */
  for (guint i = 0; i < starred->len; i++)
    g_ptr_array_insert (results, i, g_steal_pointer (&starred->pdata[i]));
  g_ptr_array_set_free_func (starred, NULL);
  g_ptr_array_unref (starred);
  if (results->len > SMART_DIAL_MAX)
    g_ptr_array_set_size (results, SMART_DIAL_MAX);
  return results;
}

static PangoAttrList *
bold_attributes (const Span *spans, guint n)
{
  PangoAttrList *attrs = pango_attr_list_new ();

  for (guint i = 0; i < n; i++)
    {
      PangoAttribute *weight = pango_attr_weight_new (BOLD);

      weight->start_index = spans[i].start;
      weight->end_index = spans[i].end;
      pango_attr_list_insert (attrs, weight);
    }
  return attrs;
}

/* ---- the rows ----------------------------------------------------------------------------------------------- */

static void
on_dialed (guint id, const GError *error, gpointer data)
{
  SearchPage *self = data;

  if (error)
    neo_snackbar_show (self->snackbar, error->message, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
  else
    {
      phone_start_app (INCALL_APP_ID);
      g_signal_emit (self, signals[CALL_PLACED], 0);
    }
  g_object_unref (self);
}

static void
on_row_clicked (GtkWidget *row, SearchPage *self)
{
  calls_client_dial (calls_client_get (), g_object_get_data (G_OBJECT (row), "search-number"), on_dialed,
                     g_object_ref (self));
}

static GtkWidget *
header_row (void)
{
  GtkWidget *label = gtk_label_new (_("All contacts"));

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_size_request (label, -1, HEADER_HEIGHT);
  gtk_widget_set_margin_start (label, HEADER_SIDE);
  gtk_widget_set_margin_end (label, HEADER_SIDE);
  gtk_widget_set_margin_bottom (label, HEADER_BOTTOM);
  gtk_widget_add_css_class (label, "dialer-secondary-text");
  neo_label_set_font_padding (label, 14);
  return label;
}

static GtkWidget *
contact_row (SearchPage *self, const Result *result)
{
  const Contact *contact = result->contact;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = dialer_avatar_new (PHOTO);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *name = gtk_label_new (contact->name);
  g_autofree char *secondary_text = NULL;
  GtkWidget *secondary;
  g_autoptr(PangoAttrList) name_attrs = bold_attributes ((Span *) result->name_bold->data, result->name_bold->len);
  PangoAttrList *number_attrs;
  GtkWidget *row;
  guint prefix = 0;

  gtk_widget_set_size_request (box, -1, ROW_HEIGHT);
  /* a second number of the same contact: the picture keeps its room, unseen, and the name is gone */
  dialer_avatar_set (DIALER_AVATAR (avatar), contact->uid, contact->name, contact->photo);
  gtk_widget_set_margin_start (avatar, PHOTO_START);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_opacity (avatar, result->first ? 1 : 0);
  gtk_box_append (GTK_BOX (box), avatar);

  gtk_widget_set_margin_start (texts, TEXT_START - PHOTO_START - PHOTO);
  gtk_widget_set_margin_end (texts, 8);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_attributes (GTK_LABEL (name), name_attrs);
  gtk_widget_add_css_class (name, "dialer-primary-text");
  neo_label_set_font_padding (name, 16);
  gtk_widget_set_visible (name, result->first);
  gtk_box_append (GTK_BOX (texts), name);

  /* "<kind> <number>"; the number alone without a kind, and always in dialpad search */
  if (!self->dialpad && result->number->label && *result->number->label)
    {
      secondary_text = g_strdup_printf (_("%1$s %2$s"), result->number->label, result->number->number);
      prefix = strlen (secondary_text) - strlen (result->number->number);
    }
  else
    secondary_text = g_strdup (result->number->number);
  secondary = gtk_label_new (secondary_text);
  if (result->number_bold.end > result->number_bold.start)
    {
      Span span = { result->number_bold.start + prefix, result->number_bold.end + prefix };

      number_attrs = bold_attributes (&span, 1);
      gtk_label_set_attributes (GTK_LABEL (secondary), number_attrs);
      pango_attr_list_unref (number_attrs);
    }
  gtk_label_set_xalign (GTK_LABEL (secondary), 0);
  gtk_label_set_ellipsize (GTK_LABEL (secondary), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (secondary, "dialer-secondary-text");
  neo_label_set_font_padding (secondary, 14);
  gtk_box_append (GTK_BOX (texts), secondary);
  gtk_box_append (GTK_BOX (box), texts);

  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &(GdkRGBA) { 1, 1, 1, 0.06 });
  g_object_set_data_full (G_OBJECT (row), "search-number", g_strdup (result->number->number), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row_clicked), self);
  return row;
}

static void
on_call_action (GtkWidget *row, SearchPage *self)
{
  g_autofree char *number = phone_number_normalize (self->query);

  calls_client_dial (calls_client_get (), number, on_dialed, g_object_ref (self));
}

static void
on_sms_action (GtkWidget *row, SearchPage *self)
{
  g_autofree char *number = phone_number_normalize (self->query);
  g_autofree char *uri = g_strconcat ("sms:", number, NULL);
  g_autoptr(GError) error = NULL;

  if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
    g_warning ("search: %s: %s", uri, error->message);
}

/* "Create new contact" (IntentUtil.getNewContactIntent) and "Add to a contact" (getAddToExistingContactIntent): the
 * dialpad's digits as they were typed, to the Contacts app */
static void
on_create_contact_action (GtkWidget *row, SearchPage *self)
{
  contacts_app_insert (GTK_WIDGET (self), self->query);
}

static void
on_add_to_contact_action (GtkWidget *row, SearchPage *self)
{
  contacts_app_insert_or_edit (GTK_WIDGET (self), self->query);
}

static GtkWidget *
action_row (SearchPage *self, const char *icon_name, const char *text, GCallback callback)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *label = gtk_label_new (text);
  GtkWidget *row;

  gtk_widget_set_size_request (box, -1, ACTION_HEIGHT);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_size_request (icon, ACTION_ICON_BOX, ACTION_ICON_BOX);
  gtk_widget_set_margin_start (icon, ACTION_START);
  gtk_widget_add_css_class (icon, "dialer-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (label, ACTION_TEXT - ACTION_START - ACTION_ICON_BOX);
  gtk_widget_set_margin_end (label, 16);
  gtk_widget_add_css_class (label, "dialer-primary-text");
  neo_label_set_font_padding (label, 16);
  gtk_box_append (GTK_BOX (box), label);
  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &(GdkRGBA) { 1, 1, 1, 0.06 });
  g_signal_connect (row, "clicked", callback, self);
  return row;
}

/* NewSearchFragment.getActions: nothing for 0 or 1 characters; for a number typed in the search bar, call it or
 * text it; for dialpad digits, make a contact of them or add them to one (where the Contacts app is there to take
 * them), or text them. There is no video calling. */
static void
add_actions (SearchPage *self)
{
  g_autoptr(GAppInfo) sms = g_app_info_get_default_for_uri_scheme ("sms");

  if (g_utf8_strlen (self->query, -1) < 2)
    return;
  if (self->dialpad && contacts_app_installed ())
    {
      gtk_box_append (GTK_BOX (self->list), action_row (self, "dialer-person-add-symbolic", _("Create new contact"),
                                                         G_CALLBACK (on_create_contact_action)));
      gtk_box_append (GTK_BOX (self->list), action_row (self, "dialer-person-add-symbolic", _("Add to a contact"),
                                                         G_CALLBACK (on_add_to_contact_action)));
    }
  if (!self->dialpad)
    {
      g_autofree char *call = NULL;

      if (!is_global_number (self->query))
        return;
      call = g_strdup_printf (_("Call %s"), self->query);
      gtk_box_append (GTK_BOX (self->list), action_row (self, "dialer-phone-symbolic", call,
                                                         G_CALLBACK (on_call_action)));
    }
  if (sms)
    gtk_box_append (GTK_BOX (self->list), action_row (self, "dialer-message-symbolic", _("Send SMS"),
                                                       G_CALLBACK (on_sms_action)));
}

static void
clear (SearchPage *self)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);
}

static gboolean
run_search (gpointer data)
{
  SearchPage *self = data;
  g_autoptr(GPtrArray) results = NULL;

  self->debounce_id = 0;
  clear (self);
  if (!*self->query)
    return G_SOURCE_REMOVE;
  results = self->dialpad ? smart_dial_results (self) : regular_results (self);
  if (results->len)
    gtk_box_append (GTK_BOX (self->list), header_row ());
  for (guint i = 0; i < results->len; i++)
    gtk_box_append (GTK_BOX (self->list), contact_row (self, results->pdata[i]));
  add_actions (self);
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroller), 0);
  return G_SOURCE_REMOVE;
}

void
search_page_set_query (SearchPage *self, const char *query, gboolean dialpad)
{
  query = query ? query : "";
  if (g_strcmp0 (query, self->query) == 0 && dialpad == self->dialpad)
    return;
  g_free (self->query);
  self->query = g_strdup (query);
  self->dialpad = dialpad;
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  if (!*query)
    run_search (self);
  else
    self->debounce_id = g_timeout_add (DEBOUNCE_MS, run_search, self);
}

static void
on_contacts_changed (SearchPage *self)
{
  /* the rows point into the contacts: they are made again */
  if (*self->query)
    run_search (self);
}

/* onSearchListTouch: the window decides (the dialpad goes, or the keyboard); the touch still reaches the list */
static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, SearchPage *self)
{
  g_signal_emit (self, signals[TOUCHED], 0);
}

static void
search_page_dispose (GObject *object)
{
  SearchPage *self = SEARCH_PAGE (object);

  g_clear_handle_id (&self->debounce_id, g_source_remove);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->contacts);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->query, g_free);
  g_clear_pointer (&self->scroller, gtk_widget_unparent);
  G_OBJECT_CLASS (search_page_parent_class)->dispose (object);
}

static void
search_page_class_init (SearchPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = search_page_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "search-page");
  signals[TOUCHED] = g_signal_new ("touched", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
  signals[CALL_PLACED] = g_signal_new ("call-placed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 0);
}

static void
search_page_init (SearchPage *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->query = g_strdup ("");
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->list, GTK_ALIGN_START);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  gtk_widget_set_parent (self->scroller, GTK_WIDGET (self));
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
search_page_new (Contacts *contacts, GSettings *settings, NeoSnackbar *snackbar)
{
  SearchPage *self = g_object_new (SEARCH_TYPE_PAGE, NULL);

  self->contacts = g_object_ref (contacts);
  self->settings = g_object_ref (settings);
  self->snackbar = snackbar;
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  return GTK_WIDGET (self);
}
