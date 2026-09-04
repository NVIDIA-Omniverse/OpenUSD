//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include <delegate/renderDelegate.h>

#include <renderer/embreeCompat.h>
#include <renderer/geometry/curveEmbree.h>
#include <renderer/geometry/curveGeometry.h>
#include <renderer/geometry/curveSamplers.h>
#include <renderer/geometry/curveTopology.h>
#include <renderer/geometry/intersectionFilter.h>
#include <renderer/geometry/normalTransforms.h>
#include <renderer/geometry/surfaceDerivatives.h>
#include <renderer/rendererMath.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/pxr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

#if defined(TYPHOON_HOUDINI_BUILD)
constexpr int _ExpectedEmbreeMajorVersion = 3;
#else
constexpr int _ExpectedEmbreeMajorVersion = 4;
#endif

static_assert(
    RTC_VERSION_MAJOR == _ExpectedEmbreeMajorVersion,
    "Unexpected Embree major version for the curve intersection test");

ty::CurvePrimvarInput
_MakePrimvar(VtValue const& value, TfToken const& interpolation)
{
    ty::CurvePrimvarInput result;
    result.authored = true;
    result.value = value;
    result.interpolation = interpolation;
    return result;
}

struct _CurveBuild
{
    ty::CurveTopologyResult topology;
    ty::CurveGeometryResult geometry;
    ty::CurveGeometryRecordBuildResult records;

    bool IsValid() const
    {
        return topology.IsValid() &&
            geometry.IsValid() &&
            records.IsValid();
    }
};

_CurveBuild
_BuildCurve(
    VtVec3fArray const& points,
    TfToken const& curveType,
    TfToken const& basis,
    TfToken const& wrap,
    VtIntArray const& counts,
    bool ribbon,
    TfToken const& widthInterpolation = HdPrimvarSchemaTokens->constant,
    VtFloatArray const& widths = VtFloatArray{0.4f})
{
    ty::CurveTopologyInput topologyInput;
    topologyInput.curveType = curveType;
    topologyInput.curveBasis = basis;
    topologyInput.curveWrap = wrap;
    topologyInput.curveVertexCounts = counts;
    topologyInput.physicalPointCount = points.size();

    _CurveBuild result;
    result.topology = ty::CanonicalizeCurveTopology(topologyInput);

    ty::CurveGeometryInput geometryInput;
    geometryInput.points = VtValue(points);
    geometryInput.minimumWidth = 0.0f;
    geometryInput.builtInWidths = _MakePrimvar(
        VtValue(widths), widthInterpolation);
    if (ribbon) {
        geometryInput.builtInNormals = _MakePrimvar(
            VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
            HdPrimvarSchemaTokens->constant);
    }
    result.geometry = ty::BuildCurveGeometry(
        result.topology, geometryInput);
    result.records = ty::BuildCurveGeometryRecords(
        result.topology, result.geometry);
    return result;
}

ty::CurveGeometryRecordData*
_GetOnlyRecord(
    _CurveBuild& build,
    ty::CurveGeometryRepresentation representation)
{
    if (!build.IsValid()) {
        return nullptr;
    }
    ty::CurveGeometryRecordData* found = nullptr;
    for (std::unique_ptr<ty::CurveGeometryRecordData> const& record :
         build.records.records) {
        if (record->GetRepresentation() == representation) {
            if (found != nullptr) {
                return nullptr;
            }
            found = record.get();
        }
    }
    return found;
}

struct _Intersection
{
    bool hit = false;
    unsigned int primitiveId = RTC_INVALID_GEOMETRY_ID;
    float u = 0.0f;
    float distance = 0.0f;
    GfVec3f geometricNormal = GfVec3f(0.0f);
};

class _DirectGeometry final
{
public:
    explicit _DirectGeometry(ty::CurveGeometryRecordData& record)
    {
        _device = rtcNewDevice(nullptr);
        if (_device == nullptr) {
            return;
        }
        _scene = rtcNewScene(_device);
        _geometry = rtcNewGeometry(_device, record.GetGeometryType());
        if (_scene == nullptr || _geometry == nullptr ||
            !ty::BindCurveGeometryBuffers(_geometry, record)) {
            return;
        }
        rtcCommitGeometry(_geometry);
        _geometryId = rtcAttachGeometry(_scene, _geometry);
        rtcCommitScene(_scene);
        _valid = _geometryId != RTC_INVALID_GEOMETRY_ID &&
            rtcGetDeviceError(_device) == RTC_ERROR_NONE;
    }

