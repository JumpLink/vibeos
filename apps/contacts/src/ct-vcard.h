/* .vcf files (spec §5.4, §10.8): the vCards in a file, and contacts written out as one.
 *
 * Import takes vCard 2.1 (Android's own export: CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE), 3.0 and 4.0; EVCard
 * parses all three. Export writes vCard 3.0, which is what evolution-data-server stores: EVCard cannot write 2.1
 * (Android's default, config_export_vcard_type "default"), and Android reads 3.0 back fine.
 *
 * A few properties are spelled differently by Android and EDS, and are converted both ways: Skype's
 * X-SKYPE-USERNAME (Android) is X-SKYPE (EDS). */
#pragma once

#include <libebook-contacts/libebook-contacts.h>

G_BEGIN_DECLS

/* the vCards in a file's text, in order, as new EContacts (an array that unrefs them); what is not a vCard is
 * skipped */
GPtrArray *ct_vcard_parse (const char *text, gssize length);
/* a copy of @contact ready to add to an address book: without its UID and REV (the book gives it new ones, so an
 * import twice adds two copies, as Android's does), and with Android's property names made EDS's */
EContact  *ct_vcard_for_import (EContact *contact);
/* @contacts (EContact) as one .vcf text, vCard 3.0, CRLF line ends, each card with Android's spellings added */
char      *ct_vcard_export (GPtrArray *contacts);
/* the file name to share or export under: the contact's name, or "contacts" for several (Android's
 * "<display name>.vcf" / contacts.vcf); characters a file name cannot have become "_" */
char      *ct_vcard_file_name (const char *display_name);

G_END_DECLS
