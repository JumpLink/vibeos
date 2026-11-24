/* Messaging's window (messaging-window.h). */
#include "messaging-window.h"
#include "blocked-contacts.h"
#include "contact-picker.h"
#include "contacts.h"
#include "conversation-list.h"
#include "conversation-view.h"
#include "msg-account-menu.h"
#include "msg-search.h"
#include "msg-settings.h"
#include "people-options.h"
#include "sms-client.h"

#include <glib/gi18n.h>
#include <vibeos.h>

#define ROW_HIGHLIGHT_MS 120    /* a tapped row's press highlight before its conversation opens */
/* Two panes on a tablet, upright too (owner, 11-21: "two panes upright too"), as every app's list and detail are
 * (VibeosListDetail's default); one on a phone. In two, the list and the conversation are rounded panes on the window's surface, PANE_EDGE from its
 * sides and bottom and PANE_GAP apart (messaging.css draws them: .msg-split) */
#define LIST_RATIO 0.36         /* the list pane's share of the width */
#define LIST_MIN   360          /* as wide as a phone at least */
#define LIST_MAX   420
#define PANE_EDGE  16
#define PANE_GAP   16
#define EMPTY_ICON 96           /* the picture in the conversation pane with nothing open */

typedef struct
{
  GtkWidget *window;
  SmsClient *client;
  Contacts *contacts;
  GtkWidget *pager;             /* the pages over everything: search, the account menu, New chat, settings … */
  GtkWidget *home;              /* the list and the conversation open, beside it on a tablet */
  GtkWidget *list;
  GtkWidget *shown;             /* the conversation view last on top, whose draft is kept when it goes */
  GtkWidget *menu;              /* the account menu while it is open (a weak pointer) */
  GtkWidget *empty;             /* the picture and the line on the conversation pane with nothing open */
  GtkWidget *empty_text;
  VibeosLayout *layout;
} Shell;

static void open_conversation (Shell *shell, gint64 id, const char *draft, gboolean replace, GtkWidget *row);

static Shell *
shell_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (window), "messaging-shell");
}

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  /* nothing is on screen any more */
  sms_client_set_visible (shell->client, -1);
  g_signal_handlers_disconnect_by_data (shell->client, shell);
  g_clear_object (&shell->contacts);
  g_clear_object (&shell->client);
  g_clear_object (&shell->shown);
  g_clear_weak_pointer (&shell->menu);
  if (shell->layout)
    {
      g_signal_handlers_disconnect_by_data (shell->layout, shell);
      g_object_unref (shell->layout);
    }
  g_free (shell);
}

static VibeosListDetail *
home (Shell *shell)
{
  return VIBEOS_LIST_DETAIL (shell->home);
}

static VibeosPageStack *
list_stack (Shell *shell)
{
  return vibeos_list_detail_get_list_stack (home (shell));
}

static VibeosPageStack *
detail_stack (Shell *shell)
{
  return vibeos_list_detail_get_detail_stack (home (shell));
}

static gboolean
is_split (Shell *shell)
{
  return vibeos_list_detail_is_split (home (shell));
}

/* the list beside a conversation: its share of the width, but as wide as a phone at least; the pane holds it with
 * the window's edge on one side and half the gap on the other */
static void
update_list_width (Shell *shell)
{
  int list = (int) CLAMP (vibeos_layout_get_width (shell->layout) * LIST_RATIO, LIST_MIN, LIST_MAX);

  vibeos_list_detail_set_list_width (home (shell), list + PANE_EDGE + PANE_GAP / 2);
}

static VibeosPageStack *
pager (Shell *shell)
{
  return VIBEOS_PAGE_STACK (shell->pager);
}

/* a page's stack, when it is one of the shell's */
static gboolean
is_on (GtkWidget *page, VibeosPageStack *stack)
{
  return page && gtk_widget_get_parent (page) == GTK_WIDGET (stack);
}