    _DirectGeometry(_DirectGeometry const&) = delete;
    _DirectGeometry& operator=(_DirectGeometry const&) = delete;

    ~_DirectGeometry()
    {
        Close();
    }

    bool IsValid() const
    {
        return _valid;
    }

    RTCScene GetScene() const
    {
        return _scene;
    }

    unsigned int GetGeometryId() const
    {
        return _geometryId;
    }

    _Intersection Intersect(
        GfVec3f const& origin,
        GfVec3f const& direction,
        unsigned int rayId = 0) const
    {
        RTCRayHit rayHit = {};
        rayHit.ray.org_x = origin[0];
        rayHit.ray.org_y = origin[1];
        rayHit.ray.org_z = origin[2];
        rayHit.ray.dir_x = direction[0];
        rayHit.ray.dir_y = direction[1];
        rayHit.ray.dir_z = direction[2];
        rayHit.ray.tnear = 0.0f;
        rayHit.ray.tfar = 100.0f;
        rayHit.ray.mask = std::numeric_limits<unsigned int>::max();
        rayHit.ray.id = rayId;
        rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        rayHit.hit.primID = RTC_INVALID_GEOMETRY_ID;
        for (unsigned int level = 0;
             level < RTC_MAX_INSTANCE_LEVEL_COUNT;
             ++level) {
            rayHit.hit.instID[level] = RTC_INVALID_GEOMETRY_ID;
        }
        ty::Intersect1(_scene, &rayHit);

        _Intersection result;
        result.hit = rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID;
        result.primitiveId = rayHit.hit.primID;
        result.u = rayHit.hit.u;
        result.distance = rayHit.ray.tfar;
        result.geometricNormal = GfVec3f(
            rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);
        return result;
    }

    bool Occluded(
        GfVec3f const& origin,
        GfVec3f const& direction,
        unsigned int rayId = 0) const
    {
        RTCRay ray = {};
        ray.org_x = origin[0];
        ray.org_y = origin[1];
        ray.org_z = origin[2];
        ray.dir_x = direction[0];
        ray.dir_y = direction[1];
        ray.dir_z = direction[2];
        ray.tnear = 0.0f;
        ray.tfar = 100.0f;
        ray.mask = std::numeric_limits<unsigned int>::max();
        ray.id = rayId;
        ty::Occluded1(_scene, &ray);
        return ray.tfar < 0.0f;
    }

    bool Close()
    {
        if (_scene != nullptr &&
            _geometryId != RTC_INVALID_GEOMETRY_ID) {
            rtcDetachGeometry(_scene, _geometryId);
            rtcCommitScene(_scene);
            _geometryId = RTC_INVALID_GEOMETRY_ID;
        }
        if (_geometry != nullptr) {
            rtcReleaseGeometry(_geometry);
            _geometry = nullptr;
        }
        bool valid = true;
        if (_device != nullptr) {
            valid = rtcGetDeviceError(_device) == RTC_ERROR_NONE;
        }
        if (_scene != nullptr) {
            rtcReleaseScene(_scene);
            _scene = nullptr;
        }
        if (_device != nullptr) {
            rtcReleaseDevice(_device);
            _device = nullptr;
        }
        _valid = false;
        return valid;
    }

private:
    RTCDevice _device = nullptr;
    RTCScene _scene = nullptr;
    RTCGeometry _geometry = nullptr;
    unsigned int _geometryId = RTC_INVALID_GEOMETRY_ID;
    bool _valid = false;
};

class _FixedNormalSampler final : public ty::PrimvarSampler
{
public:
    explicit _FixedNormalSampler(GfVec3f const& value)
        : _value(value)
    {
    }

    bool Sample(
        unsigned int,
        float,
        float,
        void* value,
        HdTupleType dataType) const override
    {
        if (!value || dataType.type != HdTypeFloatVec3 ||
            dataType.count != 1) {
            return false;
        }
        *static_cast<GfVec3f*>(value) = _value;
        return true;
    }

private:
    GfVec3f const _value;
};

