//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef USDCONTRIVED_GENERATED_DERIVED_H
#define USDCONTRIVED_GENERATED_DERIVED_H

/// \file usdContrived/derived.h

#include "pxr/pxr.h"
#include "pxr/usd/usdContrived/api.h"
#include "pxr/usd/usdContrived/base.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdContrived/tokens.h"

#include "pxr/base/vt/value.h"

#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/matrix4d.h"

#include "pxr/base/tf/token.h"
#include "pxr/base/tf/type.h"

PXR_NAMESPACE_OPEN_SCOPE

class SdfAssetPath;

// -------------------------------------------------------------------------- //
// DERIVED                                                                    //
// -------------------------------------------------------------------------- //

/// \class UsdContrivedDerived
///
/// \em Emphasized! \section Test_Section Test Section
///
/// For any described attribute \em Fallback \em Value or \em Allowed \em Values below
/// that are text/tokens, the actual token is published and defined in \ref UsdContrivedTokens.
/// So to set an attribute to the value "rightHanded", use UsdContrivedTokens->rightHanded
/// as the value.
///
class UsdContrivedDerived : public UsdContrivedBase
{
public:
    /// Compile time constant representing what kind of schema this class is.
    ///
    /// \sa UsdSchemaKind
    static const UsdSchemaKind schemaKind = UsdSchemaKind::ConcreteTyped;

    /// Construct a UsdContrivedDerived on UsdPrim \p prim .
    /// Equivalent to UsdContrivedDerived::Get(prim.GetStage(), prim.GetPath())
    /// for a \em valid \p prim, but will not immediately throw an error for
    /// an invalid \p prim
    explicit UsdContrivedDerived(const UsdPrim& prim=UsdPrim())
        : UsdContrivedBase(prim)
    {
    }

    /// Construct a UsdContrivedDerived on the prim held by \p schemaObj .
    /// Should be preferred over UsdContrivedDerived(schemaObj.GetPrim()),
    /// as it preserves SchemaBase state.
    explicit UsdContrivedDerived(const UsdSchemaBase& schemaObj)
        : UsdContrivedBase(schemaObj)
    {
    }

    /// Destructor.
    USDCONTRIVED_API
    virtual ~UsdContrivedDerived();

    /// Return a vector of names of all pre-declared attributes for this schema
    /// class and all its ancestor classes.  Does not include attributes that
    /// may be authored by custom/extended methods of the schemas involved.
    USDCONTRIVED_API
    static const TfTokenVector &
    GetSchemaAttributeNames(bool includeInherited=true);

    /// Return a UsdContrivedDerived holding the prim adhering to this
    /// schema at \p path on \p stage.  If no prim exists at \p path on
    /// \p stage, or if the prim at that path does not adhere to this schema,
    /// return an invalid schema object.  This is shorthand for the following:
    ///
    /// \code
    /// UsdContrivedDerived(stage->GetPrimAtPath(path));
    /// \endcode
    ///
    USDCONTRIVED_API
    static UsdContrivedDerived
    Get(const UsdStagePtr &stage, const SdfPath &path);

    /// Attempt to ensure a \a UsdPrim adhering to this schema at \p path
    /// is defined (according to UsdPrim::IsDefined()) on this stage.
    ///
    /// If a prim adhering to this schema at \p path is already defined on this
    /// stage, return that prim.  Otherwise author an \a SdfPrimSpec with
    /// \a specifier == \a SdfSpecifierDef and this schema's prim type name for
    /// the prim at \p path at the current EditTarget.  Author \a SdfPrimSpec s
    /// with \p specifier == \a SdfSpecifierDef and empty typeName at the
    /// current EditTarget for any nonexistent, or existing but not \a Defined
    /// ancestors.
    ///
    /// The given \a path must be an absolute prim path that does not contain
    /// any variant selections.
    ///
    /// If it is impossible to author any of the necessary PrimSpecs, (for
    /// example, in case \a path cannot map to the current UsdEditTarget's
    /// namespace) issue an error and return an invalid \a UsdPrim.
    ///
    /// Note that this method may return a defined prim whose typeName does not
    /// specify this schema class, in case a stronger typeName opinion overrides
    /// the opinion at the current EditTarget.
    ///
    USDCONTRIVED_API
    static UsdContrivedDerived
    Define(const UsdStagePtr &stage, const SdfPath &path);

protected:
    /// Returns the kind of schema this class belongs to.
    ///
    /// \sa UsdSchemaKind
    USDCONTRIVED_API
    UsdSchemaKind _GetSchemaKind() const override;

private:
    // needs to invoke _GetStaticTfType.
    friend class UsdSchemaRegistry;
    USDCONTRIVED_API
    static const TfType &_GetStaticTfType();

