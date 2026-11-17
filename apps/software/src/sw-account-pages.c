/* The account's pages: signing in and up, the emailed codes, a forgotten password, the account and its settings,
 * deleting it, its reviews; and Software's own settings and About.
 *
 * Every rule (lengths, the code's tries, how often a code may be sent) is the server's: its sentence shows under the
 * field it is about, or under the form. Nothing here asks the store until a button is pressed. */
#include "sw-pages.h"
#include "sw-account.h"
#include "sw-json.h"
#include "sw-widgets.h"
#include "sw-window.h"

#define CODE_LENGTH 6

static GSettings *
settings_new (void)
{
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                             "vibeos.Software", TRUE);

  if (!schema)
    return NULL;
  g_settings_schema_unref (schema);
  return g_settings_new ("vibeos.Software");
}

/* a page of the sign-in flow: pops with the others once it is done */
static GtkWidget *
flow_page (const char *title)
{
  GtkWidget *page = sw_page_new (title);

  g_object_set_data (G_OBJECT (page), "sw-account-flow", GINT_TO_POINTER (1));
  return page;
}

static GtkWidget *
form (GtkWidget *page)
{
  GtkWidget *column = sw_column_new (16);

  gtk_widget_add_css_class (column, "sw-form");
  sw_page_set_body (page, sw_scroll_new (column));
  return column;
}

static GtkWidget *
form_error (GtkWidget *column)
{
  GtkWidget *label = sw_label_new (NULL, "sw-form-error");

  gtk_widget_set_visible (label, FALSE);
  gtk_box_append (GTK_BOX (column), label);
  return label;
}

static void
show_error (GtkWidget *label, const SwApiError *error)
{
  g_autofree char *text = sw_api_error_describe (error);

  gtk_label_set_text (GTK_LABEL (label), text);
  gtk_widget_set_visible (label, TRUE);
}

static GtkWidget *
big_button (GtkWidget *column, const char *label, SwButtonStyle style, GCallback callback, gpointer data)
{
  GtkWidget *button = sw_button_new (label, style);

  gtk_widget_add_css_class (button, "large");
  g_signal_connect (button, "clicked", callback, data);
  gtk_box_append (GTK_BOX (column), button);
  return button;
}

static GtkWidget *
link_button (GtkWidget *column, const char *label, GCallback callback, gpointer data)
{
  GtkWidget *button = sw_button_new (label, SW_BUTTON_TEXT);

  gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
  g_signal_connect (button, "clicked", callback, data);
  gtk_box_append (GTK_BOX (column), button);
  return button;
}

/* Enter in a field: the form's button */
static void
on_field_activate (GtkText *text, GtkWidget *button)
{
  if (gtk_widget_get_sensitive (button))
    g_signal_emit_by_name (button, "clicked");
}

static void
submit_on_enter (GtkWidget *field, GtkWidget *button)
{
  g_signal_connect (sw_field_get_text_widget (field), "activate", G_CALLBACK (on_field_activate), button);
}

/* ---- signing in ------------------------------------------------------------------------------------------------ */

typedef struct {
  GtkWidget *page, *email, *password, *button, *error;
} SignIn;

static void
on_signed_in (SwApiResult *result, const SwApiError *error, gpointer data)
{
  SignIn *s = data;

  gtk_widget_set_sensitive (s->button, TRUE);
  if (error)
    {
      if (!sw_fields_show_errors (error, "email", s->email, "password", s->password, NULL))
        show_error (s->error, error);
      return;
    }
  if (!sw_account_is_verified (sw_account_get_default ()))
    {
      sw_window_replace (s->page, sw_verify_page_new (FALSE));
      return;
    }
  sw_window_toast (s->page, "You are signed in");
  sw_window_pop_account_flow (s->page);
}

static void
on_sign_in (GtkWidget *button, SignIn *s)
{
  g_autofree char *email = g_strstrip (g_strdup (sw_field_get_text (s->email)));

  gtk_widget_set_visible (s->error, FALSE);
  if (!*email)
    {
      sw_field_set_error (s->email, "Enter your email address.");
      return;
    }
  if (!*sw_field_get_text (s->password))
    {
      sw_field_set_error (s->password, "Enter your password.");
      return;
    }
  gtk_widget_set_sensitive (s->button, FALSE);
  sw_account_sign_in (sw_account_get_default (), email, sw_field_get_text (s->password), sw_cancellable_for (s->page),
                      on_signed_in, s);
}

static void
on_forgot (GtkWidget *button, SignIn *s)
{
  sw_window_push (button, sw_forgot_page_new (sw_field_get_text (s->email)));
}

static void
on_create (GtkWidget *button, SignIn *s)
{
  sw_window_replace (button, sw_register_page_new ());
}

