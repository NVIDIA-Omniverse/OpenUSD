//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "basisCurves.h"
#include "instancer.h"
#include "material.h"
#include "renderParam.h"

#include <renderer/geometry/context.h>
#include <renderer/geometry/curveEmbree.h>
#include <renderer/geometry/curveGeometry.h>
#include <renderer/geometry/curveSamplers.h>
#include <renderer/geometry/curveTopology.h>
#include <renderer/lights/lightLinking.h>
#include <renderer/renderer.h>

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/trace/trace.h"
#include "pxr/base/vt/typeHeaders.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/extComputationUtils.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/sdf/assetPath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

static const TfToken _tokensPrimvarsWidths("primvars:widths");
static const TfToken _tokensPrimvarsNormals("primvars:normals");

using _TokenSet = std::unordered_set<TfToken, TfToken::HashFunctor>;

struct _AuthoredPrimvarSource
{
    HdPrimvarDescriptor descriptor;
    VtValue value;
    VtIntArray indices;
    bool valueInitialized = false;
};

struct _ComputedPrimvarSource
{
    HdExtComputationPrimvarDescriptor descriptor;
    VtValue value;
    bool valueInitialized = false;
};

using _AuthoredPrimvarSourceMap = TfHashMap<
    TfToken, _AuthoredPrimvarSource, TfToken::HashFunctor>;
using _ComputedPrimvarSourceMap = TfHashMap<
    TfToken, _ComputedPrimvarSource, TfToken::HashFunctor>;

bool
_DescriptorsExactlyEqual(
    HdPrimvarDescriptor const& lhs,
    HdPrimvarDescriptor const& rhs)
{
    // HdPrimvarDescriptor::operator== intentionally omits indexed. The
    // storage/API transition is significant for a CPU renderer, so compare it
    // explicitly here.
    return lhs.name == rhs.name &&
        lhs.interpolation == rhs.interpolation &&
        lhs.role == rhs.role &&
        lhs.indexed == rhs.indexed;
}

bool
_DescriptorsExactlyEqual(
    HdExtComputationPrimvarDescriptor const& lhs,
    HdExtComputationPrimvarDescriptor const& rhs)
{
    return _DescriptorsExactlyEqual(
            static_cast<HdPrimvarDescriptor const&>(lhs),
            static_cast<HdPrimvarDescriptor const&>(rhs)) &&
        lhs.sourceComputationId == rhs.sourceComputationId &&
        lhs.sourceComputationOutputName ==
            rhs.sourceComputationOutputName &&
        lhs.valueType == rhs.valueType;
}

TfToken
_GetInterpolationToken(HdInterpolation interpolation)
{
    switch (interpolation) {
    case HdInterpolationConstant:
        return HdPrimvarSchemaTokens->constant;
    case HdInterpolationUniform:
        return HdPrimvarSchemaTokens->uniform;
    case HdInterpolationVarying:
        return HdPrimvarSchemaTokens->varying;
    case HdInterpolationVertex:
        return HdPrimvarSchemaTokens->vertex;
    case HdInterpolationFaceVarying:
        return HdPrimvarSchemaTokens->faceVarying;
    case HdInterpolationInstance:
        return HdPrimvarSchemaTokens->instance;
    case HdInterpolationCount:
        break;
    }
    return TfToken();
}

struct _PrimvarSourceView
{
    VtValue const* value = nullptr;
    VtIntArray const* indices = nullptr;
    HdInterpolation interpolation = HdInterpolationConstant;
    bool indexed = false;

    explicit operator bool() const noexcept
    {
        return value != nullptr;
    }
};

_PrimvarSourceView
_FindEffectiveSource(
    _AuthoredPrimvarSourceMap const& authoredSources,
    _ComputedPrimvarSourceMap const& computedSources,
    TfToken const& name)
{
    _ComputedPrimvarSourceMap::const_iterator const computedIt =
        computedSources.find(name);
    if (computedIt != computedSources.end() &&
        computedIt->second.valueInitialized) {
        _PrimvarSourceView result;
        result.value = &computedIt->second.value;
        result.interpolation =
            computedIt->second.descriptor.interpolation;
        return result;
    }

    _AuthoredPrimvarSourceMap::const_iterator const authoredIt =
        authoredSources.find(name);
    if (authoredIt != authoredSources.end() &&
        authoredIt->second.valueInitialized) {
        _PrimvarSourceView result;
        result.value = &authoredIt->second.value;
        result.indices = &authoredIt->second.indices;
        result.interpolation = authoredIt->second.descriptor.interpolation;
        result.indexed = authoredIt->second.descriptor.indexed;
        return result;
    }

    return _PrimvarSourceView();
}

ty::CurvePrimvarInput
_MakeCurvePrimvarInput(_PrimvarSourceView const& source)
{
    ty::CurvePrimvarInput result;
    if (!source) {
        return result;
    }

    result.authored = true;
    result.value = *source.value;
    result.interpolation = _GetInterpolationToken(source.interpolation);
    result.hasIndices = source.indexed;
    if (source.indices) {
        result.indices = *source.indices;
    }
    return result;
}

bool
_IsGeometrySourceName(TfToken const& name)
{
    return name == HdTokens->points ||
        name == HdTokens->widths ||
        name == HdTokens->normals ||
        name == _tokensPrimvarsWidths ||
        name == _tokensPrimvarsNormals;
}

bool
_OptionalFloatsEqual(
    std::optional<float> const& lhs,
    std::optional<float> const& rhs)
{
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    return *lhs == *rhs ||
        (std::isnan(*lhs) && std::isnan(*rhs));
}

size_t
_DiagnosticCount(size_t count)
{
    return count == 0 ? 1 : count;
}

struct _RecoveryDiagnosticItem
{
    std::string cause;
    size_t count = 0;
    std::string fallback;
};

void
_ReportFatal(
    SdfPath const& id,
    char const* category,
    char const* cause,
    size_t count)
{
    TF_RUNTIME_ERROR(
        "BasisCurves <%s>: category=%s cause=%s count=%zu "
        "fallback=emptyPrototype",
        id.GetText(), category, cause, _DiagnosticCount(count));
}

void
_ReportRecoveryGroup(
    SdfPath const& id,
    char const* category,
    std::vector<_RecoveryDiagnosticItem> const& items)
{
    std::string details;
    for (_RecoveryDiagnosticItem const& item : items) {
        if (item.count == 0) {
            continue;
        }
        if (!details.empty()) {
            details += "; ";
        }
        details += "cause=" + item.cause;
        details += " count=" + std::to_string(item.count);
        details += " fallback=" + item.fallback;
    }
    if (details.empty()) {
        return;
    }
    TF_WARN(
        "BasisCurves <%s>: category=%s %s",
        id.GetText(), category, details.c_str());
}

void
_ReportRecovery(
    SdfPath const& id,
    char const* category,
    char const* cause,
    size_t count,
    char const* fallback)
{
    _ReportRecoveryGroup(
        id, category,
        {{std::string(cause), count, std::string(fallback)}});
}