    static bool _IsTypedSchema();

    // override SchemaBase virtuals.
    USDCONTRIVED_API
    const TfType &_GetTfType() const override;

public:
    // --------------------------------------------------------------------- //
    // PIVOTPOSITION 
    // --------------------------------------------------------------------- //
    /// Rotation pivot position for this prim's transformation. 
    /// Provided as advisory data only for use by authoring applications,
    /// and should have no effect on the transformation encoded in the
    /// 'transform' attribute.
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `float3 pivotPosition = (0, 0, 0)` |
    /// | C++ Type | GfVec3f |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Float3 |
    USDCONTRIVED_API
    UsdAttribute GetPivotPositionAttr() const;

    /// See GetPivotPositionAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreatePivotPositionAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // MYVECFARRAY 
    // --------------------------------------------------------------------- //
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `float3[] myVecfArray` |
    /// | C++ Type | VtArray<GfVec3f> |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Float3Array |
    USDCONTRIVED_API
    UsdAttribute GetMyVecfArrayAttr() const;

    /// See GetMyVecfArrayAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateMyVecfArrayAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // HOLEINDICES 
    // --------------------------------------------------------------------- //
    /// The face indices (indexing into the 'faceVertexCounts'
    /// attribute) of all faces that should be made invisible.
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int[] holeIndices = []` |
    /// | C++ Type | VtArray<int> |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->IntArray |
    USDCONTRIVED_API
    UsdAttribute GetHoleIndicesAttr() const;

    /// See GetHoleIndicesAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateHoleIndicesAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // CORNERINDICES 
    // --------------------------------------------------------------------- //
    /// The vertex indices of all vertices that are sharp corners.
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int[] cornerIndices = []` |
    /// | C++ Type | VtArray<int> |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->IntArray |
    USDCONTRIVED_API
    UsdAttribute GetCornerIndicesAttr() const;

    /// See GetCornerIndicesAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateCornerIndicesAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // CORNERSHARPNESSES 
    // --------------------------------------------------------------------- //
    /// The sharpness values for corners: each corner gets a single
    /// sharpness value (Usd.Mesh.SHARPNESS_INFINITE for a perfectly sharp
    /// corner), so the size of this array must match that of
    /// 'cornerIndices'
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `float[] cornerSharpnesses = []` |
    /// | C++ Type | VtArray<float> |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->FloatArray |
    USDCONTRIVED_API
    UsdAttribute GetCornerSharpnessesAttr() const;

