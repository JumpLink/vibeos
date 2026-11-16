/* Settings, with its sub-screens: About Contacts, "Contacts to display"
 *, and the import and export dialogs. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *ct_settings_new (GtkWidget *window);

/* the .vcf import, as Settings' Import and a .vcf file opened with the app start it: the cards of @file into the
 * default address book, a toast when done */
void       ct_import_file  (GtkWidget *window, GFile *file);
/* the contacts @uids (strings) as one .vcf file, wherever the person saves it: Settings' "Export", and Share */
void       ct_export_contacts (GtkWidget *window, GPtrArray *uids, const char *title);
/* Import and Export as Settings, Organize and the account sheet offer them: the dialog asking from where (to where),
 * over @parent, then the file chooser */
void       ct_import_contacts (GtkWidget *window, GtkWidget *parent);
void       ct_export_all      (GtkWidget *window, GtkWidget *parent);

G_END_DECLS