GtkWidget *
sw_sign_in_page_new (void)
{
  SignIn *s = g_new0 (SignIn, 1);
  GtkWidget *column;

  s->page = flow_page ("Sign in");
  g_object_set_data_full (G_OBJECT (s->page), "sw-sign-in", s, g_free);
  column = form (s->page);
  gtk_box_append (GTK_BOX (column),
                  sw_label_new ("Sign in to rate and review apps, and to see your apps on all your devices.",
                                "sw-body-text"));
  s->email = sw_field_new ("Email", SW_FIELD_EMAIL);
  gtk_box_append (GTK_BOX (column), s->email);
  s->password = sw_field_new ("Password", SW_FIELD_PASSWORD);
  gtk_box_append (GTK_BOX (column), s->password);
  s->error = form_error (column);
  s->button = big_button (column, "Sign in", SW_BUTTON_FILLED, G_CALLBACK (on_sign_in), s);
  submit_on_enter (s->email, s->button);
  submit_on_enter (s->password, s->button);
  link_button (column, "Forgot your password?", G_CALLBACK (on_forgot), s);
  gtk_box_append (GTK_BOX (column), sw_divider_new ());
  gtk_box_append (GTK_BOX (column), sw_label_new ("No account yet?", "sw-subtitle"));
  big_button (column, "Create an account", SW_BUTTON_TONAL, G_CALLBACK (on_create), s);
  return s->page;
}

/* ---- signing up ------------------------------------------------------------------------------------------------ */

typedef struct {
  GtkWidget *page, *name, *email, *password, *button, *error;
} Register;

static void
on_registered (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Register *r = data;
  JsonObject *verification = sw_json_object (sw_json_node_object (result ? result->data : NULL), "verification");

  gtk_widget_set_sensitive (r->button, TRUE);
  if (error)
    {
      if (!sw_fields_show_errors (error, "name", r->name, "email", r->email, "password", r->password, NULL))
        show_error (r->error, error);
      return;
    }
  sw_window_replace (r->page, sw_verify_page_new (sw_json_bool (verification, "sent", FALSE)));
}

static void
on_register (GtkWidget *button, Register *r)
{
  g_autofree char *name = g_strstrip (g_strdup (sw_field_get_text (r->name)));
  g_autofree char *email = g_strstrip (g_strdup (sw_field_get_text (r->email)));

  gtk_widget_set_visible (r->error, FALSE);
  if (!*name)
    {
      sw_field_set_error (r->name, "Enter a name. It shows on your reviews.");
      return;
    }
  if (!*email)
    {
      sw_field_set_error (r->email, "Enter your email address.");
      return;
    }
  gtk_widget_set_sensitive (r->button, FALSE);
  sw_account_register (sw_account_get_default (), name, email, sw_field_get_text (r->password),
                       sw_cancellable_for (r->page), on_registered, r);
}

GtkWidget *
sw_register_page_new (void)
{
  Register *r = g_new0 (Register, 1);
  GtkWidget *column;

  r->page = flow_page ("Create an account");
  g_object_set_data_full (G_OBJECT (r->page), "sw-register", r, g_free);
  column = form (r->page);
  gtk_box_append (GTK_BOX (column),
                  sw_label_new ("With an account you can rate and review apps, and see your apps on all your "
                                "devices. The store emails you a code to confirm the address.", "sw-body-text"));
  r->name = sw_field_new ("Name", SW_FIELD_NAME);
  sw_field_set_hint (r->name, "Shown on your reviews");
  gtk_box_append (GTK_BOX (column), r->name);
  r->email = sw_field_new ("Email", SW_FIELD_EMAIL);
  gtk_box_append (GTK_BOX (column), r->email);
  r->password = sw_field_new ("Password", SW_FIELD_NEW_PASSWORD);
  sw_field_set_hint (r->password, "At least 8 characters");
  gtk_box_append (GTK_BOX (column), r->password);
  r->error = form_error (column);
  r->button = big_button (column, "Create the account", SW_BUTTON_FILLED, G_CALLBACK (on_register), r);
  submit_on_enter (r->password, r->button);
  return r->page;
}

/* ---- the codes ------------------------------------------------------------------------------------------------- */

/* "Send a new code" waits out the store's cooldown, counting down */
typedef struct {
  GtkWidget *button;
  int        left;
  guint      timeout;
} Cooldown;

static gboolean
on_cooldown_tick (gpointer data)
{
  Cooldown *c = data;
  g_autofree char *text = NULL;

  if (--c->left <= 0)
    {
      c->timeout = 0;
      sw_button_set_label (c->button, "Send a new code");
      gtk_widget_set_sensitive (c->button, TRUE);
      return G_SOURCE_REMOVE;
    }
  text = g_strdup_printf ("Send a new code in %d s", c->left);
  sw_button_set_label (c->button, text);
  return G_SOURCE_CONTINUE;
}

static void
cooldown_start (Cooldown *c, int seconds)
{
  g_clear_handle_id (&c->timeout, g_source_remove);
  c->left = seconds + 1;
  gtk_widget_set_sensitive (c->button, FALSE);
  on_cooldown_tick (c);
  if (c->left > 0)
    c->timeout = g_timeout_add_seconds (1, on_cooldown_tick, c);
}

/* the seconds until an answer's resend_available_at */
static int
seconds_until (JsonObject *data, const char *member, int fallback)
{
  g_autoptr (GDateTime) at = NULL, now = NULL;
  const char *text = sw_json_string (data, member);

  if (!text || !(at = g_date_time_new_from_iso8601 (text, NULL)))
    return fallback;
  now = g_date_time_new_now_utc ();
  return CLAMP ((int) (g_date_time_difference (at, now) / G_USEC_PER_SEC), 0, 3600);
}

