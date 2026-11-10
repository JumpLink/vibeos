/* The contacts from evolution-data-server (ct-store.h). */
#include "ct-store.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>

/* --- entries */

static void
entry_free (CtEntry *entry)
{
  g_free (entry->uid);
  g_free (entry->book_uid);
  g_clear_object (&entry->contact);
  for (int i = 0; i < 3; i++)
    {
      g_free (entry->name[i]);
      g_free (entry->sort_key[i]);
    }
  g_clear_pointer (&entry->labels, g_ptr_array_unref);
  g_clear_pointer (&entry->photo, g_bytes_unref);
}

CtEntry *
ct_entry_new (EContact *contact, const char *book_uid)
{
  CtEntry *entry = g_rc_box_new0 (CtEntry);
  EContactPhoto *photo = e_contact_get (contact, E_CONTACT_PHOTO);
  g_autoptr(GPtrArray) phones = ct_contact_get_items (contact, CT_KIND_PHONE);

  entry->uid = e_contact_get (contact, E_CONTACT_UID);
  entry->book_uid = g_strdup (book_uid);
  entry->contact = g_object_ref (contact);
  for (CtNameOrder order = CT_ORDER_PRIMARY; order <= CT_ORDER_ALTERNATIVE; order++)
    {
      g_autofree char *phonetic = ct_contact_phonetic_name (contact);

      entry->name[order] = ct_contact_display_name (contact, order);
      /* the sort key is the phonetic name when there is one (SORT_KEY_PRIMARY, §10.5) */
      entry->sort_key[order] = ct_sort_key (phonetic ? phonetic : entry->name[order]);
      entry->section[order] = ct_section_of (phonetic ? phonetic : entry->name[order]);
      entry->section_rank[order] = ct_section_rank (phonetic ? phonetic : entry->name[order]);
    }
  entry->starred = ct_contact_is_starred (contact);
  entry->labels = ct_contact_dup_labels (contact);
  entry->has_phone = phones->len > 0;
  if (photo && photo->type == E_CONTACT_PHOTO_TYPE_INLINED && photo->data.inlined.length > 0)
    entry->photo = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);
  else if (photo && photo->type == E_CONTACT_PHOTO_TYPE_URI && photo->data.uri)
    {
      /* EDS's file backend keeps big photos as files beside the book */
      g_autofree char *path = g_filename_from_uri (photo->data.uri, NULL, NULL);
      char *bytes = NULL;
      gsize length = 0;

      if (path && g_file_get_contents (path, &bytes, &length, NULL))
        entry->photo = g_bytes_new_take (bytes, length);
    }
  if (photo)
    e_contact_photo_free (photo);
  return entry;
}

CtEntry *
ct_entry_ref (CtEntry *entry)
{
  return g_rc_box_acquire (entry);
}

void
ct_entry_unref (CtEntry *entry)
{
  g_rc_box_release_full (entry, (GDestroyNotify) entry_free);
}

/* --- the store */

typedef struct {
  CtStore *store;               /* weak: the book goes before the store */
  CtBook book;
  ESource *source;
  EBookClient *client;
  EBookClientView *view;
  gboolean complete;            /* its view has delivered its contacts once */
} Book;

struct _CtStore {
  GObject parent;
  GSettings *settings;
  GCancellable *cancellable;
  ESourceRegistry *registry;
  GPtrArray *books;             /* Book */
  GPtrArray *book_infos;        /* CtBook, borrowed from books, for ct_store_get_books */
  GHashTable *entries;          /* uid → CtEntry */
  guint changed_idle;
  gboolean ready;
  /* "My info": EDS's self contact (e_book_client_set_self), kept out of the entries as Android keeps the profile
   * out of the list. @eds_settings is the schema whose "self-contact-uid" e_book_client_is_self reads; NULL when
   * EDS's schemas are not installed, and then there is no profile */
  GSettings *eds_settings;
  EContact *profile;
  char *profile_book;
};

G_DEFINE_FINAL_TYPE (CtStore, ct_store, G_TYPE_OBJECT)

