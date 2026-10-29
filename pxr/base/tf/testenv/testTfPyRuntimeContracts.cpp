//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Compile against the Limited API and link only Tf. No binding module is
// imported: factories here are ordinary Python C API functions, not Boost.
#include "pxr/base/tf/anyWeakPtr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyExceptionState.h"
#include "pxr/base/tf/pyIdentity.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyMakePyPtr.h"
#include "pxr/base/tf/pyNoticeCallbackImpl.h"
#include "pxr/base/tf/pyObjWrapper.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/refPtr.h"
#include "pxr/base/tf/weakBase.h"
#include "pxr/base/tf/weakPtr.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

#define CHECK(expression) \
    do { \
        if (!(expression)) \
            throw std::runtime_error(std::string("line ") + \
                std::to_string(__LINE__) + ": " + #expression); \
    } while (false)

namespace {

PyObject *_globals;
PyObject *_boxType;
int _mutableCalls = 0, _constCalls = 0;
std::atomic<int> _destroyed{0};

struct Probe : TfWeakBase {};
struct MissingFactory {};
struct ReentrantFactory {};
struct RefProbe : TfRefBase, TfWeakBase {
    ~RefProbe() { ++_destroyed; }
};

PyObject *Eval(const char *expression)
{
    PyObject *code = PyUnicode_FromString(expression);
    PyObject *eval = PyDict_GetItemString(PyEval_GetBuiltins(), "eval");
    PyObject *result = PyObject_CallFunctionObjArgs(eval, code, _globals, nullptr);
    Py_DECREF(code);
    CHECK(result);
    return result;
}

PyObject *NewBox()
{
    PyObject *result = PyObject_CallObject(_boxType, nullptr);
    CHECK(result);
    return result;
}

PyObject *NewNone()
{
    Py_INCREF(Py_None);
    return Py_None;
}

void CapsuleDestroyed(PyObject *capsule)
{
    auto *count = static_cast<std::atomic<int> *>(
        PyCapsule_GetPointer(capsule, "runtime-contract"));
    CHECK(count);
    ++*count;
}

PyObject *NewCapsule(std::atomic<int> *count)
{
    return PyCapsule_New(count, "runtime-contract", CapsuleDestroyed);
}

void TestWrapperReferencesAndThreads(TfPyLock &lock)
{
    std::atomic<int> destroyed{0};
    PyObject *object = NewCapsule(&destroyed);
    {
        TfPyObjWrapper borrowed(object, TfPyBorrowedReference);
        Py_DECREF(object); // The caller can release its reference immediately.
        {
            TfPyObjWrapper copy = borrowed;
            CHECK(copy.ptr() == borrowed.ptr());
        }
        CHECK(destroyed == 0);
    }
    CHECK(destroyed == 1);

    {
        TfPyObjWrapper adopted(NewCapsule(&destroyed), TfPyNewReference);
        CHECK(destroyed == 1);
    }
    CHECK(destroyed == 2); // The transferred reference was not leaked.

    TfPyObjWrapper owned(NewCapsule(&destroyed), TfPyNewReference);
    lock.BeginAllowThreads();
    std::thread worker([holder = std::move(owned)] {
        // The worker starts without the GIL. Both borrowing and final release
        // must establish the Python thread state themselves.
        TfPyObjWrapper borrowed(holder.ptr(), TfPyBorrowedReference);
        TfPyObjWrapper copy = borrowed;
        CHECK(copy.ptr() == holder.ptr());
    });
    worker.join();
    lock.EndAllowThreads();
    CHECK(destroyed == 3);
}

PyObject *MutableFactory(void const *pointer)
{
    ++_mutableCalls;
    // Registration inside a factory must not deadlock on the registry mutex.
    Tf_PyRegisterAnyWeakPtrObjectFactory(
        typeid(ReentrantFactory), +[](void const *) { return NewNone(); });
    return *static_cast<TfWeakPtr<Probe> const *>(pointer) ? NewBox() : NewNone();
}

PyObject *ConstFactory(void const *pointer)
{
    ++_constCalls;
    return *static_cast<TfWeakPtr<const Probe> const *>(pointer) ? NewBox() : NewNone();
}

struct RegisteredFactory {
    template <class Ptr>
    PyObject *operator()(Ptr const &ptr) const {
        return Tf_PyMakePythonObjectFromAnyWeakPtr(typeid(Ptr), &ptr);
    }
};

void TestFactoryRegistry()
{
    TfErrorMark errors;
    PyObject *missing = Tf_PyMakePythonObjectFromAnyWeakPtr(typeid(MissingFactory), nullptr);
    CHECK(missing == Py_None && !errors.IsClean());
    CHECK(!PyErr_Occurred());
    Py_DECREF(missing);
    errors.Clear();

    Tf_PyRegisterAnyWeakPtrObjectFactory(typeid(TfWeakPtr<Probe>), MutableFactory);
    Tf_PyRegisterAnyWeakPtrObjectFactory(typeid(TfWeakPtr<const Probe>), ConstFactory);
    Probe probe;
    TfWeakPtr<Probe> weak(&probe);
    TfWeakPtr<const Probe> constWeak(&probe);
    PyObject *object = RegisteredFactory()(weak);
    Py_DECREF(object);
    object = RegisteredFactory()(constWeak);
    Py_DECREF(object);
    CHECK(_mutableCalls == 1 && _constCalls == 1);
    Tf_PyRegisterAnyWeakPtrObjectFactory(typeid(TfWeakPtr<Probe>),
        +[](void const *) { return NewNone(); });
    object = RegisteredFactory()(weak);
    CHECK(object == Py_None);
    Py_DECREF(object);
    Tf_PyRegisterAnyWeakPtrObjectFactory(typeid(TfWeakPtr<Probe>), MutableFactory);
    CHECK(errors.IsClean() && !PyErr_Occurred());
}

void TestIdentityAndExpiry()
{
    auto owner = std::make_unique<Probe>();
    TfWeakPtr<Probe> weak(owner.get());
    TfWeakPtr<const Probe> constWeak(owner.get());
    const void *id = weak.GetUniqueIdentifier();
    const int calls = _mutableCalls;
    auto first = Tf_MakePyPtrWithFactory(weak, RegisteredFactory());
    CHECK(first.first && first.second && _mutableCalls == calls + 1);
    Tf_PySetPythonIdentity(weak, first.first);
    PyObject *observation = PyWeakref_NewRef(first.first, nullptr);

    auto again = Tf_MakePyPtrWithFactory(weak, RegisteredFactory());
    auto asConst = Tf_MakePyPtrWithFactory(constWeak, RegisteredFactory());
    CHECK(again.first == first.first && !again.second);
    CHECK(asConst.first == first.first && !asConst.second);
    CHECK(_mutableCalls == calls + 1);
    const auto refs = Py_REFCNT(first.first);
    Tf_PyIdentityHelper::Release(id);
    CHECK(Py_REFCNT(first.first) == refs - 1);
    Tf_PyIdentityHelper::Acquire(id);
    CHECK(Py_REFCNT(first.first) == refs);
    Py_DECREF(first.first);
    Py_DECREF(again.first);
    Py_DECREF(asConst.first);
    CHECK(PyWeakref_GetObject(observation) != Py_None);

    owner.reset();
    CHECK(weak.IsInvalid());
    // Identity follows the remnant, not only the pointee. Existing identity is
    // reused even for an expired pointer until the remaining weak handles die.
    auto expiredIdentity = Tf_MakePyPtrWithFactory(weak, RegisteredFactory());
    CHECK(expiredIdentity.first == PyWeakref_GetObject(observation));
    CHECK(!expiredIdentity.second && _mutableCalls == calls + 1);
    Py_DECREF(expiredIdentity.first);
    weak = TfWeakPtr<Probe>();
    constWeak = TfWeakPtr<const Probe>();
    CHECK(PyWeakref_GetObject(observation) == Py_None);
    Py_DECREF(observation);
    owner = std::make_unique<Probe>();
    weak = TfWeakPtr<Probe>(owner.get());
    owner.reset(); // An expired pointer with no prior identity uses None.
    auto expired = Tf_MakePyPtrWithFactory(weak, RegisteredFactory());
    CHECK(expired.first == Py_None && !expired.second);
    Py_DECREF(expired.first);

    int unexpectedCalls = 0;
    auto empty = Tf_MakePyPtrWithFactory(TfWeakPtr<Probe>(),
        [&](auto const &) { ++unexpectedCalls; return NewBox(); });
    CHECK(empty.first == Py_None && !empty.second && unexpectedCalls == 0);
    Py_DECREF(empty.first);

    // A factory can decline conversion or report a Python exception. Neither
    // case creates a Python identity; the error must remain with the caller.
    Probe live;
    TfWeakPtr<Probe> pointer(&live);
    auto none = Tf_MakePyPtrWithFactory(pointer, [](auto const &) { return NewNone(); });
    CHECK(none.first == Py_None && !none.second);
    Py_DECREF(none.first);
    auto failed = Tf_MakePyPtrWithFactory(pointer, [](auto const &) -> PyObject * {
        PyErr_SetString(PyExc_ValueError, "factory failure");
        return nullptr;
    });
    if (failed.second) {
        std::fputs("Factory failure incorrectly reported a newly created object\n", stderr);
        std::fflush(stderr);
    }
    CHECK(!failed.first && !failed.second);
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    CHECK(!Tf_PyGetPythonIdentity(pointer));
    auto retry = Tf_MakePyPtrWithFactory(pointer, [](auto const &) { return NewBox(); });
    CHECK(retry.first && retry.second);
    Py_DECREF(retry.first);
}

void TestRefPtrOwnership()
{
    const int destroyed = _destroyed;
    auto owner = TfCreateRefPtr(new RefProbe);
    TfWeakPtr<RefProbe> weak(owner);
    PyObject *box = NewBox();
    Tf_PySetPythonIdentity(weak, box);
    PyObject *observation = PyWeakref_NewRef(box, nullptr);
    Tf_PyAddPythonOwnership(owner, weak.GetUniqueIdentifier(), box);
    CHECK(PyObject_HasAttrString(box, "__owner"));
    owner.Reset(); // The Python owner capsule now keeps the C++ object alive.
    CHECK(_destroyed == destroyed && weak);
    Py_DECREF(box);
    CHECK(_destroyed == destroyed + 1 && weak.IsInvalid());
    CHECK(PyWeakref_GetObject(observation) == Py_None);
    Py_DECREF(observation);

    owner = TfCreateRefPtr(new RefProbe);
    weak = TfWeakPtr<RefProbe>(owner);
    box = NewBox();
    observation = PyWeakref_NewRef(box, nullptr);
    Tf_PySetPythonIdentity(weak, box);
    Tf_PyAddPythonOwnership(owner, weak.GetUniqueIdentifier(), box);
    Tf_PyRemovePythonOwnership(owner, box);
    CHECK(!PyObject_HasAttrString(box, "__owner"));
    Py_DECREF(box);
    CHECK(PyWeakref_GetObject(observation) != Py_None);
    owner.Reset(); // Ownership was transferred back to C++ without a cycle.
    CHECK(_destroyed == destroyed + 2);
    weak = TfWeakPtr<RefProbe>(); // Release the final remnant and its identity.
    CHECK(PyWeakref_GetObject(observation) == Py_None);
    Py_DECREF(observation);
}

std::atomic<int> _callbackResultsDestroyed{0};
PyObject *Callback(PyObject *, PyObject *)
{
    return NewCapsule(&_callbackResultsDestroyed);
}

void TestCallbackContracts(TfPyLock &lock)
{
    static PyMethodDef method = {"callback", Callback, METH_VARARGS, nullptr};
    TfPyObjWrapper callable(PyCFunction_NewEx(&method, nullptr, nullptr), TfPyNewReference);
    TfPyObjWrapper notice(NewBox(), TfPyNewReference);
    TfPyObjWrapper sender(NewBox(), TfPyNewReference);
    const auto noticeRefs = Py_REFCNT(notice.ptr()), senderRefs = Py_REFCNT(sender.ptr());
    CHECK(Tf_PyNoticeInvokeCallback(callable.ptr(), notice.ptr(), sender.ptr()));
    CHECK(_callbackResultsDestroyed == 1);
    CHECK(Py_REFCNT(notice.ptr()) == noticeRefs && Py_REFCNT(sender.ptr()) == senderRefs);

    bool succeeded = false;
    lock.BeginAllowThreads();
    std::thread worker([&] {
        succeeded = Tf_PyNoticeInvokeCallback(callable.ptr(), notice.ptr(), nullptr);
    });
    worker.join();
    lock.EndAllowThreads();
    CHECK(succeeded && _callbackResultsDestroyed == 2);

    PyErr_SetString(PyExc_ValueError, "existing error");
    CHECK(!Tf_PyNoticeInvokeCallback(callable.ptr(), notice.ptr(), nullptr));
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError) && _callbackResultsDestroyed == 2);
    PyErr_Clear();

    TfPyObjWrapper failing(Eval("lambda notice, sender: 1 / 0"), TfPyNewReference);
    TfErrorMark errors;
    CHECK(!Tf_PyNoticeInvokeCallback(failing.ptr(), notice.ptr(), nullptr));
    CHECK(!errors.IsClean() && !PyErr_Occurred());
    const auto *exception = errors.GetBegin()->GetInfo<TfPyExceptionState>();
    CHECK(exception && exception->GetType() == PyExc_ZeroDivisionError);
    errors.Clear();
    CHECK(Tf_PyNoticeInvokeCallback(callable.ptr(), notice.ptr(), nullptr));
    CHECK(errors.IsClean() && _callbackResultsDestroyed == 3);
}

