#include <glib.h>
gchar *
ic_calloc(size_t size)
{
  return g_try_malloc0(size);
}

gchar *
ic_malloc(size_t size)
{
  return g_try_malloc(size);
}

void
ic_free(void *ret_obj)
{
  g_free(ret_obj);
}