static void
code_error (GtkWidget *code, GtkWidget *label, const SwApiError *error)
{
  g_autofree char *text = sw_api_error_describe (error);

  sw_code_field_set_error (code, TRUE);
  if (!g_strcmp0 (error->code, "code_invalid") && error->attempts_left >= 0)
    {
      g_free (text);
      text = g_strdup_printf ("%s %d %s left.", error->message, error->attempts_left,
                              error->attempts_left == 1 ? "try" : "tries");
    }
  gtk_label_set_text (GTK_LABEL (label), text);
  gtk_widget_set_visible (label, TRUE);
}

typedef struct {
  GtkWidget *page, *intro, *code, *error, *send, *confirm;
  Cooldown   cooldown;
  gboolean   sent;
} Verify;

static void
verify_free (gpointer data)
{
  Verify *v = data;

  g_clear_handle_id (&v->cooldown.timeout, g_source_remove);
  g_free (v);
}

static void
verify_intro (Verify *v)
{
  g_autofree char *email = sw_mask_email (sw_account_get_email (sw_account_get_default ()));
  g_autofree char *text = v->sent
    ? g_strdup_printf ("The store sent a %d-digit code to %s. Enter it here to confirm the address. It works for 15 "
                       "minutes.", CODE_LENGTH, email)
    : g_strdup_printf ("Confirm your address to rate and review apps: enter the %d-digit code the store emailed to "
                       "%s, or ask for a new one.", CODE_LENGTH, email);

  /* the field is there either way: a code emailed at sign-up still works */
  gtk_label_set_text (GTK_LABEL (v->intro), text);
  sw_button_set_label (v->send, "Send a new code");
  if (v->sent)
    gtk_widget_grab_focus (v->code);
}

static void
on_resent (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Verify *v = data;

  gtk_widget_set_sensitive (v->send, TRUE);
  if (error)
    {
      show_error (v->error, error);
      if (!g_strcmp0 (error->code, "resend_cooldown") && error->retry_after > 0)
        cooldown_start (&v->cooldown, error->retry_after);
      return;
    }
  v->sent = TRUE;
  gtk_widget_set_visible (v->error, FALSE);
  sw_code_field_clear (v->code);
  verify_intro (v);
  sw_window_toast (v->page, "A new code is on its way");
  cooldown_start (&v->cooldown, seconds_until (sw_json_node_object (result->data), "resend_available_at", 60));
}

static void
on_send_code (GtkWidget *button, Verify *v)
{
  gtk_widget_set_sensitive (v->send, FALSE);
  sw_account_resend (sw_account_get_default (), sw_cancellable_for (v->page), on_resent, v);
}

static void
on_verified (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Verify *v = data;

  gtk_widget_set_sensitive (v->confirm, TRUE);
  if (error)
    {
      code_error (v->code, v->error, error);
      return;
    }
  sw_window_toast (v->page, "Your address is confirmed");
  sw_window_pop_account_flow (v->page);
}

static void
on_confirm (GtkWidget *button, Verify *v)
{
  g_autofree char *code = sw_code_field_get_code (v->code);

  if (strlen (code) != CODE_LENGTH)
    {
      sw_code_field_set_error (v->code, TRUE);
      return;
    }
  gtk_widget_set_visible (v->error, FALSE);
  gtk_widget_set_sensitive (v->confirm, FALSE);
  sw_account_verify (sw_account_get_default (), code, sw_cancellable_for (v->page), on_verified, v);
}

static void
on_code_complete (GtkWidget *code, const char *text, gpointer data)
{
  on_confirm (NULL, data);
}

static void
on_later (GtkWidget *button, Verify *v)
{
  sw_window_pop_account_flow (v->page);
}

GtkWidget *
sw_verify_page_new (gboolean just_sent)
{
  Verify *v = g_new0 (Verify, 1);
  GtkWidget *column;

  v->sent = just_sent;
  v->page = flow_page ("Confirm your email");
  g_object_set_data_full (G_OBJECT (v->page), "sw-verify", v, verify_free);
  column = form (v->page);
  v->intro = sw_label_new (NULL, "sw-body-text");
  gtk_box_append (GTK_BOX (column), v->intro);
  v->code = sw_code_field_new (CODE_LENGTH);
  g_signal_connect (v->code, "complete", G_CALLBACK (on_code_complete), v);
  gtk_box_append (GTK_BOX (column), v->code);
  v->error = form_error (column);
  v->confirm = big_button (column, "Confirm", SW_BUTTON_FILLED, G_CALLBACK (on_confirm), v);
  v->send = link_button (column, "Send a new code", G_CALLBACK (on_send_code), v);
  v->cooldown.button = v->send;
  link_button (column, "Later", G_CALLBACK (on_later), v);
  verify_intro (v);
  if (just_sent)
    cooldown_start (&v->cooldown, 60);
  return v->page;
}

/* ---- a forgotten password -------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *page, *column, *email, *code, *password, *error, *button, *resend;
  char      *address, *checked_code;
  int        step;   /* 0 the address, 1 the code, 2 the new password */
  Cooldown   cooldown;
} Forgot;

static void forgot_step (Forgot *f, int step);

