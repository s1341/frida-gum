/*
 * Copyright (C) 2015-2016 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumexceptor.h"

#include "gumexceptorbackend.h"
#include "gumtls.h"

#include <setjmp.h>
#include <string.h>

typedef struct _GumExceptionHandlerEntry GumExceptionHandlerEntry;
#if defined (G_OS_WIN32) || defined (HAVE_DARWIN)
# define GUM_NATIVE_SETJMP setjmp
# define GUM_NATIVE_LONGJMP longjmp
  typedef jmp_buf GumExceptorNativeJmpBuf;
#else
# if defined (sigsetjmp) && !defined (HAVE_QNX)
#   define GUM_NATIVE_SETJMP __sigsetjmp
# else
#   define GUM_NATIVE_SETJMP sigsetjmp
# endif
# define GUM_NATIVE_LONGJMP siglongjmp
  typedef sigjmp_buf GumExceptorNativeJmpBuf;
#endif

#define GUM_EXCEPTOR_LOCK()   (g_mutex_lock (&priv->mutex))
#define GUM_EXCEPTOR_UNLOCK() (g_mutex_unlock (&priv->mutex))

struct _GumExceptorPrivate
{
  GMutex mutex;

  GSList * handlers;
  GumTlsKey scope_tls;

  GumExceptorBackend * backend;
};

struct _GumExceptionHandlerEntry
{
  GumExceptionHandler func;
  gpointer user_data;
};

struct _GumExceptorScopeImpl
{
  GumExceptorNativeJmpBuf env;
  gboolean exception_occurred;
#ifdef HAVE_ANDROID
  sigset_t mask;
#endif
};

static void gum_exceptor_dispose (GObject * object);
static void gum_exceptor_finalize (GObject * object);
static void the_exceptor_weak_notify (gpointer data,
    GObject * where_the_object_was);

static gboolean gum_exceptor_handle_exception (GumExceptionDetails * details,
    GumExceptor * self);
static gboolean gum_exceptor_handle_scope_exception (
    GumExceptionDetails * details, gpointer user_data);

static void gum_exceptor_scope_impl_perform_longjmp (
    GumExceptorScopeImpl * impl);

G_DEFINE_TYPE (GumExceptor, gum_exceptor, G_TYPE_OBJECT);

G_LOCK_DEFINE_STATIC (the_exceptor);
static GumExceptor * the_exceptor = NULL;

static void
gum_exceptor_class_init (GumExceptorClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GumExceptorPrivate));

  object_class->dispose = gum_exceptor_dispose;
  object_class->finalize = gum_exceptor_finalize;
}

static void
gum_exceptor_init (GumExceptor * self)
{
  GumExceptorPrivate * priv;

  self->priv = priv = G_TYPE_INSTANCE_GET_PRIVATE (self, GUM_TYPE_EXCEPTOR,
      GumExceptorPrivate);

  g_mutex_init (&priv->mutex);

  priv->scope_tls = gum_tls_key_new ();

  gum_exceptor_add (self, gum_exceptor_handle_scope_exception, self);

  priv->backend = gum_exceptor_backend_new (
      (GumExceptionHandler) gum_exceptor_handle_exception, self);
}

static void
gum_exceptor_dispose (GObject * object)
{
  GumExceptor * self = GUM_EXCEPTOR (object);

  g_clear_object (&self->priv->backend);

  G_OBJECT_CLASS (gum_exceptor_parent_class)->dispose (object);
}

static void
gum_exceptor_finalize (GObject * object)
{
  GumExceptor * self = GUM_EXCEPTOR (object);
  GumExceptorPrivate * priv = self->priv;

  gum_exceptor_remove (self, gum_exceptor_handle_scope_exception, self);

  gum_tls_key_free (priv->scope_tls);

  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (gum_exceptor_parent_class)->finalize (object);
}

GumExceptor *
gum_exceptor_obtain (void)
{
  GumExceptor * exceptor;

  G_LOCK (the_exceptor);

  if (the_exceptor != NULL)
  {
    exceptor = GUM_EXCEPTOR_CAST (g_object_ref (the_exceptor));
  }
  else
  {
    the_exceptor = GUM_EXCEPTOR_CAST (g_object_new (GUM_TYPE_EXCEPTOR, NULL));
    g_object_weak_ref (G_OBJECT (the_exceptor), the_exceptor_weak_notify, NULL);

    exceptor = the_exceptor;
  }

  G_UNLOCK (the_exceptor);

  return exceptor;
}

static void
the_exceptor_weak_notify (gpointer data,
                          GObject * where_the_object_was)
{
  (void) data;

  G_LOCK (the_exceptor);

  g_assert (the_exceptor == (GumExceptor *) where_the_object_was);
  the_exceptor = NULL;

  G_UNLOCK (the_exceptor);
}

void
gum_exceptor_add (GumExceptor * self,
                  GumExceptionHandler func,
                  gpointer user_data)
{
  GumExceptorPrivate * priv = self->priv;
  GumExceptionHandlerEntry * entry;

  entry = g_slice_new (GumExceptionHandlerEntry);
  entry->func = func;
  entry->user_data = user_data;

  GUM_EXCEPTOR_LOCK ();
  priv->handlers = g_slist_append (priv->handlers, entry);
  GUM_EXCEPTOR_UNLOCK ();
}

void
gum_exceptor_remove (GumExceptor * self,
                     GumExceptionHandler func,
                     gpointer user_data)
{
  GumExceptorPrivate * priv = self->priv;
  GumExceptionHandlerEntry * matching_entry;
  GSList * cur;

  GUM_EXCEPTOR_LOCK ();

  for (matching_entry = NULL, cur = priv->handlers;
      matching_entry == NULL && cur != NULL;
      cur = cur->next)
  {
    GumExceptionHandlerEntry * entry = (GumExceptionHandlerEntry *) cur->data;

    if (entry->func == func && entry->user_data == user_data)
      matching_entry = entry;
  }

  g_assert (matching_entry != NULL);

  priv->handlers = g_slist_remove (priv->handlers, matching_entry);

  GUM_EXCEPTOR_UNLOCK ();

  g_slice_free (GumExceptionHandlerEntry, matching_entry);
}

static gboolean
gum_exceptor_handle_exception (GumExceptionDetails * details,
                               GumExceptor * self)
{
  GumExceptorPrivate * priv = self->priv;
  gboolean handled = FALSE;
  GSList * invoked = NULL;
  GumExceptionHandlerEntry e;

  do
  {
    GSList * cur;

    e.func = NULL;
    e.user_data = NULL;

    GUM_EXCEPTOR_LOCK ();
    for (cur = priv->handlers; e.func == NULL && cur != NULL; cur = cur->next)
    {
      GumExceptionHandlerEntry * entry = (GumExceptionHandlerEntry *) cur->data;

      if (g_slist_find (invoked, entry) == NULL)
      {
        invoked = g_slist_prepend (invoked, entry);
        e = *entry;
      }
    }
    GUM_EXCEPTOR_UNLOCK ();

    if (e.func != NULL)
      handled = e.func (details, e.user_data);
  }
  while (!handled && e.func != NULL);

  g_slist_free (invoked);

  return handled;
}

GumExceptorSetJmp
_gum_exceptor_get_setjmp (void)
{
  return GUM_POINTER_TO_FUNCPTR (GumExceptorSetJmp,
      GUM_FUNCPTR_TO_POINTER (GUM_NATIVE_SETJMP));
}

GumExceptorJmpBuf
_gum_exceptor_prepare_try (GumExceptor * self,
                           GumExceptorScope * scope)
{
  GumExceptorScopeImpl * impl;

  if (scope->impl != NULL)
    return scope->impl->env;

  impl = g_slice_new (GumExceptorScopeImpl);
  impl->exception_occurred = FALSE;
#ifdef HAVE_ANDROID
  /* Workaround for Bionic bug up to and including Android L */
  sigprocmask (SIG_SETMASK, NULL, &impl->mask);
