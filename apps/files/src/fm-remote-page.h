/* The Remote page and adding a remote: the saved remotes, the footer "+ Add a remote location", the Add
 * Remote dialog, the Local Network scan of network:///, and the server dialog for SMB, FTP/FTPS, SFTP and WebDAV. */
#pragma once

#include <gtk/gtk.h>

#include "fm-window.h"

G_BEGIN_DECLS

FmRemotePage *fm_remote_page_new        (FmShell *shell);
void          fm_remote_page_free       (FmRemotePage *page);
GtkWidget    *fm_remote_page_get_widget (FmRemotePage *page);
void          fm_remote_page_refresh    (FmRemotePage *page);
/* the toolbar's +: Add Remote */
void          fm_remote_page_add        (FmRemotePage *page);
/* the toolbar's ✎: a tap on a remote offers Edit, Rename and Remove instead of opening it */
void          fm_remote_page_toggle_edit (FmRemotePage *page);
gboolean      fm_remote_page_is_editing (FmRemotePage *page);

G_END_DECLS
