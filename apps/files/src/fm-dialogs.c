/* The dialogs (fm-dialogs.h). */
#include "fm-dialogs.h"
#include "fm-icon.h"
#include "fm-item.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <string.h>
#include <spongy-gtk.h>

VibeosDialog *
fm_dialog_new (const char *title, GtkWidget **content)
{
  VibeosDialog *dialog = vibeos_dialog_new (NULL);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (box, "fm-dialog-content");
  if (title)
    {
      /* the title, over the content */
      GtkWidget *label = fm_label (title, "fm-dialog-title", 20, FALSE);

      gtk_box_append (GTK_BOX (box), label);
    }
  else
    gtk_widget_add_css_class (box, "untitled");
  vibeos_dialog_set_content (dialog, box);
  *content = box;
  return dialog;
}

GtkWidget *
fm_dialog_add_button (VibeosDialog *dialog, const char *label, int response)
{
  g_autofree char *caps = fm_caps (label);

  return vibeos_dialog_add_button (dialog, caps, response);
}

GtkWidget *
fm_entry_new (const char *text)
{
  GtkWidget *entry = gtk_entry_new ();

  gtk_widget_add_css_class (entry, "vibeos-entry");
  gtk_widget_add_css_class (entry, "fm-entry");
  if (text)
    gtk_editable_set_text (GTK_EDITABLE (entry), text);
  gtk_widget_set_hexpand (entry, TRUE);
  return entry;
}

GtkWidget *
fm_form_row (const char *label, GtkWidget *field)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = fm_label (label, "fm-form-label", 16, TRUE);

  gtk_widget_add_css_class (row, "fm-form-row");
  /* the label's share of the row (weight 3 of 10), as a fixed width */
  gtk_widget_set_size_request (text, 79, -1);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (field, TRUE);
  gtk_box_append (GTK_BOX (row), text);
  gtk_box_append (GTK_BOX (row), field);
  return row;
}

/* ---- the name field --------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *entry, *error, *ok;
  FmTextCheck check;
  FmTextDone done;
  gpointer data;
  int select_end;
} TextDialog;

static void
text_changed (GtkEditable *entry, TextDialog *text)
{
  const char *value = gtk_editable_get_text (entry);
  const char *message = text->check ? text->check (value, text->data) : NULL;

  gtk_label_set_text (GTK_LABEL (text->error), message ? message : "");
  gtk_widget_set_visible (text->error, message != NULL);
  gtk_widget_set_sensitive (text->ok, message == NULL);
}

static void
on_text_response (VibeosDialog *dialog, int response, TextDialog *text)
{
  g_autofree char *value = g_strdup (gtk_editable_get_text (GTK_EDITABLE (text->entry)));

  text->done (response == VIBEOS_RESPONSE_OK ? value : NULL, text->data);
}

static void
on_text_activate (GtkEntry *entry, VibeosDialog *dialog)
{
  TextDialog *text = g_object_get_data (G_OBJECT (dialog), "fm-text");

  if (gtk_widget_get_sensitive (text->ok))
    vibeos_dialog_respond (dialog, VIBEOS_RESPONSE_OK);
}

static gboolean
select_stem (gpointer data)
{
  TextDialog *text = data;

  gtk_widget_grab_focus (text->entry);
  gtk_editable_select_region (GTK_EDITABLE (text->entry), 0, text->select_end);
  return G_SOURCE_REMOVE;
}

void
fm_text_dialog (GtkWidget *parent, const char *title, const char *value, int select_end, FmTextCheck check,
                FmTextDone done, gpointer data)
{
  fm_text_dialog_suffix (parent, title, value, select_end, NULL, check, done, data);
}

void
fm_text_dialog_suffix (GtkWidget *parent, const char *title, const char *value, int select_end, const char *suffix,
                       FmTextCheck check, FmTextDone done, gpointer data)
{
  GtkWidget *content;
  VibeosDialog *dialog = fm_dialog_new (title, &content);
  TextDialog *text = g_new0 (TextDialog, 1);

  text->check = check;
  text->done = done;
  text->data = data;
  text->select_end = select_end;
  text->entry = fm_entry_new (value);
  gtk_widget_add_css_class (text->entry, "fm-name-entry");
  text->error = fm_label (NULL, "fm-field-error", 12, TRUE);
  gtk_widget_set_visible (text->error, FALSE);
  if (suffix)
    {
      /* Compress: the field and ".zip" after it, on its line */
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *label = fm_label (suffix, "fm-field-suffix", 16, FALSE);

      gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (row), text->entry);
      gtk_box_append (GTK_BOX (row), label);
      gtk_box_append (GTK_BOX (content), row);
    }
  else
    gtk_box_append (GTK_BOX (content), text->entry);
  gtk_box_append (GTK_BOX (content), text->error);
  fm_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  text->ok = fm_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "fm-text", text, g_free);
  g_signal_connect (text->entry, "changed", G_CALLBACK (text_changed), text);
  g_signal_connect (text->entry, "activate", G_CALLBACK (on_text_activate), dialog);
  g_signal_connect (dialog, "response", G_CALLBACK (on_text_response), text);
  text_changed (GTK_EDITABLE (text->entry), text);
  vibeos_dialog_present (dialog, parent);
  /* after the dialog's own focus, which selects all */
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, select_stem, text, NULL);
}