    /// See GetCornerSharpnessesAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateCornerSharpnessesAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // CREASELENGTHS 
    // --------------------------------------------------------------------- //
    /// The length of this array specifies the number of creases on the
    /// surface. Each element gives the number of (must be adjacent) vertices in
    /// each crease, whose indices are linearly laid out in the 'creaseIndices'
    /// attribute. Since each crease must be at least one edge long, each
    /// element of this array should be greater than one.
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int[] creaseLengths = []` |
    /// | C++ Type | VtArray<int> |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->IntArray |
    USDCONTRIVED_API
    UsdAttribute GetCreaseLengthsAttr() const;

    /// See GetCreaseLengthsAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateCreaseLengthsAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // TRANSFORM 
    // --------------------------------------------------------------------- //
    /// Double-precision transformation matrix, which should encode
    /// the entire local transformation for a prim.
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `matrix4d transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )` |
    /// | C++ Type | GfMatrix4d |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Matrix4d |
    USDCONTRIVED_API
    UsdAttribute GetTransformAttr() const;

    /// See GetTransformAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateTransformAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // TESTINGASSET 
    // --------------------------------------------------------------------- //
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `asset[] testingAsset` |
    /// | C++ Type | VtArray<SdfAssetPath> |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->AssetArray |
    USDCONTRIVED_API
    UsdAttribute GetTestingAssetAttr() const;

    /// See GetTestingAssetAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateTestingAssetAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // NAMESPACEDPROPERTY 
    // --------------------------------------------------------------------- //
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `float namespaced:property = 1` |
    /// | C++ Type | float |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Float |
    USDCONTRIVED_API
    UsdAttribute GetNamespacedPropertyAttr() const;

    /// See GetNamespacedPropertyAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateNamespacedPropertyAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // JUSTDEFAULT 
    // --------------------------------------------------------------------- //
    /// newToken should be included in the global token set.
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `token justDefault = "newToken"` |
    /// | C++ Type | TfToken |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Token |
    USDCONTRIVED_API
    UsdAttribute GetJustDefaultAttr() const;

    /// See GetJustDefaultAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateJustDefaultAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // OVERRIDEBASETRUEDERIVEDFALSE 
    // --------------------------------------------------------------------- //
    /// API schema override explicitly set to True in Base.
    /// API schema override explicitly set to False in Derived.
    /// Generates API functions in Derived. (1/5)
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int overrideBaseTrueDerivedFalse = 1` |
    /// | C++ Type | int |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Int |
    USDCONTRIVED_API
    UsdAttribute GetOverrideBaseTrueDerivedFalseAttr() const;

    /// See GetOverrideBaseTrueDerivedFalseAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateOverrideBaseTrueDerivedFalseAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // OVERRIDEBASETRUEDERIVEDNONE 
    // --------------------------------------------------------------------- //
    /// API schema override explicitly set to True in Base.
    /// API schema override has no opinion in Derived (defaults to False).
    /// Generates API functions in Derived. (2/5)
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int overrideBaseTrueDerivedNone = 1` |
    /// | C++ Type | int |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Int |
    USDCONTRIVED_API
    UsdAttribute GetOverrideBaseTrueDerivedNoneAttr() const;

    /// See GetOverrideBaseTrueDerivedNoneAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateOverrideBaseTrueDerivedNoneAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // OVERRIDEBASEFALSEDERIVEDFALSE 
    // --------------------------------------------------------------------- //
    /// API schema override explicitly set to False in Base.
    /// API schema override explicitly set to False in Derived.
    /// Generates API functions in Derived. (3/5)
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int overrideBaseFalseDerivedFalse = 1` |
    /// | C++ Type | int |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Int |
    USDCONTRIVED_API
    UsdAttribute GetOverrideBaseFalseDerivedFalseAttr() const;

    /// See GetOverrideBaseFalseDerivedFalseAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateOverrideBaseFalseDerivedFalseAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // OVERRIDEBASEFALSEDERIVEDNONE 
    // --------------------------------------------------------------------- //
    /// API schema override explicitly set to False in Base.
    /// API schema override has no opinion in Derived (defaults to False).
    /// Generates API functions in Derived. (4/5)
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int overrideBaseFalseDerivedNone = 1` |
    /// | C++ Type | int |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Int |
    USDCONTRIVED_API
    UsdAttribute GetOverrideBaseFalseDerivedNoneAttr() const;

    /// See GetOverrideBaseFalseDerivedNoneAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateOverrideBaseFalseDerivedNoneAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // OVERRIDEBASENONEDERIVEDFALSE 
    // --------------------------------------------------------------------- //
    /// API schema override has no opinion in Base.
    /// API schema override explicitly set to False in Derived.
    /// Generates API functions in Derived. (5/5)
    /// 
    ///
    /// | ||
    /// | -- | -- |
    /// | Declaration | `int overrideBaseNoneDerivedFalse = 1` |
    /// | C++ Type | int |
    /// | \ref Usd_Datatypes "Usd Type" | SdfValueTypeNames->Int |
    USDCONTRIVED_API
    UsdAttribute GetOverrideBaseNoneDerivedFalseAttr() const;

    /// See GetOverrideBaseNoneDerivedFalseAttr(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create.
    /// If specified, author \p defaultValue as the attribute's default,
    /// sparsely (when it makes sense to do so) if \p writeSparsely is \c true -
    /// the default for \p writeSparsely is \c false.
    USDCONTRIVED_API
    UsdAttribute CreateOverrideBaseNoneDerivedFalseAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

public:
    // --------------------------------------------------------------------- //
    // BINDING 
    // --------------------------------------------------------------------- //
    /// This is my awesome relationship.
    ///
    USDCONTRIVED_API
    UsdRelationship GetBindingRel() const;

    /// See GetBindingRel(), and also 
    /// \ref Usd_Create_Or_Get_Property for when to use Get vs Create
    USDCONTRIVED_API
    UsdRelationship CreateBindingRel() const;

public:
    // ===================================================================== //
    // Feel free to add custom code below this line, it will be preserved by 
    // the code generator. 
    //
    // Just remember to: 
    //  - Close the class declaration with }; 
    //  - Close the namespace with PXR_NAMESPACE_CLOSE_SCOPE
    //  - Close the include guard with #endif
    // ===================================================================== //
    // --(BEGIN CUSTOM CODE)--
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
