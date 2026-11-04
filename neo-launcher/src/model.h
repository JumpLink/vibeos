/* What sits where on the home screen: the dock and the pages, kept in ~/.config/neo-launcher/layout.json. */
#pragma once

#include <glib.h>

/* one icon's place: dock >= 0 for a dock slot, else page/index on the home pages */
typedef struct {
  int dock;
  int page;
  int index;
} ModelPos;

#define MODEL_POS_NONE ((ModelPos) { -1, -1, -1 })

typedef struct {
  int        cols, rows, dock_size;
  int        reserved;    /* slots at the top of the first page that widgets take */
  GPtrArray *dock;        /* char* desktop ids */
  GPtrArray *pages;       /* GPtrArray* of char* desktop ids */
  GHashTable *hidden;     /* ids taken off the home screen (still in the drawer) */
} Model;

Model *model_new        (int cols, int rows, int dock_size);
void      model_free       (Model *self);
int       model_per_page   (Model *self);
int       model_capacity   (Model *self, int page);   /* the first page less the widgets' slots */
void      model_set_reserved (Model *self, int slots);
void      model_set_hidden (Model *self, char **hidden);
void      model_load       (Model *self, GHashTable *apps);
gboolean  model_reconcile  (Model *self, GHashTable *apps);
gboolean  model_on_home    (Model *self, const char *id);
gboolean  model_remove_one (Model *self, const char *id, ModelPos from);
void      model_add_shortcut (Model *self, const char *id);
void      model_move       (Model *self, const char *id, ModelPos target, ModelPos from, gboolean copy);
void      model_save       (Model *self);
