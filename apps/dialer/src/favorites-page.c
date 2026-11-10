/* The Favorites tab: see favorites-page.h. The rules are SpeedDialFragment's, SpeedDialUiItem's (the default
 * channel), FavoritesViewHolder's, ContextMenu's and DisambigDialog's; the geometry is fragment_speed_dial.xml's,
 * speed_dial_header_layout.xml's and favorite_item_layout.xml's, measured on still:dialer-main (spec §6). */
#include "favorites-page.h"
#include "avatar.h"
#include "calls-client.h"
#include "call-types.h"
#include "contacts-app.h"
#include "contacts-page.h"
#include "empty-view.h"
#include "phone.h"
#include "popup-menu.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

#define COLUMNS         3       /* SpeedDialLayoutManager's spans */
#define LIST_PAD_BOTTOM 88
#define HEADER_MARGIN   16      /* the header row's marginStart/End */
#define HEADER_HEIGHT   48
#define TILE_TOP        8
#define TILE_BOTTOM     12
#define AVATAR          104
#define AVATAR_GAP      8       /* avatar_container's marginBottom */
#define TEXT_SIDE       4
#define NAME_TOP        8
#define NAME_BOTTOM     1
#define TYPE_BOTTOM     17
#define OPTION_PAD_X    24      /* disambig_option_layout */
#define OPTION_PAD_Y    8
#define OPTION_HEIGHT   56
#define OPTIONS_MIN     168     /* disambig_scrollview's minHeight and maxHeight */
#define OPTIONS_MAX     256

/* ---- a favourite: the contact and the number its tile calls --------------------------------------------------- */

typedef struct
{
  char *uid;
  char *name;
  GBytes *photo;
  GPtrArray *numbers;           /* ContactNumber copies */
  char *number;                 /* the default channel: the only number, or the one remembered; NULL to ask */
  char *label;                  /* its kind ("Mobile"), "" when it has none */
} Favorite;

static void
number_free (ContactNumber *number)
{
  g_free (number->number);
  g_free (number->label);
  g_free (number);
}

static void
favorite_free (Favorite *favorite)
{
  g_free (favorite->uid);
  g_free (favorite->name);
  g_clear_pointer (&favorite->photo, g_bytes_unref);
  g_ptr_array_unref (favorite->numbers);
  g_free (favorite->number);
  g_free (favorite->label);
  g_free (favorite);
}

static Favorite *
favorite_new (const Contact *contact, const char *remembered)
{
  Favorite *favorite = g_new0 (Favorite, 1);

  favorite->uid = g_strdup (contact->uid);
  favorite->name = g_strdup (contact->name ? contact->name : "");
  favorite->photo = contact->photo ? g_bytes_ref (contact->photo) : NULL;
  favorite->numbers = g_ptr_array_new_with_free_func ((GDestroyNotify) number_free);
  for (guint i = 0; i < contact->numbers->len; i++)
    {
      const ContactNumber *number = contact->numbers->pdata[i];
      ContactNumber *copy = g_new0 (ContactNumber, 1);

      copy->number = g_strdup (number->number);
      copy->label = g_strdup (number->label ? number->label : "");
      g_ptr_array_add (favorite->numbers, copy);
      /* the default channel: the remembered number, or the only one */
      if ((remembered && g_str_equal (remembered, number->number)) || contact->numbers->len == 1)
        {
          favorite->number = g_strdup (copy->number);
          favorite->label = g_strdup (copy->label);
        }
    }
  return favorite;
}

/* ---- the page ----------------------------------------------------------------------------------------------- */

struct _FavoritesPage
{
  GtkWidget parent;
  Contacts *contacts;
  GSettings *settings;
  NeoSnackbar *snackbar;
  GtkWidget *stack, *scroller, *empty, *grid;
  GPtrArray *favorites;         /* Favorite, in the tiles' order */
  GtkWidget *picker;            /* the contact picker over the window, while it is up */
};

G_DEFINE_FINAL_TYPE (FavoritesPage, favorites_page, GTK_TYPE_WIDGET)

