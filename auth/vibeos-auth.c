/* vibeos-auth: checks the lock screen's PIN or pattern for the user who runs it, through PAM (service "vibeos").
 * The secret comes on stdin, one line. A pattern is the digits 1-9 of its dots in the order they were joined ("1478"),
 * as Phosh's pattern lock had it, so either one is simply the user's password.
 *
 * Exit status: 0 right, 1 wrong (or the account refused), 2 PAM could not run. PAM's own delay after a failure is
 * turned off: the lock screen shows a wrong attempt at once and throttles repeated ones itself, as Android does.
 *
 * pam_unix checks the password of the user it runs as through its setgid helper (unix_chkpwd), so this needs no
 * privileges of its own. */
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SECRET 256

static char secret[MAX_SECRET];

static int
converse (int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
  struct pam_response *r = calloc (n, sizeof *r);

  if (!r)
    return PAM_BUF_ERR;
  for (int i = 0; i < n; i++) {
    switch (msg[i]->msg_style) {
    case PAM_PROMPT_ECHO_OFF:
      r[i].resp = strdup (secret);
      break;
    case PAM_PROMPT_ECHO_ON:   /* only ever the user name */
      r[i].resp = strdup ((const char *) data);
      break;
    default:                   /* PAM_ERROR_MSG, PAM_TEXT_INFO */
      continue;
    }
    if (!r[i].resp) {
      for (int j = 0; j < i; j++)
        free (r[j].resp);
      free (r);
      return PAM_BUF_ERR;
    }
  }
  *resp = r;
  return PAM_SUCCESS;
}

static void
no_delay (int status, unsigned int usec, void *data)
{
}

int
main (void)
{
  struct passwd *pw = getpwuid (getuid ());
  struct pam_conv conv = { converse, NULL };
  pam_handle_t *pamh = NULL;
  size_t len;
  int ret;

  if (!pw || !fgets (secret, sizeof secret, stdin))
    return 2;
  len = strcspn (secret, "\n");
  secret[len] = '\0';
  if (len == 0)
    return 1;

  conv.appdata_ptr = pw->pw_name;
  ret = pam_start ("vibeos", pw->pw_name, &conv, &pamh);
  if (ret != PAM_SUCCESS)
    return 2;
  pam_set_item (pamh, PAM_FAIL_DELAY, (const void *) no_delay);
  ret = pam_authenticate (pamh, 0);
  if (ret == PAM_SUCCESS)
    ret = pam_acct_mgmt (pamh, 0);
  explicit_bzero (secret, sizeof secret);
  pam_end (pamh, ret);

  return ret == PAM_SUCCESS ? 0 : 1;
}