enum { CHANGED, BOOKS_CHANGED, READY, PROFILE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
book_free (Book *book)
{
  if (book->view)
    {
      g_signal_handlers_disconnect_by_data (book->view, book);
      e_book_client_view_stop (book->view, NULL);
      g_object_unref (book->view);
    }
  g_clear_object (&book->client);
  g_clear_object (&book->source);
  g_free (book->book.uid);
  g_free (book->book.name);
  g_free (book);
}

static void
ct_store_dispose (GObject *object)
{
  CtStore *self = CT_STORE (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->changed_idle, g_source_remove);
  g_clear_pointer (&self->books, g_ptr_array_unref);
  g_clear_pointer (&self->book_infos, g_ptr_array_unref);
  if (self->registry)
    g_signal_handlers_disconnect_by_data (self->registry, self);
  g_clear_object (&self->registry);
  g_clear_pointer (&self->entries, g_hash_table_unref);
  g_clear_object (&self->settings);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->profile);
  g_clear_pointer (&self->profile_book, g_free);
  if (self->eds_settings)
    g_signal_handlers_disconnect_by_data (self->eds_settings, self);
  g_clear_object (&self->eds_settings);
  G_OBJECT_CLASS (ct_store_parent_class)->dispose (object);
}

static void
ct_store_class_init (CtStoreClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ct_store_dispose;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
  signals[BOOKS_CHANGED] = g_signal_new ("books-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
  signals[READY] = g_signal_new ("ready", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
  signals[PROFILE_CHANGED] = g_signal_new ("profile-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                           NULL, NULL, G_TYPE_NONE, 0);
}

static void
ct_store_init (CtStore *self)
{
  self->cancellable = g_cancellable_new ();
  self->books = g_ptr_array_new_with_free_func ((GDestroyNotify) book_free);
  self->book_infos = g_ptr_array_new ();
  self->entries = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) ct_entry_unref);
}

/* a burst of changes (an import, a label rename touching fifty contacts) redraws the list once */
static gboolean
on_changed_idle (gpointer data)
{
  CtStore *self = data;

  self->changed_idle = 0;
  g_signal_emit (self, signals[CHANGED], 0);
  return G_SOURCE_REMOVE;
}

static void
queue_changed (CtStore *self)
{
  if (!self->changed_idle)
    self->changed_idle = g_timeout_add (100, on_changed_idle, self);
}

static void
check_ready (CtStore *self)
{
  if (self->ready)
    return;
  for (guint i = 0; i < self->books->len; i++)
    if (!((Book *) self->books->pdata[i])->complete && ((Book *) self->books->pdata[i])->view)
      return;
  self->ready = TRUE;
  g_signal_emit (self, signals[READY], 0);
}

/* the self contact is the profile, not an entry */
static gboolean
is_profile (CtStore *self, EContact *contact)
{
  return self->eds_settings && e_book_client_is_self (contact);
}

static void
set_profile (CtStore *self, EContact *contact, const char *book_uid)
{
  g_set_object (&self->profile, contact);
  g_free (self->profile_book);
  self->profile_book = g_strdup (book_uid);
  g_signal_emit (self, signals[PROFILE_CHANGED], 0);
}

static void
put_contacts (Book *book, const GSList *contacts)
{
  for (const GSList *l = contacts; l; l = l->next)
    {
      CtEntry *entry;

      if (is_profile (book->store, l->data))
        {
          set_profile (book->store, l->data, book->book.uid);
          continue;
        }
      entry = ct_entry_new (l->data, book->book.uid);

      if (entry->uid)
        g_hash_table_replace (book->store->entries, entry->uid, entry);
      else
        ct_entry_unref (entry);
    }
  queue_changed (book->store);
}

static void
on_objects_added (EBookClientView *view, const GSList *contacts, Book *book)
{
  put_contacts (book, contacts);
}

static void
on_objects_modified (EBookClientView *view, const GSList *contacts, Book *book)
{
  put_contacts (book, contacts);
}

static void
on_objects_removed (EBookClientView *view, const GSList *uids, Book *book)
{
  for (const GSList *l = uids; l; l = l->next)
    {
      CtEntry *entry = g_hash_table_lookup (book->store->entries, l->data);
      CtStore *self = book->store;

      if (self->profile && g_strcmp0 (self->profile_book, book->book.uid) == 0
          && g_strcmp0 (e_contact_get_const (self->profile, E_CONTACT_UID), l->data) == 0)
        set_profile (self, NULL, NULL);
      /* the same UID in another book is that book's */
      if (entry && g_strcmp0 (entry->book_uid, book->book.uid) == 0)
        g_hash_table_remove (book->store->entries, l->data);
    }
  queue_changed (book->store);
}

static void
on_view_complete (EBookClientView *view, const GError *error, Book *book)
{
  if (error)
    g_warning ("contacts: %s: %s", book->book.name, error->message);
  book->complete = TRUE;
  check_ready (book->store);
}

static void
on_view (GObject *client, GAsyncResult *result, gpointer data)
{
  Book *book = data;
  g_autoptr(GError) error = NULL;
  EBookClientView *view = NULL;

  if (!e_book_client_get_view_finish (E_BOOK_CLIENT (client), result, &view, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("contacts: no view of %s: %s", book->book.name, error->message);
          book->complete = TRUE;
          check_ready (book->store);
        }
      return;
    }
  book->view = view;
  g_signal_connect (view, "objects-added", G_CALLBACK (on_objects_added), book);
  g_signal_connect (view, "objects-modified", G_CALLBACK (on_objects_modified), book);
  g_signal_connect (view, "objects-removed", G_CALLBACK (on_objects_removed), book);
  g_signal_connect (view, "complete", G_CALLBACK (on_view_complete), book);
  e_book_client_view_start (view, &error);
  if (error)
    {
      g_warning ("contacts: %s: %s", book->book.name, error->message);
      book->complete = TRUE;
      check_ready (book->store);
    }
}

static void
on_connected (GObject *source, GAsyncResult *result, gpointer data)
{
  Book *book = data;
  g_autoptr(GError) error = NULL;
  EClient *client = e_book_client_connect_finish (result, &error);
  /* every contact: the "any field contains nothing" query EDS's own examples use */
  g_autofree char *sexp = NULL;
  g_autoptr(EBookQuery) query = e_book_query_any_field_contains ("");

  if (!client)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("contacts: cannot open %s: %s", book->book.name, error->message);
          book->complete = TRUE;
          check_ready (book->store);
        }
      return;
    }
  book->client = E_BOOK_CLIENT (client);
  book->book.writable = !e_client_is_readonly (client);
  sexp = e_book_query_to_string (query);
  e_book_client_get_view (book->client, sexp, book->store->cancellable, on_view, book);
  g_signal_emit (book->store, signals[BOOKS_CHANGED], 0);
}

