/* The file operations (fm-jobs.h). */
#include "fm-jobs.h"
#include "fm-dialogs.h"
#include "fm-item.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <neo.h>

#define SHOW_AFTER_MS  500      /* the progress dialog only for a job that takes longer */
#define TICK_MS        250

typedef enum {
  JOB_COPY,
  JOB_MOVE,
  JOB_TRASH,
  JOB_DELETE,
  JOB_RESTORE,
  JOB_UNTRASH,                  /* UNDO of a recycle: the originals looked up in the Recycle Bin */
  JOB_EMPTY,
} JobKind;

typedef enum {
  ANSWER_NONE = -1,
  ANSWER_OVERWRITE,
  ANSWER_RENAME,
  ANSWER_SKIP,
  ANSWER_CANCEL,
} Answer;

typedef struct {
  FmShell *shell;
  JobKind kind;
  GPtrArray *sources;           /* GFile */
  GPtrArray *targets;           /* RESTORE: where each goes back to (GFile) */
  GFile *dest;
  GCancellable *cancellable;

  GMutex lock;
  GCond cond;
  /* the progress, under the lock */
  gboolean preparing;
  guint64 total_bytes, done_bytes, file_base;
  guint total_files, done_files;
  char *from, *to;
  gint64 started;               /* the job's start: the dialog shows 500 ms after it */
  gint64 copy_started;          /* the copying's start, after Preparing: the rate */
  /* a conflict the worker waits on */
  char *conflict;
  Answer answer, apply_all;

  /* the result */
  guint done_items;
  char *first_name;
  char *error;
  GPtrArray *trashed;           /* TRASH: the originals' paths, for UNDO */

  /* the window's side */
  NeoDialog *dialog;
  GtkWidget *percent, *from_label, *to_label, *size_label, *rate_label, *bar, *count_label, *remaining_label;
  guint timer;
  gboolean hidden;
} Job;

static void
job_free (Job *job)
{
  g_ptr_array_unref (job->sources);
  g_clear_pointer (&job->targets, g_ptr_array_unref);
  g_clear_object (&job->dest);
  g_object_unref (job->cancellable);
  g_mutex_clear (&job->lock);
  g_cond_clear (&job->cond);
  g_free (job->from);
  g_free (job->to);
  g_free (job->conflict);
  g_free (job->first_name);
  g_free (job->error);
  g_clear_pointer (&job->trashed, g_ptr_array_unref);
  g_clear_handle_id (&job->timer, g_source_remove);
  g_free (job);
}

static void
set_paths (Job *job, GFile *from, GFile *to)
{
  g_autofree char *from_path = from ? fm_display_path (from) : NULL;
  g_autofree char *to_path = to ? fm_display_path (to) : NULL;

  g_mutex_lock (&job->lock);
  g_free (job->from);
  job->from = g_steal_pointer (&from_path);
  g_free (job->to);
  job->to = g_steal_pointer (&to_path);
  g_mutex_unlock (&job->lock);
}

static void
add_files (Job *job, guint n, guint64 bytes)
{
  g_mutex_lock (&job->lock);
  job->done_files += n;
  job->done_bytes += bytes;
  job->file_base = job->done_bytes;
  g_mutex_unlock (&job->lock);
}

static void
fail (Job *job, const char *message)
{
  if (!job->error && message)
    job->error = g_strdup (message);
}

/* ---- the conflict question --------------------------------------------------------------------------------------- */

static void
on_conflict_answer (NeoPressable *button, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "fm-answer")));
}

static void
on_conflict_response (NeoDialog *dialog, int response, Job *job)
{
  GtkWidget *all = g_object_get_data (G_OBJECT (dialog), "fm-all");
  Answer answer = response >= ANSWER_OVERWRITE && response <= ANSWER_SKIP ? (Answer) response : ANSWER_CANCEL;

  g_mutex_lock (&job->lock);
  job->answer = answer;
  if (answer != ANSWER_CANCEL && fm_check_get_active (fm_check_row_get_check (all)))
    job->apply_all = answer;
  g_cond_signal (&job->cond);
  g_mutex_unlock (&job->lock);
}