/* ---- Batch Rename ----------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *entry, *extension, *ok, *tokens, *more;
  GPtrArray *labels;            /* the list's, one a name */
  GStrv names;                  /* the names as they are */
  FmBatchNames preview;
  FmBatchDone done;
  gpointer data;
} BatchDialog;

static void
batch_free (BatchDialog *batch)
{
  g_ptr_array_unref (batch->labels);
  g_strfreev (batch->names);
  g_free (batch);
}

/* OK while the names can be made; the list shows them then, else the names as they are */
static void
batch_changed (GtkEditable *editable, BatchDialog *batch)
{
  const char *pattern = gtk_editable_get_text (GTK_EDITABLE (batch->entry));
  const char *extension = gtk_editable_get_text (GTK_EDITABLE (batch->extension));
  gboolean valid = *pattern && !strchr (pattern, '/') && !strchr (extension, '/');
  g_auto(GStrv) renamed = valid && batch->preview ? batch->preview (pattern, extension, batch->data) : NULL;

  gtk_widget_set_sensitive (batch->ok, valid);
  for (guint i = 0; i < batch->labels->len; i++)
    gtk_label_set_text (batch->labels->pdata[i], renamed ? renamed[i] : batch->names[i]);
}

/* the token at the name field's cursor, in place of what is selected there */
static void
batch_insert (BatchDialog *batch, const char *token)
{
  GtkEditable *entry = GTK_EDITABLE (batch->entry);
  int position;

  gtk_editable_delete_selection (entry);
  position = gtk_editable_get_position (entry);
  gtk_editable_insert_text (entry, token, -1, &position);
  gtk_entry_grab_focus_without_selecting (GTK_ENTRY (batch->entry));
  gtk_editable_set_position (entry, position);
}

static void
on_batch_name (VibeosPressable *button, BatchDialog *batch)
{
  batch_insert (batch, FM_BATCH_NAME);
}

static void
on_batch_number (VibeosPressable *button, BatchDialog *batch)
{
  batch_insert (batch, "(" FM_BATCH_NUMBER ")");
}

static void
on_batch_more (GtkWidget *button, BatchDialog *batch)
{
  gtk_widget_set_visible (batch->more, FALSE);
  gtk_widget_set_visible (batch->tokens, TRUE);
}

static void
on_batch_response (VibeosDialog *dialog, int response, BatchDialog *batch)
{
  g_autofree char *pattern = g_strdup (gtk_editable_get_text (GTK_EDITABLE (batch->entry)));
  g_autofree char *extension = g_strdup (gtk_editable_get_text (GTK_EDITABLE (batch->extension)));

  /* done frees the data the preview reads */
  batch->preview = NULL;
  if (response == VIBEOS_RESPONSE_OK)
    batch->done (pattern, extension, batch->data);
  else
    batch->done (NULL, NULL, batch->data);
}

