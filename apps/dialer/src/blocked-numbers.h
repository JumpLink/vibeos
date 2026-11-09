/* The blocked numbers: one list for calls and texts, as Android's BlockedNumberContract is (spec: dialer §5.8.5,
 * messaging §4.1-4.2). The Phone and the Messaging both read and change it, and both their services act on it: a call
 * from a blocked number is declined before it rings and logged as "Blocked call"; a text from one is kept, archived,
 * and not notified ("You'll continue to receive messages from this number but won't be notified anymore").
 *
 * It is a small SQLite database, $XDG_DATA_HOME/neo-phone/blocked_numbers.db, with the columns of Android's
 * BlockedNumbers table. Every process that uses it opens it itself; a change made by one comes to the others through
 * a file monitor, as "changed". */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define BLOCKED_TYPE_NUMBERS (blocked_numbers_get_type ())
G_DECLARE_FINAL_TYPE (BlockedNumbers, blocked_numbers, BLOCKED, NUMBERS, GObject)

/* the list in @path, or in its usual place for NULL; made if it is not there yet. "changed" (no arguments) comes
 * whenever this or another process blocked or unblocked a number */
BlockedNumbers *blocked_numbers_new (const char *path);

/* the list in its usual place, one for the whole process (an app's pages share it); not to be unreffed */
BlockedNumbers *blocked_numbers_get_default (void);

/* the number is on the list, in any of the ways it can be written: "030 1234567" is "+49301234567" in Germany
 * (libphonenumber's match, phone_number_equal). @country: where the phone is ("DE"), or NULL for the locale's. A
 * sender that is a name ("Vodafone") matches only itself */
gboolean        blocked_numbers_contains (BlockedNumbers *self, const char *number, const char *country);

/* puts the number on the list (once: a number already on it in another form stays as it was) */
gboolean        blocked_numbers_add (BlockedNumbers *self, const char *number, const char *country, GError **error);

/* takes the number off the list, in whatever form it was put there */
gboolean        blocked_numbers_remove (BlockedNumbers *self, const char *number, const char *country,
                                        GError **error);

/* the numbers as they were blocked, the last one first (the Blocked numbers page's order); free with g_strfreev */
GStrv           blocked_numbers_list (BlockedNumbers *self);

G_END_DECLS
