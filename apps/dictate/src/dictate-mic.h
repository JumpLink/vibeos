/* DictateMic: the screen's one action, the large FAB: the microphone to start, the stop square while listening. While
 * it listens, a disc in the primary colour behind it swells and settles with the loudness of the voice it hears. */
#pragma once

#include "dictate-engine.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DICTATE_TYPE_MIC (dictate_mic_get_type ())
G_DECLARE_FINAL_TYPE (DictateMic, dictate_mic, DICTATE, MIC, GtkWidget)

/* the loudness comes from `engine` */
GtkWidget *dictate_mic_new           (DictateEngine *engine);
void       dictate_mic_set_listening (DictateMic *self, gboolean listening);

/* Signals: "clicked" () */

G_END_DECLS