static void
on_batch_activate (GtkEntry *entry, VibeosDialog *dialog)
{
  BatchDialog *batch = g_object_get_data (G_OBJECT (dialog), "fm-batch");

  if (gtk_widget_get_sensitive (batch->ok))
    vibeos_dialog_respond (dialog, VIBEOS_RESPONSE_OK);
}

static GtkWidget *
batch_token (const char *icon, const char *name, GCallback clicked, BatchDialog *batch)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon);
  GtkWidget *button;

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  button = vibeos_pressable_new (image);
  gtk_widget_add_css_class (button, "fm-batch-token");
  gtk_widget_set_hexpand (button, TRUE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  gtk_widget_set_tooltip_text (button, name);
  g_signal_connect (button, "clicked", clicked, batch);
  return button;
}

void
fm_batch_rename_dialog (GtkWidget *parent, const char *const *names, guint n_names, const char *extension,
                        FmBatchNames preview, FmBatchDone done, gpointer data)
{
  GtkWidget *content, *list, *scroller, *row, *dot;
  VibeosDialog *dialog = fm_dialog_new (_("Batch Rename"), &content);
  BatchDialog *batch = g_new0 (BatchDialog, 1);
  g_autofree char *more = NULL;

  batch->preview = preview;
  batch->done = done;
  batch->data = data;
  batch->labels = g_ptr_array_new ();
  batch->names = g_strdupv ((char **) names);

  /* the names, in a box of their own that scrolls past four */
  list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  for (guint i = 0; i < n_names; i++)
    {
      GtkWidget *label = fm_label (names[i], "fm-batch-name", 16, FALSE);

      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
      gtk_box_append (GTK_BOX (list), label);
      g_ptr_array_add (batch->labels, label);
    }
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), 4 * 31);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), list);
  gtk_widget_add_css_class (scroller, "fm-batch-names");
  gtk_box_append (GTK_BOX (content), scroller);

  /* MORE shows these: the old name (%%) and the number (##) at the cursor */
  batch->tokens = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (batch->tokens, "fm-batch-tokens");
  gtk_box_append (GTK_BOX (batch->tokens), batch_token ("edit-copy-symbolic", _("Name"),
                                                        G_CALLBACK (on_batch_name), batch));
  gtk_box_append (GTK_BOX (batch->tokens), batch_token ("view-list-ordered-symbolic", _("Number"),
                                                        G_CALLBACK (on_batch_number), batch));
  gtk_widget_set_visible (batch->tokens, FALSE);
  gtk_box_append (GTK_BOX (content), batch->tokens);

  /* the new name, "." and the extension */
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (row, "fm-batch-row");
  batch->entry = fm_entry_new (NULL);
  batch->extension = fm_entry_new (extension);
  gtk_widget_set_hexpand (batch->extension, FALSE);
  gtk_editable_set_width_chars (GTK_EDITABLE (batch->extension), 4);
  gtk_widget_set_size_request (batch->extension, 64, -1);
  dot = fm_label (".", "fm-field-suffix", 16, FALSE);
  gtk_widget_set_valign (dot, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (row), batch->entry);
  gtk_box_append (GTK_BOX (row), dot);
  gtk_box_append (GTK_BOX (row), batch->extension);
  gtk_box_append (GTK_BOX (content), row);

  more = fm_caps (_("More"));
  batch->more = vibeos_dialog_add_neutral_button (dialog, more);
  g_signal_connect (batch->more, "clicked", G_CALLBACK (on_batch_more), batch);
  fm_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  batch->ok = fm_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "fm-batch", batch, (GDestroyNotify) batch_free);
  g_signal_connect (batch->entry, "changed", G_CALLBACK (batch_changed), batch);
  g_signal_connect (batch->extension, "changed", G_CALLBACK (batch_changed), batch);
  g_signal_connect (batch->entry, "activate", G_CALLBACK (on_batch_activate), dialog);
  g_signal_connect (batch->extension, "activate", G_CALLBACK (on_batch_activate), dialog);
  g_signal_connect (dialog, "response", G_CALLBACK (on_batch_response), batch);
  batch_changed (NULL, batch);
  vibeos_dialog_present (dialog, parent);
  gtk_widget_grab_focus (batch->entry);
}