static Book *
find_book (CtStore *self, const char *uid)
{
  for (guint i = 0; uid && i < self->books->len; i++)
    if (g_str_equal (((Book *) self->books->pdata[i])->book.uid, uid))
      return self->books->pdata[i];
  return NULL;
}

static void
rebuild_book_infos (CtStore *self)
{
  g_ptr_array_set_size (self->book_infos, 0);
  for (guint i = 0; i < self->books->len; i++)
    g_ptr_array_add (self->book_infos, &((Book *) self->books->pdata[i])->book);
}

/* EDS's own personal address book (on any system: e_source_registry_ref_builtin_address_book), which is the
 * phone's "Device" account */
static gboolean
is_builtin_book (CtStore *self, ESource *source)
{
  g_autoptr(ESource) builtin = self->registry ? e_source_registry_ref_builtin_address_book (self->registry) : NULL;

  return builtin && e_source_equal (builtin, source);
}

static void
add_source (CtStore *self, ESource *source)
{
  Book *book;

  if (find_book (self, e_source_get_uid (source)))
    return;
  book = g_new0 (Book, 1);
  book->store = self;
  book->source = g_object_ref (source);
  book->book.uid = g_strdup (e_source_get_uid (source));
  book->book.name = g_strdup (e_source_get_display_name (source));
  book->book.is_local = is_builtin_book (self, source);
  book->book.writable = TRUE;
  g_ptr_array_add (self->books, book);
  rebuild_book_infos (self);
  self->ready = FALSE;
  e_book_client_connect (source, 30, self->cancellable, on_connected, book);
}