std::string
_AssetPathToString(SdfAssetPath const& assetPath)
{
    std::string const resolvedPath = assetPath.GetResolvedPath();
    return resolvedPath.empty() ? assetPath.GetAssetPath() : resolvedPath;
}

bool
_GetConstantStringValue(VtValue const& value, std::string* result)
{
    if (!result) {
        return false;
    }
    if (value.IsHolding<std::string>()) {
        *result = value.UncheckedGet<std::string>();
        return true;
    }
    if (value.IsHolding<TfToken>()) {
        *result = value.UncheckedGet<TfToken>().GetString();
        return true;
    }
    if (value.IsHolding<SdfAssetPath>()) {
        *result = _AssetPathToString(
            value.UncheckedGet<SdfAssetPath>());
        return true;
    }
    if (value.IsHolding<VtStringArray>()) {
        VtStringArray const& values =
            value.UncheckedGet<VtStringArray>();
        if (!values.empty()) {
            *result = values[0];
            return true;
        }
    }
    if (value.IsHolding<VtTokenArray>()) {
        VtTokenArray const& values = value.UncheckedGet<VtTokenArray>();
        if (!values.empty()) {
            *result = values[0].GetString();
            return true;
        }
    }
    if (value.IsHolding<VtArray<SdfAssetPath>>()) {
        VtArray<SdfAssetPath> const& values =
            value.UncheckedGet<VtArray<SdfAssetPath>>();
        if (!values.empty()) {
            *result = _AssetPathToString(values[0]);
            return true;
        }
    }
    return false;
}

/// Production owner for one representation-homogeneous curve geometry.
/// Destruction reverses Embree's dependency order: detach, release the
/// geometry handle, then destroy the stable context and shared buffers.
class _CurveGeometryRecord final
{
public:
    static std::unique_ptr<_CurveGeometryRecord> Create(
        RTCDevice device,
        std::unique_ptr<ty::CurveGeometryRecordData> data)
    {
        if (!data) {
            return nullptr;
        }

        RTCGeometry geometry = rtcNewGeometry(
            device, data->GetGeometryType());
        if (!geometry) {
            return nullptr;
        }
        if (!ty::BindCurveGeometryBuffers(geometry, *data)) {
            rtcReleaseGeometry(geometry);
            return nullptr;
        }
        rtcSetGeometryBuildQuality(geometry, RTC_BUILD_QUALITY_LOW);
        rtcSetGeometryTimeStepCount(geometry, 1);
        rtcSetGeometryMask(geometry, ty::RayMask::Scene);

        return std::unique_ptr<_CurveGeometryRecord>(
            new _CurveGeometryRecord(geometry, std::move(data)));
    }

    _CurveGeometryRecord(_CurveGeometryRecord const&) = delete;
    _CurveGeometryRecord& operator=(_CurveGeometryRecord const&) = delete;
    _CurveGeometryRecord(_CurveGeometryRecord&&) = delete;
    _CurveGeometryRecord& operator=(_CurveGeometryRecord&&) = delete;

    ~_CurveGeometryRecord()
    {
        if (_attachedId != RTC_INVALID_GEOMETRY_ID) {
            rtcDetachGeometry(_scene, _attachedId);
        }
        if (_geometry) {
            rtcReleaseGeometry(_geometry);
        }
    }

    void Commit()
    {
        rtcCommitGeometry(_geometry);
    }

    bool Attach(RTCScene scene)
    {
        if (_attachedId != RTC_INVALID_GEOMETRY_ID || !scene) {
            return false;
        }
        unsigned int const attachedId = rtcAttachGeometry(scene, _geometry);
        if (attachedId == RTC_INVALID_GEOMETRY_ID) {
            return false;
        }
        _scene = scene;
        _attachedId = attachedId;
        return true;
    }

    ty::CurveGeometryRecordData& GetData() noexcept
    {
        return *_data;
    }

    ty::CurveGeometryRecordData const& GetData() const noexcept
    {
        return *_data;
    }

private:
    _CurveGeometryRecord(
        RTCGeometry geometry,
        std::unique_ptr<ty::CurveGeometryRecordData> data)
        : _geometry(geometry)
        , _data(std::move(data))
    {
    }

    RTCScene _scene = nullptr;
    RTCGeometry _geometry = nullptr;
    unsigned int _attachedId = RTC_INVALID_GEOMETRY_ID;
    std::unique_ptr<ty::CurveGeometryRecordData> const _data;
};

/// Production owner for one top-level Embree instance and its stable context.
class _CurveInstance final
{
public:
    static std::unique_ptr<_CurveInstance> Create(
        RTCDevice device,
        RTCScene rootScene,
        RTCScene prototypeScene)
    {
        RTCGeometry geometry = rtcNewGeometry(
            device, RTC_GEOMETRY_TYPE_INSTANCE);
        if (!geometry) {
            return nullptr;
        }

        std::unique_ptr<ty::InstanceContext> context =
            std::make_unique<ty::InstanceContext>();
        context->rootScene = prototypeScene;
        rtcSetGeometryInstancedScene(geometry, prototypeScene);
        rtcSetGeometryTimeStepCount(geometry, 1);
        rtcSetGeometryUserData(geometry, context.get());

        return std::unique_ptr<_CurveInstance>(new _CurveInstance(
            rootScene, geometry, std::move(context)));
    }

    _CurveInstance(_CurveInstance const&) = delete;
    _CurveInstance& operator=(_CurveInstance const&) = delete;
    _CurveInstance(_CurveInstance&&) = delete;
    _CurveInstance& operator=(_CurveInstance&&) = delete;

    ~_CurveInstance()
    {
        if (_attachedId != RTC_INVALID_GEOMETRY_ID) {
            rtcDetachGeometry(_rootScene, _attachedId);
        }
        if (_geometry) {
            rtcReleaseGeometry(_geometry);
        }
    }

    bool Publish(
        GfMatrix4f const& objectToWorld,
        ty::CategorySet const& categories,
        int32_t instanceId,
        bool visible)
    {
        Update(objectToWorld, categories, instanceId, visible);
        if (_attachedId == RTC_INVALID_GEOMETRY_ID) {
            unsigned int const attachedId =
                rtcAttachGeometry(_rootScene, _geometry);
            if (attachedId == RTC_INVALID_GEOMETRY_ID) {
                return false;
            }
            _attachedId = attachedId;
        }
        return true;
    }

    void Update(
        GfMatrix4f const& objectToWorld,
        ty::CategorySet const& categories,
        int32_t instanceId,
        bool visible)
    {
        rtcSetGeometryTransform(
            _geometry, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
            objectToWorld.GetArray());
        rtcSetGeometryMask(
            _geometry, visible ? ty::RayMask::Scene : 0u);
        _context->objectToWorldMatrix = objectToWorld;
        _context->worldToObjectMatrix = objectToWorld.GetInverse();
        _context->categories = categories;
        _context->instanceId = instanceId;
        rtcCommitGeometry(_geometry);
    }

    void Commit()
    {
        rtcCommitGeometry(_geometry);
    }

    ty::InstanceContext const* GetContext() const noexcept
    {
        return _context.get();
    }

private:
    _CurveInstance(
        RTCScene rootScene,
        RTCGeometry geometry,
        std::unique_ptr<ty::InstanceContext> context)
        : _rootScene(rootScene)
        , _geometry(geometry)
        , _context(std::move(context))
    {
    }