static void
forgot_free (gpointer data)
{
  Forgot *f = data;

  g_clear_handle_id (&f->cooldown.timeout, g_source_remove);
  g_free (f->address);
  g_free (f->checked_code);
  g_free (f);
}

static void
on_forgot_sent (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Forgot *f = data;

  if (f->button)
    gtk_widget_set_sensitive (f->button, TRUE);
  if (f->resend)
    gtk_widget_set_sensitive (f->resend, TRUE);
  if (error)
    {
      if (!sw_fields_show_errors (error, "email", f->email, NULL))
        show_error (f->error, error);
      if (!g_strcmp0 (error->code, "resend_cooldown") && error->retry_after > 0 && f->resend)
        cooldown_start (&f->cooldown, error->retry_after);
      return;
    }
  if (f->step == 0)
    forgot_step (f, 1);
  else
    sw_window_toast (f->page, "A new code is on its way");
  cooldown_start (&f->cooldown, seconds_until (sw_json_node_object (result->data), "resend_available_at", 60));
}

static void
on_forgot_send (GtkWidget *button, Forgot *f)
{
  if (f->step == 0)
    {
      g_free (f->address);
      f->address = g_strstrip (g_strdup (sw_field_get_text (f->email)));
      if (!*f->address)
        {
          sw_field_set_error (f->email, "Enter your email address.");
          return;
        }
    }
  gtk_widget_set_visible (f->error, FALSE);
  gtk_widget_set_sensitive (button, FALSE);
  sw_account_forgot (sw_account_get_default (), f->address, sw_cancellable_for (f->page), on_forgot_sent, f);
}

static void
on_code_checked (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Forgot *f = data;

  gtk_widget_set_sensitive (f->button, TRUE);
  if (error)
    {
      code_error (f->code, f->error, error);
      return;
    }
  forgot_step (f, 2);
}

static void
on_check_code (GtkWidget *button, Forgot *f)
{
  g_autofree char *code = sw_code_field_get_code (f->code);

  if (strlen (code) != CODE_LENGTH)
    {
      sw_code_field_set_error (f->code, TRUE);
      return;
    }
  g_free (f->checked_code);
  f->checked_code = g_steal_pointer (&code);
  gtk_widget_set_visible (f->error, FALSE);
  gtk_widget_set_sensitive (f->button, FALSE);
  sw_account_check_code (sw_account_get_default (), f->address, f->checked_code, sw_cancellable_for (f->page),
                         on_code_checked, f);
}

static void
on_forgot_code_complete (GtkWidget *code, const char *text, Forgot *f)
{
  on_check_code (NULL, f);
}

static void
on_reset (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Forgot *f = data;

  gtk_widget_set_sensitive (f->button, TRUE);
  if (error)
    {
      if (!sw_fields_show_errors (error, "password", f->password, NULL))
        show_error (f->error, error);
      /* the code died meanwhile: back to asking for one */
      if (!g_strcmp0 (error->code, "code_expired") || !g_strcmp0 (error->code, "code_attempts_exceeded"))
        forgot_step (f, 1);
      return;
    }
  sw_window_toast (f->page, "Your password is changed, and you are signed in");
  sw_window_pop_account_flow (f->page);
}

static void
on_set_password (GtkWidget *button, Forgot *f)
{
  gtk_widget_set_visible (f->error, FALSE);
  gtk_widget_set_sensitive (f->button, FALSE);
  sw_account_reset (sw_account_get_default (), f->address, f->checked_code, sw_field_get_text (f->password),
                    sw_cancellable_for (f->page), on_reset, f);
}

static void
forgot_step (Forgot *f, int step)
{
  GtkWidget *child, *column = f->column;
  g_autofree char *masked = NULL, *text = NULL;
  g_autofree char *keep_error = NULL;

  if (step == 1 && gtk_widget_get_visible (f->error))
    keep_error = g_strdup (gtk_label_get_text (GTK_LABEL (f->error)));
  f->step = step;
  while ((child = gtk_widget_get_first_child (column)))
    gtk_box_remove (GTK_BOX (column), child);
  f->email = f->code = f->password = f->resend = NULL;
  switch (step)
    {
    case 0:
      gtk_box_append (GTK_BOX (column),
                      sw_label_new ("Enter the address you signed up with. If it has an account, the store emails a "
                                    "code to it to set a new password.", "sw-body-text"));
      f->email = sw_field_new ("Email", SW_FIELD_EMAIL);
      sw_field_set_text (f->email, f->address);
      gtk_box_append (GTK_BOX (column), f->email);
      f->error = form_error (column);
      f->button = big_button (column, "Send the code", SW_BUTTON_FILLED, G_CALLBACK (on_forgot_send), f);
      submit_on_enter (f->email, f->button);
      break;
    case 1:
      masked = sw_mask_email (f->address);
      text = g_strdup_printf ("If %s has an account, a %d-digit code is on its way to it. Enter it here.", masked,
                              CODE_LENGTH);
      gtk_box_append (GTK_BOX (column), sw_label_new (text, "sw-body-text"));
      f->code = sw_code_field_new (CODE_LENGTH);
      g_signal_connect (f->code, "complete", G_CALLBACK (on_forgot_code_complete), f);
      gtk_box_append (GTK_BOX (column), f->code);
      f->error = form_error (column);
      if (keep_error)
        {
          gtk_label_set_text (GTK_LABEL (f->error), keep_error);
          gtk_widget_set_visible (f->error, TRUE);
        }
      f->button = big_button (column, "Next", SW_BUTTON_FILLED, G_CALLBACK (on_check_code), f);
      f->resend = sw_button_new ("Send a new code", SW_BUTTON_TEXT);
      gtk_widget_set_halign (f->resend, GTK_ALIGN_CENTER);
      g_signal_connect (f->resend, "clicked", G_CALLBACK (on_forgot_send), f);
      gtk_box_append (GTK_BOX (column), f->resend);
      f->cooldown.button = f->resend;
      gtk_widget_grab_focus (f->code);
      break;
    default:
      gtk_box_append (GTK_BOX (column),
                      sw_label_new ("Choose a new password. Every device signed in to the account is signed out, "
                                    "and this one signed in.", "sw-body-text"));
      f->password = sw_field_new ("New password", SW_FIELD_NEW_PASSWORD);
      sw_field_set_hint (f->password, "At least 8 characters");
      gtk_box_append (GTK_BOX (column), f->password);
      f->error = form_error (column);
      f->button = big_button (column, "Set the password", SW_BUTTON_FILLED, G_CALLBACK (on_set_password), f);
      submit_on_enter (f->password, f->button);
      sw_field_grab_focus (f->password);
      break;
    }
}

