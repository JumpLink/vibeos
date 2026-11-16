/* The touch handle under a text field's cursor, put away when typing starts.
 *
 * GTK shows a field's handles when a finger places its cursor or selects in it, and puts them away at the next key
 * press. The on-screen keyboard types through the input method, which is no key press, so the cursor's handle stayed
 * under the text as it was typed. Here a change in the focused field's text puts the handles away as a key press
 * does: the field's own key handler runs, with no key (GtkText and GtkTextView do nothing else for one but restart
 * the cursor's blink and close the selection popup). A finger placing the cursor or selecting brings them back, as
 * before. */
#include <gtk/gtk.h>

/* the key controller GTK gave the field: GtkText names its own; a GtkTextView's is the first added, the last in the
 * widget's list (each controller is put in front of those before it), behind any an app added */
static GtkEventController *
own_key_controller (GtkWidget *widget)
{
  g_autoptr (GListModel) controllers = gtk_widget_observe_controllers (widget);
  GtkEventController *found = NULL;

  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++)
    {
      g_autoptr (GtkEventController) controller = g_list_model_get_item (controllers, i);

      if (!GTK_IS_EVENT_CONTROLLER_KEY (controller))
        continue;
      if (GTK_IS_TEXT (widget))
        {
          if (g_strcmp0 (gtk_event_controller_get_name (controller), "gtk-text-key-controller") == 0)
            return controller;
        }
      else
        found = controller;
    }
  return found;
}

static void
put_handles_away (GtkWidget *field)
{
  GtkEventController *controller = own_key_controller (field);
  gboolean handled = FALSE;

  if (controller)
    g_signal_emit_by_name (controller, "key-pressed", 0u, 0u, (GdkModifierType) 0, &handled);
}

/* GtkEditable::changed, from every GtkText (an entry's text is one) */
static gboolean
on_editable_changed (GSignalInvocationHint *hint, guint n_values, const GValue *values, gpointer data)
{
  GObject *instance = g_value_get_object (&values[0]);

  if (GTK_IS_TEXT (instance) && gtk_widget_has_focus (GTK_WIDGET (instance)))
    put_handles_away (GTK_WIDGET (instance));
  return TRUE;
}

/* GtkTextBuffer::changed: the focused text view showing that buffer */
static gboolean
on_buffer_changed (GSignalInvocationHint *hint, guint n_values, const GValue *values, gpointer data)
{
  GtkTextBuffer *buffer = g_value_get_object (&values[0]);
  GListModel *windows = gtk_window_get_toplevels (); /* GTK's own */

  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr (GtkWindow) window = g_list_model_get_item (windows, i);
      GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (window));

      if (GTK_IS_TEXT_VIEW (focus) && gtk_text_view_get_buffer (GTK_TEXT_VIEW (focus)) == buffer)
        {
          put_handles_away (focus);
          break;
        }
    }
  return TRUE;
}

void
_vibeos_text_handles_init (void)
{
  /* a type's signals exist once its class (or an interface's default vtable) has been made: held for good here */
  g_type_default_interface_ref (GTK_TYPE_EDITABLE);
  g_type_class_ref (GTK_TYPE_TEXT_BUFFER);
  g_signal_add_emission_hook (g_signal_lookup ("changed", GTK_TYPE_EDITABLE), 0, on_editable_changed, NULL, NULL);
  g_signal_add_emission_hook (g_signal_lookup ("changed", GTK_TYPE_TEXT_BUFFER), 0, on_buffer_changed, NULL, NULL);
}