static void
remove_source (CtStore *self, ESource *source)
{
  Book *book = find_book (self, e_source_get_uid (source));
  GHashTableIter iter;
  CtEntry *entry;

  if (!book)
    return;
  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &entry))
    if (g_str_equal (entry->book_uid, book->book.uid))
      g_hash_table_iter_remove (&iter);
  g_ptr_array_remove (self->books, book);
  rebuild_book_infos (self);
  queue_changed (self);
  check_ready (self);
  g_signal_emit (self, signals[BOOKS_CHANGED], 0);
}

static gboolean
is_address_book (ESource *source)
{
  return e_source_has_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK);
}

static void
on_source_enabled (ESourceRegistry *registry, ESource *source, CtStore *self)
{
  if (is_address_book (source))
    add_source (self, source);
}

static void
on_source_gone (ESourceRegistry *registry, ESource *source, CtStore *self)
{
  if (is_address_book (source))
    remove_source (self, source);
}

static void
on_registry (GObject *source, GAsyncResult *result, gpointer data)
{
  CtStore *self = data;
  g_autoptr(GError) error = NULL;
  ESourceRegistry *registry = e_source_registry_new_finish (result, &error);
  GList *sources;

  if (!registry)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("contacts: no evolution-data-server: %s", error->message);
          self->ready = TRUE;
          g_signal_emit (self, signals[READY], 0);
        }
      return;
    }
  self->registry = registry;
  g_signal_connect (registry, "source-enabled", G_CALLBACK (on_source_enabled), self);
  g_signal_connect (registry, "source-added", G_CALLBACK (on_source_enabled), self);
  g_signal_connect (registry, "source-disabled", G_CALLBACK (on_source_gone), self);
  g_signal_connect (registry, "source-removed", G_CALLBACK (on_source_gone), self);
  sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  for (GList *l = sources; l; l = l->next)
    add_source (self, l->data);
  g_list_free_full (sources, g_object_unref);
  if (!self->books->len)
    {
      self->ready = TRUE;
      g_signal_emit (self, signals[READY], 0);
    }
  g_signal_emit (self, signals[BOOKS_CHANGED], 0);
}

/* "self-contact-uid" changed (here, or by another app such as Evolution): the old self contact goes back into the
 * list and the new one comes out of it */
static void
on_self_changed (GSettings *settings, const char *key, CtStore *self)
{
  g_autofree char *uid = g_settings_get_string (settings, "self-contact-uid");
  CtEntry *entry = *uid ? g_hash_table_lookup (self->entries, uid) : NULL;

  if (self->profile && g_strcmp0 (e_contact_get_const (self->profile, E_CONTACT_UID), uid) == 0)
    return;
  if (self->profile)
    {
      CtEntry *old = ct_entry_new (self->profile, self->profile_book);

      g_hash_table_replace (self->entries, old->uid, old);
    }
  if (entry)
    {
      g_autoptr(EContact) contact = g_object_ref (entry->contact);
      g_autofree char *book_uid = g_strdup (entry->book_uid);

      g_hash_table_remove (self->entries, uid);
      set_profile (self, contact, book_uid);
    }
  else
    set_profile (self, NULL, NULL);
  queue_changed (self);
}

CtStore *
ct_store_new (GSettings *settings)
{
  CtStore *self = g_object_new (CT_TYPE_STORE, NULL);

  self->settings = g_object_ref (settings);
  {
    GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
    g_autoptr(GSettingsSchema) schema = schemas ? g_settings_schema_source_lookup (
                                          schemas, "org.gnome.evolution-data-server.addressbook", TRUE) : NULL;

    if (schema && g_settings_schema_has_key (schema, "self-contact-uid"))
      {
        self->eds_settings = g_settings_new ("org.gnome.evolution-data-server.addressbook");
        g_signal_connect (self->eds_settings, "changed::self-contact-uid", G_CALLBACK (on_self_changed), self);
      }
    else
      g_message ("contacts: EDS's schemas are missing, so there is no \"My info\"");
  }
  e_source_registry_new (self->cancellable, on_registry, self);
  return self;
}

