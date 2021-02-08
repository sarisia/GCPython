// Traditional Mark-sweep garbage collector

#include "Python.h"
#include "pycore_pystate.h" // _PyRuntime
#include "object.h" // refchain, _Py_Dealloc
#include "frameobject.h"
#include "code.h"

#include "marksweep.h"

extern void PyTuple_Traverse(markproc);
extern void PyList_Traverse(markproc);
extern void PySet_Traverse(markproc);
extern void PyBytes_Traverse(markproc);
extern void PyLong_Traverse(markproc);
extern void PyFloat_Traverse(markproc);

#ifdef Py_MARKSWEEP

// _print_runtime prints 
static void _print_runtime(void) {
    _PyRuntimeState *runtime = &_PyRuntime;
    printf("runtime %p\n", runtime);
    printf("interpreter head: %p, main: %p\n", runtime->interpreters.head, runtime->interpreters.main);

    for (PyInterpreterState *i = runtime->interpreters.head; i != NULL; i = i->next) {
        printf("  interpreter %p:\n", i);
        printf("  thread head: %p\n", i->tstate_head);

        for (PyThreadState *t = i->tstate_head; t != NULL; t = t->next) {
            printf("    thread %p\n", t);
            // PyFrameObject is PyObject and parent frames are visited in frame_traverse,
            // so we do not need to dig into frames
            // see Objects/frameobject.c:459
            for (PyFrameObject *f = t->frame; f != NULL; f = f->f_back) {
                printf("      frame %p\n", f);
            }
        }
    }
}

static void reset_all_refchain(void) {
    for (PyObject *cur = refchain._ob_next; cur != &refchain; cur = cur->_ob_next) {
        PyMarkSweep_ClearFlag(cur);
    }
}

// PyInterpreterState is not PyObject. Do mark manually.
// See Python/pystate.c _PyInterpreterState_Clear()
static void interpreter_traverse(PyInterpreterState *interp)
{
    Py_MARK(interp->audit_hooks);
    Py_MARK(interp->codec_search_path);
    Py_MARK(interp->codec_search_cache);
    Py_MARK(interp->codec_error_registry);
    Py_MARK(interp->modules);
    Py_MARK(interp->modules_by_index);
    Py_MARK(interp->sysdict);
    Py_MARK(interp->builtins);
    Py_MARK(interp->builtins_copy);
    Py_MARK(interp->importlib);
    Py_MARK(interp->import_func);
    Py_MARK(interp->dict);
#ifdef HAVE_FORK
    Py_MARK(interp->before_forkers);
    Py_MARK(interp->after_forkers_parent);
    Py_MARK(interp->after_forkers_child);
#endif
}

// PyThreadState is not PyObject, so we need to mark some fields manually
// See Python/pystate.c PyThreadState_Clear()
static void thread_traverse(PyThreadState *tstate)
{
    Py_MARK(tstate->dict);
    Py_MARK(tstate->async_exc);

    Py_MARK(tstate->curexc_type);
    Py_MARK(tstate->curexc_value);
    Py_MARK(tstate->curexc_traceback);

    Py_MARK(tstate->exc_state.exc_type);
    Py_MARK(tstate->exc_state.exc_value);
    Py_MARK(tstate->exc_state.exc_traceback);

    Py_MARK(tstate->c_profileobj);
    Py_MARK(tstate->c_traceobj);

    Py_MARK(tstate->async_gen_firstiter);
    Py_MARK(tstate->async_gen_finalizer);

    Py_MARK(tstate->context);
}

// PyCodeObject (PyCode_Type) does not implement tp_traverse field,
// so we do mark manually...
static void code_traverse(PyCodeObject *co)
{
    // See Include/code.h PyCodeObject
    // and Objects/codeobject.c code_dealloc()
    Py_MARK(co->co_code);
    Py_MARK(co->co_consts);
    Py_MARK(co->co_names);
    Py_MARK(co->co_varnames);
    Py_MARK(co->co_freevars);
    Py_MARK(co->co_cellvars);

    Py_MARK(co->co_filename);
    Py_MARK(co->co_name);
    Py_MARK(co->co_lnotab);

    Py_MARK(co->co_zombieframe);
}

