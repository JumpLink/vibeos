/* dictate-recogniser: the speech recognisers Dictate runs, behind one interface, driven by dictate-engine.c.
 *
 * Neither is part of VibeOS. Each is a library loaded at run time from LIBRARY_DIR (else from the system's
 * library path), once, and kept: Moonshine's (dictate-moonshine.c; English, German, Spanish) and Vosk's
 * (dictate-vosk.c; Russian, French, Italian). Their models are found by dictate-models.c. All of it but `installed`
 * runs on the engine's worker thread, one stream at a time. */
#pragma once

#include "dictate-models.h"

G_BEGIN_DECLS

#define DICTATE_RATE 16000      /* the samples' rate */

typedef struct _DictateRecogniser DictateRecogniser;    /* a model, loaded */
typedef struct _DictateStream DictateStream;            /* a stretch of speech, being heard */

typedef struct {
  const char *name;     /* for the log */
  const char *package;  /* the Debian package with the library, for the user */
  /* the library is there to load */
  gboolean            (*installed) (void);
  /* the library loaded (the first time) and `model`; NULL with what went wrong, for the log */
  DictateRecogniser * (*load)      (const DictateModel *model, char **error);
  void                (*unload)    (DictateRecogniser *recogniser);
  DictateStream *     (*begin)     (DictateRecogniser *recogniser, char **error);
  /* `n` more samples of the speech (mono, -1..1). The sentences it finished since the last call go onto `finished`,
   * each a new string, trimmed and not empty; *partial is set to a new string, the one being spoken as heard so far
   * ("" for none). */
  void                (*hear)      (DictateStream *stream, const float *samples, guint n, GPtrArray *finished,
                                    char **partial);
  /* the speech is over: its last sentences onto `finished`, and the stream freed */
  void                (*end)       (DictateStream *stream, GPtrArray *finished);
} DictateRecogniserClass;

extern const DictateRecogniserClass dictate_moonshine;
extern const DictateRecogniserClass dictate_vosk;

static inline const DictateRecogniserClass *
dictate_recogniser_class (DictateKind kind)
{
  return kind == DICTATE_VOSK ? &dictate_vosk : &dictate_moonshine;
}

/* library: "libvosk.so", the path LIBRARY_DIR/library if it is there, else library itself, for dlopen's search */
char     *dictate_library_path      (const char *library);
gboolean  dictate_library_installed (const char *library);

G_END_DECLS
