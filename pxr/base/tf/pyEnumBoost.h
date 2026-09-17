//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_ENUM_BOOST_H
#define PXR_BASE_TF_PY_ENUM_BOOST_H

/// \file tf/pyEnumBoost.h
/// Boost.Python adapters for Tf enum wrapping.

#include "pxr/pxr.h"

#include "pxr/base/tf/pyEnum.h"

#include "pxr/base/arch/demangle.h"
#include "pxr/base/tf/pyObjWrapperBoost.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/type.h"

#include "pxr/external/boost/python/class.hpp"
#include "pxr/external/boost/python/converter/from_python.hpp"
#include "pxr/external/boost/python/converter/registered.hpp"
#include "pxr/external/boost/python/converter/rvalue_from_python_data.hpp"
#include "pxr/external/boost/python/list.hpp"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/operators.hpp"
#include "pxr/external/boost/python/refcount.hpp"
#include "pxr/external/boost/python/scope.hpp"
#include "pxr/external/boost/python/to_python_converter.hpp"
#include "pxr/external/boost/python/tuple.hpp"

#include <string>
#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

namespace Tf_PyEnumBoost {

inline PyObject *
CreateUnknownValue(const std::string &name, TfEnum const &value)
{
    pxr_boost::python::object wrappedVal =
        pxr_boost::python::object(Tf_PyEnumWrapper(name, value));
    wrappedVal.attr("_baseName") = std::string();
    return pxr_boost::python::incref(wrappedVal.ptr());
}

template <typename T>
struct EnumFromPython {
    EnumFromPython() {
        pxr_boost::python::converter::registry::insert
            (&convertible, &construct, pxr_boost::python::type_id<T>());
    }
    static void *convertible(PyObject *obj) {
        TfEnum e;
        if (!Tf_PyEnumRegistry::GetInstance().FindEnumForPythonObject(
                obj, &e)) {
            return nullptr;
        }
        // In the case of producing a TfEnum or an integer, any registered enum
        // type is fine. In all other cases, the enum types must match.
        if (std::is_same<T, TfEnum>::value ||
            (std::is_integral<T>::value && !std::is_enum<T>::value)) {
            return obj;
        }
        return e.IsA<T>() ? obj : nullptr;
    }
    static void construct(PyObject *src, pxr_boost::python::converter::
                          rvalue_from_python_stage1_data *data) {
        void *storage =
            ((pxr_boost::python::converter::
              rvalue_from_python_storage<T> *)data)->storage.bytes;
        TfEnum e;
        TF_VERIFY(Tf_PyEnumRegistry::GetInstance().FindEnumForPythonObject(
            src, &e));
        new (storage) T(_GetEnumValue(e, (T *)0));
        data->convertible = storage;
    }
private:
    // Overloads to explicitly allow conversion of the TfEnum integer value to
    // other enum/integral types.
    template <typename U>
    static U _GetEnumValue(TfEnum const &e, U *) {
        return U(e.GetValueAsInt());
    }
    static TfEnum _GetEnumValue(TfEnum const &e, TfEnum *) {
        return e;
    }
};

template <class T>
struct EnumToPython {
    static PyObject *convert(T t) {
        PyObject *obj = Tf_PyEnumRegistry
            ::GetInstance().ConvertEnumToPython(TfEnum(t));
        if (!obj) {
            pxr_boost::python::throw_error_already_set();
        }
        return obj;
    }
};

inline void
RegisterBaseConversions()
{
    Tf_PyEnumRegistry &registry = Tf_PyEnumRegistry::GetInstance();
    registry.SetUnknownValueFactory(&CreateUnknownValue);
    if (!registry.MarkPythonConvertersRegistered()) {
        return;
    }

    // Register general conversions to and from python for TfEnum.
    pxr_boost::python::to_python_converter<TfEnum, EnumToPython<TfEnum>>();

    EnumFromPython<TfEnum>();
    EnumFromPython<int>();
    EnumFromPython<unsigned int>();
    EnumFromPython<long>();
    EnumFromPython<unsigned long>();
}

template <typename T>
void RegisterConversions()
{
    RegisterBaseConversions();

    // Register conversions to and from python.
    pxr_boost::python::to_python_converter<T, EnumToPython<T> >();
    EnumFromPython<T>();
}

} // namespace Tf_PyEnumBoost

inline std::string
Tf_PyEnumRepr(pxr_boost::python::object const &self)
{
    std::string result = Tf_PyEnumRepr(self.ptr());
    if (PyErr_Occurred()) {
        pxr_boost::python::throw_error_already_set();
    }
    return result;
}

// Private template class which is instantiated and exposed to python for each
// registered enum type.
template <typename T>
struct Tf_TypedPyEnumWrapper : Tf_PyEnumWrapper
{
    Tf_TypedPyEnumWrapper(std::string const &n, TfEnum const &val) :
        Tf_PyEnumWrapper(n, val) {}

    static pxr_boost::python::object GetValueFromName(const std::string& name) {
        bool found = false;
        const TfEnum value = TfEnum::GetValueFromName<T>(name, &found);
        return found
            ? pxr_boost::python::object(value)
            : pxr_boost::python::object();
    }
};

inline void
Tf_PyEnumAddAttribute(pxr_boost::python::scope &s,
                      const std::string &name,
                      const pxr_boost::python::object &value)
{
    Tf_PyEnumAddAttribute(s.ptr(), name, value.ptr());
    if (PyErr_Occurred()) {
        pxr_boost::python::throw_error_already_set();
    }
}

