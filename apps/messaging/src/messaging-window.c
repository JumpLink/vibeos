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
#define LIST_RATIO 0.4          /* the list's share of a tablet's width */
#define LIST_MIN   360          /* as wide as a phone at least */
#define LIST_MAX   480

typedef struct
{
  GtkWidget *window;
  SmsClient *client;
  Contacts *contacts;
  GtkWidget *pager;             /* the pages over everything: search, the account menu, New chat, settings … */
  GtkWidget *home;              /* the list and the conversation open, beside it on a tablet */
  GtkWidget *list;
  GtkWidget *shown;             /* the conversation view last on top, whose draft is kept when it goes */
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

/* the list beside a conversation on a tablet: its share of the width, but as wide as a phone at least */
static void
update_list_width (Shell *shell)
{
  vibeos_list_detail_set_list_width (home (shell), (int) CLAMP (vibeos_layout_get_width (shell->layout) * LIST_RATIO,
                                                                LIST_MIN, LIST_MAX));
}

static VibeosPageStack *
pager (Shell *shell)
{
  return VIBEOS_PAGE_STACK (shell->pager);
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
  /* beside the list: its row shows which */
  {
    VibeosPageStack *detail = vibeos_list_detail_get_detail_stack (home (shell));
    GtkWidget *open = vibeos_list_detail_is_split (home (shell)) && vibeos_page_stack_get_depth (detail)
                      ? vibeos_page_stack_get_page (detail, 0) : NULL;

    conversation_list_set_current (MSG_CONVERSATION_LIST (shell->list), MSG_IS_CONVERSATION_VIEW (open)
                                   ? conversation_view_get_id (MSG_CONVERSATION_VIEW (open)) : 0);
  }
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
  /* a conversation over the list closes as it opened: a cut */
  vibeos_page_stack_set_transition (vibeos_list_detail_get_list_stack (home (shell)), VIBEOS_PAGE_TRANSITION_NONE);
  if (!vibeos_list_detail_back (home (shell)))
    gtk_window_close (GTK_WINDOW (shell->window));
}

/* a page closing itself goes only if it is the one in front (a conversation deleted under the picker stays) */
static void
on_page_closed (GtkWidget *page, gpointer data)
{
  Shell *shell = data;

  if (top_page (shell) == page)
    go_back (shell);
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

static void
on_start_chat (GtkWidget *list, gpointer data)
{
  Shell *shell = data;
  GtkWidget *picker = contact_picker_new (shell->client, shell->contacts);

  g_signal_connect (picker, "picked", G_CALLBACK (on_picked), shell);
  g_signal_connect (picker, "closed", G_CALLBACK (on_page_closed), shell);
  push (shell, picker, VIBEOS_PAGE_TRANSITION_FADE_THROUGH);
  gtk_widget_grab_focus (picker);
}

static void
on_list_at_top (GtkWidget *list, gpointer data)
{
  update_visible (data);
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
  push (shell, page, VIBEOS_PAGE_TRANSITION_FADE_THROUGH);
  /* the keyboard comes up at once */
  gtk_widget_grab_focus (page);
}

static void
on_show_advanced_settings (GtkWidget *settings, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = msg_settings_new_advanced ();

  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  push (shell, page, VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
}

/* a page the account menu opens takes its place: Back from it goes to the list */
static void
replace_menu (Shell *shell, GtkWidget *page)
{
  set_transition (page, VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
  vibeos_page_stack_set_transition (pager (shell), VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
  vibeos_page_stack_replace (pager (shell), page);
}

static void
on_menu (MsgAccountMenuItem item, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = NULL;

  switch (item)
    {
    case MSG_ACCOUNT_MENU_CLOSE:
      go_back (shell);
      return;
    case MSG_ACCOUNT_MENU_MARK_ALL_READ:
      conversation_list_mark_all_read (shell->client);
      go_back (shell);
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

  push (shell, msg_account_menu_new (on_menu, shell), VIBEOS_PAGE_TRANSITION_FADE_THROUGH);
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
  push (shell, page, VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
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
}

static GtkWidget *
make_placeholder (void)
{
  GtkWidget *label = gtk_label_new (_("Pick a conversation to see it here"));

  gtk_widget_add_css_class (label, "msg-detail-empty");
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  return label;
}

static void
on_detail_closed (VibeosListDetail *ld, Shell *shell)
{
  conversation_list_set_current (MSG_CONVERSATION_LIST (shell->list), 0);
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
  vibeos_list_detail_set_placeholder (home (shell), make_placeholder ());
  vibeos_page_stack_set_highlight_time (vibeos_list_detail_get_list_stack (home (shell)), ROW_HIGHLIGHT_MS);
  g_signal_connect (home (shell), "detail-closed", G_CALLBACK (on_detail_closed), shell);
  g_signal_connect (vibeos_list_detail_get_list_stack (home (shell)), "top-changed", G_CALLBACK (on_top_changed),
                    shell);
  g_signal_connect (vibeos_list_detail_get_detail_stack (home (shell)), "top-changed",
                    G_CALLBACK (on_top_changed), shell);
  g_signal_connect_swapped (home (shell), "notify::split", G_CALLBACK (update_visible), shell);

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
