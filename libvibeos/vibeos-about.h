/* vibeos-about: the About sheet, the same in every VibeOS app. It looks like GNOME's About (libadwaita's AdwAboutDialog
 * on a phone) and is built from libvibeos's own parts: a VibeosSheet, VibeosCardList rows, libspongy's scrolling.
 *
 * At the top the app's icon, large, its name, "VibeOS Project" and the version of the VibeOS release the app came in
 * (vibeos_release). Under them rows: Website (vibeos.top) and Report an Issue (the project's issues on GitHub) open in
 * the browser; Credits and Legal open pages in the sheet. Credits names the VibeOS Project, then whatever the app
 * acknowledges; Legal says VibeOS is under the MIT License, then the app's own notes (a data source's licence).
 *
 *   static const VibeosAboutSection legal[] = {
 *     { "Open-Meteo", "Forecasts by <a href=\"https://open-meteo.com/\">Open-Meteo.com</a>, under CC BY 4.0." },
 *     { NULL }
 *   };
 *   const VibeosAboutInfo info = { .name = "Weather", .icon_name = "vibeos.Weather", .legal = legal };
 *
 *   vibeos_about_present (button, &info);
 *
 * The texts are Pango markup; a link in one opens in the browser. Nothing in `info` has to outlive the call. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* one group on the Credits or Legal page: a header and a card with the text */
typedef struct {
  const char *title;            /* the header: a project, a data source, the people thanked */
  const char *text;             /* Pango markup */
} VibeosAboutSection;

typedef struct {
  const char               *name;        /* the app's name, "Weather" */
  const char               *icon_name;   /* its icon, "vibeos.Weather" */
  const VibeosAboutSection *credits;     /* what the app acknowledges, after the VibeOS Project; NULL for nothing */
  const VibeosAboutSection *legal;       /* the app's own notes, after VibeOS's licence; NULL for none */
  /* each list ends with a section whose title is NULL */
} VibeosAboutInfo;

/* shows the About sheet over the window `parent` is in; returns the sheet (a VibeosSheet, which destroys itself once
 * closed) */
GtkWidget  *vibeos_about_present (GtkWidget *parent, const VibeosAboutInfo *info);

/* the VibeOS release libvibeos came in, as the About sheet shows it ("0.1.1"): the version of debian/changelog's
 * newest entry when it was built, without the epoch. "" if that was not known. */
const char *vibeos_release       (void);

G_END_DECLS