/* the page in front: a page over everything, else the conversation open, else the list */
static GtkWidget *
top_page (Shell *shell)
{
  VibeosPageStack *detail = vibeos_list_detail_get_detail_stack (home (shell));

  if (vibeos_page_stack_get_depth (pager (shell)) > 1)
    return vibeos_page_stack_get_top (pager (shell));
  if (vibeos_list_detail_is_split (home (shell)) && vibeos_page_stack_get_depth (detail))
    return vibeos_page_stack_get_top (detail);
  return vibeos_page_stack_get_top (vibeos_list_detail_get_list_stack (home (shell)));
}

/* --- the transitions: each page's own, opening and closing */

static void
set_transition (GtkWidget *page, VibeosPageTransition transition)
{
  g_object_set_data (G_OBJECT (page), "msg-transition", GINT_TO_POINTER (transition));
}

static VibeosPageTransition
transition_of (GtkWidget *page)
{
  return page ? GPOINTER_TO_INT (g_object_get_data (G_OBJECT (page), "msg-transition"))
              : VIBEOS_PAGE_TRANSITION_SHARED_AXIS;
}

static void
push (Shell *shell, GtkWidget *page, VibeosPageTransition transition)
{
  set_transition (page, transition);
  vibeos_page_stack_set_transition (pager (shell), transition);
  /* a page opens with nothing focused, unless it takes the focus itself (the search field, To:) */
  gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
  vibeos_page_stack_push (pager (shell), page);
}

static void
pop (Shell *shell)
{
  vibeos_page_stack_set_transition (pager (shell), transition_of (vibeos_page_stack_get_top (pager (shell))));
  vibeos_page_stack_pop (pager (shell));
}

/* a page's transition inside the list pane: a shared axis over a pane's short distance */
static VibeosPageTransition
in_pane (VibeosPageTransition transition)
{
  return transition == VIBEOS_PAGE_TRANSITION_SHARED_AXIS ? VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE : transition;
}

/* In two panes what the list opens (search, the account menu and its places) opens in the list pane, over the list,
 * and the conversations those open beside it; in one pane over everything. */
static void
push_listward (Shell *shell, GtkWidget *page, VibeosPageTransition transition)
{
  if (!is_split (shell))
    {
      push (shell, page, transition);
      return;
    }
  set_transition (page, transition);
  vibeos_page_stack_set_transition (list_stack (shell), in_pane (transition));
  gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
  vibeos_page_stack_push (list_stack (shell), page);
}

/* closes the page on top of the list pane, back to the one under it */
static void
pop_list_pane (Shell *shell)
{
  vibeos_page_stack_set_transition (list_stack (shell),
                                    in_pane (transition_of (vibeos_page_stack_get_top (list_stack (shell)))));
  vibeos_page_stack_pop (list_stack (shell));
}

/* a page from a page: on the same stack as the one it came from */
static void
push_from (Shell *shell, GtkWidget *from, GtkWidget *page, VibeosPageTransition transition)
{
  if (is_split (shell) && is_on (from, list_stack (shell)))
    push_listward (shell, page, transition);
  else if (is_split (shell) && is_on (from, detail_stack (shell)))
    {
      set_transition (page, VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE);
      gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
      vibeos_list_detail_push (home (shell), page);
    }
  else
    push (shell, page, transition);
}

/* closes `page` if it is on top of its stack: the pager's, the list pane's, or the detail pane's (where a turn from
 * one pane to two moves what was over the list) */
static void
close_page (Shell *shell, GtkWidget *page)
{
  if (!page)
    return;
  if (vibeos_page_stack_get_depth (pager (shell)) > 1 && vibeos_page_stack_get_top (pager (shell)) == page)
    pop (shell);
  else if (vibeos_page_stack_get_depth (list_stack (shell)) > 1 && vibeos_page_stack_get_top (list_stack (shell)) == page)
    pop_list_pane (shell);
  else if (is_on (page, detail_stack (shell)) && vibeos_page_stack_get_top (detail_stack (shell)) == page)
    vibeos_list_detail_back (home (shell));
}