static void load (FavoritesPage *self);

static void
on_dialed (guint id, const GError *error, gpointer data)
{
  FavoritesPage *self = data;

  if (error)
    neo_snackbar_show (self->snackbar, error->message, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
  else
    phone_start_app (INCALL_APP_ID);
  g_object_unref (self);
}

static void
place_call (FavoritesPage *self, const char *number)
{
  calls_client_dial (calls_client_get (), number, on_dialed, g_object_ref (self));
}

/* the star goes (ContextMenu's Remove) */
static void
remove_favorite (FavoritesPage *self, const char *uid)
{
  g_auto(GStrv) uids = g_settings_get_strv (self->settings, "favorites");
  g_autoptr(GStrvBuilder) kept = g_strv_builder_new ();
  g_auto(GStrv) result = NULL;

  for (guint i = 0; uids[i]; i++)
    if (!g_str_equal (uids[i], uid))
      g_strv_builder_add (kept, uids[i]);
  result = g_strv_builder_end (kept);
  g_settings_set_strv (self->settings, "favorites", (const char * const *) result);
}

/* ---- "Choose call type" (DisambigDialog) ---------------------------------------------------------------------- */

typedef struct
{
  FavoritesPage *page;
  NeoDialog *dialog;
  GtkWidget *remember;
  char *uid;
} Disambig;

static void
disambig_free (Disambig *disambig)
{
  g_object_unref (disambig->page);
  g_free (disambig->uid);
  g_free (disambig);
}

static void
on_option_clicked (GtkWidget *option, Disambig *disambig)
{
  const char *number = g_object_get_data (G_OBJECT (option), "disambig-number");
  FavoritesPage *self = disambig->page;

  /* "Remember this choice": the number becomes the favourite's default (IS_SUPER_PRIMARY on Android) */
  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (disambig->remember)))
    {
      g_autoptr(GVariant) old = g_settings_get_value (self->settings, "favorite-numbers");
      GVariantBuilder numbers;
      GVariantIter iter;
      const char *uid, *value;

      g_variant_builder_init (&numbers, G_VARIANT_TYPE ("a{ss}"));
      g_variant_iter_init (&iter, old);
      while (g_variant_iter_next (&iter, "{&s&s}", &uid, &value))
        if (!g_str_equal (uid, disambig->uid))
          g_variant_builder_add (&numbers, "{ss}", uid, value);
      g_variant_builder_add (&numbers, "{ss}", disambig->uid, number);
      g_settings_set_value (self->settings, "favorite-numbers", g_variant_builder_end (&numbers));
    }
  place_call (self, number);
  neo_dialog_respond (disambig->dialog, NEO_RESPONSE_OK);
}

static GtkWidget *
disambig_option (Disambig *disambig, const ContactNumber *number)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *label = gtk_label_new (number->label);
  GtkWidget *digits = gtk_label_new (number->number);
  GtkWidget *icon = gtk_image_new_from_icon_name ("dialer-call-symbolic");
  GtkWidget *option;

  gtk_widget_set_margin_start (box, OPTION_PAD_X);
  gtk_widget_set_margin_end (box, OPTION_PAD_X);
  gtk_widget_set_margin_top (box, OPTION_PAD_Y);
  gtk_widget_set_margin_bottom (box, OPTION_PAD_Y);
  gtk_widget_set_size_request (box, -1, OPTION_HEIGHT - 2 * OPTION_PAD_Y);
  for (GtkWidget *text = label; text; text = text == label ? digits : NULL)
    {
      gtk_label_set_xalign (GTK_LABEL (text), 0);
      gtk_widget_add_css_class (text, "dialer-primary-text");
      neo_label_set_font_padding (text, 16);
      gtk_box_append (GTK_BOX (texts), text);
    }
  gtk_widget_set_visible (label, *number->label);
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (texts, 16);
  gtk_box_append (GTK_BOX (box), texts);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (icon, "dialer-icon");
  gtk_box_append (GTK_BOX (box), icon);
  option = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (option), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (option), &(GdkRGBA) { 1, 1, 1, 0.06 });
  gtk_accessible_update_property (GTK_ACCESSIBLE (option), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  C_("disambig_option_voice_call", "Call"), -1);
  g_object_set_data_full (G_OBJECT (option), "disambig-number", g_strdup (number->number), g_free);
  g_signal_connect (option, "clicked", G_CALLBACK (on_option_clicked), disambig);
  return option;
}

