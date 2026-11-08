/* Phone numbers (phone-number.h). */
#include "phone-number.h"

#include <glib/gi18n.h>
#include <libebook-contacts/libebook-contacts.h>
#include <string.h>

/* ITU E.212: the Mobile Country Codes, sorted. Where a code is shared (the Caribbean's 340, the UK's 234 and 235),
 * it is the country most of its networks are in. */
static const struct {
  int mcc;
  const char *iso;
} mccs[] = {
  { 202, "GR" }, { 204, "NL" }, { 206, "BE" }, { 208, "FR" }, { 212, "MC" }, { 213, "AD" }, { 214, "ES" },
  { 216, "HU" }, { 218, "BA" }, { 219, "HR" }, { 220, "RS" }, { 221, "XK" }, { 222, "IT" }, { 225, "VA" },
  { 226, "RO" }, { 228, "CH" }, { 230, "CZ" }, { 231, "SK" }, { 232, "AT" }, { 234, "GB" }, { 235, "GB" },
  { 238, "DK" }, { 240, "SE" }, { 242, "NO" }, { 244, "FI" }, { 246, "LT" }, { 247, "LV" }, { 248, "EE" },
  { 250, "RU" }, { 255, "UA" }, { 257, "BY" }, { 259, "MD" }, { 260, "PL" }, { 262, "DE" }, { 266, "GI" },
  { 268, "PT" }, { 270, "LU" }, { 272, "IE" }, { 274, "IS" }, { 276, "AL" }, { 278, "MT" }, { 280, "CY" },
  { 282, "GE" }, { 283, "AM" }, { 284, "BG" }, { 286, "TR" }, { 288, "FO" }, { 290, "GL" }, { 292, "SM" },
  { 293, "SI" }, { 294, "MK" }, { 295, "LI" }, { 297, "ME" },
  { 302, "CA" }, { 308, "PM" }, { 310, "US" }, { 311, "US" }, { 312, "US" }, { 313, "US" }, { 314, "US" },
  { 315, "US" }, { 316, "US" }, { 330, "PR" }, { 334, "MX" }, { 338, "JM" }, { 340, "GP" }, { 342, "BB" },
  { 344, "AG" }, { 346, "KY" }, { 348, "VG" }, { 350, "BM" }, { 352, "GD" }, { 354, "MS" }, { 356, "KN" },
  { 358, "LC" }, { 360, "VC" }, { 362, "CW" }, { 363, "AW" }, { 364, "BS" }, { 365, "AI" }, { 366, "DM" },
  { 368, "CU" }, { 370, "DO" }, { 372, "HT" }, { 374, "TT" }, { 376, "TC" },
  { 400, "AZ" }, { 401, "KZ" }, { 402, "BT" }, { 404, "IN" }, { 405, "IN" }, { 406, "IN" }, { 410, "PK" },
  { 412, "AF" }, { 413, "LK" }, { 414, "MM" }, { 415, "LB" }, { 416, "JO" }, { 417, "SY" }, { 418, "IQ" },
  { 419, "KW" }, { 420, "SA" }, { 421, "YE" }, { 422, "OM" }, { 424, "AE" }, { 425, "IL" }, { 426, "BH" },
  { 427, "QA" }, { 428, "MN" }, { 429, "NP" }, { 430, "AE" }, { 431, "AE" }, { 432, "IR" }, { 434, "UZ" },
  { 436, "TJ" }, { 437, "KG" }, { 438, "TM" }, { 440, "JP" }, { 441, "JP" }, { 450, "KR" }, { 452, "VN" },
  { 454, "HK" }, { 455, "MO" }, { 456, "KH" }, { 457, "LA" }, { 460, "CN" }, { 461, "CN" }, { 466, "TW" },
  { 467, "KP" }, { 470, "BD" }, { 472, "MV" },
  { 502, "MY" }, { 505, "AU" }, { 510, "ID" }, { 514, "TL" }, { 515, "PH" }, { 520, "TH" }, { 525, "SG" },
  { 528, "BN" }, { 530, "NZ" }, { 536, "NR" }, { 537, "PG" }, { 539, "TO" }, { 540, "SB" }, { 541, "VU" },
  { 542, "FJ" }, { 544, "AS" }, { 545, "KI" }, { 546, "NC" }, { 547, "PF" }, { 548, "CK" }, { 549, "WS" },
  { 550, "FM" }, { 551, "MH" }, { 552, "PW" }, { 553, "TV" }, { 555, "NU" },
  { 602, "EG" }, { 603, "DZ" }, { 604, "MA" }, { 605, "TN" }, { 606, "LY" }, { 607, "GM" }, { 608, "SN" },
  { 609, "MR" }, { 610, "ML" }, { 611, "GN" }, { 612, "CI" }, { 613, "BF" }, { 614, "NE" }, { 615, "TG" },
  { 616, "BJ" }, { 617, "MU" }, { 618, "LR" }, { 619, "SL" }, { 620, "GH" }, { 621, "NG" }, { 622, "TD" },
  { 623, "CF" }, { 624, "CM" }, { 625, "CV" }, { 626, "ST" }, { 627, "GQ" }, { 628, "GA" }, { 629, "CG" },
  { 630, "CD" }, { 631, "AO" }, { 632, "GW" }, { 633, "SC" }, { 634, "SD" }, { 635, "RW" }, { 636, "ET" },
  { 637, "SO" }, { 638, "DJ" }, { 639, "KE" }, { 640, "TZ" }, { 641, "UG" }, { 642, "BI" }, { 643, "MZ" },
  { 645, "ZM" }, { 646, "MG" }, { 647, "RE" }, { 648, "ZW" }, { 649, "NA" }, { 650, "MW" }, { 651, "LS" },
  { 652, "BW" }, { 653, "SZ" }, { 654, "KM" }, { 655, "ZA" }, { 657, "ER" }, { 658, "SH" }, { 659, "SS" },
  { 702, "BZ" }, { 704, "GT" }, { 706, "SV" }, { 708, "HN" }, { 710, "NI" }, { 712, "CR" }, { 714, "PA" },
  { 716, "PE" }, { 722, "AR" }, { 724, "BR" }, { 730, "CL" }, { 732, "CO" }, { 734, "VE" }, { 736, "BO" },
  { 738, "GY" }, { 740, "EC" }, { 744, "PY" }, { 746, "SR" }, { 748, "UY" }, { 750, "FK" },
};