GtkWidget *
sw_forgot_page_new (const char *email)
{
  Forgot *f = g_new0 (Forgot, 1);

  f->address = g_strdup (email);
  f->page = flow_page ("New password");
  g_object_set_data_full (G_OBJECT (f->page), "sw-forgot", f, forgot_free);
  f->column = form (f->page);
  forgot_step (f, 0);
  return f->page;
}

/* ---- settings -------------------------------------------------------------------------------------------------- */

static const struct {
  const char *id, *label, *summary;
} update_modes[] = {
  { "wifi", "On Wi-Fi only", "Not on a network that counts the data (a phone's mobile data)" },
  { "always", "On any network", NULL },
  { "never", "Only when I ask", "Mine's Check for updates" },
};

static const struct {
  const char *id, *label;
} themes[] = {
  { "system", "As the system" },
  { "light", "Light" },
  { "dark", "Dark" },
};

static const char *
update_mode_label (GSettings *settings)
{
  g_autofree char *mode = settings ? g_settings_get_string (settings, "update-mode") : NULL;

  for (guint i = 0; i < G_N_ELEMENTS (update_modes); i++)
    if (!g_strcmp0 (mode, update_modes[i].id))
      return update_modes[i].label;
  return update_modes[0].label;
}

static const char *
theme_label (GSettings *settings)
{
  g_autofree char *theme = settings ? g_settings_get_string (settings, "color-scheme") : NULL;

  for (guint i = 0; i < G_N_ELEMENTS (themes); i++)
    if (!g_strcmp0 (theme, themes[i].id))
      return themes[i].label;
  return themes[0].label;
}

typedef struct {
  GtkWidget *sheet;
  GSettings *settings;
  const char *key;
} Choice;

static void
choice_free (gpointer data)
{
  Choice *c = data;

  g_clear_object (&c->settings);
  g_free (c);
}

static void
on_choice (VibeosListRow *row, Choice *c)
{
  g_settings_set_string (c->settings, c->key, g_object_get_data (G_OBJECT (row), "sw-id"));
  vibeos_sheet_close (VIBEOS_SHEET (c->sheet));
}

/* a sheet of choices for a string setting, the current one ticked */
static void
choose (GtkWidget *from, const char *title, const char *text, const char *key, guint n, const char *const *ids,
        const char *const *labels, const char *const *summaries)
{
  Choice *c = g_new0 (Choice, 1);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12), *cards = vibeos_card_list_new ();
  g_autofree char *now = NULL;

  c->settings = settings_new ();
  if (!c->settings)
    {
      g_free (c);
      sw_window_toast (from, "Software's settings are not installed.");
      return;
    }
  c->key = key;
  now = g_settings_get_string (c->settings, key);
  c->sheet = vibeos_sheet_new (title);
  g_object_set_data_full (G_OBJECT (c->sheet), "sw-choice", c, choice_free);
  gtk_widget_add_css_class (column, "sw-sheet-column");
  if (text)
    gtk_box_append (GTK_BOX (column), sw_label_new (text, "sw-body-text"));
  for (guint i = 0; i < n; i++)
    {
      GtkWidget *row = vibeos_list_row_new (labels[i], summaries ? summaries[i] : NULL);

      if (!g_strcmp0 (now, ids[i]))
        vibeos_list_row_set_end_icon (VIBEOS_LIST_ROW (row), "sw-check-symbolic");
      g_object_set_data (G_OBJECT (row), "sw-id", (gpointer) ids[i]);
      g_signal_connect (row, "clicked", G_CALLBACK (on_choice), c);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
    }
  gtk_box_append (GTK_BOX (column), cards);
  vibeos_sheet_set_content (VIBEOS_SHEET (c->sheet), column);
  vibeos_sheet_present (VIBEOS_SHEET (c->sheet), from);
}