static gboolean
show_conflict (gpointer data)
{
  Job *job = data;
  const char *labels[] = { _("Overwrite"), _("Rename"), _("Skip") };
  GtkWidget *content;
  NeoDialog *dialog = fm_dialog_new (NULL, &content);
  g_autofree char *message = g_strdup_printf (_("%s already exists. Do you want to overwrite?"), job->conflict);
  GtkWidget *all = fm_check_row (_("Apply to all"), FALSE, FALSE);

  gtk_widget_add_css_class (content, "fm-conflict");
  gtk_box_append (GTK_BOX (content), fm_label (message, "fm-dialog-message", 16, TRUE));
  gtk_box_append (GTK_BOX (content), all);
  for (int i = 0; i < 3; i++)
    {
      GtkWidget *button = fm_text_button (labels[i]);

      gtk_widget_add_css_class (button, "fm-conflict-button");
      g_object_set_data (G_OBJECT (button), "fm-answer", GINT_TO_POINTER (i));
      g_signal_connect (button, "clicked", G_CALLBACK (on_conflict_answer), dialog);
      gtk_box_append (GTK_BOX (content), button);
    }
  fm_dialog_add_button (dialog, _("Cancel"), ANSWER_CANCEL);
  g_object_set_data (G_OBJECT (dialog), "fm-all", all);
  g_signal_connect (dialog, "response", G_CALLBACK (on_conflict_response), job);
  neo_dialog_present (dialog, job->shell->window);
  return G_SOURCE_REMOVE;
}

/* in the worker: asks, and waits for the answer */
static Answer
ask_conflict (Job *job, GFile *target)
{
  Answer answer;

  g_mutex_lock (&job->lock);
  if (job->apply_all != ANSWER_NONE)
    {
      answer = job->apply_all;
      g_mutex_unlock (&job->lock);
      return answer;
    }
  g_free (job->conflict);
  job->conflict = g_file_get_basename (target);
  job->answer = ANSWER_NONE;
  g_main_context_invoke (NULL, show_conflict, job);
  while (job->answer == ANSWER_NONE)
    g_cond_wait (&job->cond, &job->lock);
  answer = job->answer;
  g_mutex_unlock (&job->lock);
  if (answer == ANSWER_CANCEL)
    g_cancellable_cancel (job->cancellable);
  return answer;
}

/* "name - Copy", "name - Copy (2)" ...: the first that is not there */
static GFile *
free_name (GFile *folder, GFile *source, gboolean is_folder, GCancellable *cancellable)
{
  g_autofree char *base = g_file_get_basename (source);

  for (guint n = 1; n < 10000; n++)
    {
      g_autofree char *name = fm_copy_name (base, is_folder, n);
      GFile *candidate = g_file_get_child (folder, name);

      if (!g_file_query_exists (candidate, cancellable))
        return candidate;
      g_object_unref (candidate);
    }
  return g_file_get_child (folder, base);
}

/* ---- copying, moving and deleting (in the worker) ----------------------------------------------------------------- */

static void
on_copy_progress (goffset current, goffset total, gpointer data)
{
  Job *job = data;

  g_mutex_lock (&job->lock);
  job->done_bytes = job->file_base + (guint64) current;
  g_mutex_unlock (&job->lock);
}

static gboolean delete_tree (Job *job, GFile *file);