char *
fm_batch_name (const char *pattern, const char *stem, guint number)
{
  g_autofree char *count = g_strdup_printf ("%u", number);
  g_autoptr(GString) name = g_string_new (pattern);

  /* without either token every name would be the same: the number goes at the end */
  if (!strstr (pattern, FM_BATCH_NAME) && !strstr (pattern, FM_BATCH_NUMBER))
    g_string_append (name, " (" FM_BATCH_NUMBER ")");
  /* the number first: a name may have "##" in it, a number never has "%%" */
  g_string_replace (name, FM_BATCH_NUMBER, count, 0);
  g_string_replace (name, FM_BATCH_NAME, stem, 0);
  return g_string_free_and_steal (g_steal_pointer (&name));
}

/* ---- a question ------------------------------------------------------------------------------------------------ */

typedef struct {
  FmConfirmDone done;
  gpointer data;
} Confirm;

static void
on_confirm_response (VibeosDialog *dialog, int response, Confirm *confirm)
{
  confirm->done (response == VIBEOS_RESPONSE_OK, confirm->data);
}

void
fm_confirm_dialog (GtkWidget *parent, const char *title, const char *message, FmConfirmDone done, gpointer data)
{
  GtkWidget *content;
  VibeosDialog *dialog = fm_dialog_new (title, &content);
  Confirm *confirm = g_new0 (Confirm, 1);

  confirm->done = done;
  confirm->data = data;
  gtk_box_append (GTK_BOX (content), fm_label (message, "fm-dialog-message", 16, TRUE));
  fm_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  fm_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "fm-confirm", confirm, g_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_confirm_response), confirm);
  vibeos_dialog_present (dialog, parent);
}

/* ---- a list to pick from ---------------------------------------------------------------------------------------- */

typedef struct {
  FmChosen chosen;
  gpointer data;
  int index;
} ListDialog;

static void
on_list_row (VibeosPressable *row, VibeosDialog *dialog)
{
  ListDialog *list = g_object_get_data (G_OBJECT (dialog), "fm-list");

  list->index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "fm-index"));
  vibeos_dialog_respond (dialog, VIBEOS_RESPONSE_OK);
}

static void
on_list_response (VibeosDialog *dialog, int response, ListDialog *list)
{
  list->chosen (response == VIBEOS_RESPONSE_OK ? list->index : -1, list->data);
}

void
fm_list_dialog (GtkWidget *parent, const char *title, const FmListRow *rows, int n_rows, FmChosen chosen,
                gpointer data)
{
  GtkWidget *content;
  VibeosDialog *dialog = fm_dialog_new (title, &content);
  ListDialog *list = g_new0 (ListDialog, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();

  list->chosen = chosen;
  list->data = data;
  list->index = -1;
  gtk_widget_add_css_class (content, "fm-list-dialog");
  for (int i = 0; i < n_rows; i++)
    {
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *icon;
      GtkWidget *label = fm_label (rows[i].label, "fm-list-dialog-text", 16, FALSE);
      GtkWidget *row;

      if (rows[i].icon)
        {
          icon = gtk_image_new_from_gicon (rows[i].icon);
          gtk_image_set_pixel_size (GTK_IMAGE (icon), 32);
        }
      else
        icon = fm_icon_new (rows[i].glyph, 32);
      gtk_widget_add_css_class (icon, "fm-list-dialog-icon");
      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_box_append (GTK_BOX (line), icon);
      gtk_box_append (GTK_BOX (line), label);
      row = vibeos_pressable_new (line);
      gtk_widget_add_css_class (row, "fm-list-dialog-row");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "fm-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_list_row), dialog);
      gtk_box_append (GTK_BOX (box), row);
    }
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), box);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (content), scroll);
  g_object_set_data_full (G_OBJECT (dialog), "fm-list", list, g_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_list_response), list);
  vibeos_dialog_present (dialog, parent);
}