/* --- what is on screen (SetVisible) */

static void
update_visible (Shell *shell)
{
  GtkWidget *top = top_page (shell);
  gint64 visible = -1;

  /* only while the window is in front and seen: a message that comes to a window behind another, or under
   * the lock screen (the compositor suspends it), is still news */
  if (!gtk_window_is_active (GTK_WINDOW (shell->window)) || !gtk_widget_get_mapped (shell->window)
      || gtk_window_is_suspended (GTK_WINDOW (shell->window)))
    visible = -1;
  else if (MSG_IS_CONVERSATION_VIEW (top))
    visible = conversation_view_get_id (MSG_CONVERSATION_VIEW (top));
  else if (top == shell->list && conversation_list_is_at_top (MSG_CONVERSATION_LIST (top)))
    visible = 0;
  sms_client_set_visible (shell->client, visible);
}

static void
keep_draft (Shell *shell)
{
  if (shell->shown)
    conversation_view_save_draft (MSG_CONVERSATION_VIEW (shell->shown));
}

/* the back arrow on a conversation: beside the list it is the pane's first page, and Back there leaves the app (as an
 * entry's page beside Settings' menu); over the list, or deeper, it goes back */
static void
sync_back_arrows (Shell *shell)
{
  VibeosPageStack *stacks[] = { list_stack (shell), detail_stack (shell), pager (shell) };

  for (guint s = 0; s < G_N_ELEMENTS (stacks); s++)
    for (guint i = 0; i < vibeos_page_stack_get_depth (stacks[s]); i++)
      {
        GtkWidget *page = vibeos_page_stack_get_page (stacks[s], i);

        if (MSG_IS_CONVERSATION_VIEW (page))
          conversation_view_set_back_shown (MSG_CONVERSATION_VIEW (page),
                                            !(is_split (shell) && stacks[s] == detail_stack (shell) && i == 0));
      }
}

static void
on_top_changed (GtkWidget *stack, gpointer data)
{
  Shell *shell = data;
  GtkWidget *top = top_page (shell);

  /* the conversation going (or covered) keeps what was typed */
  if (shell->shown != top)
    {
      keep_draft (shell);
      g_clear_object (&shell->shown);
      if (MSG_IS_CONVERSATION_VIEW (top))
        shell->shown = g_object_ref (top);
    }
  /* beside the list (or the archived one over it): its row shows which */
  {
    VibeosPageStack *detail = detail_stack (shell);
    GtkWidget *open = is_split (shell) && vibeos_page_stack_get_depth (detail)
                      ? vibeos_page_stack_get_page (detail, 0) : NULL;
    gint64 current = MSG_IS_CONVERSATION_VIEW (open) ? conversation_view_get_id (MSG_CONVERSATION_VIEW (open)) : 0;

    for (guint i = 0; i < vibeos_page_stack_get_depth (list_stack (shell)); i++)
      {
        GtkWidget *page = vibeos_page_stack_get_page (list_stack (shell), i);

        if (MSG_IS_CONVERSATION_LIST (page))
          conversation_list_set_current (MSG_CONVERSATION_LIST (page), current);
      }
  }
  sync_back_arrows (shell);
  if (top && !gtk_widget_get_focus_child (top))
    gtk_widget_grab_focus (top);
  update_visible (shell);
}

static void
on_active (GtkWindow *window, GParamSpec *pspec, gpointer data)
{
  Shell *shell = data;

  /* the draft is saved when the window goes behind another */
  if (!gtk_window_is_active (window))
    keep_draft (shell);
  update_visible (shell);
}

/* --- going back */

