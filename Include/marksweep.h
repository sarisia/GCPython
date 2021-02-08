#ifndef Py_MARK_SWEEP_H
#define Py_MARK_SWEEP_H

// takes PyObject *
typedef void (*markproc)(void *);

#ifdef Py_MARKSWEEP

// flags
#define Py_MSFLAGS_MARKED (1U << 0)

// flag-related macros
#define PyMarkSweep_HasFlag(ob, f) ((Py_MSFLAGS(ob) & f) != 0)
#define PyMarkSweep_ClearFlag(ob) (Py_MSFLAGS(ob) &= 0)
#define PyMarkSweep_SetFlag(ob, f) (Py_MSFLAGS(ob) |= f)

#define PyMarkSweep_IsMarked(ob) (PyMarkSweep_HasFlag(ob, Py_MSFLAGS_MARKED))

int mark_object(PyObject *, void *);
// Py_MARK marks object if not NULL
#define Py_MARK(ob)                                \
    do                                             \
    {                                              \
        if (_PyObject_CAST(ob) != NULL)            \
        {                                          \
            mark_object(_PyObject_CAST(ob), NULL); \
        }                                          \
    } while (0)

// PyAPI_FUNC(void) PyMarkSweep_testfunc(void);

#endif // Py_MARKSWEEP

#endif // !Py_MARK_SWEEP_H