#endif

  scope->impl = impl;
  scope->next = gum_tls_key_get_value (self->priv->scope_tls);

  gum_tls_key_set_value (self->priv->scope_tls, scope);

  return impl->env;
}

gboolean
gum_exceptor_catch (GumExceptor * self,
                    GumExceptorScope * scope)
{
  GumExceptorScopeImpl * impl = scope->impl;
  gboolean exception_occurred;

  gum_tls_key_set_value (self->priv->scope_tls, scope->next);

  exception_occurred = impl->exception_occurred;
  g_slice_free (GumExceptorScopeImpl, impl);
  scope->impl = NULL;

  return exception_occurred;
}

gchar *
gum_exception_details_to_string (const GumExceptionDetails * details)
{
  GString * message;

  message = g_string_new ("");

  switch (details->type)
  {
    case GUM_EXCEPTION_ABORT:
      g_string_append (message, "abort was called");
      break;
    case GUM_EXCEPTION_ACCESS_VIOLATION:
      g_string_append (message, "access violation");
      break;
    case GUM_EXCEPTION_GUARD_PAGE:
      g_string_append (message, "guard page was hit");
      break;
    case GUM_EXCEPTION_ILLEGAL_INSTRUCTION:
      g_string_append (message, "illegal instruction");
      break;
    case GUM_EXCEPTION_STACK_OVERFLOW:
      g_string_append (message, "stack overflow");
      break;
    case GUM_EXCEPTION_ARITHMETIC:
      g_string_append (message, "arithmetic error");
      break;
    case GUM_EXCEPTION_BREAKPOINT:
      g_string_append (message, "breakpoint triggered");
      break;
    case GUM_EXCEPTION_SINGLE_STEP:
      g_string_append (message, "single-step triggered");
      break;
    case GUM_EXCEPTION_SYSTEM:
      g_string_append (message, "system error");
      break;
    default:
      break;
  }

  if (details->memory.operation != GUM_MEMOP_INVALID)
  {
    g_string_append_printf (message, " accessing 0x%" G_GSIZE_MODIFIER "x",
        GPOINTER_TO_SIZE (details->memory.address));
  }

  return g_string_free (message, FALSE);
}

