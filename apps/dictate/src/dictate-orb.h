/* DictateOrb: Vibe's orb, what it is doing at a glance, in the blue, violet and magenta of Vibe's icon. Idle it is a
 * still square. While it listens it is a circle in a soft cloud that moves, and swells with the loudness of the
 * voice; while the model answers the halo round it breathes. A ✓ in it is something done, a ✕ in the error colours a
 * command not carried out, a grey disc Vibe with less to offer (no connection, quiet hours, nothing heard), and a
 * caller's initials a call ringing.
 *
 * The cloud moves on the frame clock while it listens, and the clock is off otherwise; with animations off
 * (gtk-enable-animations) it stands still.
 *
 * Beside a bubble of what was said it is smaller (dictate_orb_set_small). It is drawn as large as its box allows, up
 * to its size: on a short screen it shrinks rather than push the text off. */
#pragma once

#include "dictate-engine.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_ORB_REST,
  DICTATE_ORB_LISTENING,
  DICTATE_ORB_WORKING,
  DICTATE_ORB_DONE,
  DICTATE_ORB_REFUSED,
  DICTATE_ORB_DIM,
  DICTATE_ORB_CALLER,
} DictateOrbLook;

#define DICTATE_TYPE_ORB (dictate_orb_get_type ())
G_DECLARE_FINAL_TYPE (DictateOrb, dictate_orb, DICTATE, ORB, GtkWidget)

/* the loudness comes from `engine` (NULL: none, for an orb that never listens) */
GtkWidget *dictate_orb_new       (DictateEngine *engine);
void       dictate_orb_set_engine (DictateOrb *self, DictateEngine *engine);
/* `initials`: the caller's, for DICTATE_ORB_CALLER */
void       dictate_orb_set_look  (DictateOrb *self, DictateOrbLook look, const char *initials);
DictateOrbLook dictate_orb_get_look (DictateOrb *self);
void       dictate_orb_set_small (DictateOrb *self, gboolean small);

G_END_DECLS