bool
_Close(float first, float second, float tolerance = 2.0e-3f)
{
    return std::abs(first - second) <= tolerance;
}

bool
TestEmbreeRuntimeVersion()
{
    RTCDevice const device = rtcNewDevice(nullptr);
    if (device == nullptr) {
        std::printf("    failed to create an Embree device\n");
        return false;
    }

    ssize_t const runtimeMajor = rtcGetDeviceProperty(
        device, RTC_DEVICE_PROPERTY_VERSION_MAJOR);
    ssize_t const runtimeMinor = rtcGetDeviceProperty(
        device, RTC_DEVICE_PROPERTY_VERSION_MINOR);
    ssize_t const runtimePatch = rtcGetDeviceProperty(
        device, RTC_DEVICE_PROPERTY_VERSION_PATCH);
    RTCError const error = rtcGetDeviceError(device);
    rtcReleaseDevice(device);

    std::printf(
        "Embree runtime version: %lld.%lld.%lld\n",
        static_cast<long long>(runtimeMajor),
        static_cast<long long>(runtimeMinor),
        static_cast<long long>(runtimePatch));
    if (error != RTC_ERROR_NONE ||
        runtimeMajor != _ExpectedEmbreeMajorVersion) {
        std::printf(
            "    expected Embree runtime major %d, got %lld\n",
            _ExpectedEmbreeMajorVersion,
            static_cast<long long>(runtimeMajor));
        return false;
    }
    return true;
}

bool
TestBasisCurvesRprimAdvertisement()
{
    HdEmbreeRenderDelegate delegate;
    TfTokenVector const& supported = delegate.GetSupportedRprimTypes();
    bool const found = std::find(
        supported.begin(), supported.end(), HdPrimTypeTokens->basisCurves) !=
        supported.end();
    if (!found) {
        std::printf("    basisCurves is not an advertised Rprim type\n");
    }
    return found;
}