    RTCScene const _rootScene;
    RTCGeometry _geometry = nullptr;
    unsigned int _attachedId = RTC_INVALID_GEOMETRY_ID;
    std::unique_ptr<ty::InstanceContext> const _context;
};

} // namespace

struct HdEmbreeBasisCurves::_Impl
{
    explicit _Impl(HdEmbreeBasisCurves* owner)
        : owner(owner)
    {
    }

    ~_Impl()
    {
        ResetEmbreeState();
    }

    void ResetEmbreeState()
    {
        instances.clear();
        records.clear();
        if (prototypeScene) {
            rtcReleaseScene(prototypeScene);
            prototypeScene = nullptr;
        }
        geometryInitialized = false;
        instancesInitialized = false;
    }

    HdDirtyBits GetRawDirtyBits(
        HdSceneDelegate* sceneDelegate,
        HdDirtyBits propagatedBits) const;
    void RefreshSourceCaches(
        HdSceneDelegate* sceneDelegate,
        HdDirtyBits rawBits,
        _TokenSet* effectiveChanges);
    _PrimvarSourceView FindSource(TfToken const& name) const;
    _PrimvarSourceView FindPointsSource() const;
    ty::CurveGeometryInput MakeGeometryInput() const;
    void EnsurePrototypeScene(RTCDevice device);
    void ClearGeomPropObservers();
    void PopulateContext(ty::PrototypeContext* context,
                         ty::MaterialEvalServices const* services) const;
    void RebuildSamplers(bool reportDiagnostics);
    void RebuildSamplers(
        ty::CurveGeometryRecordData* data,
        bool reportDiagnostics) const;
    void ResolveGeomPropBindings();
    bool RebuildGeometry(
        RTCDevice device,
        ty::MaterialEvalServices const* services);
    void ReportTopologyRecoveries() const;
    void ReportGeometryRecoveries(
        ty::CurveGeometryResult const& geometry) const;
    void UpdateInstances(
        HdSceneDelegate* sceneDelegate,
        RTCScene rootScene,
        RTCDevice device,
        bool visible);
    void CommitInstances();

    HdEmbreeBasisCurves* const owner;
    std::mutex mutex;
    RTCScene prototypeScene = nullptr;
    std::vector<std::unique_ptr<_CurveGeometryRecord>> records;
    std::vector<std::unique_ptr<_CurveInstance>> instances;

    HdBasisCurvesTopology topology;
    bool topologyInitialized = false;
    VtValue directPoints;
    bool directPointsInitialized = false;
    _AuthoredPrimvarSourceMap authoredSources;
    _ComputedPrimvarSourceMap computedSources;
    ty::CurveTopologyResult canonicalTopology;

    GfMatrix4f transform{1.0f};
    ty::CategorySet categories;
    HdCullStyle cullStyle = HdCullStyleDontCare;
    bool doubleSided = false;
    ty::MaterialData const* material = nullptr;

    std::optional<float> minimumWidth;
    std::uint64_t minimumWidthEpoch = 0;
    std::optional<float> appliedMinimumWidth;
    std::uint64_t appliedMinimumWidthEpoch = 0;

    TfToken lastRequestedReprToken;
    bool geometryInitialized = false;
    bool instancesInitialized = false;
    std::uint64_t geometryGeneration = 0;
};

HdDirtyBits
HdEmbreeBasisCurves::_Impl::GetRawDirtyBits(
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits propagatedBits) const
{
    HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();
    constexpr HdDirtyBits meaningfulBits =
        HdChangeTracker::AllSceneDirtyBits | HdChangeTracker::NewRepr;
    if ((propagatedBits & meaningfulBits) == HdChangeTracker::Clean) {
        // HdRenderIndex can call Sync once per active repr with one shared
        // mutable mask. The first call consumes it while the tracker remains
        // dirty until all reprs finish; do not replay that raw update.
        return HdChangeTracker::Clean;
    }

    HdDirtyBits rawBits = HdChangeTracker::Clean;
    if (renderIndex.GetRprim(owner->GetId()) == owner) {
        rawBits = renderIndex.GetChangeTracker().GetRprimDirtyBits(
            owner->GetId());
    }
    if ((rawBits & meaningfulBits) == HdChangeTracker::Clean &&
        (propagatedBits & meaningfulBits) != HdChangeTracker::Clean) {
        // Focused direct tests can call Sync without inserting this object in
        // the render index. In that case the incoming mask is the only valid
        // provenance available.
        rawBits = propagatedBits;
    }
    return rawBits;
}

namespace {

bool
_IsAuthoredSourceValueDirty(
    HdDirtyBits rawBits,
    SdfPath const& id,
    TfToken const& name)
{
    if (name == _tokensPrimvarsWidths) {
        return (rawBits & HdChangeTracker::DirtyWidths) != 0;
    }
    if (name == _tokensPrimvarsNormals) {
        return (rawBits & HdChangeTracker::DirtyNormals) != 0;
    }
    return HdChangeTracker::IsPrimvarDirty(rawBits, id, name);
}

bool
_IsComputedSourceValueDirty(
    HdDirtyBits rawBits,
    SdfPath const& id,
    TfToken const& name)
{
    // A Scene Index can translate a computed locator to generic DirtyPrimvar,
    // including for reserved names. Apply that compatibility rule only to
    // computed sources; authored reserved primvars retain their dedicated-bit
    // contract so a material-only DirtyPrimvar update does not rebuild curve
    // geometry.
    return (rawBits & HdChangeTracker::DirtyPrimvar) != 0 ||
        _IsAuthoredSourceValueDirty(rawBits, id, name);
}

} // namespace