/* ---- counting in a worker (Properties, Delete) ------------------------------------------------------------------- */

typedef void (*Counted) (const FmCount *count, gpointer data);

typedef struct {
  GPtrArray *files;             /* GFile */
  FmCount count;
} CountJob;

static void
count_job_free (gpointer data)
{
  CountJob *job = data;

  g_ptr_array_unref (job->files);
  g_free (job);
}

static void
count_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  CountJob *job = data;

  for (guint i = 0; i < job->files->len && !g_cancellable_is_cancelled (cancellable); i++)
    fm_count_tree (job->files->pdata[i], cancellable, &job->count);
  g_task_return_boolean (task, !g_cancellable_is_cancelled (cancellable));
}

typedef struct {
  Counted counted;
  gpointer data;
} CountReply;

static void
on_counted (GObject *source, GAsyncResult *result, gpointer data)
{
  CountReply *reply = data;
  CountJob *job = g_task_get_task_data (G_TASK (result));

  if (g_task_propagate_boolean (G_TASK (result), NULL))
    reply->counted (&job->count, reply->data);
  g_free (reply);
}

static void
cancel_and_unref (gpointer cancellable)
{
  g_cancellable_cancel (cancellable);
  g_object_unref (cancellable);
}

/* counts @items; the count is dropped when @owner (the dialog) goes first */
static void
count_items (GPtrArray *items, GtkWidget *owner, Counted counted, gpointer data)
{
  GCancellable *cancellable = g_cancellable_new ();
  CountJob *job = g_new0 (CountJob, 1);
  CountReply *reply = g_new0 (CountReply, 1);
  g_autoptr(GTask) task = NULL;

  job->files = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < items->len; i++)
    g_ptr_array_add (job->files, g_object_ref (((FmItem *) items->pdata[i])->file));
  reply->counted = counted;
  reply->data = data;
  task = g_task_new (NULL, cancellable, on_counted, reply);
  g_task_set_task_data (task, job, count_job_free);
  g_task_run_in_thread (task, count_thread);
  /* the dialog cancels its count when it goes */
  g_object_set_data_full (G_OBJECT (owner), "fm-count", cancellable, cancel_and_unref);
}

/* ---- Properties --------------------------------------------------------------------------------------------------- */

static GtkWidget *
property_row (GtkWidget *content, const char *label, const char *value)
{
  GtkWidget *text = fm_label (value, "fm-property-value", 14, TRUE);

  gtk_box_append (GTK_BOX (content), fm_form_row (label, text));
  gtk_widget_remove_css_class (gtk_widget_get_prev_sibling (text), "fm-form-label");
  gtk_widget_add_css_class (gtk_widget_get_prev_sibling (text), "fm-property-label");
  vibeos_label_set_font_padding_sp (gtk_widget_get_prev_sibling (text), 14);
  return text;
}

typedef struct {
  GtkWidget *size, *contents;
} Properties;

/* "3 folders, 12 files", each count with its noun's plural */
static char *
counts_text (guint folders, guint files)
{
  g_autofree char *f = g_strdup_printf (g_dngettext (NULL, "%u folder", "%u folders", folders), folders);
  g_autofree char *n = g_strdup_printf (g_dngettext (NULL, "%u file", "%u files", files), files);

  /* TRANSLATORS: a list of two counts, "3 folders, 12 files" */
  return g_strdup_printf (_("%s, %s"), f, n);
}

static void
on_properties_counted (const FmCount *count, gpointer data)
{
  Properties *properties = data;
  g_autofree char *size = fm_format_size (count->bytes);
  g_autofree char *contents = NULL;

  if (properties->size)
    gtk_label_set_text (GTK_LABEL (properties->size), size);
  if (properties->contents)
    {
      /* the folders asked about are not their own contents */
      guint top = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (properties->contents), "fm-top"));

      contents = counts_text (count->folders > top ? count->folders - top : 0, count->files);
      gtk_label_set_text (GTK_LABEL (properties->contents), contents);
    }
}