gboolean
ct_store_is_ready (CtStore *self)
{
  return self->ready;
}

GPtrArray *
ct_store_dup_entries (CtStore *self)
{
  GPtrArray *entries = g_ptr_array_new_full (g_hash_table_size (self->entries), (GDestroyNotify) ct_entry_unref);
  GHashTableIter iter;
  CtEntry *entry;

  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &entry))
    g_ptr_array_add (entries, ct_entry_ref (entry));
  return entries;
}

CtEntry *
ct_store_lookup (CtStore *self, const char *uid)
{
  return uid ? g_hash_table_lookup (self->entries, uid) : NULL;
}

guint
ct_store_count (CtStore *self)
{
  return g_hash_table_size (self->entries);
}

GPtrArray *
ct_store_get_books (CtStore *self)
{
  return self->book_infos;
}

CtBook *
ct_store_get_book (CtStore *self, const char *uid)
{
  Book *book = find_book (self, uid);

  return book ? &book->book : NULL;
}

const char *
ct_book_label (CtBook *book)
{
  /* the phone's own book is the Device account (DrawerAdapter's account_phone) */
  return book->is_local ? _("Device") : book->name;
}

const char *
ct_store_default_book (CtStore *self)
{
  g_autofree char *chosen = g_settings_get_string (self->settings, "default-account");
  Book *book = find_book (self, chosen);

  if (book && book->client && book->book.writable)
    return book->book.uid;
  if (self->registry)
    {
      g_autoptr(ESource) source = e_source_registry_ref_default_address_book (self->registry);

      if (source && (book = find_book (self, e_source_get_uid (source))) && book->book.writable)
        return book->book.uid;
    }
  for (guint i = 0; i < self->books->len; i++)
    if (((Book *) self->books->pdata[i])->book.writable)
      return ((Book *) self->books->pdata[i])->book.uid;
  return NULL;
}

/* --- writing */

typedef struct {
  CtStore *store;
  CtStoreDone done;
  gpointer data;
  int pending;                  /* for the operations spread over several books */
  GError *error;
  char *uid;
} Op;

static Op *
op_new (CtStore *self, CtStoreDone done, gpointer data)
{
  Op *op = g_new0 (Op, 1);

  op->store = g_object_ref (self);
  op->done = done;
  op->data = data;
  return op;
}

static void
op_finish (Op *op)
{
  if (--op->pending > 0)
    return;
  if (op->done)
    op->done (op->uid, op->error, op->data);
  g_clear_error (&op->error);
  g_free (op->uid);
  g_object_unref (op->store);
  g_free (op);
}

static void
op_error (Op *op, GError *error)
{
  if (error && !op->error)
    op->error = g_error_copy (error);
  if (error)
    g_warning ("contacts: %s", error->message);
}

static void
on_added (GObject *client, GAsyncResult *result, gpointer data)
{
  Op *op = data;
  g_autoptr(GError) error = NULL;
  char *uid = NULL;

  if (e_book_client_add_contact_finish (E_BOOK_CLIENT (client), result, &uid, &error))
    op->uid = uid;
  op_error (op, error);
  op_finish (op);
}

static void
on_modified (GObject *client, GAsyncResult *result, gpointer data)
{
  Op *op = data;
  g_autoptr(GError) error = NULL;

  e_book_client_modify_contact_finish (E_BOOK_CLIENT (client), result, &error);
  op_error (op, error);
  op_finish (op);
}

static GError *
no_book_error (void)
{
  return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("The address book is not available"));
}

/* FN must not be empty (vCard 3.0, and Evolution shows FN): a contact without a name gets its display name there,
 * the phone number or the email address as Android's DISPLAY_NAME does */
static void
ensure_full_name (EContact *contact)
{
  g_autofree char *full = e_contact_get (contact, E_CONTACT_FULL_NAME);

  if (!full || !*full)
    {
      g_autofree char *name = ct_contact_display_name (contact, CT_ORDER_PRIMARY);

      e_contact_set (contact, E_CONTACT_FULL_NAME, name);
    }
}