static gboolean
copy_tree (Job *job, GFile *source, GFile *target)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = g_file_query_info (source, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                                 G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable, &error);
  g_autoptr(GFile) renamed = NULL;
  GFileCopyFlags flags = G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA;

  if (!info)
    {
      fail (job, error->message);
      return FALSE;
    }
  set_paths (job, source, target);
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr(GFileEnumerator) children = NULL;
      GFileInfo *child_info;
      GFile *child;
      gboolean ok = TRUE;

      if (!g_file_make_directory (target, job->cancellable, &error))
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              fail (job, error->message);
              return FALSE;
            }
          /* a folder onto a folder merges; onto a file it is a conflict */
          if (g_file_query_file_type (target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable)
              != G_FILE_TYPE_DIRECTORY)
            {
              Answer answer = ask_conflict (job, target);

              if (answer == ANSWER_SKIP || answer == ANSWER_CANCEL)
                return answer == ANSWER_SKIP;
              if (answer == ANSWER_RENAME)
                {
                  g_autoptr(GFile) parent = g_file_get_parent (target);

                  renamed = free_name (parent, target, TRUE, job->cancellable);
                  target = renamed;
                }
              else if (!g_file_delete (target, job->cancellable, NULL))
                return FALSE;
              g_clear_error (&error);
              if (!g_file_make_directory (target, job->cancellable, &error))
                {
                  fail (job, error->message);
                  return FALSE;
                }
            }
        }
      add_files (job, 1, 0);
      children = g_file_enumerate_children (source, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable, &error);
      if (!children)
        {
          fail (job, error->message);
          return FALSE;
        }
      while (ok && g_file_enumerator_iterate (children, &child_info, &child, job->cancellable, NULL) && child_info)
        {
          g_autoptr(GFile) child_target = g_file_get_child (target, g_file_info_get_name (child_info));

          ok = copy_tree (job, child, child_target);
        }
      return ok && !g_cancellable_is_cancelled (job->cancellable);
    }

  for (;;)
    {
      gboolean existed = FALSE;

      g_clear_error (&error);
      if (g_file_copy (source, target, flags, job->cancellable, on_copy_progress, job, &error))
        {
          add_files (job, 1, 0);
          g_mutex_lock (&job->lock);
          job->file_base = job->done_bytes;
          g_mutex_unlock (&job->lock);
          return TRUE;
        }
      existed = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          /* a partly written file does not stay */
          g_file_delete (target, NULL, NULL);
          return FALSE;
        }
      if (!existed)
        {
          fail (job, error->message);
          return FALSE;
        }
      switch (ask_conflict (job, target))
        {
        case ANSWER_OVERWRITE:
          flags |= G_FILE_COPY_OVERWRITE;
          break;
        case ANSWER_RENAME:
          {
            g_autoptr(GFile) parent = g_file_get_parent (target);

            g_set_object (&renamed, NULL);
            renamed = free_name (parent, target, FALSE, job->cancellable);
            target = renamed;
            break;
          }
        case ANSWER_SKIP:
          add_files (job, 1, g_file_info_get_size (info));
          return TRUE;
        default:
          return FALSE;
        }
    }
}

static gboolean
move_one (Job *job, GFile *source, GFile *target, guint64 size)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) renamed = NULL;
  GFileCopyFlags flags = G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE | G_FILE_COPY_ALL_METADATA;

  set_paths (job, source, target);
  for (;;)
    {
      g_clear_error (&error);
      /* a rename on one filesystem first */
      if (g_file_move (source, target, flags, job->cancellable, NULL, NULL, &error))
        {
          add_files (job, 1, size);
          return TRUE;
        }
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          GFileType type = g_file_query_file_type (target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable);
          GFileType own = g_file_query_file_type (source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable);

          /* a folder onto a folder merges: copied in, then the source deleted */
          if (type == G_FILE_TYPE_DIRECTORY && own == G_FILE_TYPE_DIRECTORY)
            return copy_tree (job, source, target) && delete_tree (job, source);
          switch (ask_conflict (job, target))
            {
            case ANSWER_OVERWRITE:
              flags |= G_FILE_COPY_OVERWRITE;
              continue;
            case ANSWER_RENAME:
              {
                g_autoptr(GFile) parent = g_file_get_parent (target);

                g_set_object (&renamed, NULL);
                renamed = free_name (parent, target, own == G_FILE_TYPE_DIRECTORY, job->cancellable);
                target = renamed;
                continue;
              }
            case ANSWER_SKIP:
              add_files (job, 1, size);
              return TRUE;
            default:
              return FALSE;
            }
        }
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
          || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)
          || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE))
        /* another filesystem: copied, then the source deleted */
        return copy_tree (job, source, target) && !g_cancellable_is_cancelled (job->cancellable)
               && delete_tree (job, source);
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fail (job, error->message);
      return FALSE;
    }
}

/* deepest first */
static gboolean
delete_tree (Job *job, GFile *file)
{
  g_autoptr(GError) error = NULL;
  GFileType type = g_file_query_file_type (file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable);

  /* the trash backend deletes a top-level item with all that is in it, and nothing inside one */
  if (type == G_FILE_TYPE_DIRECTORY && !g_file_has_uri_scheme (file, "trash"))
    {
      g_autoptr(GFileEnumerator) children = g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                                       job->cancellable, &error);
      GFileInfo *info;
      GFile *child;

      if (!children)
        {
          fail (job, error->message);
          return FALSE;
        }
      while (g_file_enumerator_iterate (children, &info, &child, job->cancellable, NULL) && info)
        if (!delete_tree (job, child))
          return FALSE;
    }
  if (g_cancellable_is_cancelled (job->cancellable))
    return FALSE;
  set_paths (job, file, NULL);
  if (!g_file_delete (file, job->cancellable, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fail (job, error->message);
      return FALSE;
    }
  add_files (job, 1, 0);
  return TRUE;
}