// mark objects recursively, make use of tp_traverse
// the function mark_object itself is also a `visitproc` type,
// and will be used as callback for `traverseproc`.
int mark_object(PyObject *ob, void *sarisia) {
    // printf("[marksweep] mark (");
    // PyObject_Print(ob, stdout, 0);
    // printf("): ");

    // if already marked, skip processing.
    if (PyMarkSweep_IsMarked(ob)) {
        // printf("skipped\n");
        return 0;
    }

    // mark this
    PyMarkSweep_SetFlag(ob, Py_MSFLAGS_MARKED);
    // printf("marked\n");

    // if object is PyCodeObject, we mark them with own proc.
    if (Py_TYPE(ob) == &PyCode_Type) {
        // printf("!!!CODE!!! %p\n", ob);
        code_traverse((PyCodeObject *)ob);
        return 0;
    }

    // traverse object fields using tp_traverse, mark them recursively.
    // if the object does not support gc, skip.
    if (!PyObject_IS_GC(ob)) {
        return 0;
    }

    traverseproc traverse = Py_TYPE(ob)->tp_traverse;
    if (traverse == NULL) {
        return 0;
    }
    if (traverse(ob, (visitproc)mark_object, NULL)) {
        printf("something went wrong with traverse\n");
        return 1;
    }

    return 0;
}

// marker is a `markproc` callback, passed to object pool traverse functions
static void marker(void *ob) {
    Py_MARK(ob);
}

// mark_all marks all objects, traverse from runtime.
static void mark_all(void) {
    _PyRuntimeState *runtime = &_PyRuntime;
    for (PyInterpreterState *interp = runtime->interpreters.head; interp != NULL; interp = interp->next) {
        interpreter_traverse(interp);
        for (PyThreadState *tstate = interp->tstate_head; tstate != NULL; tstate = tstate->next) {
            thread_traverse(tstate);

            // PyFrameObject is PyObject, so let it go
            // this also traverses all the parent frames
            Py_MARK(tstate->frame);
        }
    }

    // PyObject_
    // mark special static objects
    PyTuple_Traverse(marker);
    PyList_Traverse(marker);
    PySet_Traverse(marker);
    PyBytes_Traverse(marker);
    PyLong_Traverse(marker);
    PyFloat_Traverse(marker);
}

static void sweep(void) {
    PyObject *next;
    PyObject *cur = refchain._ob_next;
    while (cur != &refchain) {
        // step cursor first to avoid segfault in case 'cur' itself is freed and cur._ob_next is gone
        next = cur->_ob_next;

        // printf("%p (rc %ld, ", cur, Py_REFCNT(cur));
        // PyObject_Print(cur, stdout, 0);
        // printf(": ");
        if (!PyMarkSweep_IsMarked(cur)) {
            // DEBUG: pre-deallocation check
            if (Py_REFCNT(cur) != 0) {
                // printf("UNSAFE");
            } else {
                // TODO: currently we do `else` for safety,
                // but essentialy this is not a choice since this uses refcount.
                // do same thing as callback of Py_DECREF
                _Py_Dealloc(cur); // _Py_Dealloc does ForgetReference to remove object from refchain
                // printf("dealloc");
            }
        } else {
            // clear flag inplace, avoid one more loop
            PyMarkSweep_ClearFlag(cur);
            // printf("skip (has mark)");
        }

        // printf("\n");
        cur = next;
    }
}

static void run(void) {
    mark_all();
    sweep();
}

// mark_all, then scans refchain and prints useful outputs. That's it. 
static void dry_run(void) {
    int total = 0;
    int marked = 0;
    int unsafe = 0;

    mark_all();

    for (PyObject *cur = refchain._ob_next; cur != &refchain; cur = cur->_ob_next) {
        total++;
        if (PyMarkSweep_IsMarked(cur)) {
            marked++;
        } else if (Py_REFCNT(cur) != 0) {
            // not marked (garbage) but refcount is not zero, so unsafe
            unsafe++;
        }
    }

    printf("marked %d/%d (unsafe %d/%d)\n", marked, total, unsafe, total-marked);
}

static void marksweep_test(void) {
    PyLong_Traverse(marker);
}

// *****************************************************
// marksweep Module methods
// *****************************************************

// marksweep.mark(object)
PyObject *Marksweep_mark(PyObject *module, PyObject *ob) {
    mark_object(ob, NULL);
    Py_RETURN_NONE;
}

// marksweep.print_runtime()
PyObject *Marksweep_print_runtime(PyObject *module, PyObject *args) {
    _print_runtime();
    Py_RETURN_NONE;
}