void
ct_store_save (CtStore *self, const char *book_uid, EContact *contact, gboolean is_new, CtStoreDone done,
               gpointer data)
{
  Book *book = find_book (self, book_uid);
  Op *op = op_new (self, done, data);

  op->pending = 1;
  op->uid = e_contact_get (contact, E_CONTACT_UID);
  if (!book || !book->client)
    {
      g_autoptr(GError) error = no_book_error ();

      op_error (op, error);
      op_finish (op);
      return;
    }
  ensure_full_name (contact);
  if (is_new)
    {
      g_clear_pointer (&op->uid, g_free);
      e_book_client_add_contact (book->client, contact, E_BOOK_OPERATION_FLAG_NONE, self->cancellable, on_added, op);
    }
  else
    e_book_client_modify_contact (book->client, contact, E_BOOK_OPERATION_FLAG_NONE, self->cancellable, on_modified,
                                  op);
}

void
ct_store_modify (CtStore *self, GPtrArray *contacts, CtStoreDone done, gpointer data)
{
  Op *op = op_new (self, done, data);

  /* one write per contact, each to the book it is in; @done runs once they are all through */
  op->pending = 1;
  for (guint i = 0; contacts && i < contacts->len; i++)
    {
      EContact *contact = contacts->pdata[i];
      g_autofree char *uid = e_contact_get (contact, E_CONTACT_UID);
      CtEntry *entry = ct_store_lookup (self, uid);
      Book *book = entry ? find_book (self, entry->book_uid) : NULL;

      if (!book || !book->client)
        continue;
      op->pending++;
      e_book_client_modify_contact (book->client, contact, E_BOOK_OPERATION_FLAG_NONE, self->cancellable,
                                    on_modified, op);
    }
  op_finish (op);
}

static void
on_removed (GObject *client, GAsyncResult *result, gpointer data)
{
  Op *op = data;
  g_autoptr(GError) error = NULL;

  e_book_client_remove_contacts_finish (E_BOOK_CLIENT (client), result, &error);
  op_error (op, error);
  op_finish (op);
}

void
ct_store_delete (CtStore *self, GPtrArray *uids, CtStoreDone done, gpointer data)
{
  Op *op = op_new (self, done, data);

  op->pending = 1;
  /* one call per book with that book's UIDs */
  for (guint b = 0; b < self->books->len; b++)
    {
      Book *book = self->books->pdata[b];
      GSList *list = NULL;

      for (guint i = 0; uids && i < uids->len; i++)
        {
          CtEntry *entry = ct_store_lookup (self, uids->pdata[i]);

          if (entry && g_str_equal (entry->book_uid, book->book.uid))
            list = g_slist_prepend (list, entry->uid);
        }
      if (list && book->client)
        {
          op->pending++;
          e_book_client_remove_contacts (book->client, list, E_BOOK_OPERATION_FLAG_NONE, self->cancellable,
                                         on_removed, op);
        }
      g_slist_free (list);
    }
  op_finish (op);
}

/* a copy of the contact to change and write back (an EContact in the view must not be changed in place) */
static EContact *
editable_copy (CtStore *self, const char *uid)
{
  CtEntry *entry = ct_store_lookup (self, uid);

  return entry ? e_contact_duplicate (entry->contact) : NULL;
}

void
ct_store_set_starred (CtStore *self, const char *uid, gboolean starred)
{
  g_autoptr(EContact) contact = editable_copy (self, uid);
  g_autoptr(GPtrArray) list = g_ptr_array_new ();

  if (!contact)
    return;
  ct_contact_set_starred (contact, starred);
  g_ptr_array_add (list, contact);
  ct_store_modify (self, list, NULL, NULL);
}

