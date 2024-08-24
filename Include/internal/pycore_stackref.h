#ifndef Py_INTERNAL_STACKREF_H
#define Py_INTERNAL_STACKREF_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_object_deferred.h"

#include <stddef.h>
#include <stdbool.h>

/*
  This file introduces a new API for handling references on the stack, called
  _PyStackRef. This API is inspired by HPy.

  There are 3 main operations, that convert _PyStackRef to PyObject* and
  vice versa:

    1. Borrow (discouraged)
    2. Steal
    3. New

  Borrow means that the reference is converted without any change in ownership.
  This is discouraged because it makes verification much harder. It also makes
  unboxed integers harder in the future.

  Steal means that ownership is transferred to something else. The total
  number of references to the object stays the same.

  New creates a new reference from the old reference. The old reference
  is still valid.

  With these 3 API, a strict stack discipline must be maintained. All
  _PyStackRef must be operated on by the new reference operations:

    1. DUP
    2. CLOSE

   DUP is roughly equivalent to Py_NewRef. It creates a new reference from an old
   reference. The old reference remains unchanged.

   CLOSE is roughly equivalent to Py_DECREF. It destroys a reference.

   Note that it is unsafe to borrow a _PyStackRef and then do normal
   CPython refcounting operations on it!
*/

typedef union _PyStackRef {
    uintptr_t bits;
} _PyStackRef;


#define Py_TAG_DEFERRED (1)

#define Py_TAG_PTR      (0)
#define Py_TAG_BITS     (1)

#ifdef Py_GIL_DISABLED
    static const _PyStackRef PyStackRef_NULL = { .bits = 0 | Py_TAG_DEFERRED};
#else
    static const _PyStackRef PyStackRef_NULL = { .bits = 0 };
#endif

#define PyStackRef_IsNull(stackref) ((stackref).bits == PyStackRef_NULL.bits)


#ifdef Py_GIL_DISABLED
#   define PyStackRef_True ((_PyStackRef){.bits = ((uintptr_t)&_Py_TrueStruct) | Py_TAG_DEFERRED })
#else
#   define PyStackRef_True ((_PyStackRef){.bits = ((uintptr_t)&_Py_TrueStruct) })
#endif

#ifdef Py_GIL_DISABLED
#   define PyStackRef_False ((_PyStackRef){.bits = ((uintptr_t)&_Py_FalseStruct) | Py_TAG_DEFERRED })
#else
#   define PyStackRef_False ((_PyStackRef){.bits = ((uintptr_t)&_Py_FalseStruct) })
#endif

#ifdef Py_GIL_DISABLED
#   define PyStackRef_None ((_PyStackRef){.bits = ((uintptr_t)&_Py_NoneStruct) | Py_TAG_DEFERRED })
#else
#   define PyStackRef_None ((_PyStackRef){.bits = ((uintptr_t)&_Py_NoneStruct) })
#endif

// Note: the following are all macros because MSVC (Windows) has trouble inlining them.

#define PyStackRef_Is(a, b) ((a).bits == (b).bits)

#define PyStackRef_IsDeferred(ref) (((ref).bits & Py_TAG_BITS) == Py_TAG_DEFERRED)


#ifdef Py_GIL_DISABLED
// Gets a PyObject * from a _PyStackRef
static inline PyObject *
PyStackRef_AsPyObjectBorrow(_PyStackRef stackref)
{
    PyObject *cleared = ((PyObject *)((stackref).bits & (~Py_TAG_BITS)));
    return cleared;
}
#else
#   define PyStackRef_AsPyObjectBorrow(stackref) ((PyObject *)(stackref).bits)
#endif

// Converts a PyStackRef back to a PyObject *, stealing the
// PyStackRef.
#ifdef Py_GIL_DISABLED
static inline PyObject *
PyStackRef_AsPyObjectSteal(_PyStackRef stackref)
{
    if (!PyStackRef_IsNull(stackref) && PyStackRef_IsDeferred(stackref)) {
        return Py_NewRef(PyStackRef_AsPyObjectBorrow(stackref));
    }
    return PyStackRef_AsPyObjectBorrow(stackref);
}
#else
#   define PyStackRef_AsPyObjectSteal(stackref) PyStackRef_AsPyObjectBorrow(stackref)
#endif

// Converts a PyStackRef back to a PyObject *, converting the
// stackref to a new reference.
#define PyStackRef_AsPyObjectNew(stackref) Py_NewRef(PyStackRef_AsPyObjectBorrow(stackref))

#define PyStackRef_TYPE(stackref) Py_TYPE(PyStackRef_AsPyObjectBorrow(stackref))

// Converts a PyObject * to a PyStackRef, stealing the reference
#ifdef Py_GIL_DISABLED
static inline _PyStackRef
_PyStackRef_FromPyObjectSteal(PyObject *obj)
{
    // Make sure we don't take an already tagged value.
    assert(((uintptr_t)obj & Py_TAG_BITS) == 0);
    int tag = (obj == NULL || _Py_IsImmortal(obj)) ? (Py_TAG_DEFERRED) : Py_TAG_PTR;
    return ((_PyStackRef){.bits = ((uintptr_t)(obj)) | tag});
}
#   define PyStackRef_FromPyObjectSteal(obj) _PyStackRef_FromPyObjectSteal(_PyObject_CAST(obj))
#else
#   define PyStackRef_FromPyObjectSteal(obj) ((_PyStackRef){.bits = ((uintptr_t)(obj))})
#endif