static void
note_done (Job *job, GFile *file)
{
  if (!job->first_name)
    {
      g_autoptr(GFileInfo) info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                                     G_FILE_QUERY_INFO_NONE, NULL, NULL);

      job->first_name = info ? g_strdup (g_file_info_get_display_name (info)) : g_file_get_basename (file);
    }
  job->done_items++;
}

static void
run_copy (Job *job)
{
  GPtrArray *sizes = g_ptr_array_new_with_free_func (g_free);

  /* "Preparing…": what there is to copy */
  for (guint i = 0; i < job->sources->len && !g_cancellable_is_cancelled (job->cancellable); i++)
    {
      FmCount count = { 0 };
      guint64 *size = g_new (guint64, 1);

      fm_count_tree (job->sources->pdata[i], job->cancellable, &count);
      *size = count.bytes;
      g_ptr_array_add (sizes, size);
      g_mutex_lock (&job->lock);
      job->total_bytes += count.bytes;
      job->total_files += count.files + count.folders;
      g_mutex_unlock (&job->lock);
    }
  g_mutex_lock (&job->lock);
  job->preparing = FALSE;
  job->copy_started = g_get_monotonic_time ();
  g_mutex_unlock (&job->lock);

  for (guint i = 0; i < job->sources->len && !g_cancellable_is_cancelled (job->cancellable); i++)
    {
      GFile *source = job->sources->pdata[i];
      g_autoptr(GFile) parent = fm_parent (source);
      g_autofree char *base = g_file_get_basename (source);
      g_autoptr(GFile) target = NULL;
      gboolean same_folder = parent && g_file_equal (parent, job->dest);
      gboolean ok;

      /* into itself, or into a folder under itself */
      if (g_file_equal (source, job->dest) || fm_file_is_under (job->dest, source))
        {
          fail (job, _("Target is the same as the source."));
          break;
        }
      if (same_folder && job->kind == JOB_MOVE)
        {
          add_files (job, 1, i < sizes->len ? *(guint64 *) sizes->pdata[i] : 0);
          continue;
        }
      if (same_folder)
        {
          gboolean folder = g_file_query_file_type (source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL)
                            == G_FILE_TYPE_DIRECTORY;

          target = free_name (job->dest, source, folder, job->cancellable);
        }
      else
        target = g_file_get_child (job->dest, base);
      if (job->kind == JOB_MOVE)
        ok = move_one (job, source, target, i < sizes->len ? *(guint64 *) sizes->pdata[i] : 0);
      else
        ok = copy_tree (job, source, target);
      if (ok)
        note_done (job, target);
    }
  g_ptr_array_unref (sizes);
}

static void
run_trash (Job *job)
{
  job->trashed = g_ptr_array_new_with_free_func (g_free);
  g_mutex_lock (&job->lock);
  job->total_files = job->sources->len;
  job->preparing = FALSE;
  g_mutex_unlock (&job->lock);
  for (guint i = 0; i < job->sources->len && !g_cancellable_is_cancelled (job->cancellable); i++)
    {
      GFile *source = job->sources->pdata[i];
      g_autoptr(GError) error = NULL;

      set_paths (job, source, NULL);
      note_done (job, source);
      if (!g_file_trash (source, job->cancellable, &error))
        {
          job->done_items--;
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fail (job, error->message);
          break;
        }
      if (g_file_is_native (source))
        g_ptr_array_add (job->trashed, g_file_get_path (source));
      add_files (job, 1, 0);
    }
}

static void
run_delete (Job *job)
{
  for (guint i = 0; i < job->sources->len && !g_cancellable_is_cancelled (job->cancellable); i++)
    {
      FmCount count = { 0 };

      fm_count_tree (job->sources->pdata[i], job->cancellable, &count);
      g_mutex_lock (&job->lock);
      job->total_files += count.files + count.folders;
      g_mutex_unlock (&job->lock);
    }
  g_mutex_lock (&job->lock);
  job->preparing = FALSE;
  g_mutex_unlock (&job->lock);
  for (guint i = 0; i < job->sources->len && !g_cancellable_is_cancelled (job->cancellable); i++)
    {
      GFile *source = job->sources->pdata[i];

      note_done (job, source);
      if (!delete_tree (job, source))
        {
          job->done_items--;
          break;
        }
    }
}

