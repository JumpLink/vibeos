/* Phone numbers as the Dialer shows and compares them (spec §14.4): formatted for the country the phone is in, the
 * special numbers a network sends for a withheld caller, and the country a network code stands for.
 *
 * The formatting and comparing is libphonenumber's, through Evolution Data Server's EPhoneNumber (the C face that
 * libebook-contacts puts on it), so it is the same as Android's PhoneNumberUtils. */
#pragma once

#include <glib.h>

#include "call-types.h"

/* the ISO 3166 country ("DE") of a Mobile Country Code (262), or NULL for one it does not know */
const char *phone_number_country_for_mcc (int mcc);

/* the number with what a person types between digits dropped ("+49 (30) 123-45" -> "+4930 12345" -> "+493012345"):
 * digits, a leading +, and the dial characters * # , ; stay */
char       *phone_number_normalize (const char *number);

/* the number as the Messaging's participants are keyed (PhoneUtils.getCanonicalBySimLocale): E.164, "+493023575124",
 * when it is a valid number for @country (NULL: the locale's) or carries its own country code; otherwise as it came
 * with what a person types between digits dropped, as a short code ("22000"). A sender that is a name ("Vodafone")
 * stays as it is */
char       *phone_number_canonical (const char *number, const char *country);

/* for showing: "030 23575124" in its own country, "+49 30 23575124" from anywhere else (PhoneNumberUtils
 * .formatNumber); the number as it is if libphonenumber cannot read it. @country: where the phone is, or NULL for
 * the locale's country */
char       *phone_number_format (const char *number, const char *country);

/* the two numbers are the same line: libphonenumber's match, at least "national" (the same national number, the
 * country code on one side only), as PhoneNumberUtils.compare */
gboolean    phone_number_equal (const char *a, const char *b, const char *country);

/* the presentation a number stands for: some networks send the words "PRIVATE" or "UNKNOWN" in place of a number
 * (CallerInfoUtils), and an empty number is unknown; otherwise @presentation */
Presentation phone_number_presentation (const char *number, Presentation presentation);
/* "Private number", "Unknown", "Payphone"; NULL for a number that is shown as it is */
const char *phone_number_presentation_name (Presentation presentation);

/* a number that reaches the emergency services everywhere ("112", "911"; PhoneNumberUtils' defaults when the SIM
 * gives no list): one a phone dials even without a SIM, and one that cannot be blocked */
gboolean    phone_number_is_emergency (const char *number);

/* the country to format for when the network says none: the locale's ("en_GB" -> "GB") */
const char *phone_number_locale_country (void);