// Converts a PyObject * to a PyStackRef, with a new reference
#ifdef Py_GIL_DISABLED
static inline _PyStackRef
PyStackRef_FromPyObjectNew(PyObject *obj)
{
    // Make sure we don't take an already tagged value.
    assert(((uintptr_t)obj & Py_TAG_BITS) == 0);
    assert(obj != NULL);
    if (_Py_IsImmortal(obj) || _PyObject_HasDeferredRefcount(obj)) {
        return (_PyStackRef){ .bits = (uintptr_t)obj | Py_TAG_DEFERRED };
    }
    else {
        return (_PyStackRef){ .bits = (uintptr_t)(Py_NewRef(obj)) | Py_TAG_PTR };
    }
}
#   define PyStackRef_FromPyObjectNew(obj) PyStackRef_FromPyObjectNew(_PyObject_CAST(obj))
#else
#   define PyStackRef_FromPyObjectNew(obj) ((_PyStackRef){ .bits = (uintptr_t)(Py_NewRef(obj)) })
#endif

#ifdef Py_GIL_DISABLED
// Same as PyStackRef_FromPyObjectNew but only for immortal objects.
static inline _PyStackRef
PyStackRef_FromPyObjectImmortal(PyObject *obj)
{
    // Make sure we don't take an already tagged value.
    assert(((uintptr_t)obj & Py_TAG_BITS) == 0);
    assert(obj != NULL);
    assert(_Py_IsImmortal(obj));
    return (_PyStackRef){ .bits = (uintptr_t)obj | Py_TAG_DEFERRED };
}
#   define PyStackRef_FromPyObjectImmortal(obj) PyStackRef_FromPyObjectImmortal(_PyObject_CAST(obj))
#else
#   define PyStackRef_FromPyObjectImmortal(obj) ((_PyStackRef){ .bits = (uintptr_t)(obj) })
#endif


#define PyStackRef_CLEAR(op) \
    do { \
        _PyStackRef *_tmp_op_ptr = &(op); \
        _PyStackRef _tmp_old_op = (*_tmp_op_ptr); \
        if (!PyStackRef_IsNull(_tmp_old_op)) { \
            *_tmp_op_ptr = PyStackRef_NULL; \
            PyStackRef_CLOSE(_tmp_old_op); \
        } \
    } while (0)

#ifdef Py_GIL_DISABLED
#   define PyStackRef_CLOSE(REF)                                        \
        do {                                                            \
            _PyStackRef _close_tmp = (REF);                             \
            if (!PyStackRef_IsDeferred(_close_tmp)) {                   \
                Py_DECREF(PyStackRef_AsPyObjectBorrow(_close_tmp));     \
            }                                                           \
        } while (0)
#else
#   define PyStackRef_CLOSE(stackref) Py_DECREF(PyStackRef_AsPyObjectBorrow(stackref))
#endif

#define PyStackRef_XCLOSE(stackref) \
    do {                            \
        _PyStackRef _tmp = (stackref); \
        if (!PyStackRef_IsNull(_tmp)) { \
            PyStackRef_CLOSE(_tmp); \
        } \
    } while (0);


#ifdef Py_GIL_DISABLED
static inline _PyStackRef
PyStackRef_DUP(_PyStackRef stackref)
{
    if (PyStackRef_IsDeferred(stackref)) {
        assert(PyStackRef_IsNull(stackref) ||
            _Py_IsImmortal(PyStackRef_AsPyObjectBorrow(stackref)) ||
            _PyObject_HasDeferredRefcount(PyStackRef_AsPyObjectBorrow(stackref)));
        return stackref;
    }
    Py_INCREF(PyStackRef_AsPyObjectBorrow(stackref));
    return stackref;
}
#else
#   define PyStackRef_DUP(stackref) PyStackRef_FromPyObjectSteal(Py_NewRef(PyStackRef_AsPyObjectBorrow(stackref)))
#endif

static inline void
_PyObjectStack_FromStackRefStack(PyObject **dst, const _PyStackRef *src, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        dst[i] = PyStackRef_AsPyObjectBorrow(src[i]);
    }
}

// StackRef type checks

static inline bool
PyStackRef_GenCheck(_PyStackRef stackref)
{
    return PyGen_Check(PyStackRef_AsPyObjectBorrow(stackref));
}

static inline bool
PyStackRef_BoolCheck(_PyStackRef stackref)
{
    return PyBool_Check(PyStackRef_AsPyObjectBorrow(stackref));
}

static inline bool
PyStackRef_LongCheck(_PyStackRef stackref)
{
    return PyLong_Check(PyStackRef_AsPyObjectBorrow(stackref));
}

static inline bool
PyStackRef_ExceptionInstanceCheck(_PyStackRef stackref)
{
    return PyExceptionInstance_Check(PyStackRef_AsPyObjectBorrow(stackref));
}


static inline bool
PyStackRef_FunctionCheck(_PyStackRef stackref)
{
    return PyFunction_Check(PyStackRef_AsPyObjectBorrow(stackref));
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_STACKREF_H */