/* UNDO: the items whose original paths were recycled, the latest of each */
static void
find_untrash (Job *job)
{
  g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///");
  g_autoptr(GFileEnumerator) children = g_file_enumerate_children (trash, "standard::name,trash::orig-path,"
                                                                   "trash::deletion-date", G_FILE_QUERY_INFO_NONE,
                                                                   job->cancellable, NULL);
  g_autoptr(GHashTable) best = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
  g_autoptr(GHashTable) when = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
  GPtrArray *wanted = job->trashed;
  GFileInfo *info;
  GFile *child;

  job->sources = g_ptr_array_new_with_free_func (g_object_unref);
  job->targets = g_ptr_array_new_with_free_func (g_object_unref);
  while (children && g_file_enumerator_iterate (children, &info, &child, job->cancellable, NULL) && info)
    {
      const char *orig = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
      g_autoptr(GDateTime) deleted = g_file_info_get_deletion_date (info);
      gint64 at = deleted ? g_date_time_to_unix (deleted) : 0;

      for (guint i = 0; orig && i < wanted->len; i++)
        if (!g_strcmp0 (orig, wanted->pdata[i])
            && (!g_hash_table_contains (best, wanted->pdata[i])
                || at >= (gint64) GPOINTER_TO_SIZE (g_hash_table_lookup (when, wanted->pdata[i]))))
          {
            g_hash_table_replace (best, wanted->pdata[i], g_object_ref (child));
            g_hash_table_replace (when, wanted->pdata[i], GSIZE_TO_POINTER ((gsize) at));
          }
    }
  for (guint i = 0; i < wanted->len; i++)
    {
      GFile *found = g_hash_table_lookup (best, wanted->pdata[i]);

      if (found)
        {
          g_ptr_array_add (job->sources, g_object_ref (found));
          g_ptr_array_add (job->targets, g_file_new_for_path (wanted->pdata[i]));
        }
    }
}

static void
run_restore (Job *job)
{
  if (job->kind == JOB_UNTRASH)
    {
      g_ptr_array_unref (job->sources);
      find_untrash (job);
    }
  g_mutex_lock (&job->lock);
  job->total_files = job->sources->len;
  job->preparing = FALSE;
  g_mutex_unlock (&job->lock);
  for (guint i = 0; i < job->sources->len && !g_cancellable_is_cancelled (job->cancellable); i++)
    {
      GFile *source = job->sources->pdata[i], *target = job->targets->pdata[i];
      g_autoptr(GFile) parent = g_file_get_parent (target);
      g_autoptr(GError) error = NULL;

      set_paths (job, source, target);
      /* the folders it was in, again when they are gone */
      if (parent)
        g_file_make_directory_with_parents (parent, job->cancellable, NULL);
      if (g_file_query_exists (target, job->cancellable))
        {
          Answer answer = ask_conflict (job, target);

          if (answer == ANSWER_SKIP)
            continue;
          if (answer == ANSWER_CANCEL)
            break;
          if (answer == ANSWER_RENAME)
            {
              g_autoptr(GFile) other = free_name (parent, target, FALSE, job->cancellable);

              g_ptr_array_index (job->targets, i) = g_object_ref (other);
              g_object_unref (target);
              target = other;
            }
          else if (!delete_tree (job, target))
            break;
        }
      if (!g_file_move (source, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, job->cancellable, NULL, NULL, &error))
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fail (job, error->message);
          break;
        }
      note_done (job, target);
      add_files (job, 1, 0);
    }
}

static void
run_empty (Job *job)
{
  g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///");
  g_autoptr(GFileEnumerator) children = g_file_enumerate_children (trash, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                                   G_FILE_QUERY_INFO_NONE, job->cancellable, NULL);
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func (g_object_unref);
  GFileInfo *info;
  GFile *child;

  while (children && g_file_enumerator_iterate (children, &info, &child, job->cancellable, NULL) && info)
    g_ptr_array_add (items, g_object_ref (child));
  g_mutex_lock (&job->lock);
  job->total_files = items->len;
  job->preparing = FALSE;
  g_mutex_unlock (&job->lock);
  /* the trash backend deletes a top-level item with all that is in it */
  for (guint i = 0; i < items->len && !g_cancellable_is_cancelled (job->cancellable); i++)
    {
      g_autoptr(GError) error = NULL;

      set_paths (job, items->pdata[i], NULL);
      if (!g_file_delete (items->pdata[i], job->cancellable, &error))
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fail (job, error->message);
          break;
        }
      job->done_items++;
      add_files (job, 1, 0);
    }
}

