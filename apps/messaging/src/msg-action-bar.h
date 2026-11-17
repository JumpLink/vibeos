/* Messaging's top bar, 64 px tall: a back arrow at the start when the screen has one, the title, then action icons
 * and the overflow ⋮ at the end, every button a 48 px target. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @up: with the up arrow. Its "Back" label is what the shell's back gesture presses */
GtkWidget *msg_action_bar_new (gboolean up, const char *title);
GtkWidget *msg_action_bar_get_up (GtkWidget *bar);
GtkWidget *msg_action_bar_get_title (GtkWidget *bar);
void       msg_action_bar_set_title (GtkWidget *bar, const char *title);
/* an action icon, before the overflow; @label is its accessible name */
GtkWidget *msg_action_bar_add_action (GtkWidget *bar, const char *icon, const char *label);
/* select mode's X at the start (the action mode's close button), "Back" to the shell; @label describes it */
GtkWidget *msg_action_bar_add_close (GtkWidget *bar, const char *label);
/* the overflow ⋮, last */
GtkWidget *msg_action_bar_add_overflow (GtkWidget *bar);
/* a widget in the title's place (the picker's recipient field) */
void       msg_action_bar_set_custom (GtkWidget *bar, GtkWidget *custom);
/* the box the actions are in, at the bar's end */
GtkWidget *msg_action_bar_get_end (GtkWidget *bar);

G_END_DECLS