static void
show_disambig (FavoritesPage *self, const Favorite *favorite)
{
  Disambig *disambig = g_new0 (Disambig, 1);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = gtk_label_new (_("Choose call type"));
  GtkWidget *options = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  disambig->page = g_object_ref (self);
  disambig->uid = g_strdup (favorite->uid);
  disambig->dialog = neo_dialog_new (NULL);
  neo_dialog_set_flush (disambig->dialog, TRUE);
  gtk_widget_add_css_class (content, "disambig");

  /* the title strip: Header2, 56 dp, 24 dp in */
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_set_size_request (title, -1, OPTION_HEIGHT);
  gtk_widget_add_css_class (title, "disambig-title");
  gtk_box_append (GTK_BOX (content), title);

  for (guint i = 0; i < favorite->numbers->len; i++)
    gtk_box_append (GTK_BOX (options), disambig_option (disambig, favorite->numbers->pdata[i]));
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), options);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller), OPTIONS_MIN);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), OPTIONS_MAX);
  gtk_box_append (GTK_BOX (content), scroller);

  /* "Remember this choice": a 48 dp check box in a 64 dp strip */
  disambig->remember = gtk_check_button_new_with_label (_("Remember this choice"));
  gtk_widget_add_css_class (disambig->remember, "disambig-remember");
  gtk_widget_set_size_request (disambig->remember, -1, 48);
  gtk_widget_set_valign (disambig->remember, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (disambig->remember, TRUE);
  gtk_widget_set_size_request (footer, -1, 64);
  gtk_box_append (GTK_BOX (footer), disambig->remember);
  gtk_box_append (GTK_BOX (content), footer);

  neo_dialog_set_content (disambig->dialog, content);
  g_object_set_data_full (G_OBJECT (disambig->dialog), "disambig", disambig, (GDestroyNotify) disambig_free);
  neo_dialog_present (disambig->dialog, GTK_WIDGET (self));
}

/* ---- the context menu --------------------------------------------------------------------------------------- */

typedef struct
{
  FavoritesPage *page;
  char *uid;
  char *number;
} MenuTarget;

static void
menu_target_free (gpointer data)
{
  MenuTarget *target = data;

  g_object_unref (target->page);
  g_free (target->uid);
  g_free (target->number);
  g_free (target);
}

static void
on_menu_call (gpointer data)
{
  MenuTarget *target = data;

  place_call (target->page, target->number);
}

static void
on_menu_message (gpointer data)
{
  MenuTarget *target = data;
  g_autofree char *uri = g_strconcat ("sms:", target->number, NULL);
  g_autoptr(GError) error = NULL;

  if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
    g_warning ("favorites: %s: %s", uri, error->message);
}

static void
on_menu_remove (gpointer data)
{
  MenuTarget *target = data;

  remove_favorite (target->page, target->uid);
}

static void
on_menu_info (gpointer data)
{
  MenuTarget *target = data;

  contacts_app_view (GTK_WIDGET (target->page), target->uid, target->page->snackbar);
}

/* ContextMenu: "<kind> <number>" (dimmed), Voice Call and Message for the default voice channel, Remove, Contact
 * info. No Video Call: nothing here can make one */
