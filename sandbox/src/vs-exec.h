/* Exec lines: a desktop file's or a D-Bus service's command, run through the sandbox instead.
 *
 * A desktop file's Exec=foo --new %U becomes
 *   Exec=/usr/bin/vibeos-sandbox run --file-forwarding org.example.Foo -- foo --new @@u %U @@
 * GIO still fills in the field codes; the files and URIs between @@ (paths) or @@u (URIs) and @@ are the ones the
 * user opened with the app, which vibeos-sandbox hands to the app through the document portal (the app cannot see
 * them where they are). A D-Bus service's Exec has no field codes, and no forwarding. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the sandboxed Exec line for `exec`, or NULL when it cannot be parsed */
char  *vs_exec_wrap      (const char *exec, const char *launcher, const char *app_id, gboolean forward,
                          GError **error);
/* one argument, quoted for an Exec line when it needs to be (the desktop entry spec's double quotes) */
char  *vs_exec_quote     (const char *arg);

/* the forwarded files in a command line: everything between the markers. Each one goes through `map` (a path or
 * a URI as the app will see it, or NULL to leave it out); the markers go. */
typedef char *(*VsForwardFunc) (const char *arg, gboolean uri, gpointer data);
char **vs_exec_forward   (const char *const *argv, VsForwardFunc map, gpointer data);

G_END_DECLS
