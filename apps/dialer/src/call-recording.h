/* Call recordings' files: the folder they go into and their names. Shared by the service, which writes them, and
 * the app's Settings, which shows the folder.
 *
 * The folder is LineageOS Dialer's, "Recordings/Call recordings" on the phone's storage, here in the home folder,
 * unless the "recordings-folder" setting names another. A file is named as Lineage names it, the other side then
 * the date and time to the millisecond (<number>_<yyMMdd_HHmmssSSS>), with the contact's name in place of the
 * number when the caller is a contact. Recordings are Ogg Opus (.opus). */
#pragma once

#include <gio/gio.h>

#define CALL_RECORDING_EXTENSION ".opus"

/* the default folder: ~/Recordings/Call recordings */
char *call_recording_default_folder (void);

/* the folder the "recordings-folder" key of @settings (vibeos.Dialer) names, or the default when it is empty */
char *call_recording_folder (GSettings *settings);

/* @folder as the Settings row shows it: in the home folder "Home/Recordings/Call recordings", elsewhere the path */
char *call_recording_folder_display (const char *folder);

/* the file name for a recording of a call with @party (the contact's name or the number; NULL or "" for none,
 * "unknown") that started at @when: "Anna Schmidt_261124_140322123.opus". What a file name cannot hold here or on
 * a memory card (/ \ : * ? " < > | and control characters) becomes "_", and a long name is cut short */
char *call_recording_file_name (const char *party, GDateTime *when);
