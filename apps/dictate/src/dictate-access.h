/* dictate-access: what on the phone Agent may use, a switch each in its settings (GSettings vibeos.Dictate access-*,
 * all on at first), and what its model is told of them (dictate-talk.c's system prompt): it has tools for the things
 * that are on; for one that is off it says it has no access to it and that it can be turned on in Agent's settings. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  const char *key;      /* its GSettings key: "access-calls" */
  const char *title;    /* the settings row's: "Phone calls" */
  const char *named;    /* in the model's prompt: "calls" */
} DictateAccess;

/* in the settings' order, which is the prompt's */
const DictateAccess *dictate_access_list   (guint *n);
/* the prompt's sentences about the tools, from `settings` (vibeos.Dictate) */
char                *dictate_access_prompt (GSettings *settings);

G_END_DECLS
