/* DictatePackage: a recogniser's package (vibeos-dictate-vosk) from VibeOS's archive, installed through PackageKit
 * (dictate-pk.h) as App Installer installs one: looked up in the archives the phone has (refreshed once when it is
 * not found), and installed only if it is trusted, that is from a signed archive; polkit asks for the password if it
 * must, in the system's own prompt. The library is code, so it only ever comes this way: signed, through the
 * system's package manager, never fetched by the app itself.
 *
 * Made, it looks itself up at once (its size, for the dialog). "changed" on the main thread at every step. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_PACKAGE_LOOKING_UP,
  DICTATE_PACKAGE_FOUND,        /* in an archive, not installed: its size known */
  DICTATE_PACKAGE_INSTALLING,
  DICTATE_PACKAGE_INSTALLED,    /* already, or now */
  DICTATE_PACKAGE_FAILED,
} DictatePackageState;

#define DICTATE_TYPE_PACKAGE (dictate_package_get_type ())
G_DECLARE_FINAL_TYPE (DictatePackage, dictate_package, DICTATE, PACKAGE, GObject)

DictatePackage      *dictate_package_new          (const char *name);
/* installs it, looking it up again first if that failed */
void                 dictate_package_install      (DictatePackage *self);
/* stops what it is doing (PackageKit may not stop while dpkg runs: then it ends installed) */
void                 dictate_package_cancel       (DictatePackage *self);
DictatePackageState  dictate_package_get_state    (DictatePackage *self);
const char          *dictate_package_get_name     (DictatePackage *self);
/* once found: the version to install, and its download size (0 when PackageKit did not say) */
const char          *dictate_package_get_version  (DictatePackage *self);
guint64              dictate_package_get_size     (DictatePackage *self);
/* while installing: what is happening, for the user ("Waiting for the password"), and how far, 0 to 1 or -1 */
const char          *dictate_package_get_status   (DictatePackage *self);
double               dictate_package_get_fraction (DictatePackage *self);
/* failed: why, for the user */
const char          *dictate_package_get_error    (DictatePackage *self);

G_END_DECLS