static void
show_menu (FavoritesPage *self, const Favorite *favorite, GtkWidget *anchor)
{
  DialerPopupMenu *menu = dialer_popup_menu_new ();
  MenuTarget *target = g_new0 (MenuTarget, 1);
  g_autoptr(GAppInfo) sms = g_app_info_get_default_for_uri_scheme ("sms");

  target->page = g_object_ref (self);
  target->uid = g_strdup (favorite->uid);
  target->number = g_strdup (favorite->number);
  g_object_set_data_full (G_OBJECT (menu), "menu-target", target, menu_target_free);
  if (favorite->number)
    {
      g_autofree char *title = *favorite->label ? g_strdup_printf (_("%1$s %2$s"), favorite->label, favorite->number)
                                                : g_strdup (favorite->number);

      dialer_popup_menu_append (menu, title, NULL, NULL);
      dialer_popup_menu_append (menu, _("Voice Call"), on_menu_call, target);
      if (sms)
        dialer_popup_menu_append (menu, _("Message"), on_menu_message, target);
    }
  dialer_popup_menu_append (menu, _("Remove"), on_menu_remove, target);
  dialer_popup_menu_append (menu, _("Contact info"), on_menu_info, target);
  dialer_popup_menu_present (menu, anchor, POPUP_MENU_START);
}

/* ---- the tiles ---------------------------------------------------------------------------------------------- */

static Favorite *
tile_favorite (GtkWidget *tile)
{
  return g_object_get_data (G_OBJECT (tile), "favorite");
}

static void
on_tile_clicked (GtkWidget *tile, FavoritesPage *self)
{
  Favorite *favorite = tile_favorite (tile);

  if (favorite->number)
    place_call (self, favorite->number);
  else if (favorite->numbers->len)
    show_disambig (self, favorite);
}

static void
on_tile_long_pressed (GtkWidget *tile, FavoritesPage *self)
{
  /* the menu hangs from the picture (ContextMenu's anchor is the photo view) */
  show_menu (self, tile_favorite (tile), g_object_get_data (G_OBJECT (tile), "favorite-avatar"));
}

static GtkWidget *
make_tile (FavoritesPage *self, Favorite *favorite)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *avatar = dialer_avatar_new (AVATAR);
  GtkWidget *name = gtk_label_new (favorite->name);
  GtkWidget *type = gtk_label_new (favorite->label ? favorite->label : "");
  GtkWidget *tile;

  gtk_widget_set_margin_top (box, TILE_TOP);
  gtk_widget_set_margin_bottom (box, TILE_BOTTOM);
  dialer_avatar_set (DIALER_AVATAR (avatar), favorite->uid, favorite->name, favorite->photo);
  gtk_widget_set_halign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (avatar, AVATAR_GAP);
  gtk_box_append (GTK_BOX (box), avatar);

  /* the name: 14 sp, config_headlineFontFamilyMedium, centred, wrapping */
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (name), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_justify (GTK_LABEL (name), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_start (name, TEXT_SIDE);
  gtk_widget_set_margin_end (name, TEXT_SIDE);
  gtk_widget_set_margin_top (name, NAME_TOP);
  gtk_widget_set_margin_bottom (name, NAME_BOTTOM);
  gtk_widget_add_css_class (name, "favorite-name");
  neo_label_set_font_padding (name, 14);
  gtk_box_append (GTK_BOX (box), name);

  /* the default channel's kind; empty (one line still) when there is none */
  gtk_label_set_ellipsize (GTK_LABEL (type), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start (type, TEXT_SIDE);
  gtk_widget_set_margin_end (type, TEXT_SIDE);
  gtk_widget_set_margin_bottom (type, TYPE_BOTTOM);
  gtk_widget_add_css_class (type, "favorite-type");
  neo_label_set_font_padding (type, 14);
  gtk_box_append (GTK_BOX (box), type);

  /* no ripple: the tile's root has no background */
  tile = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (tile), NEO_RIPPLE_NONE, 0);
  gtk_widget_set_hexpand (tile, TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (tile), GTK_ACCESSIBLE_PROPERTY_LABEL, favorite->name, -1);
  g_object_set_data (G_OBJECT (tile), "favorite", favorite);
  g_object_set_data (G_OBJECT (tile), "favorite-avatar", avatar);
  g_signal_connect (tile, "clicked", G_CALLBACK (on_tile_clicked), self);
  g_signal_connect (tile, "long-pressed", G_CALLBACK (on_tile_long_pressed), self);
  return tile;
}

/* ---- "Add": the contact picker ------------------------------------------------------------------------------ */