static void
on_updates_row (VibeosListRow *row, gpointer data)
{
  const char *ids[G_N_ELEMENTS (update_modes)], *labels[G_N_ELEMENTS (update_modes)];
  const char *summaries[G_N_ELEMENTS (update_modes)];

  for (guint i = 0; i < G_N_ELEMENTS (update_modes); i++)
    {
      ids[i] = update_modes[i].id;
      labels[i] = update_modes[i].label;
      summaries[i] = update_modes[i].summary;
    }
  choose (GTK_WIDGET (row), "Look for updates",
          "Software looks for updates when it starts and when apps change. It does not install them by itself: "
          "update from Mine.", "update-mode", G_N_ELEMENTS (update_modes), ids, labels, summaries);
}

static void
on_theme_row (VibeosListRow *row, gpointer data)
{
  const char *ids[G_N_ELEMENTS (themes)], *labels[G_N_ELEMENTS (themes)];

  for (guint i = 0; i < G_N_ELEMENTS (themes); i++)
    {
      ids[i] = themes[i].id;
      labels[i] = themes[i].label;
    }
  choose (GTK_WIDGET (row), "Theme", NULL, "color-scheme", G_N_ELEMENTS (themes), ids, labels, NULL);
}

static void
on_about_row (VibeosListRow *row, gpointer data)
{
  sw_about_present (GTK_WIDGET (row));
}

static void
on_setting_changed (GSettings *settings, const char *key, GtkWidget *row)
{
  if (!g_strcmp0 (key, "update-mode") && g_object_get_data (G_OBJECT (row), "sw-updates-row"))
    vibeos_list_row_set_value (VIBEOS_LIST_ROW (row), update_mode_label (settings));
  else if (!g_strcmp0 (key, "color-scheme") && g_object_get_data (G_OBJECT (row), "sw-theme-row"))
    vibeos_list_row_set_value (VIBEOS_LIST_ROW (row), theme_label (settings));
}

/* the settings' rows into `cards`, following the settings while `owner` lives */
static void
settings_rows (GtkWidget *cards, GtkWidget *owner)
{
  GSettings *settings = settings_new ();
  GtkWidget *row, *toggle;

  if (settings)
    g_object_set_data_full (G_OBJECT (owner), "sw-settings", settings, g_object_unref);
  row = vibeos_list_row_new ("Look for updates", NULL);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-update-symbolic");
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (row), update_mode_label (settings));
  g_object_set_data (G_OBJECT (row), "sw-updates-row", GINT_TO_POINTER (1));
  g_signal_connect (row, "clicked", G_CALLBACK (on_updates_row), NULL);
  if (settings)
    g_signal_connect_object (settings, "changed", G_CALLBACK (on_setting_changed), row, 0);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  row = vibeos_list_row_new ("VibeOS apps only", "None of Debian's in the store's lists");
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-apps-symbolic");
  toggle = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);
  if (settings)
    g_settings_bind (settings, "vibeos-only", toggle, "active", G_SETTINGS_BIND_DEFAULT);
  else
    gtk_widget_set_sensitive (row, FALSE);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  row = vibeos_list_row_new ("Theme", NULL);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-palette-symbolic");
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (row), theme_label (settings));
  g_object_set_data (G_OBJECT (row), "sw-theme-row", GINT_TO_POINTER (1));
  g_signal_connect (row, "clicked", G_CALLBACK (on_theme_row), NULL);
  if (settings)
    g_signal_connect_object (settings, "changed", G_CALLBACK (on_setting_changed), row, 0);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  row = vibeos_list_row_new ("About Software", NULL);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-info-symbolic");
  g_signal_connect (row, "clicked", G_CALLBACK (on_about_row), NULL);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
}

GtkWidget *
sw_settings_page_new (void)
{
  GtkWidget *page = sw_page_new ("Settings"), *cards = vibeos_card_list_new ();

  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 8, 24);
  settings_rows (cards, page);
  sw_page_set_body (page, sw_scroll_new (cards));
  return page;
}

void
sw_about_present (GtkWidget *from)
{
  static const VibeosAboutSection credits[] = {
    { "The catalog", "The apps, their descriptions and pictures come from the store at vibeos.top, which takes them "
                     "from Debian's and VibeOS's software sources and the apps' own AppStream data." },
    { NULL, NULL },
  };
  static const VibeosAboutSection legal[] = {
    { "Font Awesome", "The category icons are Font Awesome Free 7.0.0 by Fonticons, Inc. (solid style, unchanged), "
                      "under the <a href=\"https://creativecommons.org/licenses/by/4.0/\">Creative Commons Attribution "
                      "4.0</a> licence. <a href=\"https://fontawesome.com\">fontawesome.com</a>" },
    { NULL, NULL },
  };
  VibeosAboutInfo info = { "Software", "vibeos.Software", credits, legal };

  vibeos_about_present (from, &info);
}