static void
job_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Job *job = data;

  switch (job->kind)
    {
    case JOB_COPY:
    case JOB_MOVE:
      run_copy (job);
      break;
    case JOB_TRASH:
      run_trash (job);
      break;
    case JOB_DELETE:
      run_delete (job);
      break;
    case JOB_RESTORE:
    case JOB_UNTRASH:
      run_restore (job);
      break;
    case JOB_EMPTY:
      run_empty (job);
      break;
    }
  g_task_return_boolean (task, TRUE);
}

/* ---- the progress dialog ----------------------------------------------------------------------------------------- */

static const char *
job_title (JobKind kind)
{
  switch (kind)
    {
    case JOB_COPY:
      return _("Copying");
    case JOB_MOVE:
      return _("Moving");
    case JOB_TRASH:
      return _("Recycling");
    case JOB_RESTORE:
    case JOB_UNTRASH:
      return _("Restoring");
    case JOB_DELETE:
    case JOB_EMPTY:
    default:
      return _("Deleting");
    }
}

static double
job_fraction (Job *job)
{
  if (job->preparing)
    return -1;
  if (job->total_bytes > 0)
    return (double) job->done_bytes / job->total_bytes;
  return job->total_files ? (double) job->done_files / job->total_files : 0;
}

static void
update_dialog (Job *job)
{
  double fraction;
  g_autofree char *percent = NULL, *size = NULL, *count = NULL, *rate = NULL, *remaining = NULL;

  g_mutex_lock (&job->lock);
  fraction = job_fraction (job);
  if (job->dialog)
    {
      g_autofree char *done = fm_format_size (job->done_bytes);
      g_autofree char *total = fm_format_size (job->total_bytes);
      g_autofree char *done_count = g_strdup_printf ("%u/%u", MIN (job->done_files, job->total_files),
                                                     job->total_files);
      double seconds = job->copy_started ? (g_get_monotonic_time () - job->copy_started) / (double) G_USEC_PER_SEC
                                         : 0;

      percent = fraction >= 0 ? g_strdup_printf ("%d%%", (int) (fraction * 100)) : g_strdup ("");
      size = job->total_bytes ? g_strdup_printf (_("%1$s/%2$s"), done, total) : g_strdup ("");
      count = g_strdup_printf (_("Progress : %s"), done_count);
      if (seconds > 1 && job->done_bytes > 0 && job->total_bytes)
        {
          double per_second = job->done_bytes / seconds;
          g_autofree char *speed = fm_format_size ((guint64) per_second);
          g_autofree char *left = fm_format_duration ((guint) ((job->total_bytes - MIN (job->done_bytes,
                                                                                         job->total_bytes))
                                                               / MAX (per_second, 1)));

          rate = g_strdup_printf ("%s/s", speed);
          remaining = g_strdup_printf (_("Remaining time : %s"), left);
        }
      gtk_label_set_text (GTK_LABEL (job->percent), percent);
      gtk_label_set_text (GTK_LABEL (job->from_label), job->preparing ? _("Preparing…") : job->from ? job->from : "");
      gtk_label_set_text (GTK_LABEL (job->to_label), job->to ? job->to : "");
      gtk_widget_set_visible (job->to_label, job->to != NULL);
      gtk_label_set_text (GTK_LABEL (job->size_label), size);
      gtk_label_set_text (GTK_LABEL (job->rate_label), rate ? rate : "");
      gtk_label_set_text (GTK_LABEL (job->count_label), count);
      gtk_label_set_text (GTK_LABEL (job->remaining_label), remaining ? remaining : "");
      fm_progress_set_fraction (job->bar, fraction);
    }
  g_mutex_unlock (&job->lock);
  if (job->hidden)
    fm_shell_job_progress (job->shell, fraction < 0 ? 0 : fraction);
}

static void
on_progress_response (NeoDialog *dialog, int response, Job *job)
{
  job->dialog = NULL;
  /* CANCEL stops after the current file; HIDE (or a tap outside) leaves the job running with its line */
  if (response == NEO_RESPONSE_OK + 1)
    g_cancellable_cancel (job->cancellable);
  else
    {
      job->hidden = TRUE;
      update_dialog (job);
    }
}