bool
TestRoundLinearIntersectionAndOpenEnds()
{
    _CurveBuild build = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{3},
        false);
    ty::CurveGeometryRecordData* const record = _GetOnlyRecord(
        build, ty::CurveGeometryRepresentation::roundLinear);
    if (record == nullptr || record->GetCurveFlags().size() != 2) {
        std::printf("    failed to build the round-linear record\n");
        return false;
    }

    _DirectGeometry direct(*record);
    if (!direct.IsValid()) {
        std::printf("    failed to commit the round-linear geometry\n");
        return false;
    }

    _Intersection const body = direct.Intersect(
        GfVec3f(0.5f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    _Intersection const joint = direct.Intersect(
        GfVec3f(1.0f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    _Intersection const start = direct.Intersect(
        GfVec3f(-0.1f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    _Intersection const bypassStart = direct.Intersect(
        GfVec3f(-0.1f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f),
        ty::FaceCullBypassRayId);
    _Intersection const end = direct.Intersect(
        GfVec3f(2.1f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    if (!body.hit || body.primitiveId != 0 ||
        !_Close(body.u, 0.5f) ||
        body.geometricNormal[2] <= 0.0f ||
        !joint.hit || start.hit || bypassStart.hit || end.hit ||
        !direct.Occluded(
            GfVec3f(0.5f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f)) ||
        direct.Occluded(
            GfVec3f(-0.1f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f)) ||
        direct.Occluded(
            GfVec3f(2.1f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f))) {
        std::printf(
            "    round-linear U/Ng/joint/endpoint contract failed\n");
        return false;
    }

    ty::DecodedCurveHit decoded;
    if (!ty::DecodeCurveHit(
            record->GetContext().curvePrimitiveMetadata,
            body.primitiveId,
            body.u,
            &decoded) ||
        decoded.authoredCurveId != 0 ||
        decoded.authoredSegmentId != 0 ||
        !_Close(decoded.authoredU, 0.5f)) {
        std::printf("    direct hit metadata decode failed\n");
        return false;
    }
    return direct.Close();
}

bool
TestOrientedRibbonNormal()
{
    _CurveBuild build = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{3},
        true);
    ty::CurveGeometryRecordData* const record = _GetOnlyRecord(
        build, ty::CurveGeometryRepresentation::hermite);
    if (record == nullptr ||
        record->GetContext().geometryKind !=
            ty::GeometryKind::orientedRibbon) {
        std::printf("    failed to build the oriented ribbon record\n");
        return false;
    }

    _DirectGeometry direct(*record);
    if (!direct.IsValid()) {
        return false;
    }
    _Intersection const hit = direct.Intersect(
        GfVec3f(0.5f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    float const normalLength = hit.geometricNormal.GetLength();
    if (!hit.hit || !_Close(hit.u, 0.5f, 5.0e-3f) ||
        normalLength <= 0.0f ||
        std::abs(hit.geometricNormal[0] / normalLength) > 1.0e-3f ||
        std::abs(hit.geometricNormal[2] / normalLength) < 0.9f) {
        std::printf(
            "    ribbon surface Ng was not distinct from its X tangent\n");
        return false;
    }
    return direct.Close();
}

bool
TestCurveShadingFrames()
{
    _CurveBuild ribbon = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{3},
        true);
    ty::CurveGeometryRecordData* const ribbonRecord = _GetOnlyRecord(
        ribbon, ty::CurveGeometryRepresentation::hermite);
    if (!ribbonRecord) {
        return false;
    }
    _DirectGeometry directRibbon(*ribbonRecord);
    if (!directRibbon.IsValid()) {
        return false;
    }
    _Intersection const hit = directRibbon.Intersect(
        GfVec3f(0.5f, 0.1f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    if (!hit.hit) {
        return false;
    }

    // A deliberately tangent authored normal would be catastrophic if it
    // replaced Embree's actual ribbon surface Ng.
    ribbonRecord->GetContext().primvarMap[HdTokens->normals] =
        std::make_unique<_FixedNormalSampler>(GfVec3f(1.0f, 0.0f, 0.0f));
    RTCRayHit rayHit = {};
    rayHit.hit.geomID = directRibbon.GetGeometryId();
    rayHit.hit.primID = hit.primitiveId;
    rayHit.hit.u = hit.u;
    rayHit.hit.Ng_x = hit.geometricNormal[0];
    rayHit.hit.Ng_y = hit.geometricNormal[1];
    rayHit.hit.Ng_z = hit.geometricNormal[2];
    rayHit.ray.org_x = 0.5f;
    rayHit.ray.org_y = 0.1f;
    rayHit.ray.org_z = 1.0f;
    rayHit.ray.dir_z = -1.0f;
    rayHit.ray.tfar = hit.distance;
    const GfVec3f resolved = ty::ResolveObjectSpaceNormal(
        &ribbonRecord->GetContext(),
        directRibbon.GetScene(),
        directRibbon.GetGeometryId(),
        rayHit);
    GfVec3f expectedNormal = hit.geometricNormal.GetNormalized();
    GfVec3f resolvedNormal = resolved.GetNormalized();

    GfVec3f dPdu;
    GfVec3f dPdv;
    GfVec3f dndu;
    GfVec3f dndv;
    ty::ComputeCurveSurfaceDerivatives(
        &ribbonRecord->GetContext(),
        directRibbon.GetScene(),
        directRibbon.GetGeometryId(),
        hit.primitiveId,
        hit.u,
        resolvedNormal,
        &dPdu,
        &dPdv,
        &dndu,
        &dndv);
    GfVec3f tangent = dPdu.GetNormalized();
    GfVec3f bitangent = dPdv.GetNormalized();
    ty::InstanceContext instanceContext;
    instanceContext.objectToWorldMatrix = GfMatrix4f(1.0f);
    instanceContext.worldToObjectMatrix = GfMatrix4f(1.0f);
    instanceContext.rootScene = directRibbon.GetScene();
    const GfVec3f objectSurfacePosition =
        ty::ResolveObjectSpaceSurfacePosition(
            &ribbonRecord->GetContext(),
            &instanceContext,
            directRibbon.GetScene(),
            directRibbon.GetGeometryId(),
            rayHit,
            nullptr,
            true);
    const bool ribbonFrameValid =
        ty::IsFinite(resolvedNormal) &&
        ty::IsFinite(tangent) &&
        ty::IsFinite(bitangent) &&
        GfIsClose(resolvedNormal, expectedNormal, 1.0e-5f) &&
        std::abs(GfDot(resolvedNormal, tangent)) < 1.0e-5f &&
        std::abs(GfDot(resolvedNormal, bitangent)) < 1.0e-5f &&
        std::abs(GfDot(tangent, bitangent)) < 1.0e-5f &&
        std::abs(tangent[0]) > 0.99f &&
        GfDot(GfCross(tangent, bitangent), resolvedNormal) > 0.99f &&
        GfIsClose(objectSurfacePosition[1], 0.1f, 1.0e-5f) &&
        dndu == GfVec3f(0.0f) && dndv == GfVec3f(0.0f);
    if (!ribbonFrameValid || !directRibbon.Close()) {
        std::printf("    ribbon shading-frame separation failed\n");
        return false;
    }

    _CurveBuild sphere = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f), GfVec3f(0.0f), GfVec3f(0.0f)},
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{3},
        false);
    ty::CurveGeometryRecordData* const sphereRecord = _GetOnlyRecord(
        sphere, ty::CurveGeometryRepresentation::spherePoint);
    if (!sphereRecord) {
        return false;
    }
    _DirectGeometry directSphere(*sphereRecord);
    if (!directSphere.IsValid()) {
        return false;
    }
    const GfVec3f sphereNormal =
        GfVec3f(0.2f, 0.3f, 0.9f).GetNormalized();
    GfVec3f firstTangent;
    GfVec3f firstBitangent;
    GfVec3f secondTangent;
    GfVec3f secondBitangent;
    ty::ComputeCurveSurfaceDerivatives(
        &sphereRecord->GetContext(),
        directSphere.GetScene(),
        directSphere.GetGeometryId(),
        0,
        0.5f,
        sphereNormal,
        &firstTangent,
        &firstBitangent,
        &dndu,
        &dndv);
    ty::ComputeCurveSurfaceDerivatives(
        &sphereRecord->GetContext(),
        directSphere.GetScene(),
        directSphere.GetGeometryId(),
        0,
        0.5f,
        sphereNormal,
        &secondTangent,
        &secondBitangent,
        &dndu,
        &dndv);
    GfMatrix4f objectToWorld(1.0f);
    objectToWorld.SetScale(GfVec3f(2.0f, 0.5f, 3.0f));
    GfMatrix4f const worldToObject = objectToWorld.GetInverse();
    const GfVec3f worldNormal = ty::TransformNormalToWorld(
        worldToObject, sphereNormal);
    GfVec3f worldTangent = objectToWorld.TransformDir(firstTangent);
    worldTangent -= worldNormal * GfDot(worldNormal, worldTangent);
    GfVec3f worldParameterBitangent =
        objectToWorld.TransformDir(firstBitangent);
    worldParameterBitangent -=
        worldNormal * GfDot(worldNormal, worldParameterBitangent);
    if (!ty::TryNormalizeDirection(worldTangent, &worldTangent) ||
        !ty::TryNormalizeDirection(
            worldParameterBitangent, &worldParameterBitangent)) {
        return false;
    }
    const float worldHandedness =
        GfDot(
            GfCross(worldTangent, worldParameterBitangent),
            worldNormal) < 0.0f
        ? -1.0f
        : 1.0f;
    GfVec3f worldBitangent =
        worldHandedness * GfCross(worldNormal, worldTangent);
    if (!ty::TryNormalizeDirection(worldBitangent, &worldBitangent)) {
        return false;
    }
    const bool sphereFrameValid =
        ty::IsFinite(firstTangent) && ty::IsFinite(firstBitangent) &&
        GfIsClose(firstTangent, secondTangent, 0.0f) &&
        GfIsClose(firstBitangent, secondBitangent, 0.0f) &&
        GfIsClose(firstTangent.GetLength(), 1.0f, 1.0e-6f) &&
        GfIsClose(firstBitangent.GetLength(), 1.0f, 1.0e-6f) &&
        std::abs(GfDot(firstTangent, sphereNormal)) < 1.0e-6f &&
        std::abs(GfDot(firstBitangent, sphereNormal)) < 1.0e-6f &&
        std::abs(GfDot(firstTangent, firstBitangent)) < 1.0e-6f &&
        ty::IsFinite(worldNormal) && ty::IsFinite(worldTangent) &&
        ty::IsFinite(worldBitangent) &&
        std::abs(GfDot(worldNormal, worldTangent)) < 1.0e-6f &&
        std::abs(GfDot(worldNormal, worldBitangent)) < 1.0e-6f &&
        std::abs(GfDot(worldTangent, worldBitangent)) < 1.0e-6f;
    return sphereFrameValid && directSphere.Close();
}

bool
TestCubicAndSphereRepresentations()
{
    VtVec3fArray const cubicPoints = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f / 3.0f, 0.0f, 0.0f),
        GfVec3f(2.0f / 3.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f)};
    _CurveBuild native = _BuildCurve(
        cubicPoints,
        HdTokens->cubic,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{4},
        false);
    ty::CurveGeometryRecordData* const nativeRecord = _GetOnlyRecord(
        native, ty::CurveGeometryRepresentation::nativeBezier);
    if (nativeRecord == nullptr) {
        return false;
    }
    {
        _DirectGeometry direct(*nativeRecord);
        if (!direct.IsValid()) {
            return false;
        }
        _Intersection const hit = direct.Intersect(
            GfVec3f(0.5f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f));
        ty::DecodedCurveHit decoded;
        if (!hit.hit ||
            !_Close(hit.u, 0.5f, 5.0e-3f) ||
            !ty::DecodeCurveHit(
                nativeRecord->GetContext().curvePrimitiveMetadata,
                hit.primitiveId,
                hit.u,
                &decoded) ||
            decoded.authoredSegmentId != 0 ||
            !direct.Close()) {
            std::printf("    native cubic curve did not intersect\n");
            return false;
        }
    }

    _CurveBuild hermite = _BuildCurve(
        cubicPoints,
        HdTokens->cubic,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{4},
        false,
        HdPrimvarSchemaTokens->varying,
        VtFloatArray{0.4f, 0.6f});
    ty::CurveGeometryRecordData* const hermiteRecord = _GetOnlyRecord(
        hermite, ty::CurveGeometryRepresentation::hermite);
    if (hermiteRecord == nullptr ||
        hermiteRecord->GetTangents().size() != 2) {
        return false;
    }
    {
        _DirectGeometry direct(*hermiteRecord);
        if (!direct.IsValid()) {
            return false;
        }
        _Intersection const hit = direct.Intersect(
            GfVec3f(0.5f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f));
        ty::DecodedCurveHit decoded;
        if (!hit.hit ||
            !ty::DecodeCurveHit(
                hermiteRecord->GetContext().curvePrimitiveMetadata,
                hit.primitiveId,
                hit.u,
                &decoded) ||
            decoded.authoredSegmentId != 0 ||
            !direct.Close()) {
            std::printf("    Hermite curve did not intersect\n");
            return false;
        }
    }

    _CurveBuild sphere = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f)},
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{3},
        false);
    ty::CurveGeometryRecordData* const sphereRecord = _GetOnlyRecord(
        sphere, ty::CurveGeometryRepresentation::spherePoint);
    if (sphereRecord == nullptr ||
        ty::IsOpenCurveEndpoint(sphereRecord->GetContext(), 0, 0.0f)) {
        return false;
    }
    _DirectGeometry sphereDirect(*sphereRecord);
    if (!sphereDirect.IsValid()) {
        return false;
    }
    _Intersection const sphereHit = sphereDirect.Intersect(
        GfVec3f(0.0f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    ty::DecodedCurveHit sphereDecoded;
    if (!sphereHit.hit ||
        !ty::DecodeCurveHit(
            sphereRecord->GetContext().curvePrimitiveMetadata,
            sphereHit.primitiveId,
            sphereHit.u,
            &sphereDecoded) ||
        !sphereDirect.Close()) {
        std::printf("    sphere fallback was filtered as an endpoint\n");
        return false;
    }
    return true;
}

bool
TestPeriodicSeamAndNeighborFlags()
{
    _CurveBuild build = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 1.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)},
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->periodic,
        VtIntArray{4},
        false);
    ty::CurveGeometryRecordData* const record = _GetOnlyRecord(
        build, ty::CurveGeometryRepresentation::roundLinear);
    std::uint32_t const both =
        static_cast<std::uint32_t>(RTC_CURVE_FLAG_NEIGHBOR_LEFT) |
        static_cast<std::uint32_t>(RTC_CURVE_FLAG_NEIGHBOR_RIGHT);
    if (record == nullptr || record->GetCurveFlags().size() != 4) {
        return false;
    }
    for (std::uint32_t const flags : record->GetCurveFlags()) {
        if (flags != both) {
            std::printf("    periodic neighbor flags did not close the seam\n");
            return false;
        }
    }
    if (ty::IsOpenCurveEndpoint(record->GetContext(), 0, 0.0f) ||
        ty::IsOpenCurveEndpoint(record->GetContext(), 3, 1.0f)) {
        return false;
    }

    _DirectGeometry direct(*record);
    if (!direct.IsValid()) {
        return false;
    }
    _Intersection const seam = direct.Intersect(
        GfVec3f(0.0f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f));
    if (!seam.hit ||
        !direct.Occluded(
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f))) {
        std::printf("    periodic seam was removed by the endpoint filter\n");
        return false;
    }
    return direct.Close();
}