const char *
phone_number_country_for_mcc (int mcc)
{
  guint low = 0, high = G_N_ELEMENTS (mccs);

  while (low < high)
    {
      guint mid = (low + high) / 2;

      if (mccs[mid].mcc == mcc)
        return mccs[mid].iso;
      if (mccs[mid].mcc < mcc)
        low = mid + 1;
      else
        high = mid;
    }
  return NULL;
}

const char *
phone_number_locale_country (void)
{
  static char country[3];

  if (!country[0])
    {
      const char * const *names = g_get_language_names ();
      const char *underscore = names[0] ? strchr (names[0], '_') : NULL;

      /* "en_GB.UTF-8" -> "GB"; a locale without a country ("C") leaves libphonenumber to read only full numbers */
      if (underscore && g_ascii_isalpha (underscore[1]) && g_ascii_isalpha (underscore[2]))
        {
          country[0] = g_ascii_toupper (underscore[1]);
          country[1] = g_ascii_toupper (underscore[2]);
        }
      else
        country[0] = '-';
    }
  return country[0] == '-' ? NULL : country;
}

char *
phone_number_normalize (const char *number)
{
  GString *out = g_string_new (NULL);

  for (const char *c = number ? number : ""; *c; c++)
    if (g_ascii_isdigit (*c) || strchr ("*#,;", *c) || (*c == '+' && out->len == 0))
      g_string_append_c (out, *c);
  return g_string_free (out, FALSE);
}