static gboolean
gum_exceptor_handle_scope_exception (GumExceptionDetails * details,
                                     gpointer user_data)
{
  GumExceptor * self = GUM_EXCEPTOR_CAST (user_data);
  GumExceptorScope * scope;
  GumExceptorScopeImpl * impl;
  GumCpuContext * context = &details->context;

  scope = (GumExceptorScope *) gum_tls_key_get_value (self->priv->scope_tls);
  if (scope == NULL)
    return FALSE;

  impl = scope->impl;
  if (impl->exception_occurred)
    return FALSE;

  impl->exception_occurred = TRUE;
  memcpy (&scope->exception, details, sizeof (GumExceptionDetails));
  scope->exception.native_context = NULL;

  /*
   * Place IP at the start of the function as if the call already happened,
   * and set up stack and registers accordingly.
   */
#if defined (HAVE_I386)
  GUM_CPU_CONTEXT_XIP (context) = GPOINTER_TO_SIZE (
      GUM_FUNCPTR_TO_POINTER (gum_exceptor_scope_impl_perform_longjmp));

  /* Align to 16 byte boundary (Mac ABI) */
  GUM_CPU_CONTEXT_XSP (context) &= ~(gsize) (16 - 1);
  /* Avoid the red zone (when applicable) */
  GUM_CPU_CONTEXT_XSP (context) -= GUM_RED_ZONE_SIZE;
  /* Reserve spill space for first four arguments (Win64 ABI) */
  GUM_CPU_CONTEXT_XSP (context) -= 4 * 8;

# if GLIB_SIZEOF_VOID_P == 4
  /* 32-bit: First argument goes on the stack (cdecl) */
  *((GumExceptorScopeImpl **) context->esp) = impl;
# else
  /* 64-bit: First argument goes in a register */
#  if GUM_NATIVE_ABI_IS_WINDOWS
  context->rcx = GPOINTER_TO_SIZE (impl);
#  else
  context->rdi = GPOINTER_TO_SIZE (impl);
#  endif
# endif

  /* Dummy return address (we won't return) */
  GUM_CPU_CONTEXT_XSP (context) -= sizeof (gpointer);
  *((gsize *) GUM_CPU_CONTEXT_XSP (context)) = 1337;
#elif defined (HAVE_ARM) || defined (HAVE_ARM64)
  context->pc = GPOINTER_TO_SIZE (
      GUM_FUNCPTR_TO_POINTER (gum_exceptor_scope_impl_perform_longjmp));

  /* Align to 16 byte boundary */
  context->sp &= ~(gsize) (16 - 1);
  /* Avoid the red zone (when applicable) */
  context->sp -= GUM_RED_ZONE_SIZE;

# if GLIB_SIZEOF_VOID_P == 4
  context->r[0] = GPOINTER_TO_SIZE (impl);
# else
  context->x[0] = GPOINTER_TO_SIZE (impl);
# endif

  /* Dummy return address (we won't return) */
  context->lr = 1337;
#elif defined (HAVE_MIPS)
  context->pc = GPOINTER_TO_SIZE (
      GUM_FUNCPTR_TO_POINTER (gum_exceptor_scope_impl_perform_longjmp));

  /* set t9 to gum_exceptor_scope_impl_perform_longjmp, as it is PIC and needs
   * t9 for the gp calculation.
   */
  context->t9 = context->pc;

  /* Align to 16 byte boundary */
  context->sp &= ~(gsize) (16 - 1);
  /* Avoid the red zone (when applicable) */
  context->sp -= GUM_RED_ZONE_SIZE;

  context->a0 = GPOINTER_TO_SIZE (impl);

  /* Dummy return address (we won't return) */
  context->ra = 1337;
#else
# error Unsupported architecture
#endif

  return TRUE;
}

static void
gum_exceptor_scope_impl_perform_longjmp (GumExceptorScopeImpl * impl)
{
#ifdef HAVE_ANDROID
  sigprocmask (SIG_SETMASK, &impl->mask, NULL);
#endif
  GUM_NATIVE_LONGJMP (impl->env, 1);
}