/// \class TfPyWrapEnum
///
/// Used to wrap enum types for script.
///
/// TfPyWrapEnum provides a way to wrap enums for python, tying in with the \a
/// TfEnum system, and potentially providing automatic wrapping by using names
/// registered with the \a TfEnum system and by making some assumptions about
/// the way we structure our code. Enums that are not registered with TfEnum
/// may be manually wrapped using pxr_boost::python::enum_ instead.
///
/// Example usage. For an enum that looks like this:
/// \code
/// enum FooChoices {
///    FooFirst,
///    FooSecond,
///    FooThird
/// };
/// \endcode
///
/// Which has been registered in the \a TfEnum system and has names provided for
/// all values, it may be wrapped like this:
/// \code
/// TfPyWrapEnum<FooChoices>();
/// \endcode
///
/// The enum will appear in script as Foo.Choices.{First, Second, Third} and
/// the values will also appear as Foo.{First, Second, Third}.
///
/// An enum may be given an explicit name by passing a string to
/// TfPyWrapEnum's constructor.
///
/// If the enum is a C++11 scoped enum (aka enum class), the values will appear
/// as Foo.Choices.{First, Second, Third} in the following example:
/// \code
/// enum class FooChoices {
///    First,
///    Second,
///    Third
/// };
/// \endcode
///

// Detect scoped enums by using that the C++ standard does not allow them to
// be converted to int implicitly.
template <typename T, bool IsScopedEnum = !std::is_convertible<T, int>::value>
struct TfPyWrapEnum {

private:
    typedef pxr_boost::python::class_<
        Tf_TypedPyEnumWrapper<T>, pxr_boost::python::bases<Tf_PyEnumWrapper> >
    _EnumPyClassType;

public:

    /// Construct an enum wrapper object.
    /// If \a name is provided, it is used as the name of the enum. Otherwise
    /// the type name of \a T is used, with a leading MFB package name
    /// stripped.
    explicit TfPyWrapEnum( std::string const &name = std::string())
    {
        using namespace pxr_boost::python;

        const bool explicitName = !name.empty();

        // First, take either the given name, or the demangled type name.
        std::string enumName = explicitName ? name :
            TfStringReplace(ArchGetDemangled(typeid(T)), "::", ".");

        // If the name is dotted, take everything before the dot as the base
        // name. This is used in repr.
        std::string baseName = TfStringGetBeforeSuffix(enumName);
        if (baseName == enumName)
            baseName = std::string();

        // If the name is dotted, take the last element as the enum name.
        if (!TfStringGetSuffix(enumName).empty())
            enumName = TfStringGetSuffix(enumName);

        // If the name was not explicitly given, then clean it up by removing
        // the package name prefix if it exists.
        if (!explicitName) {
            if (!baseName.empty()) {
                baseName = Tf_PyCleanEnumName(
                    baseName, /* stripPackageName = */ true);
            }
            else {
                enumName = Tf_PyCleanEnumName(
                    enumName, /* stripPackageName = */ true);
            }
        }

        if (IsScopedEnum) {
            // Make the enumName appear in python representation
            // for scoped enums.
            if (!baseName.empty()) {
                baseName += ".";
            }
            baseName += enumName;
        }

        // Make a python type for T.
        _EnumPyClassType enumClass(enumName.c_str(), no_init);
        enumClass.def("GetValueFromName",
            &Tf_TypedPyEnumWrapper<T>::GetValueFromName, arg("name"));
        enumClass.staticmethod("GetValueFromName");
        enumClass.setattr("_baseName", baseName);

        // Register conversions for it.
        Tf_PyEnumBoost::RegisterConversions<T>();

        // Export values.
        //
        // Only strip the package name from top-level enum values.
        // For example, if an enum named "Foo" is declared at top-level
        // scope in Tf with values "TfBar" and "TfBaz", we want to strip
        // off Tf so that the values in Python will be Tf.Bar and Tf.Baz.
        const bool stripPackageName = baseName.empty();
        _ExportValues(stripPackageName, enumClass);

        // Register with Tf so that python clients of a TfType
        // that represents an enum are able to get to the equivalent
        // python class with .pythonclass
        const TfType &type = TfType::Find<T>();
        if (!type.IsUnknown())
            type.DefinePythonClass(TfPyObjWrapperFromBoostObject(enumClass));
    }

  private:

    /// Export all values in this enum to the enclosing scope.
    /// If no explicit names have been registered, this will export the TfEnum
    /// registered names and values (if any).
    void _ExportValues(bool stripPackageName, _EnumPyClassType &enumClass) {
        pxr_boost::python::list valueList;

        for (const std::string& name : TfEnum::GetAllNames<T>()) {
            bool success = false;
            TfEnum enumValue = TfEnum::GetValueFromName<T>(name, &success);
            if (!success) {
                continue;
            }

            const std::string cleanedName =
                Tf_PyCleanEnumName(name, stripPackageName);

            // convert value to python.
            Tf_TypedPyEnumWrapper<T> wrappedValue(cleanedName, enumValue);
            pxr_boost::python::object pyValue(wrappedValue);

            // register it as the python object for this value.
            Tf_PyEnumRegistry::GetInstance().RegisterValue(
                enumValue, pyValue.ptr());

            // Take all the values and export them into the current scope.
            std::string valueName = wrappedValue.GetName();
            if (IsScopedEnum) {
                // If scoped enum, enum values appear on the enumClass ...
                pxr_boost::python::scope s(enumClass);
                Tf_PyEnumAddAttribute(s, valueName, pyValue);
            } else {
                // ... otherwise, enum values appear on the enclosing scope.
                pxr_boost::python::scope s;
                Tf_PyEnumAddAttribute(s, valueName, pyValue);
            }

            valueList.append(pyValue);
        }

        // Add a tuple of all the values to the enum class.
        enumClass.setattr("allValues", pxr_boost::python::tuple(valueList));
    }

};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_ENUM_BOOST_H