char *
phone_number_canonical (const char *number, const char *country)
{
  g_autofree char *normalized = NULL;
  g_autofree char *national = NULL;
  g_autoptr(EPhoneNumber) parsed = NULL;

  if (!number)
    return g_strdup ("");
  for (const char *c = number; *c; c++)
    if (g_ascii_isalpha (*c))
      return g_strstrip (g_strdup (number));
  normalized = phone_number_normalize (number);
  if (strpbrk (normalized, "*#,;") || !e_phone_number_is_supported ())
    return g_steal_pointer (&normalized);
  parsed = e_phone_number_from_string (normalized, country ? country : phone_number_locale_country (), NULL);
  if (!parsed)
    return g_steal_pointer (&normalized);
  /* formatNumberToE164 takes only a valid number. EPhoneNumber cannot say whether one is, but one that fits none of
   * its country's patterns comes back as bare digits (as in phone_number_format) */
  national = e_phone_number_to_string (parsed, E_PHONE_NUMBER_FORMAT_NATIONAL);
  if (!national || strspn (national, "0123456789") == strlen (national))
    return g_steal_pointer (&normalized);
  return e_phone_number_to_string (parsed, E_PHONE_NUMBER_FORMAT_E164);
}

char *
phone_number_format (const char *number, const char *country)
{
  g_autoptr(EPhoneNumber) parsed = NULL;
  g_autofree char *formatted = NULL;
  int home_code;

  if (!number || !*number)
    return g_strdup ("");
  if (!country)
    country = phone_number_locale_country ();
  /* short codes and service numbers (*100#) stay as typed */
  if (strpbrk (number, "*#") || !e_phone_number_is_supported ())
    return g_strdup (number);
  parsed = e_phone_number_from_string (number, country, NULL);
  if (!parsed)
    return g_strdup (number);
  /* a number of the phone's own country without its country code, any other one with it */
  home_code = country ? e_phone_number_get_country_code_for_region (country, NULL) : 0;
  formatted = e_phone_number_to_string (parsed, e_phone_number_get_country_code (parsed, NULL) == home_code
                                                ? E_PHONE_NUMBER_FORMAT_NATIONAL
                                                : E_PHONE_NUMBER_FORMAT_INTERNATIONAL);
  /* A number that fits none of its country's patterns (too short, too long) comes back bare, without even the
   * trunk prefix: "+4917012345" as "17012345". Shown as it came instead, as formatInOriginalFormat does */
  if (!formatted || strspn (formatted, "0123456789") == strlen (formatted))
    return g_strdup (number);
  return g_steal_pointer (&formatted);
}

gboolean
phone_number_equal (const char *a, const char *b, const char *country)
{
  g_autofree char *na = phone_number_normalize (a), *nb = phone_number_normalize (b);
  EPhoneNumberMatch match;

  if (!*na || !*nb)
    return FALSE;
  if (g_str_equal (na, nb))
    return TRUE;
  if (!e_phone_number_is_supported ())
    return FALSE;
  match = e_phone_number_compare_strings_with_region (na, nb, country ? country : phone_number_locale_country (),
                                                      NULL);
  return match == E_PHONE_NUMBER_MATCH_EXACT || match == E_PHONE_NUMBER_MATCH_NATIONAL;
}

Presentation
phone_number_presentation (const char *number, Presentation presentation)
{
  static const char * const restricted[] = { "PRIVATE", "P", "RES", "PRIVATENUMBER", NULL };
  static const char * const unknown[] = { "UNAVAILABLE", "UNKNOWN", "UNA", "U", "ABSENT NUMBER", "ABSENTNUMBER",
                                          NULL };

  if (presentation != PRESENTATION_ALLOWED && presentation != 0)
    return presentation;
  if (!number || !*number)
    return PRESENTATION_UNKNOWN;
  for (guint i = 0; restricted[i]; i++)
    if (g_ascii_strcasecmp (number, restricted[i]) == 0)
      return PRESENTATION_RESTRICTED;
  for (guint i = 0; unknown[i]; i++)
    if (g_ascii_strcasecmp (number, unknown[i]) == 0)
      return PRESENTATION_UNKNOWN;
  return PRESENTATION_ALLOWED;
}

const char *
phone_number_presentation_name (Presentation presentation)
{
  switch (presentation)
    {
    case PRESENTATION_RESTRICTED:
      return _("Private number");
    case PRESENTATION_UNKNOWN:
      return _("Unknown");
    case PRESENTATION_PAYPHONE:
      return _("Payphone");
    default:
      return NULL;
    }
}