void
HdEmbreeBasisCurves::_Impl::RefreshSourceCaches(
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits rawBits,
    _TokenSet* effectiveChanges)
{
    SdfPath const& id = owner->GetId();
    _TokenSet authoredChanges;
    _TokenSet computedChanges;

    constexpr HdDirtyBits descriptorBits =
        HdChangeTracker::DirtyPrimvar |
        HdChangeTracker::DirtyWidths |
        HdChangeTracker::DirtyNormals |
        HdChangeTracker::DirtyComputationPrimvarDesc;
    bool const enumerateDescriptors =
        (rawBits & descriptorBits) != HdChangeTracker::Clean;

    if (enumerateDescriptors) {
        _AuthoredPrimvarSourceMap nextSources;
        for (size_t interpolationIndex = 0;
             interpolationIndex < HdInterpolationCount;
             ++interpolationIndex) {
            HdInterpolation const interpolation =
                static_cast<HdInterpolation>(interpolationIndex);
            HdPrimvarDescriptorVector const descriptors =
                sceneDelegate->GetPrimvarDescriptors(id, interpolation);
            for (HdPrimvarDescriptor descriptor : descriptors) {
                descriptor.interpolation = interpolation;
                _AuthoredPrimvarSourceMap::const_iterator const previousIt =
                    authoredSources.find(descriptor.name);
                bool const descriptorChanged =
                    previousIt == authoredSources.end() ||
                    !_DescriptorsExactlyEqual(
                        previousIt->second.descriptor, descriptor);

                _AuthoredPrimvarSource source;
                if (previousIt != authoredSources.end()) {
                    source = previousIt->second;
                }
                source.descriptor = descriptor;

                bool const pullValue = descriptorChanged ||
                    _IsAuthoredSourceValueDirty(
                        rawBits, id, descriptor.name);
                if (pullValue) {
                    if (descriptor.indexed) {
                        VtIntArray indices;
                        source.value = sceneDelegate->GetIndexedPrimvar(
                            id, descriptor.name, &indices);
                        source.indices = std::move(indices);
                    } else {
                        source.value = sceneDelegate->Get(
                            id, descriptor.name);
                        source.indices.clear();
                    }
                    source.valueInitialized = true;
                    authoredChanges.insert(descriptor.name);
                } else if (descriptorChanged) {
                    authoredChanges.insert(descriptor.name);
                }
                nextSources[descriptor.name] = std::move(source);
            }
        }

        for (_AuthoredPrimvarSourceMap::value_type const& oldSource :
                authoredSources) {
            if (nextSources.find(oldSource.first) == nextSources.end()) {
                authoredChanges.insert(oldSource.first);
            }
        }
        authoredSources = std::move(nextSources);
    } else {
        for (_AuthoredPrimvarSourceMap::value_type& sourceEntry :
                authoredSources) {
            TfToken const& name = sourceEntry.first;
            _AuthoredPrimvarSource& source = sourceEntry.second;
            if (!_IsAuthoredSourceValueDirty(rawBits, id, name)) {
                continue;
            }
            if (source.descriptor.indexed) {
                VtIntArray indices;
                source.value = sceneDelegate->GetIndexedPrimvar(
                    id, name, &indices);
                source.indices = std::move(indices);
            } else {
                source.value = sceneDelegate->Get(id, name);
                source.indices.clear();
            }
            source.valueInitialized = true;
            authoredChanges.insert(name);
        }
    }

    // Some legacy delegates expose points only through Get(), not through a
    // primvar descriptor. Cache that authored source even while a computed
    // points descriptor shadows it so removing the computed descriptor can
    // restore the latest authored value without a clean pull.
    if ((rawBits & HdChangeTracker::DirtyPoints) != 0 &&
        authoredSources.find(HdTokens->points) == authoredSources.end()) {
        directPoints = sceneDelegate->Get(id, HdTokens->points);
        directPointsInitialized = true;
        authoredChanges.insert(HdTokens->points);
    }

    HdExtComputationPrimvarDescriptorVector descriptorsToCompute;
    if (enumerateDescriptors) {
        _ComputedPrimvarSourceMap nextSources;
        for (size_t interpolationIndex = 0;
             interpolationIndex < HdInterpolationCount;
             ++interpolationIndex) {
            HdInterpolation const interpolation =
                static_cast<HdInterpolation>(interpolationIndex);
            HdExtComputationPrimvarDescriptorVector const descriptors =
                sceneDelegate->GetExtComputationPrimvarDescriptors(
                    id, interpolation);
            for (HdExtComputationPrimvarDescriptor descriptor : descriptors) {
                descriptor.interpolation = interpolation;
                _ComputedPrimvarSourceMap::const_iterator const previousIt =
                    computedSources.find(descriptor.name);
                bool const descriptorChanged =
                    previousIt == computedSources.end() ||
                    !_DescriptorsExactlyEqual(
                        previousIt->second.descriptor, descriptor);

                _ComputedPrimvarSource source;
                if (previousIt != computedSources.end()) {
                    source = previousIt->second;
                }
                source.descriptor = descriptor;

                bool const pullValue = descriptorChanged ||
                    (rawBits &
                        HdChangeTracker::DirtyComputationPrimvarDesc) != 0 ||
                    _IsComputedSourceValueDirty(
                        rawBits, id, descriptor.name);
                if (pullValue) {
                    descriptorsToCompute.push_back(descriptor);
                    computedChanges.insert(descriptor.name);
                }
                nextSources[descriptor.name] = std::move(source);
            }
        }

        for (_ComputedPrimvarSourceMap::value_type const& oldSource :
                computedSources) {
            if (nextSources.find(oldSource.first) == nextSources.end()) {
                computedChanges.insert(oldSource.first);
            }
        }
        computedSources = std::move(nextSources);
    } else {
        for (_ComputedPrimvarSourceMap::value_type const& sourceEntry :
                computedSources) {
            if (_IsComputedSourceValueDirty(
                    rawBits, id, sourceEntry.first)) {
                descriptorsToCompute.push_back(
                    sourceEntry.second.descriptor);
                computedChanges.insert(sourceEntry.first);
            }
        }
    }

    if (!descriptorsToCompute.empty()) {
        HdExtComputationUtils::ValueStore const values =
            HdExtComputationUtils::GetComputedPrimvarValues(
                descriptorsToCompute, sceneDelegate);
        for (HdExtComputationPrimvarDescriptor const& descriptor :
                descriptorsToCompute) {
            _ComputedPrimvarSourceMap::iterator const sourceIt =
                computedSources.find(descriptor.name);
            if (sourceIt == computedSources.end()) {
                continue;
            }
            HdExtComputationUtils::ValueStore::const_iterator const valueIt =
                values.find(descriptor.name);
            sourceIt->second.value = valueIt == values.end()
                ? VtValue()
                : valueIt->second;
            // Descriptor presence owns precedence even when computation
            // execution produced an empty/invalid value.
            sourceIt->second.valueInitialized = true;
        }
    }

    for (TfToken const& name : computedChanges) {
        effectiveChanges->insert(name);
    }
    for (TfToken const& name : authoredChanges) {
        if (computedSources.find(name) == computedSources.end()) {
            effectiveChanges->insert(name);
        }
    }
}

_PrimvarSourceView
HdEmbreeBasisCurves::_Impl::FindSource(TfToken const& name) const
{
    return _FindEffectiveSource(authoredSources, computedSources, name);
}

_PrimvarSourceView
HdEmbreeBasisCurves::_Impl::FindPointsSource() const
{
    _PrimvarSourceView const describedSource = FindSource(HdTokens->points);
    if (describedSource) {
        return describedSource;
    }
    if (!directPointsInitialized) {
        return _PrimvarSourceView();
    }

    _PrimvarSourceView result;
    result.value = &directPoints;
    result.interpolation = HdInterpolationVertex;
    return result;
}

ty::CurveGeometryInput
HdEmbreeBasisCurves::_Impl::MakeGeometryInput() const
{
    ty::CurveGeometryInput result;
    _PrimvarSourceView const points = FindPointsSource();
    if (points) {
        result.points = *points.value;
    }
    result.primvarWidths = _MakeCurvePrimvarInput(
        FindSource(_tokensPrimvarsWidths));
    result.builtInWidths = _MakeCurvePrimvarInput(
        FindSource(HdTokens->widths));
    result.primvarNormals = _MakeCurvePrimvarInput(
        FindSource(_tokensPrimvarsNormals));
    result.builtInNormals = _MakeCurvePrimvarInput(
        FindSource(HdTokens->normals));
    if (minimumWidth) {
        result.minimumWidth = *minimumWidth;
    }
    return result;
}

