#ifndef DM_EXT
#define DM_EXT

void Init_dm(void);

/* tell rbx not to use it's caching compat layer
   by doing this we're making a promise to RBX that
   we'll never modify the pointers we get back from RSTRING_PTR */
#define RSTRING_NOT_MODIFIED
#include <ruby.h>

#include "DPI.h"
#include "DPIext.h"
#include "DPItypes.h"

#include <ruby/encoding.h>
#include <ruby/thread.h>

#if defined(__GNUC__) && (__GNUC__ >= 3)
#define RB_DM_NORETURN __attribute__ ((noreturn))
#define RB_DM_UNUSED __attribute__ ((unused))
#else
#define RB_DM_NORETURN
#define RB_DM_UNUSED
#endif

// ruby 2.7+
#ifdef HAVE_RB_GC_MARK_MOVABLE
#define rb_dm_gc_location(ptr) ptr = rb_gc_location(ptr)
#else
#define rb_gc_mark_movable(ptr) rb_gc_mark(ptr)
#define rb_dm_gc_location(ptr)
#endif

// ruby 2.2+
#ifdef TypedData_Make_Struct
#define NEW_TYPEDDATA_WRAPPER 1
#endif

#include <client.h>
#include <result.h>
#include <statement.h>
#endif