static void
close_picker (FavoritesPage *self)
{
  GtkWidget *picker = g_steal_pointer (&self->picker);
  GtkWidget *overlay;

  if (!picker)
    return;
  overlay = gtk_widget_get_parent (picker);
  if (overlay)
    gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), picker);
}

static void
on_picked (ContactsPage *page, const char *uid, FavoritesPage *self)
{
  g_auto(GStrv) uids = g_settings_get_strv (self->settings, "favorites");

  /* a new favourite goes at the end (the next pinned position) */
  if (!g_strv_contains ((const char * const *) uids, uid))
    {
      g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
      g_auto(GStrv) result = NULL;

      g_strv_builder_addv (builder, (const char **) uids);
      g_strv_builder_add (builder, uid);
      result = g_strv_builder_end (builder);
      g_settings_set_strv (self->settings, "favorites", (const char * const *) result);
    }
  close_picker (self);
}

static gboolean
on_picker_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, FavoritesPage *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  close_picker (self);
  return TRUE;
}

/* ACTION_PICK on the phones: the contacts with a number, over the whole window under a bar with a back arrow */
static void
open_picker (FavoritesPage *self)
{
  GtkOverlay *overlay = neo_window_get_overlay (GTK_WIDGET (self));
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *back = neo_icon_button_new ("dialer-back-symbolic");
  GtkWidget *title = gtk_label_new (_("Choose a contact"));
  GtkWidget *list = contacts_page_new_picker (self->contacts, self->snackbar);
  GtkEventController *keys = gtk_event_controller_key_new ();

  if (!overlay || self->picker)
    return;
  self->picker = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->picker, "dialer-picker");
  gtk_widget_set_size_request (bar, -1, 64);
  gtk_widget_set_margin_start (back, 8);
  gtk_widget_set_valign (back, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (back, "dialer-icon");
  /* Android's "Navigate up"; named for the shell's back gesture, which clicks a Back button */
  gtk_accessible_update_property (GTK_ACCESSIBLE (back), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Navigate up"), -1);
  g_signal_connect_swapped (back, "clicked", G_CALLBACK (close_picker), self);
  gtk_box_append (GTK_BOX (bar), back);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_set_margin_start (title, 24);
  gtk_widget_add_css_class (title, "dialer-picker-title");
  gtk_box_append (GTK_BOX (bar), title);
  gtk_box_append (GTK_BOX (self->picker), bar);
  gtk_widget_set_vexpand (list, TRUE);
  gtk_box_append (GTK_BOX (self->picker), list);
  g_signal_connect (list, "picked", G_CALLBACK (on_picked), self);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_picker_key), self);
  gtk_widget_add_controller (self->picker, keys);
  gtk_overlay_add_overlay (overlay, self->picker);
  gtk_widget_set_focusable (self->picker, TRUE);
  gtk_widget_grab_focus (self->picker);
}

static void
on_add_clicked (FavoritesPage *self)
{
  open_picker (self);
}

static void
on_add_favorite (gpointer data)
{
  open_picker (data);
}