void
HdEmbreeBasisCurves::_Impl::EnsurePrototypeScene(RTCDevice device)
{
    if (prototypeScene) {
        return;
    }

    prototypeScene = rtcNewScene(device);
    if (!prototypeScene) {
        return;
    }
    rtcSetSceneFlags(
        prototypeScene,
        static_cast<RTCSceneFlags>(
            RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST));
    rtcSetSceneBuildQuality(prototypeScene, RTC_BUILD_QUALITY_LOW);
}

void
HdEmbreeBasisCurves::_Impl::ClearGeomPropObservers()
{
    for (std::unique_ptr<_CurveGeometryRecord> const& record : records) {
        ty::PrototypeContext& context = record->GetData().GetContext();
        context.geomPropSamplers.clear();
        context.geomPropUniformValues.clear();
    }
}

void
HdEmbreeBasisCurves::_Impl::PopulateContext(
    ty::PrototypeContext* context,
    ty::MaterialEvalServices const* services) const
{
    if (!context) {
        return;
    }
    context->primId = owner->GetPrimId();
    context->cullStyle = cullStyle;
    context->doubleSided = doubleSided;
    context->refined = false;
    context->wireframeMode = ty::WireframeMode::disabled;
    context->materialEvalServices = services;
    context->material = material;
}

void
HdEmbreeBasisCurves::_Impl::RebuildSamplers(
    ty::CurveGeometryRecordData* data,
    bool reportDiagnostics) const
{
    if (!data) {
        return;
    }

    ty::PrototypeContext& context = data->GetContext();
    context.primvarMap.clear();

    _TokenSet nameSet;
    for (_AuthoredPrimvarSourceMap::value_type const& source :
            authoredSources) {
        nameSet.insert(source.first);
    }
    for (_ComputedPrimvarSourceMap::value_type const& source :
            computedSources) {
        nameSet.insert(source.first);
    }
    if (directPointsInitialized) {
        nameSet.insert(HdTokens->points);
    }
    if (FindSource(_tokensPrimvarsWidths) ||
        FindSource(HdTokens->widths)) {
        nameSet.insert(HdTokens->widths);
    }
    if (FindSource(_tokensPrimvarsNormals) ||
        FindSource(HdTokens->normals)) {
        nameSet.insert(HdTokens->normals);
    }

    std::vector<TfToken> names(nameSet.begin(), nameSet.end());
    std::sort(
        names.begin(), names.end(),
        [](TfToken const& lhs, TfToken const& rhs) {
            return lhs.GetString() < rhs.GetString();
        });

    std::vector<_RecoveryDiagnosticItem> samplerDiagnostics;
    for (TfToken const& name : names) {
        _PrimvarSourceView source;
        if (name == HdTokens->points) {
            source = FindPointsSource();
        } else if (name == HdTokens->widths) {
            source = FindSource(_tokensPrimvarsWidths);
            if (!source) {
                source = FindSource(HdTokens->widths);
            }
        } else if (name == HdTokens->normals) {
            source = FindSource(_tokensPrimvarsNormals);
            if (!source) {
                source = FindSource(HdTokens->normals);
            }
        } else {
            source = FindSource(name);
        }
        if (!source || source.interpolation == HdInterpolationInstance) {
            continue;
        }

        // Constant string-like geomprops are served from the material binding
        // table rather than from a numeric PrimvarSampler.
        std::string stringValue;
        if (source.interpolation == HdInterpolationConstant &&
            _GetConstantStringValue(*source.value, &stringValue)) {
            continue;
        }

        ty::CurvePrimvarSamplerResult samplerResult =
            ty::CreateCurvePrimvarSampler(
                name,
                _MakeCurvePrimvarInput(source),
                canonicalTopology,
                context.curvePrimitiveMetadata);
        if (samplerResult.IsValid()) {
            context.primvarMap[name] =
                std::move(samplerResult.sampler);
            continue;
        }

        if (reportDiagnostics && !_IsGeometrySourceName(name)) {
            std::string const cause = name.GetString() + ":" +
                ty::GetCurveSamplerErrorName(
                    samplerResult.diagnostic.error);
            samplerDiagnostics.push_back(
                {cause,
                 _DiagnosticCount(
                     samplerResult.diagnostic.affectedCount),
                 "omitSampler"});
        }
    }
    _ReportRecoveryGroup(
        owner->GetId(), "primvar", samplerDiagnostics);
}

void
HdEmbreeBasisCurves::_Impl::RebuildSamplers(bool reportDiagnostics)
{
    // geomPropSamplers observes primvarMap entries. Invalidate every observer
    // before erasing any owner, then rebuild bindings only after every record
    // has finished its sampler mutation.
    ClearGeomPropObservers();
    bool emitForRecord = reportDiagnostics;
    for (std::unique_ptr<_CurveGeometryRecord> const& record : records) {
        RebuildSamplers(&record->GetData(), emitForRecord);
        emitForRecord = false;
    }
    ResolveGeomPropBindings();
}

void
HdEmbreeBasisCurves::_Impl::ResolveGeomPropBindings()
{
    for (std::unique_ptr<_CurveGeometryRecord> const& record : records) {
        ty::PrototypeContext& context = record->GetData().GetContext();
        if (!context.material) {
            context.geomPropSamplers.clear();
            context.geomPropUniformValues.clear();
            continue;
        }

        std::vector<TfToken> const& tokens =
            context.material->geomPropTokens;
        context.geomPropSamplers.assign(tokens.size(), nullptr);
        context.geomPropUniformValues.assign(tokens.size(), mxcpp::Value());
        for (size_t handle = 0; handle < tokens.size(); ++handle) {
            TfToken const& name = tokens[handle];
            std::unordered_map<
                TfToken,
                std::unique_ptr<ty::PrimvarSampler>,
                TfToken::HashFunctor>::const_iterator const samplerIt =
                    context.primvarMap.find(name);
            if (samplerIt != context.primvarMap.end()) {
                context.geomPropSamplers[handle] = samplerIt->second.get();
            }

            _PrimvarSourceView source;
            if (name == HdTokens->points) {
                source = FindPointsSource();
            } else if (name == HdTokens->widths) {
                source = FindSource(_tokensPrimvarsWidths);
                if (!source) {
                    source = FindSource(HdTokens->widths);
                }
            } else if (name == HdTokens->normals) {
                source = FindSource(_tokensPrimvarsNormals);
                if (!source) {
                    source = FindSource(HdTokens->normals);
                }
            } else {
                source = FindSource(name);
            }
            if (!source ||
                source.interpolation != HdInterpolationConstant) {
                continue;
            }

            std::string value;
            if (_GetConstantStringValue(*source.value, &value)) {
                context.geomPropUniformValues[handle] =
                    mxcpp::Value(std::move(value));
            }
        }
    }
}

