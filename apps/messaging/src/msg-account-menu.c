/* The account menu (msg-account-menu.h). */
#include "msg-account-menu.h"
#include "msg-account.h"
#include "msg-avatar.h"
#include "msg-card.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define CLOSE_BAR 48            /* the band the close button sits in */
#define EDGE 4                  /* the close button's inset from the end */
#define SIDE 16                 /* the cards' margin from the sides */
#define STACK_PAD 6             /* over and under each card stack: 12 between two */
#define HEADER_H 80
#define HEADER_AVATAR 68
#define HEADER_AVATAR_X 10
#define HEADER_TEXT_GAP 14      /* from the picture to the name */
#define ROW_H 56

typedef struct {
  MsgAccountMenuFunc func;
  gpointer data;
} Menu;

static void
activate (GtkWidget *row, Menu *menu)
{
  menu->func (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "msg-item")), menu->data);
}

static GtkWidget *
item_row (Menu *menu, const char *icon, const char *title, MsgAccountMenuItem item)
{
  GtkWidget *row = msg_card_menu_row_new (icon, title, ROW_H);

  g_object_set_data (G_OBJECT (row), "msg-item", GINT_TO_POINTER (item));
  g_signal_connect (row, "clicked", G_CALLBACK (activate), menu);
  return row;
}

static GtkWidget *
stack (void)
{
  GtkWidget *group = msg_card_group_new ();

  gtk_widget_set_margin_top (group, STACK_PAD);
  gtk_widget_set_margin_bottom (group, STACK_PAD);
  return group;
}

static void
update_header (GtkWidget *header)
{
  MsgAccount *account = msg_account_get_default ();
  GtkWidget *avatar = g_object_get_data (G_OBJECT (header), "msg-avatar");
  GtkWidget *name = g_object_get_data (G_OBJECT (header), "msg-name");
  GtkWidget *login = g_object_get_data (G_OBJECT (header), "msg-login");

  msg_avatar_set_person (MSG_AVATAR (avatar), msg_account_get_name (account), msg_account_get_login (account),
                         msg_account_get_picture (account));
  gtk_label_set_text (GTK_LABEL (name), msg_account_get_name (account));
  gtk_label_set_text (GTK_LABEL (login), msg_account_get_login (account));
}

static GtkWidget *
make_header (void)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (HEADER_AVATAR);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *name = gtk_label_new (NULL), *login = gtk_label_new (NULL);

  gtk_widget_add_css_class (card, "msg-card");
  gtk_widget_add_css_class (card, "first");
  gtk_widget_add_css_class (card, "last");
  gtk_widget_add_css_class (card, "msg-account-header");
  gtk_widget_set_size_request (card, -1, HEADER_H);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (avatar, HEADER_AVATAR_X);
  gtk_box_append (GTK_BOX (card), avatar);
  for (GtkWidget **label = (GtkWidget *[]) { name, login, NULL }; *label; label++)
    {
      gtk_label_set_xalign (GTK_LABEL (*label), 0);
      gtk_label_set_ellipsize (GTK_LABEL (*label), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (text), *label);
    }
  gtk_widget_add_css_class (name, "msg-account-name");
  gtk_widget_add_css_class (login, "msg-account-login");
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_margin_start (text, HEADER_TEXT_GAP);
  gtk_widget_set_margin_end (text, SIDE);
  gtk_box_append (GTK_BOX (card), text);
  g_object_set_data (G_OBJECT (card), "msg-avatar", avatar);
  g_object_set_data (G_OBJECT (card), "msg-name", name);
  g_object_set_data (G_OBJECT (card), "msg-login", login);
  update_header (card);
  g_signal_connect_object (msg_account_get_default (), "changed", G_CALLBACK (update_header), card,
                           G_CONNECT_SWAPPED);
  return card;
}

static void
on_close (GtkWidget *button, Menu *menu)
{
  menu->func (MSG_ACCOUNT_MENU_CLOSE, menu->data);
}

GtkWidget *
msg_account_menu_new (MsgAccountMenuFunc func, gpointer data)
{
  Menu *menu = g_new0 (Menu, 1);
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *close = vibeos_icon_button_new ("msg-close-symbolic");
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();
  GtkWidget *group, *header_stack;

  menu->func = func;
  menu->data = data;
  g_object_set_data_full (G_OBJECT (page), "msg-menu", menu, g_free);
  gtk_widget_add_css_class (page, "msg-account-menu");
  gtk_widget_set_focusable (page, TRUE);

  /* the ✕ is what Back does here: the back gesture presses it */
  gtk_accessible_update_property (GTK_ACCESSIBLE (close), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Close"), -1);
  gtk_widget_set_size_request (close, 48, 48);
  gtk_widget_set_halign (close, GTK_ALIGN_END);
  gtk_widget_set_hexpand (close, TRUE);
  gtk_widget_set_margin_end (close, EDGE);
  gtk_widget_add_css_class (close, "msg-bar-icon");
  g_signal_connect (close, "clicked", G_CALLBACK (on_close), menu);
  gtk_widget_set_size_request (bar, -1, CLOSE_BAR);
  gtk_box_append (GTK_BOX (bar), close);
  gtk_box_append (GTK_BOX (page), bar);

  gtk_widget_set_margin_start (content, SIDE);
  gtk_widget_set_margin_end (content, SIDE);
  header_stack = stack ();
  gtk_box_append (GTK_BOX (header_stack), make_header ());
  gtk_box_append (GTK_BOX (content), header_stack);
  group = stack ();
  msg_card_group_append (group, item_row (menu, "msg-archive-symbolic", _("Archived"), MSG_ACCOUNT_MENU_ARCHIVED));
  msg_card_group_append (group, item_row (menu, "msg-block-symbolic", _("Spam & blocked"), MSG_ACCOUNT_MENU_BLOCKED));
  msg_card_group_append (group, item_row (menu, "msg-mark-chat-read-symbolic", _("Mark all as read"),
                                          MSG_ACCOUNT_MENU_MARK_ALL_READ));
  gtk_box_append (GTK_BOX (content), group);
  group = stack ();
  msg_card_group_append (group, item_row (menu, "msg-settings-symbolic", _("Messages settings"),
                                          MSG_ACCOUNT_MENU_SETTINGS));
  msg_card_group_append (group, item_row (menu, "msg-info-symbolic", _("About"), MSG_ACCOUNT_MENU_ABOUT));
  gtk_box_append (GTK_BOX (content), group);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), content);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  return page;
}