/* ---- the account ----------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *page, *column;
} Profile;

static void profile_fill (Profile *p);

static void
on_profile_sign_in (GtkWidget *button, Profile *p)
{
  sw_window_push (button, sw_sign_in_page_new ());
}

static void
on_profile_register (GtkWidget *button, Profile *p)
{
  sw_window_push (button, sw_register_page_new ());
}

static void
on_confirm_row (VibeosListRow *row, Profile *p)
{
  sw_window_push (GTK_WIDGET (row), sw_verify_page_new (FALSE));
}

static void
on_reviews_row (VibeosListRow *row, Profile *p)
{
  sw_window_push (GTK_WIDGET (row), sw_my_reviews_page_new ());
}

static void
on_signed_out (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Profile *p = data;

  sw_window_toast (p->page, "You are signed out");
}

static void
on_sign_out_response (VibeosDialog *dialog, int response, Profile *p)
{
  if (response == VIBEOS_RESPONSE_OK)
    sw_account_sign_out (sw_account_get_default (), sw_cancellable_for (p->page), on_signed_out, p);
}

static void
on_sign_out_row (VibeosListRow *row, Profile *p)
{
  VibeosDialog *dialog = vibeos_dialog_new ("Sign out?");

  vibeos_dialog_set_message (dialog, "Your reviews and your apps stay. Sign in again to write reviews.");
  vibeos_dialog_add_button (dialog, "Cancel", VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, "Sign out", VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_sign_out_response), p);
  vibeos_dialog_present (dialog, p->page);
}

static void
on_delete_row (VibeosListRow *row, Profile *p)
{
  sw_window_push (GTK_WIDGET (row), sw_delete_account_page_new ());
}

static void
profile_fill (Profile *p)
{
  SwAccount *account = sw_account_get_default ();
  GtkWidget *child, *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8), *cards = vibeos_card_list_new (), *row;

  while ((child = gtk_widget_get_first_child (p->column)))
    gtk_box_remove (GTK_BOX (p->column), child);
  gtk_widget_add_css_class (card, "sw-profile-card");
  gtk_box_append (GTK_BOX (card), sw_avatar_new (72));
  if (sw_account_is_signed_in (account))
    {
      const char *name = sw_account_get_name (account), *email = sw_account_get_email (account);
      GtkWidget *label = gtk_label_new (name ? name : "Signed in");

      gtk_widget_add_css_class (label, "sw-profile-name");
      gtk_box_append (GTK_BOX (card), label);
      if (email)
        {
          label = gtk_label_new (email);
          gtk_widget_add_css_class (label, "sw-profile-email");
          gtk_box_append (GTK_BOX (card), label);
        }
      gtk_box_append (GTK_BOX (p->column), card);
      if (!sw_account_is_verified (account))
        {
          row = vibeos_list_row_new ("Confirm your email address", "Needed to rate and review apps");
          vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-email-symbolic");
          vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
          g_signal_connect (row, "clicked", G_CALLBACK (on_confirm_row), p);
          vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
        }
      row = vibeos_list_row_new ("My reviews", NULL);
      vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-star-outline-symbolic");
      vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
      g_signal_connect (row, "clicked", G_CALLBACK (on_reviews_row), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), "Settings");
      settings_rows (cards, p->page);
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), NULL);
      row = vibeos_list_row_new ("Sign out", NULL);
      vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-logout-symbolic");
      g_signal_connect (row, "clicked", G_CALLBACK (on_sign_out_row), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
      row = vibeos_list_row_new ("Delete the account", NULL);
      vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-delete-symbolic");
      g_signal_connect (row, "clicked", G_CALLBACK (on_delete_row), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
    }
  else
    {
      GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12), *button;

      gtk_box_append (GTK_BOX (card),
                      sw_label_new ("Sign in to rate and review apps, and to see your apps on all your devices.",
                                    "sw-profile-text"));
      gtk_widget_set_halign (buttons, GTK_ALIGN_CENTER);
      button = sw_button_new ("Sign in", SW_BUTTON_FILLED);
      g_signal_connect (button, "clicked", G_CALLBACK (on_profile_sign_in), p);
      gtk_box_append (GTK_BOX (buttons), button);
      button = sw_button_new ("Create an account", SW_BUTTON_TONAL);
      g_signal_connect (button, "clicked", G_CALLBACK (on_profile_register), p);
      gtk_box_append (GTK_BOX (buttons), button);
      gtk_box_append (GTK_BOX (card), buttons);
      gtk_box_append (GTK_BOX (p->column), card);
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), "Settings");
      settings_rows (cards, p->page);
    }
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 8, 24);
  gtk_box_append (GTK_BOX (p->column), cards);
}

static void
on_profile_account (SwAccount *account, GtkWidget *page)
{
  profile_fill (g_object_get_data (G_OBJECT (page), "sw-profile"));
}

GtkWidget *
sw_profile_page_new (void)
{
  Profile *p = g_new0 (Profile, 1);

  p->page = sw_page_new ("Account and settings");
  g_object_set_data_full (G_OBJECT (p->page), "sw-profile", p, g_free);
  p->column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  sw_page_set_body (p->page, sw_scroll_new (p->column));
  g_signal_connect_object (sw_account_get_default (), "changed", G_CALLBACK (on_profile_account), p->page, 0);
  profile_fill (p);
  /* what the account looks like now */
  sw_account_refresh (sw_account_get_default ());
  return p->page;
}