static void
on_imported (GObject *client, GAsyncResult *result, gpointer data)
{
  Op *op = data;
  g_autoptr(GError) error = NULL;
  GSList *uids = NULL;

  if (e_book_client_add_contacts_finish (E_BOOK_CLIENT (client), result, &uids, &error))
    {
      int before = op->uid ? atoi (op->uid) : 0;

      g_free (op->uid);
      op->uid = g_strdup_printf ("%u", before + g_slist_length (uids));
    }
  g_slist_free_full (uids, g_free);
  op_error (op, error);
  op_finish (op);
}

void
ct_store_import (CtStore *self, const char *book_uid, GPtrArray *contacts, CtStoreDone done, gpointer data)
{
  Book *book = find_book (self, book_uid);
  Op *op = op_new (self, done, data);
  GSList *list = NULL;

  op->pending = 1;
  op->uid = g_strdup ("0");
  if (!book || !book->client)
    {
      g_autoptr(GError) error = no_book_error ();

      op_error (op, error);
      op_finish (op);
      return;
    }
  for (guint i = contacts->len; i > 0; i--)
    {
      EContact *contact = contacts->pdata[i - 1];

      ensure_full_name (contact);
      list = g_slist_prepend (list, contact);
    }
  if (list)
    {
      op->pending++;
      e_book_client_add_contacts (book->client, list, E_BOOK_OPERATION_FLAG_NONE, self->cancellable, on_imported,
                                  op);
    }
  g_slist_free (list);
  op_finish (op);
}

/* --- labels */

static int
compare_labels (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate (*(const char **) a, *(const char **) b);
}

GPtrArray *
ct_store_dup_labels (CtStore *self)
{
  g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  g_auto(GStrv) made = g_settings_get_strv (self->settings, "labels");
  GPtrArray *labels = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  CtEntry *entry;

  for (int i = 0; made[i]; i++)
    if (*made[i] && g_hash_table_add (seen, made[i]))
      g_ptr_array_add (labels, g_strdup (made[i]));
  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &entry))
    for (guint i = 0; i < entry->labels->len; i++)
      if (g_hash_table_add (seen, entry->labels->pdata[i]))
        g_ptr_array_add (labels, g_strdup (entry->labels->pdata[i]));
  g_ptr_array_sort (labels, compare_labels);
  return labels;
}

gboolean
ct_store_has_label (CtStore *self, const char *label)
{
  g_autoptr(GPtrArray) labels = ct_store_dup_labels (self);

  for (guint i = 0; i < labels->len; i++)
    if (g_str_equal (labels->pdata[i], label))
      return TRUE;
  return FALSE;
}

GPtrArray *
ct_store_label_members (CtStore *self, const char *label)
{
  GPtrArray *uids = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  CtEntry *entry;

  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &entry))
    for (guint i = 0; i < entry->labels->len; i++)
      if (g_str_equal (entry->labels->pdata[i], label))
        {
          g_ptr_array_add (uids, g_strdup (entry->uid));
          break;
        }
  return uids;
}

/* the labels made here, so that one outlives its last member */
static void
remember_label (CtStore *self, const char *label, const char *forget)
{
  g_auto(GStrv) made = g_settings_get_strv (self->settings, "labels");
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  g_auto(GStrv) out = NULL;

  for (int i = 0; made[i]; i++)
    if (g_strcmp0 (made[i], forget) != 0 && g_strcmp0 (made[i], label) != 0)
      g_strv_builder_add (builder, made[i]);
  if (label)
    g_strv_builder_add (builder, label);
  out = g_strv_builder_end (builder);
  g_settings_set_strv (self->settings, "labels", (const char *const *) out);
}

void
ct_store_create_label (CtStore *self, const char *label)
{
  remember_label (self, label, NULL);
  queue_changed (self);
}

typedef gboolean (*LabelEdit) (EContact *contact, const char *a, const char *b);

static gboolean
edit_add (EContact *contact, const char *label, const char *unused)
{
  return ct_contact_add_label (contact, label);
}

static gboolean
edit_remove (EContact *contact, const char *label, const char *unused)
{
  return ct_contact_remove_label (contact, label);
}

static gboolean
edit_rename (EContact *contact, const char *from, const char *to)
{
  return ct_contact_rename_label (contact, from, to);
}

