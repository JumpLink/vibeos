/* msg-seed: fills a test rig's private message store and address book with made-up conversations, so that every
 * screen has something to show: runs of messages on both sides with an hour's gap between them (date lines), one
 * unread, one with a failed message, a draft, a link, a sender that is a name and cannot be answered, a number that
 * is no contact, an archived conversation, and enough rows to scroll the list.
 *
 * It runs only inside a test rig: MSG_RIG_DIR set to the rig's directory, XDG_DATA_HOME inside it and a session bus
 * of the rig's own (not the user's), with an evolution-data-server on it; the app then runs with the same three. It
 * fills only a new store and an empty address book, so it cannot write into real ones by mistake. --no-contacts
 * leaves the address book alone (a rig without EDS).
 * The numbers are in the 555-0100 to 555-0199 range, which North America keeps for fiction; the addresses are
 * example.com. Times are counted back from now, so "Today" and "Yesterday" stay true whenever the rig runs. */
#include "sms-store.h"

#include <libebook/libebook.h>
#include <locale.h>

#define MINUTE (60 * G_GINT64_CONSTANT (1000))
#define HOUR (60 * MINUTE)
#define DAY (24 * HOUR)

typedef struct {
  const char *name;             /* NULL: no contact */
  const char *number;           /* canonical, as the service stores it */
  const char *email;
} Person;

static const Person people[] = {
  { "Anna Albrecht", "+12025550101", "anna@example.com" },
  { "Ben Bauer", "+12025550102", NULL },
  { "Clara Conrad", "+12025550103", "clara@example.com" },
  { "Emma Engel", "+12025550105", NULL },
  { "Felix Fischer", "+12025550106", NULL },
  { "Greta Graf", "+12025550107", "greta@example.com" },
  { "Hannah Hoffmann", "+12025550108", NULL },
  { "Jonas Jäger", "+12025550110", NULL },
  { "Karl König", "+12025550111", NULL },
  { "Lena Lange", "+12025550112", NULL },
  { "Max Maier", "+12025550113", NULL },
  { "Nora Neumann", "+12025550114", NULL },
};

/* one message: from them (in) or from here (out), `ago` ms before now */
typedef struct {
  gboolean out;
  gint64 ago;
  const char *text;
} Line;

static gint64 now;

static gint64
add_in (SmsStore *store, const char *sender, gint64 ago, const char *text)
{
  return sms_store_add_incoming (store, sender, text, now - ago, now - ago, NULL);
}

static gint64
add_out (SmsStore *store, gint64 conversation, gint64 ago, const char *text, MessageStatus status)
{
  gint64 id = sms_store_add_outgoing (store, conversation, text, now - ago);

  sms_store_set_status (store, id, status, status == MESSAGE_STATUS_OUTGOING_FAILED ? 1 : 0, now - ago);
  return id;
}

static gint64
conversation_with (SmsStore *store, const char *number)
{
  const char *destinations[] = { number, NULL };

  return sms_store_get_conversation (store, destinations);
}

/* a conversation's lines, oldest first; read unless `unread` */
static gint64
add_lines (SmsStore *store, const char *number, const Line *lines, guint n, gboolean unread)
{
  gint64 conversation = conversation_with (store, number);

  for (guint i = 0; i < n; i++)
    if (lines[i].out)
      add_out (store, conversation, lines[i].ago, lines[i].text, MESSAGE_STATUS_OUTGOING_DELIVERED);
    else
      add_in (store, number, lines[i].ago, lines[i].text);
  if (!unread)
    sms_store_mark_read (store, conversation);
  sms_store_mark_seen (store, conversation);
  return conversation;
}

static gboolean
seed_contacts (GError **error)
{
  g_autoptr(ESourceRegistry) registry = e_source_registry_new_sync (NULL, error);
  g_autoptr(ESource) source = registry ? e_source_registry_ref_builtin_address_book (registry) : NULL;
  g_autoptr(EClient) client = source ? e_book_client_connect_sync (source, 30, NULL, error) : NULL;
  GSList *uids = NULL, *contacts = NULL;
  gboolean ok;

  if (!client)
    return FALSE;
  if (!e_book_client_get_contacts_uids_sync (E_BOOK_CLIENT (client), "", &uids, NULL, error))
    return FALSE;
  if (uids)
    {
      g_slist_free_full (uids, g_free);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "the address book is not empty; it fills only an empty one");
      return FALSE;
    }
  for (guint i = 0; i < G_N_ELEMENTS (people); i++)
    {
      EContact *contact = e_contact_new ();
      EVCardAttribute *tel = e_vcard_attribute_new (NULL, EVC_TEL);

      e_contact_set (contact, E_CONTACT_FULL_NAME, people[i].name);
      e_vcard_attribute_add_param_with_value (tel, e_vcard_attribute_param_new (EVC_TYPE), "CELL");
      e_vcard_append_attribute_with_value (E_VCARD (contact), tel, people[i].number);
      if (people[i].email)
        e_contact_set (contact, E_CONTACT_EMAIL_1, people[i].email);
      contacts = g_slist_prepend (contacts, contact);
    }
  ok = e_book_client_add_contacts_sync (E_BOOK_CLIENT (client), contacts, E_BOOK_OPERATION_FLAG_NONE, NULL, NULL,
                                        error);
  g_slist_free_full (contacts, g_object_unref);
  return ok;
}

