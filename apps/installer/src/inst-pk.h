/* inst-pk: one PackageKit transaction over D-Bus (org.freedesktop.PackageKit on the system bus), the stock API: no
 * libpackagekit-glib2, only GDBus.
 *
 *   tx = inst_pk_tx_new (on_changed, on_finished, self);
 *   inst_pk_tx_install_files (tx, INST_PK_FLAG_SIMULATE, path);
 *
 * The transaction asks PackageKit for a transaction object, sets its hints (interactive: polkit may ask for the
 * password, through the session's authentication agent), calls the method and follows the object's signals and
 * properties until Finished. `changed` runs on each progress, package or status; `finished` once, at the end, also
 * when PackageKit cannot be reached (error_code INST_PK_ERROR_NO_PACKAGEKIT) or refuses the call. Freeing a
 * transaction stops following it; it does not cancel it in PackageKit.
 *
 * With the demo on (inst-demo.h), nothing reaches PackageKit: each method plays a script instead. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* PackageKit's transaction flags (pk_bitfield_value (PK_TRANSACTION_FLAG_ENUM_...)) */
#define INST_PK_FLAG_NONE             ((guint64) 0)
#define INST_PK_FLAG_ONLY_TRUSTED     ((guint64) 1 << 1)
#define INST_PK_FLAG_SIMULATE         ((guint64) 1 << 2)
#define INST_PK_FLAG_ALLOW_REINSTALL  ((guint64) 1 << 4)
#define INST_PK_FLAG_ALLOW_DOWNGRADE  ((guint64) 1 << 6)

/* its filters (pk_bitfield_value (PK_FILTER_ENUM_...)) */
#define INST_PK_FILTER_NONE           ((guint64) 1 << 1)
#define INST_PK_FILTER_INSTALLED      ((guint64) 1 << 2)
#define INST_PK_FILTER_NOT_INSTALLED  ((guint64) 1 << 3)
#define INST_PK_FILTER_NEWEST         ((guint64) 1 << 16)
#define INST_PK_FILTER_ARCH           ((guint64) 1 << 18)

/* PkInfoEnum: what a Package signal says is happening to a package */
enum {
  INST_PK_INFO_INSTALLED = 1,
  INST_PK_INFO_AVAILABLE = 2,
  INST_PK_INFO_DOWNLOADING = 10,
  INST_PK_INFO_UPDATING = 11,
  INST_PK_INFO_INSTALLING = 12,
  INST_PK_INFO_REMOVING = 13,
  INST_PK_INFO_OBSOLETING = 15,
  INST_PK_INFO_REINSTALLING = 19,
  INST_PK_INFO_DOWNGRADING = 20,
};

/* PkStatusEnum */
enum {
  INST_PK_STATUS_UNKNOWN = 0,
  INST_PK_STATUS_WAIT = 1,
  INST_PK_STATUS_SETUP = 2,
  INST_PK_STATUS_RUNNING = 3,
  INST_PK_STATUS_QUERY = 4,
  INST_PK_STATUS_INFO = 5,
  INST_PK_STATUS_REMOVE = 6,
  INST_PK_STATUS_REFRESH_CACHE = 7,
  INST_PK_STATUS_DOWNLOAD = 8,
  INST_PK_STATUS_INSTALL = 9,
  INST_PK_STATUS_UPDATE = 10,
  INST_PK_STATUS_CLEANUP = 11,
  INST_PK_STATUS_DEP_RESOLVE = 13,
  INST_PK_STATUS_SIG_CHECK = 14,
  INST_PK_STATUS_TEST_COMMIT = 15,
  INST_PK_STATUS_COMMIT = 16,
  INST_PK_STATUS_FINISHED = 18,
  INST_PK_STATUS_CANCEL = 19,
  INST_PK_STATUS_DOWNLOAD_REPOSITORY = 20,
  INST_PK_STATUS_DOWNLOAD_UPDATEINFO = 25,
  INST_PK_STATUS_LOADING_CACHE = 27,
  INST_PK_STATUS_WAITING_FOR_LOCK = 30,
  INST_PK_STATUS_WAITING_FOR_AUTH = 31,
  INST_PK_STATUS_RUN_HOOK = 36,
};

/* PkExitEnum */
enum {
  INST_PK_EXIT_UNKNOWN = 0,
  INST_PK_EXIT_SUCCESS = 1,
  INST_PK_EXIT_FAILED = 2,
  INST_PK_EXIT_CANCELLED = 3,
};

