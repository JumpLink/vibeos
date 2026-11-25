/*
 * vibeos-set-own-password: changes the calling user's own password, after checking the current one, for VibeOS
 * Settings' "Set PIN" and "Set pattern". It does what passwd(1) does, without passwd's length and similarity rules
 * for a user (a PIN is four digits) and without a terminal.
 *
 * Run through pkexec only. polkit's action org.vibeos.set-own-password (its exec.path is this program) is granted
 * without a prompt to a process in the active local session and to nothing else (the .policy's defaults and
 * 50-vibeos-set-own-password.rules), so nothing about it is a password text box: Settings has already asked for the
 * current PIN or pattern on its own page. That grant is safe because this program trusts nothing from its caller but
 * the two secrets:
 *
 *   - the user is the one pkexec names (PKEXEC_UID), never an argument; root's own password is refused;
 *   - the current secret is checked through PAM (the vibeos-set-own-password service: the lock screen's stack,
 *     common-auth and common-account) before anything changes, with pam_unix's delay after a wrong one;
 *   - the new one is set by pam_chauthtok as root, through pam_unix alone (the service's password stage), so the
 *     shadow file is written the way passwd writes it.
 *
 * So whoever can run it can do nothing that passwd does not let them do already, given the current secret.
 *
 * Input, on stdin: the current secret, a NUL, the new one, a NUL; nothing else, each 1 to 127 bytes.
 * Exit status: 0 changed; 1 the current secret is wrong (nothing changed); 2 PAM did not change it; 3 a wrong call
 * (not run by pkexec, root, no such user, the input malformed). pkexec itself exits 126 or 127 when polkit refuses.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SERVICE "vibeos-set-own-password"
#define MAX_SECRET 128

enum { CHANGED = 0, WRONG_CURRENT = 1, NOT_CHANGED = 2, WRONG_CALL = 3 };

/* both secrets as read, and the one the conversation answers with now (the current one while authenticating, the new
 * one while changing) */
static char input[2 * MAX_SECRET + 1];
static const char *answer;

static void
wipe (void)
{
  explicit_bzero (input, sizeof input);
}

static int
converse (int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
  struct pam_response *r;

  if (n <= 0 || !(r = calloc (n, sizeof *r)))
    return PAM_BUF_ERR;
  for (int i = 0; i < n; i++) {
    switch (msg[i]->msg_style) {
    case PAM_PROMPT_ECHO_OFF:
      /* "Password:", "New password:", "Retype new password:" */
      if (!answer || !(r[i].resp = strdup (answer)))
        goto fail;
      break;
    case PAM_ERROR_MSG:
    case PAM_TEXT_INFO:
      if (msg[i]->msg)
        fprintf (stderr, "%s\n", msg[i]->msg);
      break;
    default:
      /* asked for something typed in the clear (the user name is set already): there is nothing to answer with */
      goto fail;
    }
  }
  *resp = r;
  return PAM_SUCCESS;

fail:
  for (int i = 0; i < n; i++) {
    if (r[i].resp) {
      explicit_bzero (r[i].resp, strlen (r[i].resp));
      free (r[i].resp);
    }
  }
  free (r);
  return PAM_CONV_ERR;
}

/* the two secrets from stdin into `input`; the second one's offset in `second` */
static int
read_secrets (size_t *second)
{
  size_t len = 0;
  ssize_t got;

  while (len < sizeof input && (got = read (STDIN_FILENO, input + len, sizeof input - len)) != 0) {
    if (got < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    len += got;
  }
  if (len == sizeof input)
    return -1;                  /* longer than two secrets can be */
  /* "current\0new\0": the first NUL ends the current one, the second ends the input */
  const char *nul = memchr (input, '\0', len);
  if (!nul || nul == input || nul - input >= MAX_SECRET)
    return -1;
  *second = nul - input + 1;
  size_t rest = len - *second;
  const char *end = memchr (input + *second, '\0', rest);
  if (!end || end == input + *second || end != input + len - 1 || (size_t) (end - (input + *second)) >= MAX_SECRET)
    return -1;
  return 0;
}

int
main (int argc, char **argv)
{
  const char *caller = getenv ("PKEXEC_UID");
  char *end;
  unsigned long uid;
  struct passwd *pw;
  char user[256];
  size_t second;
  struct pam_conv conv = { converse, NULL };
  pam_handle_t *pam = NULL;
  int rc, status;

  if (argc != 1 || getuid () != 0 || !caller || !*caller) {
    fprintf (stderr, "%s: run by VibeOS Settings through pkexec, with the secrets on stdin\n", argv[0]);
    return WRONG_CALL;
  }
  errno = 0;
  uid = strtoul (caller, &end, 10);
  if (errno || *end || uid == 0 || uid > (unsigned long) (uid_t) -2 || !(pw = getpwuid ((uid_t) uid))
      || strlen (pw->pw_name) >= sizeof user) {
    fprintf (stderr, "%s: not for this user\n", argv[0]);
    return WRONG_CALL;
  }
  strcpy (user, pw->pw_name);

  /* the secrets stay out of swap and are wiped on the way out */
  mlock (input, sizeof input);
  atexit (wipe);
  if (read_secrets (&second) < 0) {
    fprintf (stderr, "%s: the input is not two secrets, each ending in a NUL\n", argv[0]);
    return WRONG_CALL;
  }

  rc = pam_start (SERVICE, user, &conv, &pam);
  if (rc != PAM_SUCCESS) {
    fprintf (stderr, "%s: PAM: %s\n", argv[0], pam_strerror (pam, rc));
    return NOT_CHANGED;
  }
  pam_set_item (pam, PAM_RUSER, user);

  answer = input;
  rc = pam_authenticate (pam, PAM_DISALLOW_NULL_AUTHTOK);
  if (rc == PAM_SUCCESS) {
    rc = pam_acct_mgmt (pam, 0);
    /* an expired password is what is being changed */
    if (rc == PAM_NEW_AUTHTOK_REQD)
      rc = PAM_SUCCESS;
    status = rc == PAM_SUCCESS ? CHANGED : NOT_CHANGED;
  } else {
    status = WRONG_CURRENT;
  }

  if (status == CHANGED) {
    answer = input + second;
    rc = pam_chauthtok (pam, PAM_SILENT);
    if (rc != PAM_SUCCESS) {
      fprintf (stderr, "%s: PAM: %s\n", argv[0], pam_strerror (pam, rc));
      status = NOT_CHANGED;
    }
  }
  answer = NULL;
  pam_end (pam, rc);
  wipe ();
  return status;
}
