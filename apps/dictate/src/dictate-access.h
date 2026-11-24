/* dictate-access: what on the phone Agent may use, a switch each in its settings (GSettings vibeos.Dictate access-*,
 * all on at first), and what its model is told of them (dictate-talk.c's system prompt): it has tools for the things
 * that are on (dictate-tools.h); for one that is off it says it has no access to it and that it can be turned on in
 * Agent's settings; for one with no tools yet, that it can't do that yet. */
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
/* whether an area (an access-* key) has tools (dictate_tools_area_has_tools ()) */
typedef gboolean (*DictateAccessHasTools) (const char *area);
/* the prompt's sentences about the phone, from `settings` (vibeos.Dictate): with `has_tools`, what the model can
 * use and how; NULL for a model without tools (the one on the phone), which is told it can't do such things yet */
char                *dictate_access_prompt (GSettings *settings, DictateAccessHasTools has_tools);

G_END_DECLS