void
HdEmbreeBasisCurves::_Impl::ReportTopologyRecoveries() const
{
    size_t const invalidIdCount =
        canonicalTopology.recovery.ignoredInvisibleCurveCount +
        canonicalTopology.recovery.ignoredInvisiblePointCount;
    _ReportRecovery(
        owner->GetId(), "topology", "invalidTopologicalVisibilityId",
        invalidIdCount,
        "ignoreIds");
}

void
HdEmbreeBasisCurves::_Impl::ReportGeometryRecoveries(
    ty::CurveGeometryResult const& geometry) const
{
    ty::CurveGeometryRecoverySummary const& recovery = geometry.recovery;
    _ReportRecoveryGroup(
        owner->GetId(), "minimumWidth",
        {{"negativeValue", recovery.negativeMinimumWidthCount,
          "clampToZero"},
         {"nonFiniteValue", recovery.nonFiniteMinimumWidthCount,
          "clampToZero"}});
    _ReportRecoveryGroup(
        owner->GetId(), "width",
        {{"missingSource", recovery.missingWidthFallbackCount,
          "minimumWidth"},
         {"invalidSource", recovery.invalidWidthFallbackCount,
          "minimumWidth"},
         {"negativeSample", recovery.negativeWidthCount,
          "clampToZero"},
         {"nonFiniteSample", recovery.nonFiniteWidthCount,
          "minimumWidth"},
         {"allEffectiveWidthsZero",
          recovery.allEffectiveWidthsZeroCount,
          "emptyPrototype"}});
    _ReportRecoveryGroup(
        owner->GetId(), "normal",
        {{"invalidSource", recovery.invalidNormalSourceCount, "tube"},
         {"invalidSample", recovery.invalidNormalSampleCount,
          "repairOrTube"},
         {"localFrameIssue", recovery.localNormalIssueSpanCount,
          "repairOrTube"},
         {"repairedSpan", recovery.repairedNormalSpanCount,
          "transportedFrame"},
         {"tubeFallbackSpan", recovery.tubeFallbackSpanCount, "tube"}});
    _ReportRecoveryGroup(
        owner->GetId(), "centerline",
        {{"zeroTangentSpan", recovery.linearizedSpanCount,
          "roundLinear"},
         {"zeroLengthPiece", recovery.removedZeroLengthSegmentCount,
          "omitPiece"},
         {"collapsedSpan", recovery.spherePointSpanCount,
          "spherePoint"}});
}

bool
HdEmbreeBasisCurves::_Impl::RebuildGeometry(
    RTCDevice device,
    ty::MaterialEvalServices const* services)
{
    EnsurePrototypeScene(device);
    ++geometryGeneration;
    geometryInitialized = true;
    appliedMinimumWidth = minimumWidth;
    appliedMinimumWidthEpoch = minimumWidthEpoch;
    if (!prototypeScene) {
        _ReportFatal(owner->GetId(), "embree", "prototypeSceneAllocation", 1);
        records.clear();
        return false;
    }

    if (!minimumWidth) {
        ClearGeomPropObservers();
        records.clear();
        rtcCommitScene(prototypeScene);
        _ReportFatal(owner->GetId(), "minimumWidth", "notInjected", 1);
        return false;
    }

    if (!topologyInitialized) {
        ClearGeomPropObservers();
        records.clear();
        rtcCommitScene(prototypeScene);
        _ReportFatal(owner->GetId(), "topology", "notPulled", 1);
        return false;
    }

    _PrimvarSourceView const points = FindPointsSource();
    size_t physicalPointCount = topology.GetNumPoints();
    if (points && points.value->IsArrayValued()) {
        physicalPointCount = points.value->GetArraySize();
    }

    ty::CurveTopologyInput topologyInput;
    topologyInput.curveType = topology.GetCurveType();
    topologyInput.curveBasis = topology.GetCurveBasis();
    topologyInput.curveWrap = topology.GetCurveWrap();
    topologyInput.curveVertexCounts = topology.GetCurveVertexCounts();
    topologyInput.curveIndices = topology.GetCurveIndices();
    topologyInput.invisibleCurves = topology.GetInvisibleCurves();
    topologyInput.invisiblePoints = topology.GetInvisiblePoints();
    topologyInput.physicalPointCount = physicalPointCount;
    canonicalTopology = ty::CanonicalizeCurveTopology(topologyInput);
    if (!canonicalTopology.IsValid()) {
        ClearGeomPropObservers();
        records.clear();
        rtcCommitScene(prototypeScene);
        _ReportFatal(
            owner->GetId(), "topology",
            ty::GetCurveTopologyErrorName(
                canonicalTopology.diagnostic.error),
            canonicalTopology.diagnostic.affectedCount);
        return false;
    }
    ReportTopologyRecoveries();

    ty::CurveGeometryResult const geometry =
        ty::BuildCurveGeometry(canonicalTopology, MakeGeometryInput());
    if (!geometry.IsValid()) {
        ClearGeomPropObservers();
        records.clear();
        rtcCommitScene(prototypeScene);
        _ReportFatal(
            owner->GetId(), "geometry",
            ty::GetCurveGeometryErrorName(geometry.diagnostic.error),
            geometry.diagnostic.affectedCount);
        return false;
    }
    ReportGeometryRecoveries(geometry);

    ty::CurveGeometryRecordBuildResult buildResult =
        ty::BuildCurveGeometryRecords(canonicalTopology, geometry);
    if (!buildResult.IsValid()) {
        ClearGeomPropObservers();
        records.clear();
        rtcCommitScene(prototypeScene);
        _ReportFatal(
            owner->GetId(), "record",
            ty::GetCurveRecordErrorName(buildResult.diagnostic.error), 1);
        return false;
    }

    std::vector<std::unique_ptr<_CurveGeometryRecord>> newRecords;
    newRecords.reserve(buildResult.records.size());
    bool reportSamplerDiagnostics = true;
    for (std::unique_ptr<ty::CurveGeometryRecordData>& data :
            buildResult.records) {
        std::unique_ptr<_CurveGeometryRecord> record =
            _CurveGeometryRecord::Create(device, std::move(data));
        if (!record) {
            newRecords.clear();
            ClearGeomPropObservers();
            records.clear();
            rtcCommitScene(prototypeScene);
            _ReportFatal(
                owner->GetId(), "embree", "curveGeometryAllocation", 1);
            return false;
        }
        PopulateContext(
            &record->GetData().GetContext(), services);
        RebuildSamplers(
            &record->GetData(), reportSamplerDiagnostics);
        reportSamplerDiagnostics = false;
        record->Commit();
        newRecords.push_back(std::move(record));
    }

    // New records are fully populated and committed before any attachment.
    // Remove every old record first so a failed replacement can only publish
    // a committed empty prototype, never stale geometry.
    ClearGeomPropObservers();
    records.clear();
    bool attachmentFailed = false;
    for (std::unique_ptr<_CurveGeometryRecord> const& record : newRecords) {
        if (!record->Attach(prototypeScene)) {
            attachmentFailed = true;
            break;
        }
    }
    if (attachmentFailed) {
        newRecords.clear();
        rtcCommitScene(prototypeScene);
        _ReportFatal(
            owner->GetId(), "embree", "curveGeometryAttachment", 1);
        return false;
    }
    records = std::move(newRecords);
    ResolveGeomPropBindings();
    rtcCommitScene(prototypeScene);
    return true;
}