/* ---- deleting the account -------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *page, *password, *reviews, *button, *error;
} Delete;

static void
on_deleted (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Delete *d = data;

  gtk_widget_set_sensitive (d->button, TRUE);
  if (error)
    {
      if (!g_strcmp0 (error->code, "invalid_credentials"))
        sw_field_set_error (d->password, error->message);
      else
        show_error (d->error, error);
      return;
    }
  sw_window_toast (d->page, "Your account is deleted");
  sw_window_pop_to_tabs (d->page);
}

static void
on_delete (GtkWidget *button, Delete *d)
{
  if (!*sw_field_get_text (d->password))
    {
      sw_field_set_error (d->password, "Enter your password to delete the account.");
      return;
    }
  gtk_widget_set_visible (d->error, FALSE);
  gtk_widget_set_sensitive (d->button, FALSE);
  sw_account_delete (sw_account_get_default (), sw_field_get_text (d->password),
                     vibeos_switch_get_active (VIBEOS_SWITCH (d->reviews)), sw_cancellable_for (d->page), on_deleted,
                     d);
}

GtkWidget *
sw_delete_account_page_new (void)
{
  Delete *d = g_new0 (Delete, 1);
  GtkWidget *column, *cards = vibeos_card_list_new (), *row;

  d->page = sw_page_new ("Delete the account");
  g_object_set_data_full (G_OBJECT (d->page), "sw-delete", d, g_free);
  column = form (d->page);
  gtk_box_append (GTK_BOX (column),
                  sw_label_new ("The store deletes the account, its devices, the list of apps it had and its helpful "
                                "votes. It cannot be undone. Your apps stay installed.", "sw-body-text"));
  row = vibeos_list_row_new ("Delete my reviews too",
                             "Otherwise they stay, under \"Deleted account\", and nobody can change them");
  d->reviews = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  gtk_box_append (GTK_BOX (column), cards);
  d->password = sw_field_new ("Password", SW_FIELD_PASSWORD);
  gtk_box_append (GTK_BOX (column), d->password);
  d->error = form_error (column);
  d->button = big_button (column, "Delete the account", SW_BUTTON_DANGER, G_CALLBACK (on_delete), d);
  return d->page;
}

/* ---- my reviews ------------------------------------------------------------------------------------------------ */

typedef struct {
  GtkWidget *page, *status, *rows, *more;
  char      *cursor;
} Mine;

static void my_reviews_load (Mine *m);

static void
my_reviews_free (gpointer data)
{
  Mine *m = data;

  g_free (m->cursor);
  g_free (m);
}

static void
on_my_reviews (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Mine *m = data;
  JsonArray *list;
  gboolean first = !m->cursor;

  if (error)
    {
      sw_status_error (SW_STATUS (m->status), error, (SwRetryFunc) my_reviews_load, m);
      return;
    }
  list = sw_json_node_array (result->data);
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *entry = sw_json_array_object (list, i), *app = sw_json_object (entry, "app");
      JsonObject *review = sw_json_object (entry, "review");

      if (!review)
        continue;
      if (app)
        gtk_box_append (GTK_BOX (m->rows), sw_app_row_new (app, NULL));
      if (sw_json_bool (entry, "hidden", FALSE))
        gtk_box_append (GTK_BOX (m->rows),
                        sw_label_new ("A moderator hid this review: only you see it.", "sw-caption-padded"));
      gtk_box_append (GTK_BOX (m->rows), sw_review_card_new (review, sw_json_string (app, "id"),
                                                             sw_json_string (app, "name")));
    }
  g_free (m->cursor);
  m->cursor = g_strdup (result->next_cursor);
  gtk_widget_set_visible (m->more, m->cursor != NULL);
  if (first && !gtk_widget_get_first_child (m->rows))
    sw_status_empty (SW_STATUS (m->status), "sw-star-outline-symbolic",
                     "You have not reviewed any apps yet. An app's page has its stars to tap.", NULL, NULL, NULL);
  else
    sw_status_show (SW_STATUS (m->status));
}

static void
my_reviews_load (Mine *m)
{
  g_autofree char *path = sw_api_path_with_cursor ("/me/reviews", m->cursor);

  if (!m->cursor)
    sw_status_loading (SW_STATUS (m->status));
  sw_api_get (sw_api_get_default (), path, SW_API_NONE, sw_cancellable_for (m->page), on_my_reviews, m);
}

static void
on_more_reviews (GtkWidget *button, Mine *m)
{
  my_reviews_load (m);
}

GtkWidget *
sw_my_reviews_page_new (void)
{
  Mine *m = g_new0 (Mine, 1);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  m->page = sw_page_new ("My reviews");
  g_object_set_data_full (G_OBJECT (m->page), "sw-my-reviews", m, my_reviews_free);
  m->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (m->rows, "sw-reviews");
  gtk_box_append (GTK_BOX (column), m->rows);
  m->more = sw_button_new ("Show more", SW_BUTTON_TEXT);
  gtk_widget_set_halign (m->more, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (m->more, FALSE);
  g_signal_connect (m->more, "clicked", G_CALLBACK (on_more_reviews), m);
  gtk_box_append (GTK_BOX (column), m->more);
  m->status = sw_status_new (sw_scroll_new (column));
  sw_page_set_body (m->page, m->status);
  my_reviews_load (m);
  return m->page;
}