static void
go_back (Shell *shell)
{
  GtkWidget *top = top_page (shell);

  if (MSG_IS_CONVERSATION_VIEW (top) && conversation_view_back (MSG_CONVERSATION_VIEW (top)))
    return;
  if (MSG_IS_CONVERSATION_LIST (top) && conversation_list_back (MSG_CONVERSATION_LIST (top)))
    return;
  if (MSG_IS_SEARCH (top) && msg_search_back (MSG_SEARCH (top)))
    return;
  if (vibeos_page_stack_get_depth (pager (shell)) > 1)
    {
      pop (shell);
      return;
    }
  /* nothing beside the list: a page over the list in its pane closes */
  if (is_split (shell) && !vibeos_page_stack_get_depth (detail_stack (shell))
      && vibeos_page_stack_get_depth (list_stack (shell)) > 1)
    {
      pop_list_pane (shell);
      return;
    }
  /* a conversation over the list closes as it opened: a cut */
  vibeos_page_stack_set_transition (list_stack (shell), VIBEOS_PAGE_TRANSITION_NONE);
  if (!vibeos_list_detail_back (home (shell)))
    gtk_window_close (GTK_WINDOW (shell->window));
}

/* a page closing itself goes only if it is the one in front (a conversation deleted under the picker stays), or on
 * top of the list pane beside a conversation (its own back arrow) */
static void
on_page_closed (GtkWidget *page, gpointer data)
{
  Shell *shell = data;

  if (top_page (shell) == page)
    go_back (shell);
  else if (is_split (shell) && vibeos_page_stack_get_depth (list_stack (shell)) > 1
           && vibeos_page_stack_get_top (list_stack (shell)) == page)
    pop_list_pane (shell);
  else if (gtk_widget_get_ancestor (page, VIBEOS_TYPE_LIST_DETAIL) && MSG_IS_CONVERSATION_VIEW (page))
    /* the conversation beside the list, deleted or archived from elsewhere */
    vibeos_list_detail_close_detail (home (shell));
}

static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  go_back (data);
  return TRUE;
}

/* --- the pages */

static void
on_open_conversation (GtkWidget *list, gint64 id, gpointer data)
{
  GtkWidget *row = MSG_IS_CONVERSATION_LIST (list)
                   ? conversation_list_get_pressed_row (MSG_CONVERSATION_LIST (list)) : NULL;

  open_conversation (data, id, NULL, FALSE, row);
}

static void
on_picked (ContactPicker *picker, const char *number, gpointer data)
{
  Shell *shell = data;
  const char *numbers[] = { number, NULL };
  gint64 id = sms_client_get_conversation (shell->client, numbers);

  /* New chat becomes the conversation, in its place */
  if (id > 0)
    open_conversation (shell, id, NULL, TRUE, NULL);
}

/* New chat: in two panes in the conversation's pane, where the conversation it starts then takes its place; else
 * over everything */
static void
on_start_chat (GtkWidget *list, gpointer data)
{
  Shell *shell = data;
  GtkWidget *picker = contact_picker_new (shell->client, shell->contacts);

  g_signal_connect (picker, "picked", G_CALLBACK (on_picked), shell);
  g_signal_connect (picker, "closed", G_CALLBACK (on_page_closed), shell);
  if (is_split (shell) && vibeos_page_stack_get_depth (pager (shell)) < 2)
    {
      set_transition (picker, VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE);
      gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
      vibeos_list_detail_show (home (shell), picker);
    }
  else
    push (shell, picker, VIBEOS_PAGE_TRANSITION_FADE_THROUGH);
  gtk_widget_grab_focus (picker);
}

/* the conversation pane's line with nothing open: pick one, or with none yet, start one */
static void
update_empty_text (Shell *shell)
{
  gboolean none = conversation_list_get_n_conversations (MSG_CONVERSATION_LIST (shell->list)) == 0;

  if (shell->empty_text)
    gtk_label_set_text (GTK_LABEL (shell->empty_text), none ? _("Start a chat to see it here")
                                                            : _("Pick a conversation to see it here"));
}

static void
on_list_at_top (GtkWidget *list, gpointer data)
{
  Shell *shell = data;

  if (list == shell->list)
    update_empty_text (shell);
  update_visible (shell);
}

