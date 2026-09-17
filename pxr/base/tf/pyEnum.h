//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_ENUM_H
#define PXR_BASE_TF_PY_ENUM_H

/// \file tf/pyEnum.h
/// Provide binding-neutral facilities for wrapping enums for script.

#include "pxr/pxr.h"

#include "pxr/base/tf/api.h"
#include "pxr/base/tf/enum.h"
#include "pxr/base/tf/hash.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/tf/pySafePython.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/singleton.h"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

/// \class Tf_PyEnum
///
/// Base class of all python enum classes.
class Tf_PyEnum { };

/// \class Tf_PyEnumRegistry
///
/// This is a private class that manages registered enum objects.
/// \private
class Tf_PyEnumRegistry {

  public:
    typedef Tf_PyEnumRegistry This;
    typedef PyObject *(*UnknownValueFactory)(const std::string &name,
                                             TfEnum const &value);

  private:
    Tf_PyEnumRegistry();
    virtual ~Tf_PyEnumRegistry();
    friend class TfSingleton<This>;

  public:

    TF_API static This &GetInstance() {
        return TfSingleton<This>::GetInstance();
    }

    TF_API
    void RegisterValue(TfEnum const &e, PyObject *obj);

    TF_API
    bool FindEnumForPythonObject(PyObject *obj, TfEnum *e) const;

    TF_API
    PyObject *ConvertEnumToPython(TfEnum const &e);

    TF_API
    void SetUnknownValueFactory(UnknownValueFactory factory);

    TF_API
    bool MarkPythonConvertersRegistered();

  private:

    // Since our enum objects live as long as the registry does, we can use the
    // pointer values for a hash.
    struct _ObjectHash {
        size_t operator()(PyObject *o) const {
            return reinterpret_cast<size_t>(o);
        }
    };

    TfHashMap<TfEnum, PyObject *, TfHash> _enumsToObjects;
    TfHashMap<PyObject *, TfEnum, _ObjectHash> _objectsToEnums;
    UnknownValueFactory _unknownValueFactory;
    bool _pythonConvertersRegistered;
};

TF_API_TEMPLATE_CLASS(TfSingleton<Tf_PyEnumRegistry>);

// Private function used for __repr__ of wrapped enum types.
TF_API
std::string Tf_PyEnumRepr(PyObject *self);

// Private base class for types which are instantiated and exposed to python
// for each registered enum type.
struct Tf_PyEnumWrapper : public Tf_PyEnum
{
    typedef Tf_PyEnumWrapper This;

    Tf_PyEnumWrapper(std::string const &n, TfEnum const &val) :
        name(n), value(val) {}
    long GetValue() const {
        return value.GetValueAsInt();
    }
    std::string GetName() const{
        return name;
    }
    std::string GetDisplayName() const {
        return TfEnum::GetDisplayName(value);
    }
    std::string GetFullName() const {
        return TfEnum::GetFullName(value);
    }
    friend bool operator ==(Tf_PyEnumWrapper const &self,
                            long other) {
        return self.value.GetValueAsInt() == other;
    }

    friend bool operator ==(Tf_PyEnumWrapper const &lhs,
                            Tf_PyEnumWrapper const &rhs) {
        return lhs.value == rhs.value;
    }

    friend bool operator !=(Tf_PyEnumWrapper const &lhs,
                            Tf_PyEnumWrapper const &rhs) {
        return !(lhs == rhs);
    }

    friend bool operator <(Tf_PyEnumWrapper const &lhs,
                           Tf_PyEnumWrapper const &rhs)
    {
        // If same, not less.
        if (lhs == rhs)
            return false;
        // If types don't match, string compare names.
        if (!lhs.value.IsA(rhs.value.GetType()))
            return TfEnum::GetFullName(lhs.value) <
                TfEnum::GetFullName(rhs.value);
        // If types do match, numerically compare values.
        return lhs.GetValue() < rhs.GetValue();
    }

    friend bool operator >(Tf_PyEnumWrapper const& lhs,
                           Tf_PyEnumWrapper const& rhs)
    {
        return rhs < lhs;
    }