bool
TestPinnedLinearizedOpenEnds()
{
    _CurveBuild startBuild = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f)},
        HdTokens->cubic,
        HdTokens->bspline,
        HdTokens->pinned,
        VtIntArray{3},
        false);
    ty::CurveGeometryRecordData* const startRecord = _GetOnlyRecord(
        startBuild, ty::CurveGeometryRepresentation::roundLinear);
    if (startRecord == nullptr ||
        !ty::IsOpenCurveEndpoint(startRecord->GetContext(), 0, 0.0f)) {
        std::printf("    pinned start did not retain endpoint metadata\n");
        return false;
    }
    {
        _DirectGeometry direct(*startRecord);
        if (!direct.IsValid() ||
            direct.Intersect(
                GfVec3f(-0.1f, 0.0f, 1.0f),
                GfVec3f(0.0f, 0.0f, -1.0f)).hit ||
            direct.Occluded(
                GfVec3f(-0.1f, 0.0f, 1.0f),
                GfVec3f(0.0f, 0.0f, -1.0f)) ||
            !direct.Close()) {
            std::printf("    pinned start sphere remained closed\n");
            return false;
        }
    }

    _CurveBuild endBuild = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f)},
        HdTokens->cubic,
        HdTokens->bspline,
        HdTokens->pinned,
        VtIntArray{3},
        false);
    ty::CurveGeometryRecordData* const endRecord = _GetOnlyRecord(
        endBuild, ty::CurveGeometryRepresentation::roundLinear);
    if (endRecord == nullptr || endRecord->GetPrimitiveCount() == 0) {
        return false;
    }
    unsigned int const lastPrimitive = static_cast<unsigned int>(
        endRecord->GetPrimitiveCount() - 1);
    if (!ty::IsOpenCurveEndpoint(
            endRecord->GetContext(), lastPrimitive, 1.0f)) {
        std::printf("    pinned end did not retain endpoint metadata\n");
        return false;
    }
    _DirectGeometry direct(*endRecord);
    if (!direct.IsValid() ||
        direct.Intersect(
            GfVec3f(1.1f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f)).hit ||
        direct.Occluded(
            GfVec3f(1.1f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f))) {
        std::printf("    pinned end sphere remained closed\n");
        return false;
    }
    return direct.Close();
}