static GtkWidget *
make_list (Shell *shell, gboolean archived)
{
  GtkWidget *list = conversation_list_new (shell->client, shell->contacts, archived);

  g_signal_connect (list, "open-conversation", G_CALLBACK (on_open_conversation), shell);
  g_signal_connect (list, "closed", G_CALLBACK (on_page_closed), shell);
  g_signal_connect (list, "at-top-changed", G_CALLBACK (on_list_at_top), shell);
  return list;
}

static void
on_search (GtkWidget *list, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = msg_search_new (shell->client, shell->contacts);

  g_signal_connect (page, "open-conversation", G_CALLBACK (on_open_conversation), shell);
  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  push_listward (shell, page, VIBEOS_PAGE_TRANSITION_FADE_THROUGH);
  /* the keyboard comes up at once */
  gtk_widget_grab_focus (page);
}

static void
on_show_advanced_settings (GtkWidget *settings, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = msg_settings_new_advanced ();

  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  push_from (shell, settings, page, VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
}

/* a page the account menu opens takes its place, in the list pane or over everything: Back from it goes to the
 * list */
static void
replace_menu (Shell *shell, GtkWidget *page)
{
  VibeosPageStack *stacks[] = { pager (shell), list_stack (shell), detail_stack (shell) };

  set_transition (page, VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
  for (guint i = 0; i < G_N_ELEMENTS (stacks); i++)
    if (is_on (shell->menu, stacks[i]) && vibeos_page_stack_get_top (stacks[i]) == shell->menu)
      {
        vibeos_page_stack_set_transition (stacks[i], i == 0 ? VIBEOS_PAGE_TRANSITION_SHARED_AXIS
                                                            : VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE);
        vibeos_page_stack_replace (stacks[i], page);
        return;
      }
  /* the menu went meanwhile */
  g_object_ref_sink (page);
  g_object_unref (page);
}

/* About: VibeOS's About sheet, as every VibeOS app has it */
static const VibeosAboutInfo msg_about = { .name = N_("Messaging"), .icon_name = "vibeos.Messaging" };

static void
on_menu (MsgAccountMenuItem item, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = NULL;

  switch (item)
    {
    case MSG_ACCOUNT_MENU_CLOSE:
      close_page (shell, shell->menu);
      return;
    case MSG_ACCOUNT_MENU_MARK_ALL_READ:
      conversation_list_mark_all_read (shell->client);
      close_page (shell, shell->menu);
      return;
    case MSG_ACCOUNT_MENU_ABOUT:
      close_page (shell, shell->menu);
      vibeos_about_present (shell->window, &msg_about);
      return;
    case MSG_ACCOUNT_MENU_ARCHIVED:
      page = make_list (shell, TRUE);
      break;
    case MSG_ACCOUNT_MENU_BLOCKED:
      page = blocked_contacts_new (shell->client, shell->contacts);
      g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
      break;
    case MSG_ACCOUNT_MENU_SETTINGS:
      page = msg_settings_new ();
      g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
      g_signal_connect (page, "show-advanced", G_CALLBACK (on_show_advanced_settings), shell);
      break;
    }
  replace_menu (shell, page);
}

static void
on_account (GtkWidget *list, gpointer data)
{
  Shell *shell = data;
  GtkWidget *menu = msg_account_menu_new (on_menu, shell);

  g_set_weak_pointer (&shell->menu, menu);
  push_listward (shell, menu, VIBEOS_PAGE_TRANSITION_FADE_THROUGH);
}

/* the person blocked from the details: the page and its conversation both close */
static void
on_options_blocked (GtkWidget *page, gpointer data)
{
  Shell *shell = data;

  if (top_page (shell) != page)
    return;
  go_back (shell);
  if (MSG_IS_CONVERSATION_VIEW (top_page (shell)))
    go_back (shell);
}

static void
on_show_options (ConversationView *view, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = people_options_new (shell->client, shell->contacts, conversation_view_get_id (view));

  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  g_signal_connect (page, "blocked", G_CALLBACK (on_options_blocked), shell);
  /* beside the list in the conversation's pane, over it; else over everything */
  push_from (shell, GTK_WIDGET (view), page, VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
}

/* --- Forward: a dialog over the conversation, titled "Forward message", with the conversations to pick from */

typedef struct
{
  Shell *shell;
  VibeosDialog *dialog;
  char *text;
  gboolean picked;
} Forward;

static void
forward_free (gpointer data)
{
  Forward *forward = data;

  g_free (forward->text);
  g_free (forward);
}

static void
on_forward_picked (GtkWidget *list, gint64 id, gpointer data)
{
  Forward *forward = data;
  Shell *shell = forward->shell;
  g_autofree char *text = g_strdup (forward->text);

  /* a second tap while the dialog fades */
  if (forward->picked)
    return;
  forward->picked = TRUE;
  vibeos_dialog_respond (forward->dialog, VIBEOS_RESPONSE_OK);
  /* the conversation picked, with the text as its draft, takes the place of the one it came from, so Back goes to the
   * list */
  open_conversation (shell, id, text, MSG_IS_CONVERSATION_VIEW (top_page (shell)), NULL);
}

static void
on_forward (ConversationView *view, const char *text, gpointer data)
{
  Shell *shell = data;
  Forward *forward = g_new0 (Forward, 1);
  GtkWidget *list = conversation_list_new_forward (shell->client, shell->contacts);

  forward->shell = shell;
  forward->text = g_strdup (text);
  forward->dialog = vibeos_dialog_new (_("Forward message"));
  gtk_widget_add_css_class (GTK_WIDGET (forward->dialog), "msg-forward-dialog");
  g_object_set_data_full (G_OBJECT (forward->dialog), "msg-forward", forward, forward_free);
  g_signal_connect (list, "open-conversation", G_CALLBACK (on_forward_picked), forward);
  vibeos_dialog_set_content (forward->dialog, list);
  /* beside the list, over the conversation's pane */
  if (is_split (shell) && is_on (GTK_WIDGET (view), detail_stack (shell)))
    vibeos_dialog_set_anchor (forward->dialog, vibeos_list_detail_get_detail_pane (home (shell)));
  vibeos_dialog_present (forward->dialog, shell->pager);
}

/* A conversation opens beside the list on a tablet, else over it with a cut: the tapped row shows its press
 * highlight first, then the page draws (conversation-view.c draws its messages a moment after its bar). Over a page
 * of the pager (search, the archived list, New chat) it opens there. */
static void
open_conversation (Shell *shell, gint64 id, const char *draft, gboolean replace, GtkWidget *row)
{
  GtkWidget *top = top_page (shell);
  GtkWidget *view;

  if (MSG_IS_CONVERSATION_VIEW (top) && conversation_view_get_id (MSG_CONVERSATION_VIEW (top)) == id)
    {
      if (draft)
        conversation_view_set_text (MSG_CONVERSATION_VIEW (top), draft);
      return;
    }
  view = conversation_view_new (shell->client, shell->contacts, id);
  if (draft)
    conversation_view_set_text (MSG_CONVERSATION_VIEW (view), draft);
  g_signal_connect (view, "closed", G_CALLBACK (on_page_closed), shell);
  g_signal_connect (view, "show-options", G_CALLBACK (on_show_options), shell);
  g_signal_connect (view, "forward", G_CALLBACK (on_forward), shell);
  set_transition (view, VIBEOS_PAGE_TRANSITION_NONE);
  if (vibeos_page_stack_get_depth (pager (shell)) > 1)
    {
      vibeos_page_stack_set_transition (pager (shell), VIBEOS_PAGE_TRANSITION_NONE);
      if (replace)
        vibeos_page_stack_replace (pager (shell), view);
      else
        vibeos_page_stack_push (pager (shell), view);
      return;
    }
  vibeos_page_stack_set_transition (vibeos_list_detail_get_list_stack (home (shell)), VIBEOS_PAGE_TRANSITION_NONE);
  if (row)
    vibeos_list_detail_show_from (home (shell), view, row);
  else
    vibeos_list_detail_show (home (shell), view);
}

/* --- from outside */

void
messaging_window_open_conversation (GtkWidget *window, gint64 conversation, const char *draft)
{
  Shell *shell = shell_of (window);
  GtkWidget *top = top_page (shell);

  /* a notification's tap: the conversation, over the list */
  if (!(MSG_IS_CONVERSATION_VIEW (top) && conversation_view_get_id (MSG_CONVERSATION_VIEW (top)) == conversation))
    vibeos_page_stack_pop_to_root (pager (shell));
  open_conversation (shell, conversation, draft, FALSE, NULL);
}

void
messaging_window_open_numbers (GtkWidget *window, const char * const *numbers, const char *draft)
{
  Shell *shell = shell_of (window);
  gint64 id = sms_client_get_conversation (shell->client, numbers);

  if (id > 0)
    messaging_window_open_conversation (window, id, draft);
  else
    on_start_chat (shell->list, shell);
}

void
messaging_window_show_conversations (GtkWidget *window)
{
  Shell *shell = shell_of (window);

  vibeos_page_stack_pop_to_root (pager (shell));
  vibeos_list_detail_close_detail (home (shell));
  /* and the list itself, not a page over it in its pane */
  if (is_split (shell))
    vibeos_page_stack_pop_to_root (list_stack (shell));
}

/* a page that belongs in the conversation's pane: a conversation, New chat, People & options */
static gboolean
opens_beside (GtkWidget *page)
{
  return MSG_IS_CONVERSATION_VIEW (page) || MSG_IS_CONTACT_PICKER (page) || MSG_IS_PEOPLE_OPTIONS (page);
}

/* Two panes now (a tablet turned on its side): each open page goes where it would have opened in two, the pages the
 * list opens over the list and a conversation, with what it opens, beside it. The list-detail has moved every page
 * that was over the list beside it, and the pager holds what was open over everything; in each the pages the list
 * opens come first */
static void
sort_into_panes (Shell *shell)
{
  VibeosPageStack *detail = detail_stack (shell);
  VibeosPageStack *beside = VIBEOS_PAGE_STACK (g_object_ref_sink (vibeos_page_stack_new ()));
  guint i;

  for (i = 0; i < vibeos_page_stack_get_depth (detail) && !opens_beside (vibeos_page_stack_get_page (detail, i)); i++)
    ;
  vibeos_page_stack_move_pages (detail, i, beside);
  vibeos_page_stack_move_pages (detail, 0, list_stack (shell));
  for (i = 1; i < vibeos_page_stack_get_depth (pager (shell))
              && !opens_beside (vibeos_page_stack_get_page (pager (shell), i)); i++)
    ;
  vibeos_page_stack_move_pages (pager (shell), i, beside);
  vibeos_page_stack_move_pages (pager (shell), 1, list_stack (shell));
  vibeos_page_stack_move_pages (beside, 0, detail);
  g_object_unref (beside);
}

/* the conversation pane with nothing open: an empty pane with a quiet picture and a line in its middle */
static GtkWidget *
make_placeholder (Shell *shell)
{
  GtkWidget *pane = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *middle = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("msg-chat-symbolic");
  GtkWidget *label = gtk_label_new (_("Pick a conversation to see it here"));

  gtk_widget_add_css_class (pane, "msg-detail-empty");
  gtk_widget_set_overflow (pane, GTK_OVERFLOW_HIDDEN);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), EMPTY_ICON);
  gtk_widget_add_css_class (icon, "msg-detail-empty-icon");
  gtk_widget_set_name (icon, "empty_state_image");
  gtk_box_append (GTK_BOX (middle), icon);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (label, "msg-detail-empty-text");
  gtk_widget_set_name (label, "empty_state_text");
  gtk_box_append (GTK_BOX (middle), label);
  gtk_widget_set_valign (middle, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (middle, TRUE);
  /* only with a pane to show them on: one pane gives it no width */
  gtk_widget_set_visible (middle, FALSE);
  gtk_box_append (GTK_BOX (pane), middle);
  shell->empty = middle;
  shell->empty_text = label;
  update_empty_text (shell);
  return pane;
}

static void
on_detail_closed (VibeosListDetail *ld, Shell *shell)
{
  for (guint i = 0; i < vibeos_page_stack_get_depth (list_stack (shell)); i++)
    {
      GtkWidget *page = vibeos_page_stack_get_page (list_stack (shell), i);

      if (MSG_IS_CONVERSATION_LIST (page))
        conversation_list_set_current (MSG_CONVERSATION_LIST (page), 0);
    }
}

/* two panes or one: the panes' look (messaging.css), and the back arrows of the conversations that moved */
static void
on_split (Shell *shell)
{
  if (is_split (shell))
    {
      sort_into_panes (shell);
      gtk_widget_add_css_class (shell->home, "msg-split");
    }
  else
    gtk_widget_remove_css_class (shell->home, "msg-split");
  gtk_widget_set_visible (shell->empty, is_split (shell));
  sync_back_arrows (shell);
  update_visible (shell);
}

GtkWidget *
messaging_window_new (GtkApplication *app)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = vibeos_window_new (app);
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();

  shell->window = window;
  shell->client = sms_client_new ();
  shell->contacts = contacts_new ();
  g_object_set_data_full (G_OBJECT (window), "messaging-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Messages"));
  gtk_widget_add_css_class (window, "msg-window");

  shell->list = make_list (shell, FALSE);
  g_signal_connect (shell->list, "start-chat", G_CALLBACK (on_start_chat), shell);
  g_signal_connect (shell->list, "show-search", G_CALLBACK (on_search), shell);
  g_signal_connect (shell->list, "show-account", G_CALLBACK (on_account), shell);
  shell->home = vibeos_list_detail_new ();
  gtk_widget_add_css_class (shell->home, "msg-home");
  vibeos_list_detail_set_list (home (shell), shell->list);
  vibeos_list_detail_set_placeholder (home (shell), make_placeholder (shell));
  vibeos_page_stack_set_highlight_time (vibeos_list_detail_get_list_stack (home (shell)), ROW_HIGHLIGHT_MS);
  g_signal_connect (home (shell), "detail-closed", G_CALLBACK (on_detail_closed), shell);
  g_signal_connect (vibeos_list_detail_get_list_stack (home (shell)), "top-changed", G_CALLBACK (on_top_changed),
                    shell);
  g_signal_connect (vibeos_list_detail_get_detail_stack (home (shell)), "top-changed",
                    G_CALLBACK (on_top_changed), shell);
  g_signal_connect_swapped (home (shell), "notify::split", G_CALLBACK (on_split), shell);

  shell->pager = vibeos_page_stack_new ();
  vibeos_page_stack_push (pager (shell), shell->home);
  g_signal_connect (shell->pager, "top-changed", G_CALLBACK (on_top_changed), shell);
  vibeos_window_set_content (GTK_WINDOW (window), shell->pager);
  /* the window's own, which goes with the window: held until the shell goes */
  shell->layout = g_object_ref (vibeos_layout_get_for_window (GTK_WINDOW (window)));
  g_signal_connect_swapped (shell->layout, "changed", G_CALLBACK (update_list_width), shell);
  update_list_width (shell);

  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_active), shell);
  g_signal_connect_swapped (window, "notify::suspended", G_CALLBACK (update_visible), shell);
  g_signal_connect_swapped (window, "map", G_CALLBACK (update_visible), shell);
  /* Escape is Back (a keyboard's; the shell's gesture presses the page's "Back" button) */
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_escape, shell, NULL)));
  gtk_widget_add_controller (window, shortcuts);
  return window;
}