void
fm_properties_dialog (GtkWidget *parent, GPtrArray *items)
{
  GtkWidget *content;
  VibeosDialog *dialog = fm_dialog_new (_("Properties"), &content);
  Properties *properties = g_new0 (Properties, 1);
  FmItem *first = items->pdata[0];
  guint folders = 0;
  gboolean any_folder = FALSE;

  for (guint i = 0; i < items->len; i++)
    if (fm_item_is_folder (items->pdata[i]))
      {
        folders++;
        any_folder = TRUE;
      }
  gtk_widget_add_css_class (content, "fm-properties");
  if (items->len == 1)
    {
      g_autofree char *type = NULL;
      g_autofree char *modified = first->mtime ? fm_format_date_time (first->mtime) : NULL;
      g_autofree char *path = fm_display_path (first->file);

      if (fm_item_is_folder (first))
        type = g_strdup (_("Folder"));
      else if (first->content_type)
        type = g_content_type_get_description (first->content_type);
      property_row (content, _("Name"), first->name);
      property_row (content, _("Type"), type ? type : "-");
      if (any_folder)
        {
          properties->size = property_row (content, _("Size"), _("Calculating…"));
          properties->contents = property_row (content, _("Contents"), _("Calculating…"));
        }
      else
        {
          g_autofree char *size = fm_format_size (first->size);

          property_row (content, _("Size"), size);
        }
      if (modified)
        property_row (content, _("Modified"), modified);
      gtk_label_set_selectable (GTK_LABEL (property_row (content, _("Path"), path)), TRUE);
      if (first->mode)
        {
          g_autofree char *permissions = fm_format_permissions (first->mode);

          property_row (content, _("Permissions"), permissions);
        }
      property_row (content, _("Hidden"), first->hidden ? _("Yes") : _("No"));
    }
  else
    {
      g_autoptr(GFile) parent_dir = g_file_get_parent (first->file);
      g_autofree char *path = parent_dir ? fm_display_path (parent_dir) : NULL;

      properties->size = property_row (content, _("Size"), _("Calculating…"));
      properties->contents = property_row (content, _("Contents"), _("Calculating…"));
      if (path)
        gtk_label_set_selectable (GTK_LABEL (property_row (content, _("Path"), path)), TRUE);
    }
  if (properties->contents)
    g_object_set_data (G_OBJECT (properties->contents), "fm-top", GUINT_TO_POINTER (folders));
  fm_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "fm-properties", properties, g_free);
  if (properties->size)
    count_items (items, GTK_WIDGET (dialog), on_properties_counted, properties);
  vibeos_dialog_present (dialog, parent);
}

/* ---- View ------------------------------------------------------------------------------------------------------- */

typedef struct {
  GSettings *settings;
  GtkWidget *types[2];          /* List, Grid */
  GtkWidget *sorts[FM_SORT_TYPE_DOWN + 1];
  GtkWidget *hidden, *thumbnails;
} View;

static void
view_free (gpointer data)
{
  View *view = data;

  g_object_unref (view->settings);
  g_free (view);
}

static void
on_sort_toggled (GtkWidget *check, GParamSpec *pspec, View *view)
{
  if (!fm_check_get_active (check))
    return;
  for (int i = FM_SORT_NAME_UP; i <= FM_SORT_TYPE_DOWN; i++)
    if (view->sorts[i] && fm_check_row_get_check (view->sorts[i]) != check)
      fm_check_set_active (fm_check_row_get_check (view->sorts[i]), FALSE);
}

static void
on_type_toggled (GtkWidget *check, GParamSpec *pspec, View *view)
{
  if (!fm_check_get_active (check))
    return;
  for (guint i = 0; i < G_N_ELEMENTS (view->types); i++)
    if (fm_check_row_get_check (view->types[i]) != check)
      fm_check_set_active (fm_check_row_get_check (view->types[i]), FALSE);
}

