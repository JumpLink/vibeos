/* Rows from GFileInfo, and the sort orders. */
#include "fm-item.h"

static FmItem *
item (const char *name, GFileType type, goffset size, gint64 mtime)
{
  g_autoptr(GFile) parent = g_file_new_for_path ("/tmp");
  g_autoptr(GFileInfo) info = g_file_info_new ();
  g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc (mtime);

  g_file_info_set_name (info, name);
  g_file_info_set_display_name (info, name);
  g_file_info_set_file_type (info, type);
  g_file_info_set_size (info, size);
  g_file_info_set_modification_date_time (info, when);
  if (type == G_FILE_TYPE_DIRECTORY)
    g_file_info_set_content_type (info, "inode/directory");
  else
    g_file_info_set_content_type (info, "text/plain");
  return fm_item_new_from_info (parent, info);
}

static char *
names (GPtrArray *items)
{
  GString *text = g_string_new (NULL);

  for (guint i = 0; i < items->len; i++)
    g_string_append_printf (text, "%s%s", i ? " " : "", ((FmItem *) items->pdata[i])->name);
  return g_string_free (text, FALSE);
}

static void
test_sort (void)
{
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func (g_object_unref);
  g_autofree char *a = NULL, *b = NULL, *c = NULL, *d = NULL, *e = NULL;

  g_ptr_array_add (items, item ("file10.txt", G_FILE_TYPE_REGULAR, 30, 300));
  g_ptr_array_add (items, item ("File2.txt", G_FILE_TYPE_REGULAR, 10, 100));
  g_ptr_array_add (items, item ("zeta", G_FILE_TYPE_DIRECTORY, 0, 50));
  g_ptr_array_add (items, item ("Alpha", G_FILE_TYPE_DIRECTORY, 0, 60));
  g_ptr_array_add (items, item ("b.jpg", G_FILE_TYPE_REGULAR, 20, 200));

  fm_items_sort (items, FM_SORT_NAME_UP);
  a = names (items);
  /* folders first; numbers by value, case left out */
  g_assert_cmpstr (a, ==, "Alpha zeta b.jpg File2.txt file10.txt");
  fm_items_sort (items, FM_SORT_NAME_DOWN);
  b = names (items);
  g_assert_cmpstr (b, ==, "zeta Alpha file10.txt File2.txt b.jpg");
  fm_items_sort (items, FM_SORT_SIZE_DOWN);
  c = names (items);
  g_assert_cmpstr (c, ==, "Alpha zeta file10.txt b.jpg File2.txt");
  fm_items_sort (items, FM_SORT_DATE_UP);
  d = names (items);
  g_assert_cmpstr (d, ==, "zeta Alpha File2.txt b.jpg file10.txt");
  fm_items_sort (items, FM_SORT_TYPE_UP);
  e = names (items);
  g_assert_cmpstr (e, ==, "Alpha zeta b.jpg File2.txt file10.txt");
}

static void
test_info (void)
{
  g_autoptr(GFile) parent = g_file_new_for_uri ("trash:///");
  g_autoptr(GFileInfo) info = g_file_info_new ();
  g_autoptr(FmItem) row = NULL;
  g_autoptr(FmItem) hidden = item (".config", G_FILE_TYPE_DIRECTORY, 0, 0);

  g_file_info_set_name (info, "a.txt");
  g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
  g_file_info_set_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, "/home/user/a.txt");
  row = fm_item_new_from_info (parent, info);
  g_assert_cmpstr (row->orig_path, ==, "/home/user/a.txt");
  g_assert_cmpint (row->count, ==, FM_COUNT_UNKNOWN);
  g_assert_cmpstr (fm_item_extension (row), ==, "txt");
  g_assert_false (row->hidden);
  g_assert_true (hidden->hidden);
  g_assert_cmpint (hidden->count, ==, FM_COUNT_PENDING);
  g_assert_cmpstr (fm_sort_to_string (fm_sort_from_string ("DateDown")), ==, "DateDown");
  g_assert_cmpint (fm_sort_from_string ("bogus"), ==, FM_SORT_NAME_UP);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/item/sort", test_sort);
  g_test_add_func ("/item/info", test_info);
  return g_test_run ();
}