void
HdEmbreeBasisCurves::_Impl::UpdateInstances(
    HdSceneDelegate* sceneDelegate,
    RTCScene rootScene,
    RTCDevice device,
    bool visible)
{
    if (!prototypeScene) {
        // RebuildGeometry already reported the prototype allocation failure.
        // Keep instance initialization pending so a later dirty retry can
        // publish instances after the prototype scene becomes available.
        instances.clear();
        instancesInitialized = false;
        return;
    }

    std::vector<HdEmbreeInstanceData> instanceData;
    if (owner->GetInstancerId().IsEmpty()) {
        instanceData.emplace_back();
        instanceData.back().categories = categories;
    } else {
        HdInstancer* const baseInstancer =
            sceneDelegate->GetRenderIndex().GetInstancer(
                owner->GetInstancerId());
        HdEmbreeInstancer* const instancer =
            dynamic_cast<HdEmbreeInstancer*>(baseInstancer);
        if (!instancer) {
            _ReportFatal(
                owner->GetId(), "instancer", "missingEmbreeInstancer", 1);
            instances.clear();
            instancesInitialized = false;
            return;
        }
        instanceData = instancer->ComputeInstanceData(owner->GetId());
        for (HdEmbreeInstanceData& instance : instanceData) {
            ty::MergeCategories(categories, &instance.categories);
        }
    }

    if (instanceData.size() < instances.size()) {
        instances.resize(instanceData.size());
    }

    for (size_t instanceIndex = 0;
         instanceIndex < instanceData.size();
         ++instanceIndex) {
        HdEmbreeInstanceData const& source = instanceData[instanceIndex];
        GfMatrix4f const objectToWorld =
            transform * GfMatrix4f(source.transform);
        int32_t const instanceId = static_cast<int32_t>(instanceIndex);

        if (instanceIndex < instances.size()) {
            instances[instanceIndex]->Update(
                objectToWorld, source.categories, instanceId, visible);
            continue;
        }

        std::unique_ptr<_CurveInstance> instance =
            _CurveInstance::Create(
                device, rootScene, prototypeScene);
        if (!instance || !instance->Publish(
                objectToWorld, source.categories, instanceId, visible)) {
            instance.reset();
            instances.clear();
            _ReportFatal(
                owner->GetId(), "embree", "curveInstanceAllocation", 1);
            instancesInitialized = false;
            return;
        }
        instances.push_back(std::move(instance));
    }
    instancesInitialized = true;
}

void
HdEmbreeBasisCurves::_Impl::CommitInstances()
{
    for (std::unique_ptr<_CurveInstance> const& instance : instances) {
        instance->Commit();
    }
}

HdEmbreeBasisCurves::HdEmbreeBasisCurves(SdfPath const& id)
    : HdBasisCurves(id)
    , _impl(std::make_unique<_Impl>(this))
{
}

HdEmbreeBasisCurves::~HdEmbreeBasisCurves() = default;

HdDirtyBits
HdEmbreeBasisCurves::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr |
        HdChangeTracker::DirtyRepr |
        HdChangeTracker::DirtyPoints |
        HdChangeTracker::DirtyTopology |
        HdChangeTracker::DirtyWidths |
        HdChangeTracker::DirtyNormals |
        HdChangeTracker::DirtyPrimvar |
        HdChangeTracker::DirtyComputationPrimvarDesc |
        HdChangeTracker::DirtyMaterialId |
        HdChangeTracker::DirtyTransform |
        HdChangeTracker::DirtyVisibility |
        HdChangeTracker::DirtyCullStyle |
        HdChangeTracker::DirtyDoubleSided |
        HdChangeTracker::DirtyDisplayStyle |
        HdChangeTracker::DirtyInstancer |
        HdChangeTracker::DirtyInstanceIndex |
        HdChangeTracker::DirtyCategories |
        HdChangeTracker::DirtyPrimID;
}

void
HdEmbreeBasisCurves::_InitRepr(
    TfToken const& reprToken,
    HdDirtyBits* dirtyBits)
{
    _ReprVector::iterator const reprIt = std::find_if(
        _reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
    if (reprIt == _reprs.end()) {
        _reprs.emplace_back(reprToken, HdReprSharedPtr());
    }

    if (_impl->lastRequestedReprToken != reprToken) {
        _impl->lastRequestedReprToken = reprToken;
        *dirtyBits |= HdChangeTracker::NewRepr;
    }
}

HdDirtyBits
HdEmbreeBasisCurves::_PropagateDirtyBits(HdDirtyBits bits) const
{
    // HdRprim propagates topology to points/normals/general primvars. Widths
    // have a topology-dependent domain too and need the same invalidation,
    // while Sync still uses the pre-propagation tracker snapshot to avoid
    // pulling their clean authored data.
    if ((bits & HdChangeTracker::DirtyTopology) != 0) {
        bits |= HdChangeTracker::DirtyWidths;
    }
    return bits;
}

void
HdEmbreeBasisCurves::SetMinimumWidth(
    float minimumWidth,
    std::uint64_t epoch)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->minimumWidth = minimumWidth;
    _impl->minimumWidthEpoch = epoch;
}

bool
HdEmbreeBasisCurves::RebuildForMinimumWidth(
    float minimumWidth,
    std::uint64_t epoch,
    HdEmbreeRenderParam* renderParam)
{
    if (!renderParam) {
        TF_CODING_ERROR(
            "HdEmbreeBasisCurves::RebuildForMinimumWidth received a null "
            "render param for <%s>",
            GetId().GetText());
        return false;
    }

    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->minimumWidth = minimumWidth;
    _impl->minimumWidthEpoch = epoch;

    const bool pendingMinimumWidthChange =
        !_OptionalFloatsEqual(
            _impl->minimumWidth, _impl->appliedMinimumWidth) ||
        _impl->minimumWidthEpoch != _impl->appliedMinimumWidthEpoch;
    if (!_impl->geometryInitialized || !pendingMinimumWidthChange) {
        return false;
    }

    _impl->RebuildGeometry(
        renderParam->GetEmbreeDevice(),
        renderParam->GetMaterialEvalServices());
    _impl->CommitInstances();
    return true;
}