/* changes the contacts @uids with @edit, and writes back the ones it changed */
static void
edit_contacts (CtStore *self, GPtrArray *uids, LabelEdit edit, const char *a, const char *b)
{
  g_autoptr(GPtrArray) changed = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < uids->len; i++)
    {
      EContact *contact = editable_copy (self, uids->pdata[i]);

      if (contact && edit (contact, a, b))
        g_ptr_array_add (changed, contact);
      else if (contact)
        g_object_unref (contact);
    }
  if (changed->len)
    ct_store_modify (self, changed, NULL, NULL);
}

void
ct_store_rename_label (CtStore *self, const char *from, const char *to)
{
  g_autoptr(GPtrArray) members = ct_store_label_members (self, from);

  remember_label (self, to, from);
  edit_contacts (self, members, edit_rename, from, to);
  queue_changed (self);
}

GPtrArray *
ct_store_delete_label (CtStore *self, const char *label)
{
  GPtrArray *members = ct_store_label_members (self, label);

  remember_label (self, NULL, label);
  edit_contacts (self, members, edit_remove, label, NULL);
  queue_changed (self);
  return members;
}

void
ct_store_add_to_label (CtStore *self, GPtrArray *uids, const char *label)
{
  remember_label (self, label, NULL);
  edit_contacts (self, uids, edit_add, label, NULL);
}

void
ct_store_remove_from_label (CtStore *self, GPtrArray *uids, const char *label)
{
  /* the label stays when its last member goes */
  remember_label (self, label, NULL);
  edit_contacts (self, uids, edit_remove, label, NULL);
}

/* --- "My info" */

EContact *
ct_store_get_profile (CtStore *self)
{
  return self->profile;
}

gboolean
ct_store_can_have_profile (CtStore *self)
{
  return self->eds_settings != NULL;
}

/* a new profile is added, then made the self contact */
static void
on_profile_added (GObject *client, GAsyncResult *result, gpointer data)
{
  Op *op = data;
  g_autoptr(GError) error = NULL;
  char *uid = NULL;

  if (e_book_client_add_contact_finish (E_BOOK_CLIENT (client), result, &uid, &error))
    {
      g_autoptr(EContact) marker = e_contact_new ();

      /* e_book_client_set_self only reads the UID */
      e_contact_set (marker, E_CONTACT_UID, uid);
      e_book_client_set_self (E_BOOK_CLIENT (client), marker, &error);
      op->uid = g_strdup (CT_PROFILE_UID);
      g_free (uid);
    }
  op_error (op, error);
  op_finish (op);
}

void
ct_store_save_profile (CtStore *self, EContact *profile, CtStoreDone done, gpointer data)
{
  Op *op = op_new (self, done, data);
  Book *book = NULL;

  op->pending = 1;
  ensure_full_name (profile);
  if (self->profile)
    {
      /* over the self contact, in its book */
      book = find_book (self, self->profile_book);
      e_contact_set (profile, E_CONTACT_UID, e_contact_get_const (self->profile, E_CONTACT_UID));
      op->uid = g_strdup (CT_PROFILE_UID);
      if (book && book->client)
        {
          e_book_client_modify_contact (book->client, profile, E_BOOK_OPERATION_FLAG_NONE, self->cancellable,
                                        on_modified, op);
          return;
        }
    }
  else if (self->eds_settings)
    {
      /* a new one goes to EDS's own personal book, as Android's profile is the device's (else to the default) */
      g_autoptr(ESource) builtin = self->registry ? e_source_registry_ref_builtin_address_book (self->registry)
                                                  : NULL;

      book = builtin ? find_book (self, e_source_get_uid (builtin)) : NULL;
      if (!book || !book->client || !book->book.writable)
        book = find_book (self, ct_store_default_book (self));
      e_contact_set (profile, E_CONTACT_UID, NULL);
      if (book && book->client)
        {
          e_book_client_add_contact (book->client, profile, E_BOOK_OPERATION_FLAG_NONE, self->cancellable,
                                     on_profile_added, op);
          return;
        }
    }
  {
    g_autoptr(GError) error = no_book_error ();

    op_error (op, error);
    op_finish (op);
  }
}