/* the header row: "Favorites" in Secondary at 16 sp, and the borderless "Add" at its end */
static GtkWidget *
make_header (FavoritesPage *self)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *title = gtk_label_new (_("Favorites"));
  GtkWidget *add_label = gtk_label_new (C_("speed_dial_add_button_text", "Add"));
  GtkWidget *add;

  gtk_widget_set_margin_start (row, HEADER_MARGIN);
  gtk_widget_set_margin_end (row, HEADER_MARGIN);
  gtk_widget_set_size_request (row, -1, HEADER_HEIGHT);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_widget_set_valign (title, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (title, "favorites-header");
  neo_label_set_font_padding (title, 16);
  gtk_box_append (GTK_BOX (row), title);
  gtk_widget_set_margin_start (add_label, 8);
  gtk_widget_set_margin_end (add_label, 8);
  gtk_widget_set_valign (add_label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (add_label, "favorites-add");
  add = neo_pressable_new (add_label);
  gtk_widget_set_size_request (add, 48, 48);
  gtk_widget_set_valign (add, GTK_ALIGN_CENTER);
  neo_pressable_set_ripple (NEO_PRESSABLE (add), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (add), &(GdkRGBA) { 1, 1, 1, 0.06 });
  g_signal_connect_swapped (add, "clicked", G_CALLBACK (on_add_clicked), self);
  gtk_box_append (GTK_BOX (row), add);
  return row;
}

/* the favourites against the contacts as they are now: a deleted contact's star is dropped from view */
static void
load (FavoritesPage *self)
{
  g_auto(GStrv) uids = g_settings_get_strv (self->settings, "favorites");
  g_autoptr(GVariant) remembered = g_settings_get_value (self->settings, "favorite-numbers");
  GtkWidget *child;
  guint shown = 0;

  while ((child = gtk_widget_get_first_child (self->grid)))
    gtk_grid_remove (GTK_GRID (self->grid), child);
  g_ptr_array_set_size (self->favorites, 0);
  for (guint i = 0; uids[i]; i++)
    {
      const Contact *contact = contacts_get (self->contacts, uids[i]);
      const char *number = NULL;
      Favorite *favorite;

      if (!contact || !contact->numbers->len)
        continue;
      g_variant_lookup (remembered, uids[i], "&s", &number);
      favorite = favorite_new (contact, number);
      g_ptr_array_add (self->favorites, favorite);
      gtk_grid_attach (GTK_GRID (self->grid), make_tile (self, favorite), shown % COLUMNS, shown / COLUMNS, 1, 1);
      shown++;
    }
  /* only the headers left: the empty view (no "Frequents" without usage counts) */
  gtk_stack_set_visible_child (GTK_STACK (self->stack),
                               shown || !contacts_is_loaded (self->contacts) ? self->scroller : self->empty);
}

static void
on_changed (FavoritesPage *self)
{
  load (self);
}

static void
favorites_page_dispose (GObject *object)
{
  FavoritesPage *self = FAVORITES_PAGE (object);

  close_picker (self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->contacts);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->stack, gtk_widget_unparent);
  g_clear_pointer (&self->favorites, g_ptr_array_unref);
  G_OBJECT_CLASS (favorites_page_parent_class)->dispose (object);
}

static void
favorites_page_class_init (FavoritesPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = favorites_page_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "favorites-page");
}

static void
favorites_page_init (FavoritesPage *self)
{
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  self->favorites = g_ptr_array_new_with_free_func ((GDestroyNotify) favorite_free);
  self->stack = gtk_stack_new ();
  gtk_widget_set_parent (self->stack, GTK_WIDGET (self));
  gtk_widget_set_margin_bottom (list, LIST_PAD_BOTTOM);
  gtk_widget_set_valign (list, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (list), make_header (self));
  self->grid = gtk_grid_new ();
  gtk_grid_set_column_homogeneous (GTK_GRID (self->grid), TRUE);
  gtk_box_append (GTK_BOX (list), self->grid);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), list);
  gtk_stack_add_child (GTK_STACK (self->stack), self->scroller);
  /* SpeedDialFragment's empty view: the speed-dial picture, the line, "Add favorite" */
  self->empty = empty_view_new ("/de/yesman/Dialer/images/empty-speed-dial.png",
                                _("You haven't added any favorites yet"), _("Add favorite"), on_add_favorite, self);
  gtk_stack_add_child (GTK_STACK (self->stack), self->empty);
}

GtkWidget *
favorites_page_new (Contacts *contacts, GSettings *settings, NeoSnackbar *snackbar)
{
  FavoritesPage *self = g_object_new (FAVORITES_TYPE_PAGE, NULL);

  self->contacts = g_object_ref (contacts);
  self->settings = g_object_ref (settings);
  self->snackbar = snackbar;
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_changed), self);
  g_signal_connect_swapped (settings, "changed::favorites", G_CALLBACK (on_changed), self);
  g_signal_connect_swapped (settings, "changed::favorite-numbers", G_CALLBACK (on_changed), self);
  load (self);
  return GTK_WIDGET (self);
}