void
HdEmbreeBasisCurves::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits,
    TfToken const& reprToken)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    if (!sceneDelegate || !renderParam || !dirtyBits) {
        TF_CODING_ERROR(
            "HdEmbreeBasisCurves::Sync received a null argument for <%s>",
            GetId().GetText());
        return;
    }

    std::lock_guard<std::mutex> lock(_impl->mutex);

    HdDirtyBits const rawBits =
        _impl->GetRawDirtyBits(sceneDelegate, *dirtyBits);
    HdDirtyBits const propagatedBits = *dirtyBits;
    constexpr HdDirtyBits meaningfulBits =
        HdChangeTracker::AllSceneDirtyBits | HdChangeTracker::NewRepr;
    bool const pendingMinimumWidthChange =
        !_OptionalFloatsEqual(
            _impl->minimumWidth, _impl->appliedMinimumWidth) ||
        _impl->minimumWidthEpoch != _impl->appliedMinimumWidthEpoch;
    if ((rawBits & meaningfulBits) == HdChangeTracker::Clean &&
        !pendingMinimumWidthChange) {
        *dirtyBits &= ~(HdChangeTracker::AllSceneDirtyBits |
                        HdChangeTracker::NewRepr);
        return;
    }

    HdEmbreeRenderParam* const embreeRenderParam =
        static_cast<HdEmbreeRenderParam*>(renderParam);
    // This is the synchronization boundary for every context, sampler, scene,
    // geometry, and registry-visible mutation below.
    RTCScene const rootScene = embreeRenderParam->AcquireSceneForEdit();
    RTCDevice const device = embreeRenderParam->GetEmbreeDevice();
    SdfPath const& id = GetId();

    // Querying the descriptor establishes the supported policy. Patch is the
    // native surface mode; wire and points deliberately use the exact same
    // curve surface records until dedicated representations exist.
    _BasisCurvesReprConfig::DescArray const reprDescs =
        _GetReprDesc(reprToken);
    (void)reprDescs;

    bool const topologyDirty =
        (propagatedBits & HdChangeTracker::DirtyTopology) != 0;
    if (topologyDirty) {
        _impl->topology = GetBasisCurvesTopology(sceneDelegate);
        _impl->topologyInitialized = true;
    }

    _TokenSet effectiveSourceChanges;
    _impl->RefreshSourceCaches(
        sceneDelegate, rawBits, &effectiveSourceChanges);

    if ((rawBits & HdChangeTracker::DirtyTransform) != 0) {
        _impl->transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }
    if ((rawBits & HdChangeTracker::DirtyVisibility) != 0) {
        HdDirtyBits visibilityBits = rawBits;
        _UpdateVisibility(sceneDelegate, &visibilityBits);
    }
    if ((rawBits & HdChangeTracker::DirtyCullStyle) != 0) {
        _impl->cullStyle = sceneDelegate->GetCullStyle(id);
    }
    if ((rawBits & HdChangeTracker::DirtyDoubleSided) != 0) {
        _impl->doubleSided = sceneDelegate->GetDoubleSided(id);
    }
    if ((rawBits & HdChangeTracker::DirtyCategories) != 0) {
        _impl->categories = sceneDelegate->GetCategories(id);
    }

    bool const materialDirty =
        (rawBits & HdChangeTracker::DirtyMaterialId) != 0;
    if (materialDirty) {
        SdfPath const materialId = sceneDelegate->GetMaterialId(id);
        SetMaterialId(materialId);
        HdEmbreeMaterial* material = nullptr;
        if (!materialId.IsEmpty()) {
            HdSprim* const sprim =
                sceneDelegate->GetRenderIndex().GetSprim(
                    HdPrimTypeTokens->material, materialId);
            material = dynamic_cast<HdEmbreeMaterial*>(sprim);
        }
        _impl->material = material
            ? material->GetRenderMaterial()
            : nullptr;
    }

    bool geometrySourceDirty = false;
    for (TfToken const& name : effectiveSourceChanges) {
        if (_IsGeometrySourceName(name)) {
            geometrySourceDirty = true;
            break;
        }
    }
    bool const rebuildGeometry =
        !_impl->geometryInitialized || topologyDirty ||
        geometrySourceDirty || pendingMinimumWidthChange;

    if (rebuildGeometry) {
        _impl->RebuildGeometry(
            device, embreeRenderParam->GetMaterialEvalServices());
    } else if (!effectiveSourceChanges.empty()) {
        _impl->RebuildSamplers(true);
    }

    bool const contextDirty = rebuildGeometry || materialDirty ||
        (rawBits & (HdChangeTracker::DirtyCullStyle |
                    HdChangeTracker::DirtyDoubleSided |
                    HdChangeTracker::DirtyPrimID)) != 0;
    if (contextDirty) {
        for (std::unique_ptr<_CurveGeometryRecord> const& record :
                _impl->records) {
            _impl->PopulateContext(
                &record->GetData().GetContext(),
                embreeRenderParam->GetMaterialEvalServices());
        }
    }
    if (materialDirty || contextDirty || !effectiveSourceChanges.empty()) {
        _impl->ResolveGeomPropBindings();
    }

    HdDirtyBits instancerBits = rawBits;
    _UpdateInstancer(sceneDelegate, &instancerBits);
    HdInstancer::_SyncInstancerAndParents(
        sceneDelegate->GetRenderIndex(), GetInstancerId());

    bool const instancesDirty = !_impl->instancesInitialized ||
        (rawBits & (HdChangeTracker::DirtyInstancer |
                    HdChangeTracker::DirtyInstanceIndex |
                    HdChangeTracker::DirtyTransform |
                    HdChangeTracker::DirtyVisibility |
                    HdChangeTracker::DirtyCategories)) != 0;
    if (instancesDirty) {
        _impl->UpdateInstances(
            sceneDelegate, rootScene, device, IsVisible());
    } else if (rebuildGeometry) {
        // The prototype scene was recommitted, so every referencing instance
        // must be recommitted before the deferred root-scene commit.
        _impl->CommitInstances();
    }

    *dirtyBits &= ~(HdChangeTracker::AllSceneDirtyBits |
                    HdChangeTracker::NewRepr);
}

void
HdEmbreeBasisCurves::Finalize(HdRenderParam* renderParam)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (renderParam) {
        static_cast<HdEmbreeRenderParam*>(renderParam)->AcquireSceneForEdit();
    }
    _impl->ResetEmbreeState();
}

void
HdEmbreeBasisCurves::RefreshMaterialBindings()
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->ResolveGeomPropBindings();
}

size_t
HdEmbreeBasisCurves::GetCurveGeometryRecordCount() const noexcept
{
    return _impl->records.size();
}

size_t
HdEmbreeBasisCurves::GetInstanceCount() const noexcept
{
    return _impl->instances.size();
}

std::uint64_t
HdEmbreeBasisCurves::GetGeometryGeneration() const noexcept
{
    return _impl->geometryGeneration;
}

float
HdEmbreeBasisCurves::GetCurveGeometryRadius(
    size_t recordIndex,
    size_t vertexIndex) const noexcept
{
    if (recordIndex >= _impl->records.size()) {
        return -1.0f;
    }
    std::vector<GfVec4f> const& vertices =
        _impl->records[recordIndex]->GetData().GetVertices();
    return vertexIndex < vertices.size() ? vertices[vertexIndex][3] : -1.0f;
}

ty::PrototypeContext const*
HdEmbreeBasisCurves::GetPrototypeContext(size_t recordIndex) const noexcept
{
    if (recordIndex >= _impl->records.size()) {
        return nullptr;
    }
    return &_impl->records[recordIndex]->GetData().GetContext();
}

ty::InstanceContext const*
HdEmbreeBasisCurves::GetInstanceContext(size_t instanceIndex) const noexcept
{
    if (instanceIndex >= _impl->instances.size()) {
        return nullptr;
    }
    return _impl->instances[instanceIndex]->GetContext();
}

PXR_NAMESPACE_CLOSE_SCOPE