    friend bool operator <=(Tf_PyEnumWrapper const& lhs,
                            Tf_PyEnumWrapper const& rhs)
    {
        return !(lhs > rhs);
    }

    friend bool operator >=(Tf_PyEnumWrapper const& lhs,
                            Tf_PyEnumWrapper const& rhs)
    {
        return !(lhs < rhs);
    }

    //
    // XXX Bitwise operators for Enums are a temporary measure to support the
    // use of Enums as Bitmasks in libSd.  It should be noted that Enums are
    // NOT closed under these operators. The proper place for such operators
    // is in a yet-nonexistent Bitmask type.
    //

    friend TfEnum operator |(Tf_PyEnumWrapper const &lhs,
                        Tf_PyEnumWrapper const &rhs) {
        if (lhs.value.IsA(rhs.value.GetType())) {
            return TfEnum(lhs.value.GetType(),
                          lhs.value.GetValueAsInt() |
                          rhs.value.GetValueAsInt());
        }
        TfPyThrowTypeError("Enum type mismatch");
        return TfEnum();
    }
    friend TfEnum operator |(Tf_PyEnumWrapper const &lhs, long rhs) {
        return TfEnum(lhs.value.GetType(), lhs.value.GetValueAsInt() | rhs);
    }
    friend TfEnum operator |(long lhs, Tf_PyEnumWrapper const &rhs) {
        return TfEnum(rhs.value.GetType(), lhs | rhs.value.GetValueAsInt());
    }

    friend TfEnum operator &(Tf_PyEnumWrapper const &lhs,
                             Tf_PyEnumWrapper const &rhs) {
        if (lhs.value.IsA(rhs.value.GetType())) {
            return TfEnum(lhs.value.GetType(),
                          lhs.value.GetValueAsInt() &
                          rhs.value.GetValueAsInt());
        }
        TfPyThrowTypeError("Enum type mismatch");
        return TfEnum();
    }
    friend TfEnum operator &(Tf_PyEnumWrapper const &lhs, long rhs) {
        return TfEnum(lhs.value.GetType(), lhs.value.GetValueAsInt() & rhs);
    }
    friend TfEnum operator &(long lhs, Tf_PyEnumWrapper const &rhs) {
        return TfEnum(rhs.value.GetType(), lhs & rhs.value.GetValueAsInt());
    }

    friend TfEnum operator ^(Tf_PyEnumWrapper const &lhs,
                             Tf_PyEnumWrapper const &rhs) {
        if (lhs.value.IsA(rhs.value.GetType())) {
            return TfEnum(lhs.value.GetType(),
                          lhs.value.GetValueAsInt() ^
                          rhs.value.GetValueAsInt());
        }
        TfPyThrowTypeError("Enum type mismatch");
        return TfEnum();
    }
    friend TfEnum operator ^(Tf_PyEnumWrapper const &lhs, long rhs) {
        return TfEnum(lhs.value.GetType(), lhs.value.GetValueAsInt() ^ rhs);
    }
    friend TfEnum operator ^(long lhs, Tf_PyEnumWrapper const &rhs) {
        return TfEnum(rhs.value.GetType(), lhs ^ rhs.value.GetValueAsInt());
    }

    friend TfEnum operator ~(Tf_PyEnumWrapper const &rhs) {
        return TfEnum(rhs.value.GetType(), ~rhs.value.GetValueAsInt());
    }
    std::string name;
    TfEnum value;
};

// Sanitizes the given \p name for use as a Python identifier. This includes
// replacing spaces with '_' and appending '_' to names matching Python
// keywords.
//
// If \p stripPackageName is true and \p name begins with the package name,
// it will be stripped off.
TF_API
std::string Tf_PyCleanEnumName(std::string name,
                               bool stripPackageName = false);

// Adds attribute of given name with given value to given scope.
// Issues a coding error if attribute by that name already existed.
TF_API
void Tf_PyEnumAddAttribute(PyObject *scope,
                           const std::string &name,
                           PyObject *value);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_ENUM_H