bool
TestCombinedPacketFilter()
{
    ty::PrototypeContext context;
    context.geometryKind = ty::GeometryKind::roundCurve;
    context.curveRepresentation =
        ty::CurveGeometryRepresentation::roundLinear;
    context.cullStyle = HdCullStyleBack;
    context.curvePrimitiveMetadata.resize(4);
    context.curvePrimitiveMetadata[1].isAuthoredCurveStart = true;

    int valid[4] = {7, -1, -1, -1};
    RTCRayHit4 packet = {};
    for (size_t lane = 0; lane < 4; ++lane) {
        packet.ray.dir_z[lane] = 1.0f;
        packet.hit.Ng_z[lane] = 1.0f;
        packet.hit.primID[lane] = static_cast<unsigned int>(lane);
        packet.hit.u[lane] = 0.5f;
    }
    packet.hit.u[1] = 0.0f;
    packet.hit.u[2] = 0.0f;
    packet.hit.u[3] = 1.0f;
    packet.ray.id[1] = ty::FaceCullBypassRayId;
    packet.ray.id[3] = ty::FaceCullBypassRayId;

    RTCFilterFunctionNArguments arguments = {};
    arguments.valid = valid;
    arguments.geometryUserPtr = &context;
    arguments.ray = reinterpret_cast<RTCRayN*>(&packet.ray);
    arguments.hit = reinterpret_cast<RTCHitN*>(&packet.hit);
    arguments.N = 4;
    ty::PrototypeGeometryFilter(&arguments);
    if (valid[0] != 7 || valid[1] != 0 ||
        valid[2] != 0 || valid[3] != -1) {
        std::printf(
            "    endpoint/culling packet lanes were not independent\n");
        return false;
    }

    context.curveRepresentation =
        ty::CurveGeometryRepresentation::spherePoint;
    if (ty::IsOpenCurveEndpoint(context, 1, 0.0f)) {
        return false;
    }
    context.curveRepresentation =
        ty::CurveGeometryRepresentation::nativeBezier;
    if (ty::IsOpenCurveEndpoint(context, 1, 0.0f)) {
        return false;
    }
    context.geometryKind = ty::GeometryKind::orientedRibbon;
    context.curveRepresentation =
        ty::CurveGeometryRepresentation::roundLinear;
    return !ty::IsOpenCurveEndpoint(context, 1, 0.0f);
}

