/* Exec lines (vs-exec.h) */
#include "vs-exec.h"

#include <string.h>

/* the characters the desktop entry spec reserves: an argument with one of them is quoted */
static const char reserved[] = " \t\n\"'\\><~|&;$*?#()`";

char *
vs_exec_quote (const char *arg)
{
  if (*arg && !strpbrk (arg, reserved))
    return g_strdup (arg);
  GString *s = g_string_new ("\"");
  for (const char *c = arg; *c; c++) {
    /* inside double quotes these four take a backslash */
    if (strchr ("\"`$\\", *c))
      g_string_append_c (s, '\\');
    g_string_append_c (s, *c);
  }
  g_string_append_c (s, '"');
  return g_string_free (s, FALSE);
}

static gboolean
is_code (const char *arg, const char *codes)
{
  return arg[0] == '%' && arg[1] && strchr (codes, arg[1]) && !arg[2];
}

char *
vs_exec_wrap (const char *exec, const char *launcher, const char *app_id, gboolean forward, GError **error)
{
  g_auto (GStrv) argv = NULL;
  if (!g_shell_parse_argv (exec, NULL, &argv, error))
    return NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (out, vs_exec_quote (launcher));
  g_ptr_array_add (out, g_strdup ("run"));
  if (forward)
    g_ptr_array_add (out, g_strdup ("--file-forwarding"));
  g_ptr_array_add (out, vs_exec_quote (app_id));
  g_ptr_array_add (out, g_strdup ("--"));
  for (guint i = 0; argv[i]; i++) {
    const char *a = argv[i];
    /* deprecated field codes: GIO drops them too */
    if (is_code (a, "dDnNvm"))
      continue;
    if (forward && is_code (a, "fF")) {
      g_ptr_array_add (out, g_strdup ("@@"));
      g_ptr_array_add (out, g_strdup (a));
      g_ptr_array_add (out, g_strdup ("@@"));
    } else if (forward && is_code (a, "uU")) {
      g_ptr_array_add (out, g_strdup ("@@u"));
      g_ptr_array_add (out, g_strdup (a));
      g_ptr_array_add (out, g_strdup ("@@"));
    } else if (a[0] == '%' && a[1] && !a[2]) {
      /* any other field code stays bare, for GIO */
      g_ptr_array_add (out, g_strdup (a));
    } else {
      g_ptr_array_add (out, vs_exec_quote (a));
    }
  }
  g_ptr_array_add (out, NULL);
  char *line = g_strjoinv (" ", (char **) out->pdata);
  g_ptr_array_unref (out);
  return line;
}

char **
vs_exec_forward (const char *const *argv, VsForwardFunc map, gpointer data)
{
  GPtrArray *out = g_ptr_array_new ();
  gboolean in = FALSE, uri = FALSE;
  for (guint i = 0; argv && argv[i]; i++) {
    const char *a = argv[i];
    if (!in && (!strcmp (a, "@@") || !strcmp (a, "@@u"))) {
      in = TRUE;
      uri = a[2] == 'u';
      continue;
    }
    if (in && !strcmp (a, "@@")) {
      in = FALSE;
      continue;
    }
    if (in) {
      char *mapped = map ? map (a, uri, data) : g_strdup (a);
      if (mapped)
        g_ptr_array_add (out, mapped);
    } else {
      g_ptr_array_add (out, g_strdup (a));
    }
  }
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}