/* PkErrorEnum (the ones App Installer tells apart), and two of App Installer's own */
enum {
  INST_PK_ERROR_NONE = 0,
  INST_PK_ERROR_NO_NETWORK = 2,
  INST_PK_ERROR_INTERNAL_ERROR = 4,
  INST_PK_ERROR_PACKAGE_NOT_FOUND = 8,
  INST_PK_ERROR_PACKAGE_ALREADY_INSTALLED = 9,
  INST_PK_ERROR_PACKAGE_DOWNLOAD_FAILED = 10,
  INST_PK_ERROR_DEP_RESOLUTION_FAILED = 13,
  INST_PK_ERROR_TRANSACTION_ERROR = 16,
  INST_PK_ERROR_TRANSACTION_CANCELLED = 17,
  INST_PK_ERROR_NO_CACHE = 18,
  INST_PK_ERROR_CANNOT_REMOVE_SYSTEM_PACKAGE = 20,
  INST_PK_ERROR_PROCESS_KILL = 21,
  INST_PK_ERROR_CANNOT_CANCEL = 25,
  INST_PK_ERROR_CANNOT_GET_LOCK = 26,
  INST_PK_ERROR_LOCAL_INSTALL_FAILED = 29,
  INST_PK_ERROR_BAD_GPG_SIGNATURE = 30,
  INST_PK_ERROR_MISSING_GPG_SIGNATURE = 31,
  INST_PK_ERROR_FILE_CONFLICTS = 35,
  INST_PK_ERROR_PACKAGE_CONFLICTS = 36,
  INST_PK_ERROR_REPO_NOT_AVAILABLE = 37,
  INST_PK_ERROR_INVALID_PACKAGE_FILE = 38,
  INST_PK_ERROR_PACKAGE_INSTALL_BLOCKED = 39,
  INST_PK_ERROR_PACKAGE_CORRUPT = 40,
  INST_PK_ERROR_ALL_PACKAGES_ALREADY_INSTALLED = 41,
  INST_PK_ERROR_FILE_NOT_FOUND = 42,
  INST_PK_ERROR_NO_MORE_MIRRORS_TO_TRY = 43,
  INST_PK_ERROR_INCOMPATIBLE_ARCHITECTURE = 45,
  INST_PK_ERROR_NO_SPACE_ON_DEVICE = 46,
  INST_PK_ERROR_NOT_AUTHORIZED = 48,
  INST_PK_ERROR_CANNOT_INSTALL_REPO_UNSIGNED = 50,
  INST_PK_ERROR_PACKAGE_FAILED_TO_CONFIGURE = 56,
  INST_PK_ERROR_PACKAGE_FAILED_TO_INSTALL = 58,
  INST_PK_ERROR_PACKAGE_FAILED_TO_REMOVE = 59,
  INST_PK_ERROR_PACKAGE_DATABASE_CHANGED = 61,
  INST_PK_ERROR_CANNOT_FETCH_SOURCES = 64,
  INST_PK_ERROR_UNFINISHED_TRANSACTION = 66,
  INST_PK_ERROR_LOCK_REQUIRED = 67,

  INST_PK_ERROR_NO_PACKAGEKIT = 1000,   /* PackageKit cannot be reached (not installed, or its D-Bus call failed) */
  INST_PK_ERROR_REFUSED = 1001,         /* PackageKit refused the call itself (the D-Bus error says why) */
};

typedef struct {
  guint  info;          /* INST_PK_INFO_... */
  char  *id;            /* "name;version;arch;data" */
  char  *name, *version, *arch, *data;
  char  *summary;
} InstPkPackage;

typedef struct _InstPkTx InstPkTx;
typedef void (*InstPkFunc) (InstPkTx *tx, gpointer data);

InstPkTx *inst_pk_tx_new  (InstPkFunc changed, InstPkFunc finished, gpointer data);
void      inst_pk_tx_free (InstPkTx *tx);

void      inst_pk_tx_resolve          (InstPkTx *tx, guint64 filters, const char *const *names);
void      inst_pk_tx_get_details      (InstPkTx *tx, const char *const *package_ids);
void      inst_pk_tx_refresh_cache    (InstPkTx *tx, gboolean force);
void      inst_pk_tx_install_files    (InstPkTx *tx, guint64 flags, const char *path);
void      inst_pk_tx_install_packages (InstPkTx *tx, guint64 flags, const char *const *package_ids);
void      inst_pk_tx_remove_packages  (InstPkTx *tx, guint64 flags, const char *const *package_ids,
                                       gboolean allow_deps, gboolean autoremove);
/* asks PackageKit to stop (it may not, while dpkg runs: inst_pk_tx_get_allow_cancel) */
void      inst_pk_tx_cancel           (InstPkTx *tx);

gboolean    inst_pk_tx_is_finished      (InstPkTx *tx);
guint       inst_pk_tx_get_exit         (InstPkTx *tx);
/* the first error PackageKit reported (INST_PK_ERROR_...), and its detail text */
guint       inst_pk_tx_get_error_code   (InstPkTx *tx);
const char *inst_pk_tx_get_error_details (InstPkTx *tx);
guint       inst_pk_tx_get_status       (InstPkTx *tx);
/* 0 to 100, or 101 when PackageKit does not know */
guint       inst_pk_tx_get_percentage   (InstPkTx *tx);
gboolean    inst_pk_tx_get_allow_cancel (InstPkTx *tx);
/* the package an ItemProgress is about, or NULL, and its own status */
const InstPkPackage *inst_pk_tx_get_item (InstPkTx *tx);
guint64     inst_pk_tx_get_download_size (InstPkTx *tx);
/* InstPkPackage *, in the order PackageKit sent them */
GPtrArray  *inst_pk_tx_get_packages     (InstPkTx *tx);
/* GVariant * (a{sv}: package-id, summary, description, url, license, size, download-size) */
GPtrArray  *inst_pk_tx_get_details_list (InstPkTx *tx);

/* "name;version;arch;data" split; NULL when it is not one */
InstPkPackage *inst_pk_package_new  (guint info, const char *package_id, const char *summary);
void           inst_pk_package_free (InstPkPackage *package);

G_END_DECLS
