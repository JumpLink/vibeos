/* dictate-output: where Agent's voice would play now: in headphones or earbuds (wired, or Bluetooth), or out loud on
 * a speaker, from the sound server's default output (PipeWire's PulseAudio side). For private mode: only through
 * headphones, and on the speaker short answers with no names or message texts.
 *
 * DICTATE_TEST_ROUTE ("speaker" or "headphones") says it instead, for the tests. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* TRUE when the default output is headphones (*sink: its name, for the playback to keep to it); FALSE on a speaker,
 * or when the sound server cannot be asked within a second. Blocks: not on the main thread */
gboolean dictate_output_headphones (char **sink);

G_END_DECLS