int
main (int argc, char **argv)
{
  const char *rig = g_getenv ("MSG_RIG_DIR");
  const char *bus = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  g_autofree char *path = g_build_filename (g_get_user_data_dir (), "vibeos-messaging", "bugle_db", NULL);
  g_autoptr(GError) error = NULL;
  SmsStore *store;
  gint64 id;

  setlocale (LC_ALL, "");
  if (!rig || !*rig || !g_str_has_prefix (g_get_user_data_dir (), rig) || !bus
      || g_str_has_prefix (bus, "unix:path=/run/user/"))
    {
      g_printerr ("msg-seed: only inside a test rig (MSG_RIG_DIR, its XDG dirs and private bus)\n");
      return 2;
    }
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_printerr ("msg-seed: %s exists; it fills only a new store\n", path);
      return 1;
    }
  if (!(argc > 1 && g_str_equal (argv[1], "--no-contacts")) && !seed_contacts (&error))
    {
      g_printerr ("msg-seed: address book: %s\n", error ? error->message : "none");
      return 1;
    }
  store = sms_store_open (path, FALSE, &error);
  if (!store)
    {
      g_printerr ("msg-seed: %s\n", error->message);
      return 1;
    }
  now = g_get_real_time () / 1000;

  /* the oldest first, so the list is newest first as written here bottom up */
  for (guint i = 4; i < G_N_ELEMENTS (people); i++)
    {
      const Line lines[] = {
        { FALSE, (i + 2) * DAY, "Are we still on for the weekend?" },
        { TRUE, (i + 2) * DAY - 5 * MINUTE, "Yes, see you Saturday" },
      };

      add_lines (store, people[i].number, lines, G_N_ELEMENTS (lines), FALSE);
    }
  /* archived */
  {
    const Line lines[] = { { FALSE, 20 * DAY, "Thanks for the book!" } };

    id = add_lines (store, people[3].number, lines, G_N_ELEMENTS (lines), FALSE);
    sms_store_set_archived (store, id, TRUE);
  }
  /* a number that is no contact */
  {
    const Line lines[] = { { FALSE, 3 * DAY, "Your parcel is at the front desk" } };

    add_lines (store, "+12025550177", lines, G_N_ELEMENTS (lines), FALSE);
  }
  /* a name for a sender: no reply possible */
  {
    const Line lines[] = {
      { FALSE, 2 * DAY, "Your code is 482913. It expires in 10 minutes." },
      { FALSE, 26 * HOUR, "Your statement is ready: https://example.com/statement" },
    };

    add_lines (store, "ExampleCo", lines, G_N_ELEMENTS (lines), FALSE);
  }
  /* unread */
  {
    const Line lines[] = {
      { FALSE, 5 * HOUR, "Did you see the game last night?" },
      { FALSE, 5 * HOUR - MINUTE, "That last minute goal!" },
    };

    add_lines (store, people[1].number, lines, G_N_ELEMENTS (lines), TRUE);
  }
  /* a message that could not be sent */
  {
    const Line lines[] = {
      { FALSE, 3 * HOUR, "Can you send me the address?" },
      { FALSE, 3 * HOUR - MINUTE, "And the time" },
    };

    id = add_lines (store, people[2].number, lines, G_N_ELEMENTS (lines), FALSE);
    add_out (store, id, 2 * HOUR + 50 * MINUTE, "Sure", MESSAGE_STATUS_OUTGOING_FAILED);
    add_out (store, id, 2 * HOUR + 49 * MINUTE, "1 Example St", MESSAGE_STATUS_OUTGOING_FAILED);
  }
  /* runs on both sides, a date line between yesterday and today, a link */
  {
    const Line lines[] = {
      { FALSE, 26 * HOUR, "Hi! Are you coming to dinner on Friday?" },
      { TRUE, 26 * HOUR - 2 * MINUTE, "Yes, I'd love to" },
      { TRUE, 26 * HOUR - 2 * MINUTE - 5000, "What time?" },
      { FALSE, 25 * HOUR, "Seven. I'll book the table" },
      { FALSE, 40 * MINUTE, "Here is the place: https://example.com/table" },
      { FALSE, 39 * MINUTE, "It's a short walk from the station" },
      { FALSE, 39 * MINUTE - 10000, "See you there" },
      { TRUE, 30 * MINUTE, "Great, thanks! I'll bring dessert, and maybe a bottle of something nice for the "
                           "two of us to share after dinner" },
    };

    add_lines (store, people[0].number, lines, G_N_ELEMENTS (lines), FALSE);
  }
  sms_store_set_draft (store, conversation_with (store, people[5].number), "Let me check and get back to you");
  sms_store_free (store);
  g_print ("msg-seed: made-up conversations in %s\n", path);
  return 0;
}
