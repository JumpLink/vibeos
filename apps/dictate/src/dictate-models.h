/* dictate-models: the languages Dictate hears and the speech models installed for them.
 *
 * A model is a directory in vibeos-dictate/models under the user's data directory or one of the system's, named as its
 * recogniser names its downloads: Moonshine's tiny-streaming-en, small-streaming-de, ...; Vosk's
 * vosk-model-small-ru-0.22, or vosk-small-ru. Its language is the part of the name that is a language's code, its
 * recogniser what the directory holds. Where a language has several, the one used is the first of: Moonshine's tiny,
 * small, base and medium streaming models, then Vosk's; the user's before the system's. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_MOONSHINE,    /* punctuation and capitals of its own */
  DICTATE_VOSK,         /* lower case, no punctuation */
} DictateKind;

typedef struct {
  const char *code;     /* ISO 639-1: "de" */
  const char *name;     /* the language's name for itself: "Deutsch" */
} DictateLanguage;

typedef struct {
  char *language;       /* its code */
  char *dir;
  DictateKind kind;
  guint arch;           /* Moonshine's model architecture (moonshine-api.h); 0 for Vosk */
} DictateModel;

/* the languages Dictate knows, in the order the language picker lists them (by their names) */
const DictateLanguage *dictate_languages        (guint *n);
const DictateLanguage *dictate_language_lookup  (const char *code);

/* the model for each language that has one, in dictate_languages' order */
GPtrArray             *dictate_models_installed (void);
/* the model for `code`, or NULL */
DictateModel          *dictate_model_find       (const char *code);
DictateModel          *dictate_model_copy       (const DictateModel *model);
void                   dictate_model_free       (DictateModel *model);

/* what to listen in when the user has not chosen: the first of the user's languages with a model, else English, else
 * the first language with one; NULL when there is no model at all */
char                  *dictate_language_default (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DictateModel, dictate_model_free)

G_END_DECLS
