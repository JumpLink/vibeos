/* The account menu: a page over the conversation list, opened from the account's picture. A close button at the
 * top right, a card with the person's picture and name, then the app's places in segmented cards: Archived,
 * Spam & blocked and Mark all as read in one group, Messages settings in the next. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  MSG_ACCOUNT_MENU_CLOSE,
  MSG_ACCOUNT_MENU_ARCHIVED,
  MSG_ACCOUNT_MENU_BLOCKED,
  MSG_ACCOUNT_MENU_MARK_ALL_READ,
  MSG_ACCOUNT_MENU_SETTINGS,
  MSG_ACCOUNT_MENU_ABOUT,
} MsgAccountMenuItem;

typedef void (*MsgAccountMenuFunc) (MsgAccountMenuItem item, gpointer data);

GtkWidget *msg_account_menu_new (MsgAccountMenuFunc func, gpointer data);

G_END_DECLS