/* a radio row with the theme's view-list or view-grid glyph between the radio and the label */
static GtkWidget *
view_type_row (const char *label, const char *icon_name, const char *description, gboolean active)
{
  GtkWidget *row = fm_check_row (label, TRUE, active);
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 20);
  gtk_widget_add_css_class (image, "fm-view-type-icon");
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
  gtk_box_insert_child_after (GTK_BOX (vibeos_pressable_get_child (VIBEOS_PRESSABLE (row))), image,
                              fm_check_row_get_check (row));
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, description, -1);
  return row;
}

static void
on_view_response (VibeosDialog *dialog, int response, View *view)
{
  if (response != VIBEOS_RESPONSE_OK)
    return;
  g_settings_set_string (view->settings, "view-type",
                         fm_check_get_active (fm_check_row_get_check (view->types[1])) ? "grid" : "list");
  for (int i = FM_SORT_NAME_UP; i <= FM_SORT_TYPE_DOWN; i++)
    if (fm_check_get_active (fm_check_row_get_check (view->sorts[i])))
      g_settings_set_string (view->settings, "sort-order", fm_sort_to_string (i));
  g_settings_set_boolean (view->settings, "show-hidden", fm_check_get_active (fm_check_row_get_check (view->hidden)));
  g_settings_set_boolean (view->settings, "show-thumbnails",
                          fm_check_get_active (fm_check_row_get_check (view->thumbnails)));
}

void
fm_view_dialog (GtkWidget *parent, GSettings *settings)
{
  static const char *const names[] = {
    NULL, N_("Name ▲"), N_("Name ▼"), N_("Size ▲"), N_("Size ▼"), N_("Date ▲"), N_("Date ▼"), N_("Type ▲"),
    N_("Type ▼"),
  };
  GtkWidget *content;
  VibeosDialog *dialog = fm_dialog_new (_("View"), &content);
  View *view = g_new0 (View, 1);
  GtkWidget *grid = gtk_grid_new (), *types = gtk_grid_new ();
  g_autofree char *current = g_settings_get_string (settings, "sort-order");
  g_autofree char *type = g_settings_get_string (settings, "view-type");
  gboolean tiles = !g_strcmp0 (type, "grid");
  FmSort sort = fm_sort_from_string (current);

  view->settings = g_object_ref (settings);
  /* the view type: the radios side by side, each with its glyph */
  view->types[0] = view_type_row (_("List"), "view-list-symbolic", _("List view"), !tiles);
  view->types[1] = view_type_row (_("Grid"), "view-grid-symbolic", _("Grid view"), tiles);
  gtk_grid_set_column_homogeneous (GTK_GRID (types), TRUE);
  for (guint i = 0; i < G_N_ELEMENTS (view->types); i++)
    {
      g_signal_connect (fm_check_row_get_check (view->types[i]), "notify::active", G_CALLBACK (on_type_toggled),
                        view);
      gtk_grid_attach (GTK_GRID (types), view->types[i], (int) i, 0, 1, 1);
    }
  gtk_widget_add_css_class (types, "fm-view-types");
  gtk_box_append (GTK_BOX (content), types);
  gtk_box_append (GTK_BOX (content), fm_label (_("Sort By"), "fm-dialog-subtitle", 14, FALSE));
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  for (int i = FM_SORT_NAME_UP; i <= FM_SORT_TYPE_DOWN; i++)
    {
      view->sorts[i] = fm_check_row (_(names[i]), TRUE, i == (int) sort);
      g_signal_connect (fm_check_row_get_check (view->sorts[i]), "notify::active", G_CALLBACK (on_sort_toggled),
                        view);
      gtk_grid_attach (GTK_GRID (grid), view->sorts[i], (i - 1) % 2, (i - 1) / 2, 1, 1);
    }
  gtk_box_append (GTK_BOX (content), grid);
  view->hidden = fm_check_row (_("Show hidden files"), FALSE, g_settings_get_boolean (settings, "show-hidden"));
  view->thumbnails = fm_check_row (_("Show thumbnails"), FALSE, g_settings_get_boolean (settings, "show-thumbnails"));
  gtk_widget_add_css_class (view->hidden, "fm-view-first-check");
  gtk_box_append (GTK_BOX (content), view->hidden);
  gtk_box_append (GTK_BOX (content), view->thumbnails);
  fm_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  fm_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "fm-view", view, view_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_view_response), view);
  vibeos_dialog_present (dialog, parent);
}