bool
TestRepeatedOwnershipTeardown()
{
    _CurveBuild build = _BuildCurve(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{3},
        false);
    ty::CurveGeometryRecordData* const record = _GetOnlyRecord(
        build, ty::CurveGeometryRepresentation::roundLinear);
    if (record == nullptr) {
        return false;
    }

    for (size_t iteration = 0; iteration < 32; ++iteration) {
        _DirectGeometry direct(*record);
        if (!direct.IsValid() ||
            !direct.Intersect(
                GfVec3f(0.5f, 0.0f, 1.0f),
                GfVec3f(0.0f, 0.0f, -1.0f)).hit ||
            !direct.Close()) {
            std::printf(
                "    repeated direct geometry teardown failed at %zu\n",
                iteration);
            return false;
        }
    }
    return true;
}

} // anonymous namespace

int
main()
{
    std::printf(
        "Embree compile-time version: %d.%d.%d\n",
        RTC_VERSION_MAJOR,
        RTC_VERSION_MINOR,
        RTC_VERSION_PATCH);

    struct _Test
    {
        char const* name;
        bool (*function)();
    };
    const _Test tests[] = {
        {"CurveIntersections.EmbreeRuntimeVersion",
         &TestEmbreeRuntimeVersion},
        {"CurveIntersections.BasisCurvesRprimAdvertisement",
         &TestBasisCurvesRprimAdvertisement},
        {"CurveIntersections.RoundLinearIntersectionAndOpenEnds",
         &TestRoundLinearIntersectionAndOpenEnds},
        {"CurveIntersections.OrientedRibbonNormal",
         &TestOrientedRibbonNormal},
        {"CurveIntersections.CurveShadingFrames",
         &TestCurveShadingFrames},
        {"CurveIntersections.CubicAndSphereRepresentations",
         &TestCubicAndSphereRepresentations},
        {"CurveIntersections.PeriodicSeamAndNeighborFlags",
         &TestPeriodicSeamAndNeighborFlags},
        {"CurveIntersections.PinnedLinearizedOpenEnds",
         &TestPinnedLinearizedOpenEnds},
        {"CurveIntersections.CombinedPacketFilter",
         &TestCombinedPacketFilter},
        {"CurveIntersections.RepeatedOwnershipTeardown",
         &TestRepeatedOwnershipTeardown},
    };

    int failed = 0;
    for (_Test const& test : tests) {
        std::printf("  [RUN ] %s\n", test.name);
        if (test.function()) {
            std::printf("  [PASS] %s\n", test.name);
        } else {
            std::printf("  [FAIL] %s\n", test.name);
            ++failed;
        }
    }
    if (failed != 0) {
        std::printf("%d/%zu tests failed.\n", failed, std::size(tests));
        return 1;
    }
    std::printf("%zu/%zu tests passed.\n", std::size(tests), std::size(tests));
    return 0;
}
