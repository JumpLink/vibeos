/* What libvibeos sets up for the user's language when it is loaded, before an app makes its first widget:
 *  - its own strings' translations: they are in its gettext domain, libvibeos-1 (GETTEXT_PACKAGE), apart from the
 *    app's, which is bound to the installed translations;
 *  - ICU's default locale, the user's (vibeos_locale_id): what an app asks ICU itself (a time zone's or a country's
 *    name, a date pattern) is then in the interface's language, in the Formats region's conventions. */
#include "vibeos-locale.h"

#include <glib/gi18n-lib.h>
#include <unicode/uloc.h>

__attribute__ ((constructor)) static void
vibeos_i18n_init (void)
{
  g_autofree char *locale = vibeos_locale_id ();
  UErrorCode status = U_ZERO_ERROR;

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  uloc_setDefault (locale, &status);
}
