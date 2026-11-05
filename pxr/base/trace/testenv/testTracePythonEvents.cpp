//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/trace/collector.h"
#include "pxr/base/trace/aggregateNode.h"
#include "pxr/base/trace/reporter.h"
#include "pxr/base/trace/reporterDataSourceCollector.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/pyTracing.h"

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

static_assert(std::is_same<decltype(TfPyTraceInfo::what), int>::value,
              "Keep the existing callback field type");

static void
Run()
{
    const char *source =
        "def trace_test_inner(n):\n"
        "    if n:\n"
        "        return trace_test_inner(n - 1) + 1\n"
        "    try:\n"
        "        raise ValueError('expected')\n"
        "    except ValueError:\n"
        "        return 40\n"
        "def trace_test_outer():\n"
        "    return trace_test_inner(2)\n"
        "assert trace_test_outer() == 42\n";
    PyObject *globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    TF_AXIOM(globals);
    PyObject *code = Py_CompileString(source, "<trace-events>", Py_file_input);
    TF_AXIOM(code);
    PyObject *result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!result) {
        PyErr_Print();
    }
    TF_AXIOM(result);
    Py_DECREF(result);
    TF_AXIOM(!PyErr_Occurred());
}

static int
CountScopes(TraceAggregateNodePtr node, const std::string &prefix)
{
    int count = node->GetKey().GetString().find(prefix) == 0
        ? node->GetCount() : 0;
    for (auto child : node->GetChildren()) {
        count += CountScopes(child, prefix);
    }
    return count;
}

int main()
{
    // Initialize directly: Tf's module loader must not supply bindings to this
    // native consumer. All Python calls in this test use the Limited API.
    Py_Initialize();
    std::vector<std::string> stack;
    int calls = 0, returns = 0, exceptions = 0, lines = 0;
    TfPyTraceFn callback = [&](const TfPyTraceInfo &info) {
        const std::string name(info.funcName);
        if (name.find("trace_test_") != 0) {
            return;
        }
        TF_AXIOM(std::string(info.fileName) == "<trace-events>");
        TF_AXIOM(info.funcLine > 0);
        switch (info.what) {
        case TfPyTraceCall:
            ++calls;
            stack.push_back(name);
            break;
        case TfPyTraceReturn:
            ++returns;
            TF_AXIOM(!stack.empty() && stack.back() == name);
            stack.pop_back();
            TF_AXIOM(PyLong_AsLong(info.arg) >= 40);
            break;
        case TfPyTraceException:
            ++exceptions;
            TF_AXIOM(PyTuple_Size(info.arg) == 3);
            break;
        case TfPyTraceLine:
            ++lines;
            break;
        default:
            TF_FATAL_ERROR("Unexpected Python event: %d", info.what);
        }
    };
    auto registration = TfPyRegisterTraceFn(callback);
    auto reporter = TraceReporter::New(
        "Python events", TraceReporterDataSourceCollector::New());
    auto &collector = TraceCollector::GetInstance();
    collector.Clear();
    collector.SetEnabled(true);
    collector.SetPythonTracingEnabled(true);
    Run();
    collector.SetPythonTracingEnabled(false);
    collector.SetEnabled(false);
    TF_AXIOM(calls == 4 && returns == 4 && exceptions == 1 && lines > 0);
    TF_AXIOM(stack.empty());
    reporter->UpdateTraceTrees();
    TF_AXIOM(CountScopes(reporter->GetAggregateTreeRoot(),
                         "trace_test_outer() (py)") == 1);
    TF_AXIOM(CountScopes(reporter->GetAggregateTreeRoot(),
                         "trace_test_inner() (py)") == 3);

    // Removing the last listener disables the Python trace hook; registering a
    // new listener must reinstall it and retain call/return/exception semantics.
    registration.reset();
    Run();
    TF_AXIOM(calls == 4 && returns == 4 && exceptions == 1);
    PyObject *sys = PyImport_ImportModule("sys");
    TF_AXIOM(sys);
    PyObject *hook = PyObject_CallMethod(sys, "gettrace", nullptr);
    TF_AXIOM(hook == Py_None);
    Py_DECREF(hook);
    Py_DECREF(sys);
    registration = TfPyRegisterTraceFn(callback);
    Run();
    TF_AXIOM(calls == 8 && returns == 8 && exceptions == 2 && stack.empty());
    registration.reset();
    for (const char *module : {"pxr.Tf", "pxr.Trace"}) {
        TF_AXIOM(!PyDict_GetItemString(PyImport_GetModuleDict(), module));
    }
    std::puts(Py_GetVersion());
    std::puts("PASS: native Python events, recursive Trace scopes and listeners");
}
