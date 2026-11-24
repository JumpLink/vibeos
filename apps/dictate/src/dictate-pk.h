/* dictate-pk: one PackageKit transaction over D-Bus (org.freedesktop.PackageKit on the system bus), as App Installer
 * runs them (apps/installer/src/inst-pk.c): only GDBus, no libpackagekit-glib2.
 *
 * A transaction asks PackageKit for a transaction object, sets its hints (interactive: polkit may ask for the
 * password through the shell's authentication agent, which is the system's own prompt), calls the method and
 * follows the object's signals and properties until Finished. `changed` runs on each progress, package or status;
 * `finished` once at the end, also when PackageKit cannot be reached (DICTATE_PK_ERROR_NO_PACKAGEKIT) or refuses
 * the call. Freeing a transaction stops following it; it does not cancel it in PackageKit. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* PackageKit's transaction flags and filters (pk_bitfield_value of PK_TRANSACTION_FLAG_ENUM_ / PK_FILTER_ENUM_) */
#define DICTATE_PK_FLAG_ONLY_TRUSTED     ((guint64) 1 << 1)
#define DICTATE_PK_FILTER_NOT_INSTALLED  ((guint64) 1 << 3)
#define DICTATE_PK_FILTER_NEWEST         ((guint64) 1 << 16)
#define DICTATE_PK_FILTER_ARCH           ((guint64) 1 << 18)

/* PkInfoEnum: the Package signal's */
enum {
  DICTATE_PK_INFO_INSTALLED = 1,
  DICTATE_PK_INFO_AVAILABLE = 2,
};

/* PkStatusEnum: the ones told to the user */
enum {
  DICTATE_PK_STATUS_WAIT = 1,
  DICTATE_PK_STATUS_SETUP = 2,
  DICTATE_PK_STATUS_QUERY = 4,
  DICTATE_PK_STATUS_REFRESH_CACHE = 7,
  DICTATE_PK_STATUS_DOWNLOAD = 8,
  DICTATE_PK_STATUS_INSTALL = 9,
  DICTATE_PK_STATUS_DEP_RESOLVE = 13,
  DICTATE_PK_STATUS_SIG_CHECK = 14,
  DICTATE_PK_STATUS_COMMIT = 16,
  DICTATE_PK_STATUS_FINISHED = 18,
  DICTATE_PK_STATUS_WAITING_FOR_LOCK = 30,
  DICTATE_PK_STATUS_WAITING_FOR_AUTH = 31,
};

/* PkExitEnum */
enum {
  DICTATE_PK_EXIT_SUCCESS = 1,
  DICTATE_PK_EXIT_FAILED = 2,
  DICTATE_PK_EXIT_CANCELLED = 3,
};

/* PkErrorEnum, the ones told apart, and two of Dictate's own */
enum {
  DICTATE_PK_ERROR_NONE = 0,
  DICTATE_PK_ERROR_NO_NETWORK = 2,
  DICTATE_PK_ERROR_PACKAGE_NOT_FOUND = 8,
  DICTATE_PK_ERROR_PACKAGE_ALREADY_INSTALLED = 9,
  DICTATE_PK_ERROR_PACKAGE_DOWNLOAD_FAILED = 10,
  DICTATE_PK_ERROR_DEP_RESOLUTION_FAILED = 13,
  DICTATE_PK_ERROR_TRANSACTION_CANCELLED = 17,
  DICTATE_PK_ERROR_NO_CACHE = 18,
  DICTATE_PK_ERROR_CANNOT_GET_LOCK = 26,
  DICTATE_PK_ERROR_BAD_GPG_SIGNATURE = 30,
  DICTATE_PK_ERROR_MISSING_GPG_SIGNATURE = 31,
  DICTATE_PK_ERROR_REPO_NOT_AVAILABLE = 37,
  DICTATE_PK_ERROR_NO_MORE_MIRRORS_TO_TRY = 43,
  DICTATE_PK_ERROR_NO_SPACE_ON_DEVICE = 46,
  DICTATE_PK_ERROR_NOT_AUTHORIZED = 48,
  DICTATE_PK_ERROR_CANNOT_INSTALL_REPO_UNSIGNED = 50,
  DICTATE_PK_ERROR_CANNOT_FETCH_SOURCES = 64,
  DICTATE_PK_ERROR_NO_PACKAGEKIT = 1000,        /* PackageKit cannot be reached */
  DICTATE_PK_ERROR_REFUSED = 1001,              /* PackageKit refused the call itself (the D-Bus error says why) */
};

typedef struct {
  guint info;           /* DICTATE_PK_INFO_... */
  char *id;             /* "name;version;arch;data" */
  char *name, *version, *arch, *data;
} DictatePkPackage;

typedef struct _DictatePkTx DictatePkTx;
typedef void (*DictatePkFunc) (DictatePkTx *tx, gpointer data);

DictatePkTx *dictate_pk_tx_new  (DictatePkFunc changed, DictatePkFunc finished, gpointer data);
void         dictate_pk_tx_free (DictatePkTx *tx);

void dictate_pk_tx_resolve          (DictatePkTx *tx, guint64 filters, const char *name);
void dictate_pk_tx_get_details      (DictatePkTx *tx, const char *package_id);
void dictate_pk_tx_refresh_cache    (DictatePkTx *tx);
void dictate_pk_tx_install_packages (DictatePkTx *tx, guint64 flags, const char *package_id);
/* asks PackageKit to stop (it may not, while dpkg runs) */
void dictate_pk_tx_cancel           (DictatePkTx *tx);

gboolean    dictate_pk_tx_is_finished     (DictatePkTx *tx);
guint       dictate_pk_tx_get_exit        (DictatePkTx *tx);
/* the first error PackageKit reported (DICTATE_PK_ERROR_...), and its detail text */
guint       dictate_pk_tx_get_error_code  (DictatePkTx *tx);
const char *dictate_pk_tx_get_error_details (DictatePkTx *tx);
guint       dictate_pk_tx_get_status      (DictatePkTx *tx);
/* 0 to 100, or 101 when PackageKit does not know */
guint       dictate_pk_tx_get_percentage  (DictatePkTx *tx);
/* DictatePkPackage *, in the order PackageKit sent them */
GPtrArray  *dictate_pk_tx_get_packages    (DictatePkTx *tx);
/* the download size GetDetails gave (bytes), or 0 */
guint64     dictate_pk_tx_get_download_size (DictatePkTx *tx);

void dictate_pk_package_free (DictatePkPackage *package);

G_END_DECLS