void TestExceptionOwnership()
{
    TfPyObjWrapper left(Eval("type('Eq', (), {'__eq__': lambda a, b: 1 / 0})()"),
                        TfPyNewReference);
    TfPyObjWrapper right(NewBox(), TfPyNewReference);
    bool equalityThrew = false;
    try { (void)(left == right); }
    catch (TfPyErrorAlreadySet const &) { equalityThrew = true; }
    CHECK(equalityThrew && PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
    PyErr_Clear();

    bool caught = false;
    try { TfPyThrowValueError("neutral sentinel"); }
    catch (TfPyErrorAlreadySet const &) { caught = true; }
    CHECK(caught && PyErr_ExceptionMatches(PyExc_ValueError));
    auto state = TfPyExceptionState::Fetch();
    CHECK(!PyErr_Occurred());
    TfPyExceptionState copy(state);
    auto assigned = TfPyExceptionState::Fetch();
    assigned = state;
    PyObject *value = state.GetValue();
    state.Restore();
    CHECK(!state.GetType() && !state.GetValue());
    PyErr_Clear();
    CHECK(copy.GetValue() == value);
    copy.Restore();
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    CHECK(assigned.GetValue() == value);
    assigned.Restore();
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
}

int RunContracts()
{
    Py_Initialize();
    TfPyLock lock;
    _globals = PyDict_New();
    PyDict_SetItemString(_globals, "__builtins__", PyEval_GetBuiltins());
    _boxType = Eval("type('Box', (), {})");
    auto run = [](const char *name, auto test) {
        std::printf("Testing %s\n", name);
        std::fflush(stdout);
        test();
    };
    run("wrapper references and worker-thread GIL", [&] { TestWrapperReferencesAndThreads(lock); });
    run("factory dispatch, missing registration and reentrancy", TestFactoryRegistry);
    run("identity reuse, expiry and failed factories", TestIdentityAndExpiry);
    run("Python/C++ ownership transfer", TestRefPtrOwnership);
    run("callback references, errors and worker-thread GIL", [&] { TestCallbackContracts(lock); });
    run("exception-state reference ownership", TestExceptionOwnership);
    CHECK(!PyErr_Occurred());
    CHECK(!PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Tf"));
    Py_DECREF(_boxType);
    Py_DECREF(_globals);
    std::puts("PASS: binding-neutral ownership, identity, factories, callbacks and errors");
    return 0;
}

} // namespace

int main()
{
    try {
        return RunContracts();
    } catch (std::exception const &error) {
        std::fprintf(stderr, "Contract failed: %s\n", error.what());
        return 1;
    }
}