static void
show_dialog (Job *job)
{
  GtkWidget *content, *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0), *title, *sizes;
  NeoDialog *dialog = fm_dialog_new (NULL, &content);

  title = fm_label (job_title (job->kind), "fm-dialog-title", 20, FALSE);
  gtk_widget_set_hexpand (title, TRUE);
  job->percent = fm_label (NULL, "fm-progress-percent", 16, FALSE);
  gtk_box_append (GTK_BOX (head), title);
  gtk_box_append (GTK_BOX (head), job->percent);
  gtk_widget_add_css_class (head, "fm-progress-head");
  gtk_box_append (GTK_BOX (content), head);
  job->from_label = fm_label (NULL, "fm-progress-text", 14, FALSE);
  job->to_label = fm_label (NULL, "fm-progress-text", 14, FALSE);
  gtk_label_set_ellipsize (GTK_LABEL (job->from_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_ellipsize (GTK_LABEL (job->to_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append (GTK_BOX (content), job->from_label);
  gtk_box_append (GTK_BOX (content), job->to_label);
  sizes = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  job->size_label = fm_label (NULL, "fm-progress-text", 14, FALSE);
  job->rate_label = fm_label (NULL, "fm-progress-text", 14, FALSE);
  gtk_widget_set_hexpand (job->size_label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (job->rate_label), 1);
  gtk_box_append (GTK_BOX (sizes), job->size_label);
  gtk_box_append (GTK_BOX (sizes), job->rate_label);
  gtk_box_append (GTK_BOX (content), sizes);
  job->bar = fm_progress_new (4);
  gtk_widget_add_css_class (job->bar, "fm-progress-bar");
  gtk_box_append (GTK_BOX (content), job->bar);
  job->count_label = fm_label (NULL, "fm-progress-text", 14, FALSE);
  job->remaining_label = fm_label (NULL, "fm-progress-text", 14, FALSE);
  gtk_box_append (GTK_BOX (content), job->count_label);
  gtk_box_append (GTK_BOX (content), job->remaining_label);
  fm_dialog_add_button (dialog, _("Hide"), NEO_RESPONSE_OK);
  fm_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_OK + 1);
  g_signal_connect (dialog, "response", G_CALLBACK (on_progress_response), job);
  job->dialog = dialog;
  update_dialog (job);
  neo_dialog_present (dialog, job->shell->window);
}

static gboolean
on_tick (gpointer data)
{
  Job *job = data;

  /* the conflict question is up: the dialog waits under it */
  if (!job->dialog && !job->hidden && g_get_monotonic_time () - job->started >= SHOW_AFTER_MS * 1000)
    {
      gboolean asking;

      g_mutex_lock (&job->lock);
      asking = job->answer == ANSWER_NONE && job->conflict;
      g_mutex_unlock (&job->lock);
      if (!asking)
        show_dialog (job);
    }
  update_dialog (job);
  return G_SOURCE_CONTINUE;
}

/* ---- the end ------------------------------------------------------------------------------------------------------- */

static Job *job_new (FmShell *shell, JobKind kind, GPtrArray *sources);
static void job_start (Job *job);

/* UNDO of a recycle: @data is the recycled paths (the shell keeps them while the snackbar shows) */
static void
on_undo (FmShell *shell, gpointer data)
{
  Job *undo = job_new (shell, JOB_UNTRASH, NULL);

  undo->trashed = g_ptr_array_ref (data);
  job_start (undo);
}

static char *
folder_name (GFile *folder)
{
  g_autoptr(GFileInfo) info = g_file_query_info (folder, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);

  return info ? g_strdup (g_file_info_get_display_name (info)) : fm_display_path (folder);
}

static void
on_job_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Job *job = data;
  FmShell *shell = job->shell;
  g_autofree char *message = NULL;
  gboolean stopped = g_cancellable_is_cancelled (job->cancellable);
  GPtrArray *undo = NULL;

  g_clear_handle_id (&job->timer, g_source_remove);
  if (job->dialog)
    {
      NeoDialog *dialog = job->dialog;

      g_signal_handlers_disconnect_by_data (dialog, job);
      job->dialog = NULL;
      neo_dialog_respond (dialog, NEO_RESPONSE_CANCEL);
    }
  fm_shell_job_progress (shell, -1);
  if (job->error)
    {
      const char *what;

      switch (job->kind)
        {
        case JOB_COPY:
          what = _("Failed to copy files");
          break;
        case JOB_MOVE:
          what = _("Failed to move files");
          break;
        case JOB_RESTORE:
        case JOB_UNTRASH:
          what = _("Failed to restore files");
          break;
        default:
          what = _("Failed to delete files");
          break;
        }
      g_warning ("%s: %s", what, job->error);
      message = g_strdup_printf ("%s: %s", what, job->error);
    }
  else if (!stopped && job->done_items)
    {
      g_autofree char *where = job->dest ? folder_name (job->dest) : NULL;
      guint n = job->done_items;

      switch (job->kind)
        {
        case JOB_COPY:
          message = n == 1 ? g_strdup_printf (_("'%1$s' copied to %2$s"), job->first_name, where)
                           : g_strdup_printf (ngettext ("%1$d item copied to %2$s", "%1$d items copied to %2$s", n),
                                              n, where);
          break;
        case JOB_MOVE:
          message = n == 1 ? g_strdup_printf (_("'%1$s' moved to %2$s"), job->first_name, where)
                           : g_strdup_printf (ngettext ("%1$d item moved to %2$s", "%1$d items moved to %2$s", n), n,
                                              where);
          break;
        case JOB_TRASH:
          message = n == 1 ? g_strdup_printf (_("'%s' moved to the Recycle Bin"), job->first_name)
                           : g_strdup_printf (ngettext ("%d item moved to the Recycle Bin",
                                                        "%d items moved to the Recycle Bin", n), n);
          if (job->trashed && job->trashed->len)
            undo = job->trashed;
          break;
        case JOB_DELETE:
          message = n == 1 ? g_strdup_printf (_("'%s' deleted"), job->first_name)
                           : g_strdup_printf (ngettext ("%1$d item deleted", "%1$d items deleted", n), n);
          break;
        case JOB_RESTORE:
        case JOB_UNTRASH:
          message = n == 1 ? g_strdup_printf (_("'%s' restored from the Recycle Bin"), job->first_name)
                           : g_strdup_printf (ngettext ("%d item restored from the Recycle Bin",
                                                        "%d items restored from the Recycle Bin", n), n);
          break;
        case JOB_EMPTY:
          message = g_strdup (_("The Recycle Bin is empty"));
          break;
        }
    }
  if (message && undo)
    fm_shell_snackbar (shell, message, _("Undo"), on_undo, g_ptr_array_ref (undo),
                       (GDestroyNotify) g_ptr_array_unref);
  else if (message)
    fm_shell_snackbar (shell, message, NULL, NULL, NULL, NULL);
  fm_shell_reload (shell);
  job_free (job);
}

static Job *
job_new (FmShell *shell, JobKind kind, GPtrArray *sources)
{
  Job *job = g_new0 (Job, 1);

  job->shell = shell;
  job->kind = kind;
  job->sources = sources ? g_ptr_array_ref (sources) : g_ptr_array_new_with_free_func (g_object_unref);
  job->cancellable = g_cancellable_new ();
  job->preparing = TRUE;
  job->answer = job->apply_all = ANSWER_NONE;
  g_mutex_init (&job->lock);
  g_cond_init (&job->cond);
  return job;
}

static void
job_start (Job *job)
{
  g_autoptr(GTask) task = g_task_new (NULL, NULL, on_job_done, job);

  /* the worker's data (on_job_done gets it as its user data, the worker as the task's) */
  g_task_set_task_data (task, job, NULL);
  job->started = g_get_monotonic_time ();
  job->timer = g_timeout_add (TICK_MS, on_tick, job);
  g_task_run_in_thread (task, job_thread);
}

static GPtrArray *
copy_files (GPtrArray *files)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < files->len; i++)
    g_ptr_array_add (copy, g_object_ref (files->pdata[i]));
  return copy;
}

void
fm_jobs_copy (FmShell *shell, GPtrArray *files, GFile *dest, gboolean move)
{
  g_autoptr(GPtrArray) sources = copy_files (files);
  Job *job = job_new (shell, move ? JOB_MOVE : JOB_COPY, sources);

  job->dest = g_object_ref (dest);
  job_start (job);
}

void
fm_jobs_delete (FmShell *shell, GPtrArray *files, gboolean trash)
{
  g_autoptr(GPtrArray) sources = copy_files (files);

  job_start (job_new (shell, trash ? JOB_TRASH : JOB_DELETE, sources));
}

void
fm_jobs_restore (FmShell *shell, GPtrArray *items)
{
  Job *job = job_new (shell, JOB_RESTORE, NULL);

  job->targets = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < items->len; i++)
    {
      FmItem *item = items->pdata[i];

      if (!item->orig_path)
        continue;
      g_ptr_array_add (job->sources, g_object_ref (item->file));
      g_ptr_array_add (job->targets, g_file_new_for_path (item->orig_path));
    }
  job_start (job);
}

void
fm_jobs_empty_trash (FmShell *shell)
{
  job_start (job_new (shell, JOB_EMPTY, NULL));
}