// marksweep.print_object()
PyObject *Marksweep_print_object(PyObject *module, PyObject *ob) {
    printf("Object %p (", ob);
    PyObject_Print(ob, stdout, 0);
    printf("):\n");

    printf("  refcount %ld, refchain prev: %p, next: %p\n", Py_REFCNT(ob), ob->_ob_prev, ob->_ob_next);
    printf("  marksweep flag: %d (marked: %d)\n", Py_MSFLAGS(ob), PyMarkSweep_IsMarked(ob));

    Py_RETURN_NONE;
}

// marksweep.mark_all()
PyObject *Marksweep_mark_all(PyObject *module, PyObject *args) {
    mark_all();
    Py_RETURN_NONE;
}

PyObject *Marksweep_reset_all(PyObject *module, PyObject *args) {
    reset_all_refchain();
    Py_RETURN_NONE;
}

PyObject *Marksweep_sweep(PyObject *module, PyObject *args) {
    sweep();
    Py_RETURN_NONE;
}

PyObject *Marksweep_run(PyObject *module, PyObject *args) {
    run();
    Py_RETURN_NONE;
}

PyObject *Marksweep_dry_run(PyObject *module, PyObject *args) {
    dry_run();
    Py_RETURN_NONE;
}

PyObject *Marksweep__sancheck(PyObject *module, PyObject *args) {
    // refchain checks
    printf("*** REFCHAIN ***\n");
    printf("# Initially broken objects\n");
    for (PyObject *cur = refchain._ob_next; cur != &refchain; cur = cur->_ob_next)
    {
        if (Py_MSFLAGS(cur) != 0) {
            printf("%p (flag %d)\n", cur, Py_MSFLAGS(cur));
        }
    }

    Py_RETURN_NONE;
}

PyObject *Marksweep__unsafe_sanitize(PyObject *module, PyObject *args) {
    PyObject *next;
    PyObject *cur = refchain._ob_next;
    int removed = 0;
    while (cur != &refchain)
    {
        // step cursor first to avoid segfault in case 'cur' itself is freed and cur._ob_next is gone
        next = cur->_ob_next;

        if (Py_MSFLAGS(cur) != 0) {
            _Py_ForgetReference(cur);
            removed++;
        }

        cur = next;
    }

    printf("removed %d objects from refchain\n", removed);
    Py_RETURN_NONE;
}

PyObject *Marksweep__test(PyObject *module, PyObject *args) {
    marksweep_test();
    Py_RETURN_NONE;
}

#endif // Py_MARKSWEEP

// *****************************************************
// marksweep Module definitions
// *****************************************************

static PyMethodDef MarksweepMethods[] = {
#ifdef Py_MARKSWEEP
    { "mark", Marksweep_mark, METH_O, "mark the object specified" },
    { "print_runtime", Marksweep_print_runtime, METH_NOARGS, "print all frames in all interpreters in the runtime" },
    { "print_object", Marksweep_print_object, METH_O, "print information of the object" },
    { "mark_all", Marksweep_mark_all, METH_NOARGS, "perform mark process" },
    { "reset_all", Marksweep_reset_all, METH_NOARGS, "reset flags of all objects in refchain" },
    { "sweep", Marksweep_sweep, METH_NOARGS, "sweep unused objects in refchain" },
    { "run", Marksweep_run, METH_NOARGS, "run entire garbage collect cycle (mark & sweep)" },
    { "dry_run", Marksweep_dry_run, METH_NOARGS, "show dry-run result simulations" },
    { "_sancheck", Marksweep__sancheck, METH_NOARGS, "various sanity checks" },
    { "_unsafe_sanitize", Marksweep__unsafe_sanitize, METH_NOARGS, "remove broken objects from refchain" },
    { "_test", Marksweep__test, METH_NOARGS, "execute marksweep_test() defined in Python/marksweep.c" },
#endif // Py_MARKSWEEP
    { NULL, NULL, 0, NULL } // Sentinel
};

static PyModuleDef MarksweepModule = {
    PyModuleDef_HEAD_INIT,
    "marksweep", // name
    NULL, // doc
    -1, // size
    MarksweepMethods // methods
};

PyMODINIT_FUNC
PyInit_marksweep(void) {
    return PyModule_Create(&MarksweepModule);
}
