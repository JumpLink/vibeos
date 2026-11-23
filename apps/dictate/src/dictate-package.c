/* DictatePackage: see dictate-package.h. One PackageKit transaction at a time: Resolve (and RefreshCache once, when
 * the package is not in the lists), GetDetails for its size, then InstallPackages with only-trusted. */
#include "dictate-package.h"
#include "dictate-pk.h"

#include <glib/gi18n.h>
#include <string.h>

typedef enum { STEP_NONE, STEP_RESOLVE, STEP_REFRESH, STEP_DETAILS, STEP_INSTALL } Step;

struct _DictatePackage {
  GObject parent_instance;
  char *name;
  DictatePackageState state;
  DictatePkTx *tx;
  Step step;
  gboolean refreshed;           /* the lists, once */
  gboolean install_wanted;      /* install once it is found */
  char *id, *version;
  guint64 size;
  char *status, *error;
  double fraction;
};

G_DEFINE_FINAL_TYPE (DictatePackage, dictate_package, G_TYPE_OBJECT)

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void run (DictatePackage *self, Step step);

static void
changed (DictatePackage *self)
{
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
set_state (DictatePackage *self, DictatePackageState state)
{
  self->state = state;
  changed (self);
}

/* what went wrong, for the user */
static char *
error_text (DictatePackage *self, guint code, const char *details)
{
  switch (code)
    {
    case DICTATE_PK_ERROR_NO_PACKAGEKIT:
      return g_strdup (_("Software cannot be installed on this phone: PackageKit is not there"));
    case DICTATE_PK_ERROR_NOT_AUTHORIZED:
      return g_strdup (_("The password was not given"));
    case DICTATE_PK_ERROR_TRANSACTION_CANCELLED:
      return g_strdup (_("The installation was cancelled"));
    case DICTATE_PK_ERROR_NO_NETWORK:
    case DICTATE_PK_ERROR_PACKAGE_DOWNLOAD_FAILED:
    case DICTATE_PK_ERROR_REPO_NOT_AVAILABLE:
    case DICTATE_PK_ERROR_NO_MORE_MIRRORS_TO_TRY:
    case DICTATE_PK_ERROR_CANNOT_FETCH_SOURCES:
    case DICTATE_PK_ERROR_NO_CACHE:
      return g_strdup (_("The VibeOS archive could not be reached. Is the phone online?"));
    case DICTATE_PK_ERROR_PACKAGE_NOT_FOUND:
      /* TRANSLATORS: %s is a package's name (vibeos-dictate-vosk) */
      return g_strdup_printf (_("%s is not in the archives this phone uses"), self->name);
    case DICTATE_PK_ERROR_BAD_GPG_SIGNATURE:
    case DICTATE_PK_ERROR_MISSING_GPG_SIGNATURE:
    case DICTATE_PK_ERROR_CANNOT_INSTALL_REPO_UNSIGNED:
      return g_strdup (_("The package's signature could not be checked, so it was not installed"));
    case DICTATE_PK_ERROR_CANNOT_GET_LOCK:
      return g_strdup (_("Another installation is running. Try again when it is done"));
    case DICTATE_PK_ERROR_NO_SPACE_ON_DEVICE:
      return g_strdup (_("There is not enough space on the phone"));
    case DICTATE_PK_ERROR_DEP_RESOLUTION_FAILED:
      return g_strdup (_("What the package needs cannot be installed with it"));
    default:
      if (details && *details)
        /* TRANSLATORS: %s is PackageKit's own explanation */
        return g_strdup_printf (_("The package could not be installed: %s"), details);
      return g_strdup (_("The package could not be installed"));
    }
}

static void
fail (DictatePackage *self, char *why)
{
  g_free (self->error), self->error = why;
  self->install_wanted = FALSE;
  self->step = STEP_NONE;
  set_state (self, DICTATE_PACKAGE_FAILED);
}

static void
fail_from_tx (DictatePackage *self, DictatePkTx *tx)
{
  guint code = dictate_pk_tx_get_error_code (tx);

  if (dictate_pk_tx_get_exit (tx) == DICTATE_PK_EXIT_CANCELLED && !code)
    code = DICTATE_PK_ERROR_TRANSACTION_CANCELLED;
  g_message ("package: %s: PackageKit error %u%s%s", self->name, code,
             dictate_pk_tx_get_error_details (tx) ? ": " : "", dictate_pk_tx_get_error_details (tx) ?: "");
  fail (self, error_text (self, code, dictate_pk_tx_get_error_details (tx)));
}

static const char *
status_text (guint status)
{
  switch (status)
    {
    case DICTATE_PK_STATUS_WAITING_FOR_AUTH:
      return _("Waiting for the password");
    case DICTATE_PK_STATUS_WAITING_FOR_LOCK:
      return _("Waiting for another installation to finish");
    case DICTATE_PK_STATUS_REFRESH_CACHE:
      return _("Updating the package lists");
    case DICTATE_PK_STATUS_DOWNLOAD:
      return _("Downloading");
    case DICTATE_PK_STATUS_SIG_CHECK:
      return _("Checking the signature");
    case DICTATE_PK_STATUS_INSTALL:
    case DICTATE_PK_STATUS_COMMIT:
      return _("Installing");
    default:
      return _("Getting ready");
    }
}

static void
on_tx_changed (DictatePkTx *tx, gpointer data)
{
  DictatePackage *self = data;
  guint percentage = dictate_pk_tx_get_percentage (tx);

  if (self->step != STEP_INSTALL)
    return;
  g_free (self->status), self->status = g_strdup (status_text (dictate_pk_tx_get_status (tx)));
  self->fraction = percentage <= 100 ? percentage / 100.0 : -1;
  changed (self);
}

/* the newest package of that name in the lists, installed or not */
static void
take_resolved (DictatePackage *self, DictatePkTx *tx)
{
  GPtrArray *packages = dictate_pk_tx_get_packages (tx);
  DictatePkPackage *installed = NULL, *available = NULL;

  for (guint i = 0; i < packages->len; i++)
    {
      DictatePkPackage *p = packages->pdata[i];

      if (g_strcmp0 (p->name, self->name) != 0)
        continue;
      if (p->info == DICTATE_PK_INFO_INSTALLED)
        installed = p;
      else if (p->info == DICTATE_PK_INFO_AVAILABLE && !available)
        available = p;
    }
  if (installed)
    {
      g_free (self->version), self->version = g_strdup (installed->version);
      self->install_wanted = FALSE;
      self->step = STEP_NONE;
      set_state (self, DICTATE_PACKAGE_INSTALLED);
      return;
    }
  if (!available)
    {
      if (!self->refreshed)
        {
          run (self, STEP_REFRESH);
          return;
        }
      fail (self, error_text (self, DICTATE_PK_ERROR_PACKAGE_NOT_FOUND, NULL));
      return;
    }
  g_debug ("package: %s is %s", self->name, available->id);
  g_free (self->id), self->id = g_strdup (available->id);
  g_free (self->version), self->version = g_strdup (available->version);
  run (self, STEP_DETAILS);
}

static void
on_tx_finished (DictatePkTx *tx, gpointer data)
{
  DictatePackage *self = data;
  gboolean ok = dictate_pk_tx_get_exit (tx) == DICTATE_PK_EXIT_SUCCESS;

  switch (self->step)
    {
    case STEP_RESOLVE:
      if (!ok)
        fail_from_tx (self, tx);
      else
        take_resolved (self, tx);
      break;
    case STEP_REFRESH:
      /* a refresh that failed is not the end: the lists there are may have it after all */
      self->refreshed = TRUE;
      run (self, STEP_RESOLVE);
      break;
    case STEP_DETAILS:
      /* without the size it can still be installed */
      self->size = ok ? dictate_pk_tx_get_download_size (tx) : 0;
      self->step = STEP_NONE;
      set_state (self, DICTATE_PACKAGE_FOUND);
      if (self->install_wanted)
        run (self, STEP_INSTALL);
      break;
    case STEP_INSTALL:
      if (!ok)
        fail_from_tx (self, tx);
      else
        {
          self->step = STEP_NONE;
          self->install_wanted = FALSE;
          set_state (self, DICTATE_PACKAGE_INSTALLED);
        }
      break;
    case STEP_NONE:
      break;
    }
}

static void
run (DictatePackage *self, Step step)
{
  /* the last transaction is freed from its own callback: later, on the main loop */
  if (self->tx)
    g_idle_add_once ((GSourceOnceFunc) dictate_pk_tx_free, g_steal_pointer (&self->tx));
  self->step = step;
  self->tx = dictate_pk_tx_new (on_tx_changed, on_tx_finished, self);
  switch (step)
    {
    case STEP_RESOLVE:
      dictate_pk_tx_resolve (self->tx, DICTATE_PK_FILTER_ARCH | DICTATE_PK_FILTER_NEWEST, self->name);
      break;
    case STEP_REFRESH:
      dictate_pk_tx_refresh_cache (self->tx);
      break;
    case STEP_DETAILS:
      dictate_pk_tx_get_details (self->tx, self->id);
      break;
    case STEP_INSTALL:
      g_free (self->status), self->status = g_strdup (_("Getting ready"));
      self->fraction = -1;
      set_state (self, DICTATE_PACKAGE_INSTALLING);
      /* from a signed archive only: PackageKit refuses anything else */
      dictate_pk_tx_install_packages (self->tx, DICTATE_PK_FLAG_ONLY_TRUSTED, self->id);
      break;
    case STEP_NONE:
      break;
    }
}

static void
look_up (DictatePackage *self)
{
  self->refreshed = FALSE;
  g_clear_pointer (&self->error, g_free);
  set_state (self, DICTATE_PACKAGE_LOOKING_UP);
  run (self, STEP_RESOLVE);
}

DictatePackage *
dictate_package_new (const char *name)
{
  DictatePackage *self = g_object_new (DICTATE_TYPE_PACKAGE, NULL);

  self->name = g_strdup (name);
  look_up (self);
  return self;
}

void
dictate_package_install (DictatePackage *self)
{
  switch (self->state)
    {
    case DICTATE_PACKAGE_FOUND:
      run (self, STEP_INSTALL);
      break;
    case DICTATE_PACKAGE_FAILED:
      self->install_wanted = TRUE;
      look_up (self);
      break;
    case DICTATE_PACKAGE_LOOKING_UP:
      self->install_wanted = TRUE;
      break;
    case DICTATE_PACKAGE_INSTALLING:
    case DICTATE_PACKAGE_INSTALLED:
      break;
    }
}

void
dictate_package_cancel (DictatePackage *self)
{
  self->install_wanted = FALSE;
  if (self->tx && !dictate_pk_tx_is_finished (self->tx))
    dictate_pk_tx_cancel (self->tx);
}

DictatePackageState dictate_package_get_state (DictatePackage *self) { return self->state; }
const char *dictate_package_get_name (DictatePackage *self) { return self->name; }
const char *dictate_package_get_version (DictatePackage *self) { return self->version; }
guint64 dictate_package_get_size (DictatePackage *self) { return self->size; }
const char *dictate_package_get_status (DictatePackage *self) { return self->status ? self->status : ""; }
double dictate_package_get_fraction (DictatePackage *self) { return self->fraction; }
const char *dictate_package_get_error (DictatePackage *self) { return self->error ? self->error : ""; }

static void
dictate_package_finalize (GObject *object)
{
  DictatePackage *self = DICTATE_PACKAGE (object);

  g_clear_pointer (&self->tx, dictate_pk_tx_free);
  g_free (self->name);
  g_free (self->id);
  g_free (self->version);
  g_free (self->status);
  g_free (self->error);
  G_OBJECT_CLASS (dictate_package_parent_class)->finalize (object);
}

static void
dictate_package_class_init (DictatePackageClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = dictate_package_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 0);
}

static void
dictate_package_init (DictatePackage *self)
{
  self->fraction = -1;
}