/* ---- Delete ----------------------------------------------------------------------------------------------------- */

typedef struct {
  GPtrArray *items;
  GtkWidget *message, *count, *permanent;
  FmDeleteDone done;
  gpointer data;
} Delete;

static void
delete_free (gpointer data)
{
  Delete *del = data;

  g_ptr_array_unref (del->items);
  g_free (del);
}

static void
delete_message (Delete *del)
{
  gboolean permanently = !del->permanent || fm_check_get_active (fm_check_row_get_check (del->permanent));
  g_autofree char *text = NULL;

  if (del->items->len == 1)
    text = g_strdup_printf (permanently ? _("Are you sure you want to delete '%s'?")
                                        : _("Are you sure you want to move '%s' to the Recycle Bin?"),
                            ((FmItem *) del->items->pdata[0])->name);
  else if (permanently)
    text = g_strdup_printf (ngettext ("Are you sure you want to delete %d item?",
                                      "Are you sure you want to delete %d items?", del->items->len),
                            del->items->len);
  else
    text = g_strdup_printf (ngettext ("Are you sure you want to move %d item to the Recycle Bin?",
                                      "Are you sure you want to move %d items to the Recycle Bin?", del->items->len),
                            del->items->len);
  gtk_label_set_text (GTK_LABEL (del->message), text);
}

static void
on_permanent_toggled (GtkWidget *check, GParamSpec *pspec, Delete *del)
{
  delete_message (del);
}

static void
on_delete_counted (const FmCount *count, gpointer data)
{
  Delete *del = data;
  g_autofree char *size = fm_format_size (count->bytes);
  g_autofree char *counts = counts_text (count->folders, count->files);
  /* TRANSLATORS: the counts and the size of what is deleted, "3 folders, 12 files, 4.2 MB" */
  g_autofree char *text = g_strdup_printf (_("%s, %s"), counts, size);

  gtk_label_set_text (GTK_LABEL (del->count), text);
  gtk_widget_set_visible (del->count, TRUE);
}

static void
on_delete_response (VibeosDialog *dialog, int response, Delete *del)
{
  gboolean permanently = !del->permanent || fm_check_get_active (fm_check_row_get_check (del->permanent));

  del->done (response == VIBEOS_RESPONSE_OK, permanently, del->data);
}

void
fm_delete_dialog (GtkWidget *parent, GPtrArray *items, gboolean can_trash, gboolean use_trash, FmDeleteDone done,
                  gpointer data)
{
  GtkWidget *content;
  VibeosDialog *dialog = fm_dialog_new (_("Delete"), &content);
  Delete *del = g_new0 (Delete, 1);

  del->items = g_ptr_array_ref (items);
  del->done = done;
  del->data = data;
  del->message = fm_label (NULL, "fm-dialog-message", 16, TRUE);
  del->count = fm_label (NULL, "fm-dialog-count", 14, TRUE);
  gtk_widget_set_visible (del->count, FALSE);
  gtk_box_append (GTK_BOX (content), del->message);
  gtk_box_append (GTK_BOX (content), del->count);
  if (can_trash)
    {
      del->permanent = fm_check_row (_("Permanently delete"), FALSE, !use_trash);
      gtk_widget_add_css_class (del->permanent, "fm-delete-check");
      g_signal_connect (fm_check_row_get_check (del->permanent), "notify::active",
                        G_CALLBACK (on_permanent_toggled), del);
      gtk_box_append (GTK_BOX (content), del->permanent);
    }
  delete_message (del);
  fm_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  fm_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "fm-delete", del, delete_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), del);
  count_items (items, GTK_WIDGET (dialog), on_delete_counted, del);
  vibeos_dialog_present (dialog, parent);
}
