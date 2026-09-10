//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "mesh.h"
#include "adaptiveSubdivision.h"
#include "displacement.h"
#include "instancer.h"
#include "material.h"
#include "renderParam.h"
#include "renderPass.h"

#include <renderer/geometry/context.h>
#include <renderer/geometry/displacementEvaluation.h>
#include <renderer/geometry/intersectionFilter.h>
#include <renderer/geometry/primvarSampling.h>
#include <renderer/materials/MaterialXCpp/graph.h>

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/vt/typeHeaders.h"
#include "pxr/base/vt/visitValue.h"
#include "pxr/imaging/hd/extComputationUtils.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/pxOsd/tokens.h"
#include "pxr/usd/sdf/assetPath.h"

#include <algorithm> // sort
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <numeric>       // std::iota
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

static const TfToken _tokensTangent("tangent");
static const TfToken _tokensBitangent("bitangent");
static const TfToken _tokensComputedTangent("hdEmbreeComputedTangent");
static const TfToken _tokensComputedBitangent("hdEmbreeComputedBitangent");
static const TfToken _tokensSt("st");

bool
_IsMatrixPrimvarType(HdTupleType tupleType)
{
    return tupleType == ty::TypeHelper::GetTupleType<GfMatrix4f>() ||
           tupleType == ty::TypeHelper::GetTupleType<GfMatrix4d>();
}

const char*
_InterpolationName(HdInterpolation interpolation)
{
    switch (interpolation) {
    case HdInterpolationConstant:
        return "constant";
    case HdInterpolationUniform:
        return "uniform";
    case HdInterpolationVertex:
        return "vertex";
    case HdInterpolationVarying:
        return "varying";
    case HdInterpolationFaceVarying:
        return "faceVarying";
    case HdInterpolationInstance:
        return "instance";
    default:
        return "unknown";
    }
}

struct _FlattenIndexedPrimvar
{
    explicit _FlattenIndexedPrimvar(VtIntArray const& indices)
        : indices(indices)
    {
    }

    template <typename T>
    VtValue operator()(VtArray<T> const& source) const
    {
        VtArray<T> flattened(indices.size());
        bool valid = true;
        for (size_t i = 0; i < indices.size(); ++i) {
            int const index = indices[i];
            if (index < 0 || static_cast<size_t>(index) >= source.size()) {
                valid = false;
                continue;
            }
            flattened[i] = source[index];
        }
        if (!valid) {
            TF_WARN("Invalid indexed primvar data");
        }
        return VtValue(std::move(flattened));
    }

    VtValue operator()(VtValue const& source) const
    {
        TF_WARN("Unsupported indexed primvar type");
        return source;
    }

    VtIntArray const& indices;
};

RTCSubdivisionMode
_GetFaceVaryingSubdivisionMode(TfToken const& rule)
{
    // Embree has fewer face-varying rules than OpenSubdiv. It can represent
    // smooth boundaries, pinned corners, pinned boundaries, and fully linear
    // patches, but cannot distinguish OpenSubdiv's three corner-sharpening
    // variants. Preserve the meaningful classes and use PIN_CORNERS for the
    // closest available behavior for cornersOnly/cornersPlus1/cornersPlus2.
    if (rule == PxOsdOpenSubdivTokens->boundaries) {
        return RTC_SUBDIVISION_MODE_PIN_BOUNDARY;
    }
    if (rule == PxOsdOpenSubdivTokens->all) {
        return RTC_SUBDIVISION_MODE_PIN_ALL;
    }
    if (rule == PxOsdOpenSubdivTokens->cornersOnly ||
        rule == PxOsdOpenSubdivTokens->cornersPlus1 ||
        rule == PxOsdOpenSubdivTokens->cornersPlus2) {
        return RTC_SUBDIVISION_MODE_PIN_CORNERS;
    }
    // "none" (and the legacy empty default) keeps discontinuity boundaries
    // smooth. NO_BOUNDARY would drop boundary patches, not make them smooth.
    return RTC_SUBDIVISION_MODE_SMOOTH_BOUNDARY;
}

void
_WarnUnsupportedMatrixPrimvarOnce(
    TfToken const& name,
    HdInterpolation interpolation)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> warnedKeys;

    const std::string key =
        name.GetString() + ":" + _InterpolationName(interpolation);

    std::lock_guard<std::mutex> lock(mutex);
    if (!warnedKeys.insert(key).second) {
        return;
    }

    TF_WARN(
        "hdEmbree does not support matrix primvar '%s' with %s "
        "interpolation; only constant and uniform are supported",
        name.GetText(),
        _InterpolationName(interpolation));
}

std::string
_AssetPathToString(const SdfAssetPath& assetPath)
{
    const std::string resolvedPath = assetPath.GetResolvedPath();
    return resolvedPath.empty() ? assetPath.GetAssetPath() : resolvedPath;
}

bool
_GetUniformStringPrimvarValue(const VtValue& value, std::string* result)
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
        *result = _AssetPathToString(value.UncheckedGet<SdfAssetPath>());
        return true;
    }

    if (value.IsHolding<VtStringArray>()) {
        VtStringArray const& array = value.UncheckedGet<VtStringArray>();
        if (!array.empty()) {
            *result = array[0];
            return true;
        }
    }
    if (value.IsHolding<VtTokenArray>()) {
        VtTokenArray const& array = value.UncheckedGet<VtTokenArray>();
        if (!array.empty()) {
            *result = array[0].GetString();
            return true;
        }
    }
    if (value.IsHolding<VtArray<SdfAssetPath>>()) {
        VtArray<SdfAssetPath> const& array =
            value.UncheckedGet<VtArray<SdfAssetPath>>();
        if (!array.empty()) {
            *result = _AssetPathToString(array[0]);
            return true;
        }
    }

    return false;
}

float
_DifferenceOfProducts(float a, float b, float c, float d)
{
    return a * b - c * d;
}

template <typename T>
bool
_SampleTriangleCorners(ty::PrimvarSampler const* sampler,
                       unsigned int primID,
                       T* v0, T* v1, T* v2)
{
    if (!sampler) {
        return false;
    }

    if (ty::TriangleVertexSampler const* vertexSampler =
            dynamic_cast<ty::TriangleVertexSampler const*>(sampler)) {
        return vertexSampler->SampleVertices(primID, v0, v1, v2);
    }

    if (ty::TriangleFaceVaryingSampler const* faceVaryingSampler =
            dynamic_cast<ty::TriangleFaceVaryingSampler const*>(sampler)) {
        return faceVaryingSampler->SampleVertices(primID, v0, v1, v2);
    }

    if (!sampler->Sample(primID, 0.0f, 0.0f, v0)) {
        return false;
    }
    *v1 = *v0;
    *v2 = *v0;
    return true;
}

bool
_SampleTriangleCorners(ty::PrimvarSampler const* sampler,
                       unsigned int primID,
                       GfVec2f* v0, GfVec2f* v1, GfVec2f* v2)
{
    if (_SampleTriangleCorners<GfVec2f>(sampler, primID, v0, v1, v2)) {
        return true;
    }

    GfVec3f triSt[3];
    if (!_SampleTriangleCorners<GfVec3f>(
            sampler, primID, &triSt[0], &triSt[1], &triSt[2])) {
        return false;
    }

    *v0 = GfVec2f(triSt[0][0], triSt[0][1]);
    *v1 = GfVec2f(triSt[1][0], triSt[1][1]);
    *v2 = GfVec2f(triSt[2][0], triSt[2][1]);
    return true;
}

GfVec3f
_BuildGeometricFaceNormal(GfVec3f const& pos0,
                          GfVec3f const& pos1,
                          GfVec3f const& pos2)
{
    GfVec3f normal = GfCross(pos1 - pos0, pos2 - pos0);
    if (normal.GetLengthSq() > 1e-18f) {
        normal.Normalize();
        return normal;
    }

    GfVec3f fallback0, fallback1;
    GfBuildOrthonormalFrame(GfVec3f(0.0f, 0.0f, 1.0f), &fallback0, &fallback1);
    return GfCross(fallback0, fallback1).GetNormalized();
}

struct _SmoothingKey
{
    int vertexIndex = -1;
    uint32_t normalBits[3] = {0u, 0u, 0u};

    bool operator==(const _SmoothingKey& other) const
    {
        return vertexIndex == other.vertexIndex &&
               normalBits[0] == other.normalBits[0] &&
               normalBits[1] == other.normalBits[1] &&
               normalBits[2] == other.normalBits[2];
    }
};

struct _SmoothingKeyHash
{
    size_t operator()(const _SmoothingKey& key) const
    {
        size_t h = std::hash<int>()(key.vertexIndex);
        auto combine = [&h](uint32_t value) {
            h ^= std::hash<uint32_t>()(value) + 0x9e3779b9u + (h << 6) + (h >> 2);
        };
        combine(key.normalBits[0]);
        combine(key.normalBits[1]);
        combine(key.normalBits[2]);
        return h;
    }
};

uint32_t
_FloatBits(float value)
{
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value), "Unexpected float size");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

_SmoothingKey
_MakeSmoothingKey(int vertexIndex, GfVec3f const& normal)
{
    _SmoothingKey key;
    key.vertexIndex = vertexIndex;
    key.normalBits[0] = _FloatBits(normal[0]);
    key.normalBits[1] = _FloatBits(normal[1]);
    key.normalBits[2] = _FloatBits(normal[2]);
    return key;
}

bool
_GeomStyleShouldHonorRefineLevel(HdMeshGeomStyle geomStyle)
{
    switch (geomStyle) {
        case HdMeshGeomStyleSurf:
        case HdMeshGeomStyleEdgeOnly:
        case HdMeshGeomStyleEdgeOnSurf:
        case HdMeshGeomStyleHull:
        case HdMeshGeomStyleHullEdgeOnly:
        case HdMeshGeomStyleHullEdgeOnSurf:
        case HdMeshGeomStylePoints:
            return true;

        case HdMeshGeomStyleInvalid:
            return false;
    }

    return false;
}

ty::WireframeMode
_GetWireframeMode(HdMeshGeomStyle geomStyle)
{
    switch (geomStyle) {
        case HdMeshGeomStyleEdgeOnly:
        case HdMeshGeomStyleHullEdgeOnly:
            return ty::WireframeMode::edgeOnly;

        case HdMeshGeomStyleEdgeOnSurf:
        case HdMeshGeomStyleHullEdgeOnSurf:
            return ty::WireframeMode::edgeOnSurface;

        case HdMeshGeomStyleInvalid:
        case HdMeshGeomStyleSurf:
        case HdMeshGeomStyleHull:
        case HdMeshGeomStylePoints:
            return ty::WireframeMode::disabled;
    }

    return ty::WireframeMode::disabled;
}

VtIntArray
_ComputeTriangulatedCornerIds(HdMeshTopology const& topology)
{
    VtIntArray result;

    VtIntArray const& faceVertexCounts = topology.GetFaceVertexCounts();
    VtIntArray const& holeFaces =
        (topology.GetRefineLevel() > 0) ? VtIntArray() : topology.GetHoleIndices();
    const bool flip = (topology.GetOrientation() != HdTokens->rightHanded);

    int numTris = 0;
    for (int faceIndex = 0, holeIndex = 0;
         faceIndex < static_cast<int>(faceVertexCounts.size());
         ++faceIndex) {
        const int numVerts = faceVertexCounts[faceIndex];
        if (numVerts < 3) {
            continue;
        }
        if (holeIndex < static_cast<int>(holeFaces.size()) &&
            holeFaces[holeIndex] == faceIndex) {
            ++holeIndex;
            continue;
        }
        numTris += (numVerts - 2);
    }

    result.reserve(numTris * 3);
    for (int faceIndex = 0, cornerOffset = 0, holeIndex = 0;
         faceIndex < static_cast<int>(faceVertexCounts.size());
         ++faceIndex) {
        const int numVerts = faceVertexCounts[faceIndex];
        if (numVerts < 3) {
            cornerOffset += numVerts;
            continue;
        }
        if (holeIndex < static_cast<int>(holeFaces.size()) &&
            holeFaces[holeIndex] == faceIndex) {
            ++holeIndex;
            cornerOffset += numVerts;
            continue;
        }

        for (int tri = 0; tri < numVerts - 2; ++tri) {
            int corners[3];
            if (flip) {
                corners[0] = cornerOffset;
                corners[1] = cornerOffset + tri + 2;
                corners[2] = cornerOffset + tri + 1;
                if (numVerts > 3) {
                    if (tri == 0) {
                        std::swap(corners[0], corners[1]);
                        std::swap(corners[1], corners[2]);
                    } else if (tri == numVerts - 3) {
                        std::swap(corners[1], corners[2]);
                        std::swap(corners[0], corners[1]);
                    }
                }
            } else {
                corners[0] = cornerOffset;
                corners[1] = cornerOffset + tri + 1;
                corners[2] = cornerOffset + tri + 2;
            }
            result.push_back(corners[0]);
            result.push_back(corners[1]);
            result.push_back(corners[2]);
        }

        cornerOffset += numVerts;
    }

    return result;
}

} // namespace

RTCSubdivisionMode
HdEmbreeGetFaceVaryingSubdivisionMode(TfToken const& rule)
{
    return _GetFaceVaryingSubdivisionMode(rule);
}

void
HdEmbreeDisplacementFunction(
    const RTCDisplacementFunctionNArguments* args)
{
    ty::PrototypeContext* prototypeContext =
        static_cast<ty::PrototypeContext*>(args->geometryUserPtr);
    // A missing terminal means the Embree limit surface is already the
    // desired result. Treat it as a no-op so the same geometry path supports
    // both displaced and ordinary subdivision materials.
    if (!prototypeContext || !prototypeContext->displaced ||
        !prototypeContext->material ||
        !prototypeContext->material->displacementGraph) {
        return;
    }

    for (unsigned int i = 0; i < args->N; ++i) {
        // Embree's C callback boundary must not receive an exception from an
        // allocation failure or a renderer callback that violates its
        // non-throwing contract. A failed lane retains its input position
        // without preventing independent lanes from being evaluated.
        try {
            // Embree supplies position and a normalized base normal but not
            // the tangent frame needed by geometric MaterialX nodes.
            // Interpolate the undisplaced limit-surface derivatives at the
            // same patch location.
            alignas(16) float sampledDu[4] = {};
            alignas(16) float sampledDv[4] = {};
            rtcInterpolate1(
                args->geometry,
                args->primID,
                args->u[i],
                args->v[i],
                RTC_BUFFER_TYPE_VERTEX,
                0,
                nullptr,
                sampledDu,
                sampledDv,
                3);

            const GfVec3f position(
                args->P_x[i], args->P_y[i], args->P_z[i]);
            const GfVec3f normal =
                prototypeContext->orientationSign *
                GfVec3f(args->Ng_x[i], args->Ng_y[i], args->Ng_z[i]);
            const GfVec3f dPdu(
                sampledDu[0], sampledDu[1], sampledDu[2]);
            const GfVec3f dPdv(
                sampledDv[0], sampledDv[1], sampledDv[2]);

            float displacement = 0.0f;
            // The shared evaluator supplies texture/frame/time, transforms,
            // st, and geomprops. Authored failures return false; exceptional
            // backend failures are contained by this C boundary.
            if (!ty::EvaluateDisplacement(
                    prototypeContext,
                    args->primID,
                    args->u[i],
                    args->v[i],
                    position,
                    normal,
                    dPdu,
                    dPdv,
                    &displacement)) {
                continue;
            }

            // The terminal value is a world-space distance along the semantic
            // world normal. Convert that vector back to the prototype's object
            // space before modifying Embree's positions. This preserves both
            // direction and magnitude under non-uniform transforms.
            GfVec3f objectOffset;
            if (!ty::ComputeObjectSpaceDisplacementOffset(
                    prototypeContext, normal, displacement, &objectOffset)) {
                continue;
            }

            const double displacedPosition[3] = {
                static_cast<double>(args->P_x[i]) + objectOffset[0],
                static_cast<double>(args->P_y[i]) + objectOffset[1],
                static_cast<double>(args->P_z[i]) + objectOffset[2]};
            constexpr double maxFloat =
                static_cast<double>(std::numeric_limits<float>::max());
            if (!std::isfinite(displacedPosition[0]) ||
                !std::isfinite(displacedPosition[1]) ||
                !std::isfinite(displacedPosition[2]) ||
                std::abs(displacedPosition[0]) > maxFloat ||
                std::abs(displacedPosition[1]) > maxFloat ||
                std::abs(displacedPosition[2]) > maxFloat) {
                continue;
            }
            args->P_x[i] = static_cast<float>(displacedPosition[0]);
            args->P_y[i] = static_cast<float>(displacedPosition[1]);
            args->P_z[i] = static_cast<float>(displacedPosition[2]);
        } catch (const std::exception& error) {
            if (!prototypeContext->displacementExceptionReported.exchange(
                    true)) {
                TF_RUNTIME_ERROR(
                    "HdEmbreeDisplacementFunction: exception at Embree "
                    "callback boundary: %s",
                    error.what());
            }
        } catch (...) {
            if (!prototypeContext->displacementExceptionReported.exchange(
                    true)) {
                TF_RUNTIME_ERROR(
                    "HdEmbreeDisplacementFunction: unknown exception at "
                    "Embree callback boundary");
            }
        }
    }
}

HdEmbreeMesh::HdEmbreeMesh(SdfPath const& id)
    : HdMesh(id)
    , _rtcMeshId(RTC_INVALID_GEOMETRY_ID)
    , _rtcMeshScene(nullptr)
    , _adjacencyValid(false)
    , _normalsValid(false)
    , _surfaceDerivativesValid(false)
    , _tangentFrameValid(false)
    , _refined(false)
    , _smoothNormals(false)
    , _displacementEnabled(true)
    , _warnedInstancedDisplacementIsLimited(false)
    , _doubleSided(false)
    , _cullStyle(HdCullStyleDontCare)
    , _nextFvarTopologyId(2)
{
}

HdEmbreeMesh::~HdEmbreeMesh() = default;

std::vector<float>
HdEmbreeMesh::_ComputeAdaptiveSubdivisionLevels(
    GfMatrix4d const& viewMatrix,
    GfMatrix4d const& projectionMatrix,
    GfRect2i const& dataWindow) const
{
    if (!_refined || !_geometry || _subdivisionLevels.empty() ||
        _points.empty() || _instances.empty() ||
        dataWindow.GetWidth() <= 0 || dataWindow.GetHeight() <= 0) {
        return {};
    }

    // One prototype can appear at several screen sizes. Tessellating for
    // every instance prevents a small instance from making a larger one
    // coarse.
    std::vector<GfMatrix4f> instanceTransforms;
    instanceTransforms.reserve(_instances.size());
    for (_Instance const& instance : _instances) {
        if (!instance.context) {
            return {};
        }
        instanceTransforms.push_back(
            instance.context->objectToWorldMatrix);
    }

    HdEmbreeDisplacedPositionProbe displacedPositionProbe;
    ty::PrototypeContext const* const prototypeContext =
        _prototypeContext.get();
    if (prototypeContext && prototypeContext->displaced) {
        const RTCGeometry geometry = _geometry;
        displacedPositionProbe =
            [geometry, prototypeContext](
                unsigned int primID,
                float u,
                float v,
                GfVec3f* posObj) {
                return ty::ComputeDisplacedSubdivPosition(
                    geometry, prototypeContext, primID, u,
                    v, posObj);
            };
    }

    return HdEmbreeComputeAdaptiveSubdivisionLevels(
        _points, _topology.GetFaceVertexCounts(),
        _topology.GetFaceVertexIndices(), instanceTransforms,
        viewMatrix, projectionMatrix, dataWindow,
        _topology.GetRefineLevel(), displacedPositionProbe);
}

bool
HdEmbreeMesh::UpdateSubdivisionLevels(
    GfMatrix4d const& viewMatrix,
    GfMatrix4d const& projectionMatrix,
    GfRect2i const& dataWindow,
    bool forceDisplacementRebuild)
{
    if (!_refined || !_geometry) {
        return false;
    }

    // Material graphs are held through a stable handle and can change
    // without dirtying every bound mesh. Refresh the effective state before
    // any viewport/instance early exit so disabling or replacing a terminal
    // cannot leave the prototype context stale.
    ty::PrototypeContext* const prototypeContext =
        _prototypeContext.get();
    const bool wasDisplaced = prototypeContext->displaced;
    const bool displacementStateChanged = _RefreshDisplacementState();
    const bool rebuildDisplacement =
        displacementStateChanged ||
        (forceDisplacementRebuild &&
         (wasDisplaced || prototypeContext->displaced));

    const bool canCommit =
        !_subdivisionLevels.empty() && !_points.empty();
    const std::vector<float> newLevels =
        _ComputeAdaptiveSubdivisionLevels(
            viewMatrix, projectionMatrix, dataWindow);
    const bool levelsChanged =
        newLevels.size() == _subdivisionLevels.size() &&
        newLevels != _subdivisionLevels;

    // Equal edge levels do not imply equal geometry: a material edit can
    // change displacement while the camera stays fixed. Recommit even for a
    // zero-sized viewport or a temporarily uninstanced prototype; neither is
    // needed to reevaluate the callback with existing edge levels.
    if (!canCommit || (!levelsChanged && !rebuildDisplacement)) {
        return false;
    }

    // Embree retains the LEVEL buffer pointer. Updating in place avoids
    // invalidating that pointer while still telling Embree to retessellate.
    if (levelsChanged) {
        std::copy(newLevels.begin(), newLevels.end(),
                  _subdivisionLevels.begin());
    }
    prototypeContext->displacementExceptionReported.store(false);
    rtcUpdateGeometryBuffer(_geometry, RTC_BUFFER_TYPE_LEVEL, 0);
    rtcCommitGeometry(_geometry);
    rtcCommitScene(_rtcMeshScene);
    _CommitPrototypeInstances();
    return true;
}

void
HdEmbreeMesh::_CommitPrototypeInstances()
{
    // Embree's required commit order for instancing is prototype geometry,
    // prototype scene, instance geometry, then the root scene. The caller
    // commits the root scene after Sync or adaptive subdivision updates.
    for (_Instance const& instance : _instances) {
        rtcCommitGeometry(instance.geometry);
    }
}

bool
HdEmbreeMesh::_RefreshDisplacementState()
{
    ty::PrototypeContext* const prototypeContext =
        _prototypeContext.get();
    const bool displaced =
        _refined &&
        prototypeContext->displacementEnabled &&
        prototypeContext->material &&
        prototypeContext->material->displacementGraph;
    if (prototypeContext->displaced == displaced) {
        _WarnIfInstancedDisplacementIsLimited(prototypeContext);
        return false;
    }

    prototypeContext->displaced = displaced;
    // Ordinary subdivision meshes must not pay Embree's displaced-patch
    // normal/callback cost. rtcSetGeometryDisplacementFunction itself does
    // not mark geometry modified, so callers must use the returned true value
    // to perform the complete commit chain.
    if (_refined) {
        rtcSetGeometryDisplacementFunction(
            _geometry,
            displaced ? HdEmbreeDisplacementFunction : nullptr);
    }
    _WarnIfInstancedDisplacementIsLimited(prototypeContext);
    return true;
}

void
HdEmbreeMesh::_ResolveGeomPropBindings()
{
    ty::PrototypeContext* const context = _prototypeContext.get();
    if (!context || !context->material) {
        if (context) {
            context->geomPropSamplers.clear();
            context->geomPropUniformValues.clear();
        }
        return;
    }

    const std::vector<TfToken>& tokens =
        context->material->geomPropTokens;
    context->geomPropSamplers.assign(tokens.size(), nullptr);
    context->geomPropUniformValues.assign(tokens.size(), mxcpp::Value());

    for (size_t handle = 0; handle < tokens.size(); ++handle) {
        const TfToken& name = tokens[handle];
        const auto samplerIt = context->primvarMap.find(name);
        if (samplerIt != context->primvarMap.end()) {
            context->geomPropSamplers[handle] = samplerIt->second.get();
        }

        const auto sourceIt = _primvarSourceMap.find(name);
        if (sourceIt == _primvarSourceMap.end() ||
            sourceIt->second.interpolation != HdInterpolationConstant) {
            continue;
        }
        std::string value;
        if (_GetUniformStringPrimvarValue(sourceIt->second.data, &value)) {
            context->geomPropUniformValues[handle] =
                mxcpp::Value(std::move(value));
        }
    }
}

void
HdEmbreeMesh::RefreshMaterialBindings()
{
    // geomPropSamplers observes primvarMap storage. Every sampler mutation in
    // _PopulateRtMesh must precede this rebuild so no observer survives an
    // erased or replaced owning entry.
    _ResolveGeomPropBindings();
}

void
HdEmbreeMesh::_WarnIfInstancedDisplacementIsLimited(
    ty::PrototypeContext const* prototypeContext)
{
    if (_warnedInstancedDisplacementIsLimited || !prototypeContext ||
        !prototypeContext->displaced || GetInstancerId().IsEmpty()) {
        return;
    }

    TF_WARN(
        "Mesh <%s> uses displacement through a shared point-instancer "
        "prototype. Displacement evaluation currently sees the rprim "
        "transform and primvars only; point-instancer transforms and "
        "per-instance primvars are unavailable, so world-space or "
        "instance-dependent displacement graphs may be incorrect.",
        GetId().GetText());
    _warnedInstancedDisplacementIsLimited = true;
}

void
HdEmbreeMesh::Finalize(HdRenderParam *renderParam)
{
    RTCScene scene = static_cast<HdEmbreeRenderParam*>(renderParam)
        ->AcquireSceneForEdit();
    // Detach and release every instance before destroying the user data
    // address Embree borrowed from its owning record.
    for (_Instance& instance : _instances) {
        rtcDetachGeometry(scene, instance.rtcId);
        rtcReleaseGeometry(instance.geometry);
        instance.geometry = nullptr;
        instance.rtcId = RTC_INVALID_GEOMETRY_ID;
        instance.context.reset();
    }
    _instances.clear();

    // Detach and release the prototype before destroying its borrowed context
    // address. Sampler destructors only update the mesh-owned buffer allocator,
    // so they do not require the geometry to remain live. The scene is released
    // last because instances refer to it and it retains attached geometry.
    if (_rtcMeshScene != nullptr) {
        if (_rtcMeshId != RTC_INVALID_GEOMETRY_ID) {
            rtcDetachGeometry(_rtcMeshScene, _rtcMeshId);
            rtcReleaseGeometry(_geometry);
            _geometry = nullptr;
            _rtcMeshId = RTC_INVALID_GEOMETRY_ID;
            _prototypeContext.reset();
        }
        rtcReleaseScene(_rtcMeshScene);
        _rtcMeshScene = nullptr;
    }
}

HdDirtyBits
HdEmbreeMesh::GetInitialDirtyBitsMask() const
{
    // The initial dirty bits control what data is available on the first
    // run through _PopulateRtMesh(), so it should list every data item
    // that _PopulateRtMesh requests.
    int mask = HdChangeTracker::Clean
        | HdChangeTracker::InitRepr
        | HdChangeTracker::DirtyPoints
        | HdChangeTracker::DirtyTopology
        | HdChangeTracker::DirtyTransform
        | HdChangeTracker::DirtyVisibility
        | HdChangeTracker::DirtyCullStyle
        | HdChangeTracker::DirtyDoubleSided
        | HdChangeTracker::DirtyDisplayStyle
        | HdChangeTracker::DirtySubdivTags
        | HdChangeTracker::DirtyPrimvar
        | HdChangeTracker::DirtyNormals
        | HdChangeTracker::DirtyInstancer
        | HdChangeTracker::DirtyCategories
        | HdChangeTracker::DirtyMaterialId
        ;

    return (HdDirtyBits)mask;
}

HdDirtyBits
HdEmbreeMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void
HdEmbreeMesh::_InitRepr(TfToken const &reprToken,
                        HdDirtyBits *dirtyBits)
{
    // Create an empty repr.
    _ReprVector::iterator it = std::find_if(_reprs.begin(), _reprs.end(),
                                            _ReprComparator(reprToken));
    if (it == _reprs.end()) {
        _reprs.emplace_back(reprToken, HdReprSharedPtr());
    }

    // A collection repr change can be the only scene update generated by
    // usdview. Keep this rprim in Hydra's sync request after InitRepr is
    // cleared so Sync() can publish the new display mode to Embree. Check the
    // last requested repr rather than registration: returning to a previously
    // used repr must also replace the mode retained by the prototype context.
    if (_lastRequestedReprToken != reprToken) {
        _lastRequestedReprToken = reprToken;
        *dirtyBits |= HdChangeTracker::NewRepr;
    }
}

void
HdEmbreeMesh::Sync(HdSceneDelegate *sceneDelegate,
                   HdRenderParam   *renderParam,
                   HdDirtyBits     *dirtyBits,
                   TfToken const   &reprToken)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    // XXX: A mesh repr can have multiple repr decs; this is done, for example, 
    // when the drawstyle specifies different rasterizing modes between front
    // faces and back faces.
    // With raytracing, this concept makes less sense, but
    // combining semantics of two HdMeshReprDesc is tricky in the general case.
    // For now, HdEmbreeMesh only respects the first desc; this should be fixed.
    _MeshReprConfig::DescArray descs = _GetReprDesc(reprToken);
    const HdMeshReprDesc &desc = descs[0];

    // Pull top-level embree state out of the render param.
    HdEmbreeRenderParam *embreeRenderParam =
        static_cast<HdEmbreeRenderParam*>(renderParam);
    RTCScene scene = embreeRenderParam->AcquireSceneForEdit();
    RTCDevice device = embreeRenderParam->GetEmbreeDevice();

    // Create embree geometry objects.
    _PopulateRtMesh(
        sceneDelegate,
        scene,
        device,
        embreeRenderParam->GetMaterialEvalServices(),
        dirtyBits,
        desc);
}

bool
HdEmbreeMesh::_CreateEmbreeSubdivMesh(
    RTCScene scene,
    RTCDevice device,
    RTCGeometry* geometry,
    unsigned* rtcId)
{
    *geometry = nullptr;
    *rtcId = RTC_INVALID_GEOMETRY_ID;
    const PxOsdSubdivTags &subdivTags = _topology.GetSubdivTags();

    // The embree edge crease buffer expects ungrouped edges: a pair
    // of indices marking an edge and one weight per crease.
    // HdMeshTopology stores edge creases compactly. A crease length
    // buffer stores the number of indices per crease and groups the
    // crease index buffer, much like the face buffer groups the vertex index
    // buffer except that creases don't automatically close. Crease weights
    // can be specified per crease or per individual edge.
    //
    // For example, to add the edges [v0->v1@2.0f] and [v1->v2@2.0f],
    // HdMeshTopology might store length = [3], indices = [v0, v1, v2],
    // and weight = [2.0f], or it might store weight = [2.0f, 2.0f].
    //
    // This loop calculates the number of edge creases, in preparation for
    // unrolling the edge crease buffer below.
    VtIntArray const creaseLengths = subdivTags.GetCreaseLengths();
    int numEdgeCreases = 0;
    for (size_t i = 0; i < creaseLengths.size(); ++i) {
        numEdgeCreases += creaseLengths[i] - 1;
    }

    // For vertex creases, sanity check that the weights and indices
    // arrays are the same length.
    int numVertexCreases =
        static_cast<int>(subdivTags.GetCornerIndices().size());
    if (numVertexCreases !=
            static_cast<int>(subdivTags.GetCornerWeights().size())) {
        TF_WARN("Mismatch between vertex crease indices and weights");
        numVertexCreases = 0;
    }

    // Medium and higher complexity use Embree subdivision geometry, which is
    // the only geometry type that supports adaptive levels and displacement.
    RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_SUBDIVISION);
    if (!geom) {
        TF_CODING_ERROR("Couldn't allocate RTC subdivision geometry");
        return false;
    }
    // Adaptive levels and displacement change generated primitives, not only
    // vertex positions. Embree documents REFIT for vertex-only changes, so a
    // low-quality rebuild is required for these production update paths.
    rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_LOW);
    rtcSetGeometryTimeStepCount(geom,1);
    rtcSetGeometryMask(geom, ty::RayMask::Scene);
    const unsigned attachedId = rtcAttachGeometry(scene,geom);
    if (attachedId == RTC_INVALID_GEOMETRY_ID) {
        TF_CODING_ERROR("Couldn't attach RTC subdivision geometry");
        rtcReleaseGeometry(geom);
        return false;
    }

    // Fill the topology buffers.
    rtcSetSharedGeometryBuffer(geom,
        RTC_BUFFER_TYPE_FACE,
        0, /* unsigned int slot */
        RTC_FORMAT_UINT,
        _topology.GetFaceVertexCounts().cdata(),
        0, /* size_t byteOffset */
        sizeof(int),  /*must be 4 byte aligned */
        _topology.GetFaceVertexCounts().size());
    rtcSetSharedGeometryBuffer(geom,
        RTC_BUFFER_TYPE_INDEX,
        0, /* unsigned int slot */
        RTC_FORMAT_UINT,
        _topology.GetFaceVertexIndices().cdata(),
        0, /* size_t byteOffset */
        sizeof(int),  /*must be 4 byte aligned */
        _topology.GetFaceVertexIndices().size());

    _subdivisionLevels.assign(
        _topology.GetFaceVertexIndices().size(), 1.0f);
    rtcSetSharedGeometryBuffer(geom,
        RTC_BUFFER_TYPE_LEVEL,
        0,
        RTC_FORMAT_FLOAT,
        _subdivisionLevels.data(),
        0,
        sizeof(float),
        _subdivisionLevels.size());

    if (_topology.GetHoleIndices().size()>0) {
        // PSA : creating a hole buffer with 0 length has very unexpected
        // behavior in Embree (things draw wrong, but not determinisitcally)
        rtcSetSharedGeometryBuffer(geom,
            RTC_BUFFER_TYPE_HOLE,
            0, /* unsigned int slot */
            RTC_FORMAT_UINT,
            _topology.GetHoleIndices().cdata(),
            0, /* size_t byteOffset */
            sizeof(unsigned int),  /*must be 4 byte aligned */
            _topology.GetHoleIndices().size());
    }

    // If this topology has edge creases, unroll the edge crease buffer.
    if (numEdgeCreases > 0) {
        int *embreeCreaseIndices = static_cast<int*>(
            rtcSetNewGeometryBuffer(
                geom,
                RTC_BUFFER_TYPE_EDGE_CREASE_INDEX,
                0, /* unsigned int slot */
                RTC_FORMAT_UINT2,
                2*sizeof(int), /*must be 4 byte aligned */
                numEdgeCreases));
        float *embreeCreaseWeights = static_cast<float*>(
            rtcSetNewGeometryBuffer(
                geom,
                RTC_BUFFER_TYPE_EDGE_CREASE_WEIGHT,
                0, /* unsigned int slot */
                RTC_FORMAT_FLOAT,
                sizeof(float), /*must be 4 byte aligned */
                numEdgeCreases));
        int embreeEdgeIndex = 0;

        VtIntArray const creaseIndices = subdivTags.GetCreaseIndices();
        VtFloatArray const creaseWeights =
            subdivTags.GetCreaseWeights();

        bool weightPerCrease =
            (creaseWeights.size() == creaseLengths.size());

        // Loop through the creases; for each crease, loop through
        // the edges.
        int creaseIndexStart = 0;
        for (size_t i = 0; i < creaseLengths.size(); ++i) {
            int numEdges = creaseLengths[i] - 1;
            for(int j = 0; j < numEdges; ++j) {
                // Store the crease indices.
                embreeCreaseIndices[2*embreeEdgeIndex+0] =
                    creaseIndices[creaseIndexStart+j];
                embreeCreaseIndices[2*embreeEdgeIndex+1] =
                    creaseIndices[creaseIndexStart+j+1];

                // Store the crease weight.
                embreeCreaseWeights[embreeEdgeIndex] = weightPerCrease ?
                    creaseWeights[i] : creaseWeights[embreeEdgeIndex];

                embreeEdgeIndex++;
            }
            creaseIndexStart += creaseLengths[i];
        }
    }

    if (numVertexCreases > 0) {
        rtcSetSharedGeometryBuffer(
            geom,
            RTC_BUFFER_TYPE_VERTEX_CREASE_INDEX,
            0, /* unsigned int slot */
            RTC_FORMAT_UINT,
            subdivTags.GetCornerIndices().cdata(),
            0, /* size_t byteOffset */
            sizeof(int), /*must be 4 byte aligned */
            numVertexCreases);
        rtcSetSharedGeometryBuffer(
            geom,
            RTC_BUFFER_TYPE_VERTEX_CREASE_WEIGHT,
            0, /* unsigned int slot */
            RTC_FORMAT_FLOAT,
            subdivTags.GetCornerWeights().cdata(),
            0, /* size_t byteOffset */
            sizeof(float), /*must be 4 byte aligned */
            numVertexCreases);
    }

    // Attribute topologies are configured after all dirty primvars have been
    // pulled, so indexed face-varying data can retain its authored sharing.

    *geometry = geom;
    *rtcId = attachedId;
    return true;
}

void
HdEmbreeMesh::_ConfigureSubdivAttributeTopologies(RTCGeometry geometry)
{
    VtIntArray const& meshIndices = _topology.GetFaceVertexIndices();
    size_t const faceVertexCount = meshIndices.size();

    // Topology 0 is geometry; topology 1 is the shared linear topology for
    // varying primvars. Face-varying primvars start at 2 because their index
    // arrays (and therefore seams) are independent.
    rtcSetGeometryTopologyCount(geometry, _nextFvarTopologyId);
    rtcSetGeometrySubdivisionMode(
        geometry, 1, RTC_SUBDIVISION_MODE_PIN_ALL);
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 1, RTC_FORMAT_UINT,
        meshIndices.cdata(), 0, sizeof(int), faceVertexCount);

    _fvarIndices.resize(faceVertexCount);
    std::iota(_fvarIndices.begin(), _fvarIndices.end(), 0u);

    RTCSubdivisionMode const faceVaryingMode =
        HdEmbreeGetFaceVaryingSubdivisionMode(
            _topology.GetSubdivTags().GetFaceVaryingInterpolationRule());
    TF_FOR_ALL(it, _primvarSourceMap) {
        PrimvarSource const& source = it->second;
        if (source.interpolation != HdInterpolationFaceVarying ||
            source.topologyId < 2) {
            continue;
        }

        void const* indices = source.indices.empty()
            ? static_cast<void const*>(_fvarIndices.data())
            : static_cast<void const*>(source.indices.cdata());
        size_t const indexCount = source.indices.empty()
            ? faceVertexCount
            : source.indices.size();
        if (indexCount != faceVertexCount) {
            TF_WARN(
                "Face-varying primvar '%s' has %zu indices for %zu face "
                "vertices; skipping its subdivision topology",
                it->first.GetText(), indexCount, faceVertexCount);
            continue;
        }

        rtcSetGeometrySubdivisionMode(
            geometry, source.topologyId, faceVaryingMode);
        rtcSetSharedGeometryBuffer(
            geometry,
            RTC_BUFFER_TYPE_INDEX,
            source.topologyId,
            RTC_FORMAT_UINT,
            indices,
            0,
            sizeof(int),
            indexCount);
    }
}

bool
HdEmbreeMesh::_CreateEmbreeTriangleMesh(
    RTCScene scene,
    RTCDevice device,
    RTCGeometry* geometry,
    unsigned* rtcId)
{
    *geometry = nullptr;
    *rtcId = RTC_INVALID_GEOMETRY_ID;

    // Triangulate the input faces.
    HdMeshUtil meshUtil(&_topology, GetId());
    meshUtil.ComputeTriangleIndices(&_triangulatedIndices,
        &_trianglePrimitiveParams);

    // Create the new mesh.
    // geometry will be committed in the calling function
    RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
    if (!geom) {
        TF_CODING_ERROR("Couldn't allocate RTC triangle geometry");
        return false;
    }
    // Rebuild on vertex edits. In particular, this keeps bounds deterministic
    // when the Embree-owned vertex buffer is updated and then instanced.
    rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_LOW);
    rtcSetGeometryTimeStepCount(geom,1);
    rtcSetGeometryMask(geom, ty::RayMask::Scene);
    const unsigned attachedId = rtcAttachGeometry(scene,geom);
    if (attachedId == RTC_INVALID_GEOMETRY_ID) {
        TF_CODING_ERROR("Couldn't attach RTC triangle geometry");
        rtcReleaseGeometry(geom);
        return false;
    }

    // Populate topology in Embree-owned memory. VtArray uses copy-on-write;
    // later non-const access to _triangulatedIndices can detach its storage
    // even though this member remains alive, which would leave a shared
    // Embree buffer pointing at freed memory.
    GfVec3i* const embreeIndices = static_cast<GfVec3i*>(
        rtcSetNewGeometryBuffer(
            geom,
            RTC_BUFFER_TYPE_INDEX,
            0, /* unsigned int slot */
            RTC_FORMAT_UINT3,
            sizeof(GfVec3i),
            _triangulatedIndices.size()));
    if (!embreeIndices && !_triangulatedIndices.empty()) {
        TF_CODING_ERROR("Couldn't allocate RTC triangle index buffer");
        rtcDetachGeometry(scene, attachedId);
        rtcReleaseGeometry(geom);
        return false;
    }
    if (embreeIndices) {
        std::copy(
            _triangulatedIndices.cbegin(),
            _triangulatedIndices.cend(),
            embreeIndices);
    }

    *geometry = geom;
    *rtcId = attachedId;
    return true;
}

void
HdEmbreeMesh::_UpdatePrimvarSources(HdSceneDelegate* sceneDelegate,
                                    HdDirtyBits dirtyBits,
                                    bool* requiresRefinedGeometryRebuild)
{
    HD_TRACE_FUNCTION();
    SdfPath const& id = GetId();

    // Update _primvarSourceMap, our local cache of raw primvar data.
    // This function pulls data from the scene delegate, but defers processing.
    //
    // While iterating primvars, we skip "points" (vertex positions) because
    // the points primvar is processed by _PopulateRtMesh. We only call
    // GetPrimvar on primvars that have been marked dirty.
    //
    // Currently, hydra doesn't have a good way of communicating changes in
    // the set of primvars, so we only ever add and update to the primvar set.

    HdPrimvarDescriptorVector primvars;
    for (size_t i=0; i < HdInterpolationCount; ++i) {
        HdInterpolation interp = static_cast<HdInterpolation>(i);
        primvars = GetPrimvarDescriptors(sceneDelegate, interp);
        for (HdPrimvarDescriptor const& pv: primvars) {
            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name) &&
                pv.name != HdTokens->points) {
                const auto previous = _primvarSourceMap.find(pv.name);
                const bool leavesFaceVarying =
                    previous != _primvarSourceMap.end() &&
                    previous->second.interpolation ==
                        HdInterpolationFaceVarying &&
                    interp != HdInterpolationFaceVarying;
                if (leavesFaceVarying &&
                    requiresRefinedGeometryRebuild) {
                    *requiresRefinedGeometryRebuild = true;
                }
                PrimvarSource& source = _primvarSourceMap[pv.name];
                VtIntArray indices;
                source.data = pv.indexed
                    ? GetIndexedPrimvar(sceneDelegate, pv.name, &indices)
                    : GetPrimvar(sceneDelegate, pv.name);
                source.interpolation = interp;
                source.indices = std::move(indices);
                if (interp == HdInterpolationFaceVarying &&
                    source.topologyId < 2) {
                    source.topologyId = _nextFvarTopologyId++;
                }
            }
        }
    }
}

TfTokenVector
HdEmbreeMesh::_UpdateComputedPrimvarSources(HdSceneDelegate* sceneDelegate,
                                            HdDirtyBits dirtyBits,
                                            bool* requiresRefinedGeometryRebuild)
{
    HD_TRACE_FUNCTION();
    
    SdfPath const& id = GetId();

    // Get all the dirty computed primvars
    HdExtComputationPrimvarDescriptorVector dirtyCompPrimvars;
    for (size_t i=0; i < HdInterpolationCount; ++i) {
        HdExtComputationPrimvarDescriptorVector compPrimvars;
        HdInterpolation interp = static_cast<HdInterpolation>(i);
        compPrimvars = sceneDelegate->GetExtComputationPrimvarDescriptors
                                    (GetId(),interp);

        for (auto const& pv: compPrimvars) {
            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name)) {
                dirtyCompPrimvars.emplace_back(pv);
            }
        }
    }

    if (dirtyCompPrimvars.empty()) {
        return TfTokenVector();
    }
    
    HdExtComputationUtils::ValueStore valueStore
        = HdExtComputationUtils::GetComputedPrimvarValues(
            dirtyCompPrimvars, sceneDelegate);

    TfTokenVector compPrimvarNames;
    // Update local primvar map and track the ones that were computed
    for (auto const& compPrimvar : dirtyCompPrimvars) {
        auto const it = valueStore.find(compPrimvar.name);
        if (!TF_VERIFY(it != valueStore.end())) {
            continue;
        }
        
        compPrimvarNames.emplace_back(compPrimvar.name);
        if (compPrimvar.name == HdTokens->points) {
            _points = it->second.Get<VtVec3fArray>();
            _normalsValid = false;
            _surfaceDerivativesValid = false;
            _tangentFrameValid = false;
        } else {
            const auto previous =
                _primvarSourceMap.find(compPrimvar.name);
            const bool leavesFaceVarying =
                previous != _primvarSourceMap.end() &&
                previous->second.interpolation ==
                    HdInterpolationFaceVarying &&
                compPrimvar.interpolation !=
                    HdInterpolationFaceVarying;
            if (leavesFaceVarying &&
                requiresRefinedGeometryRebuild) {
                *requiresRefinedGeometryRebuild = true;
            }
            PrimvarSource& source = _primvarSourceMap[compPrimvar.name];
            source.data = it->second;
            source.interpolation = compPrimvar.interpolation;
            source.indices.clear();
            if (source.interpolation == HdInterpolationFaceVarying &&
                source.topologyId < 2) {
                source.topologyId = _nextFvarTopologyId++;
            }
            if (compPrimvar.name == HdTokens->normals ||
                compPrimvar.name == _tokensSt) {
                _surfaceDerivativesValid = false;
                _tangentFrameValid = false;
            }
        }
    }

    return compPrimvarNames;
}

void
HdEmbreeMesh::_UpdateSurfaceDerivativeCache()
{
    _triangleDPdu.clear();
    _triangleDPdv.clear();
    _surfaceDerivativesValid = false;

    if (_refined || _triangulatedIndices.empty() || _points.empty()) {
        return;
    }

    _triangleDPdu.resize(_triangulatedIndices.size(), GfVec3f(0.0f));
    _triangleDPdv.resize(_triangulatedIndices.size(), GfVec3f(0.0f));

    ty::PrimvarSampler const* stSampler = nullptr;
    if (ty::PrototypeContext* ctx = _prototypeContext.get()) {
        auto it = ctx->primvarMap.find(_tokensSt);
        if (it != ctx->primvarMap.end()) {
            stSampler = it->second.get();
        }
    }

    for (size_t triIndex = 0; triIndex < _triangulatedIndices.size(); ++triIndex) {
        GfVec3i const& tri = _triangulatedIndices[triIndex];
        if (tri[0] >= static_cast<int>(_points.size()) ||
            tri[1] >= static_cast<int>(_points.size()) ||
            tri[2] >= static_cast<int>(_points.size())) {
            continue;
        }

        GfVec3f const& pos0 = _points[tri[0]];
        GfVec3f const& pos1 = _points[tri[1]];
        GfVec3f const& pos2 = _points[tri[2]];
        GfVec3f dP1 = pos1 - pos0;
        GfVec3f dP2 = pos2 - pos0;

        GfVec3f dPdu = dP1;
        GfVec3f dPdv = dP2;

        GfVec2f st0, st1, st2;
        if (_SampleTriangleCorners(
                stSampler, static_cast<unsigned int>(triIndex),
                &st0, &st1, &st2)) {
            GfVec2f const dst1 = st1 - st0;
            GfVec2f const dst2 = st2 - st0;
            const float det = _DifferenceOfProducts(
                dst1[0], dst2[1], dst1[1], dst2[0]);
            if (std::abs(det) > 1e-9f) {
                const float invDet = 1.0f / det;
                dPdu = ( dst2[1] * dP1 - dst1[1] * dP2) * invDet;
                dPdv = (-dst2[0] * dP1 + dst1[0] * dP2) * invDet;
            }
        }

        if (GfCross(dPdu, dPdv).GetLengthSq() < 1e-18f) {
            GfVec3f geometricNormal =
                _BuildGeometricFaceNormal(pos0, pos1, pos2);
            GfBuildOrthonormalFrame(geometricNormal, &dPdu, &dPdv);
        }

        _triangleDPdu[triIndex] = dPdu;
        _triangleDPdv[triIndex] = dPdv;
    }

    _surfaceDerivativesValid = true;
}

void
HdEmbreeMesh::_UpdateTangentFrameCache()
{
    // Build each corner tangent by projecting dPdu off the normal, accumulate
    // the frames by smoothing group, then re-orthonormalize the group result
    // for each corner.
    _computedTangents.clear();
    _computedBitangents.clear();
    _tangentFrameValid = false;

    ty::PrototypeContext* ctx = _prototypeContext.get();
    if (_refined || !ctx || !_surfaceDerivativesValid ||
        _triangleDPdu.size() != _triangulatedIndices.size()) {
        return;
    }

    const VtIntArray& faceVertexIndices = _topology.GetFaceVertexIndices();
    if (faceVertexIndices.empty()) {
        return;
    }

    const VtIntArray triangulatedCornerIds = _ComputeTriangulatedCornerIds(_topology);
    if (triangulatedCornerIds.size() != _triangulatedIndices.size() * 3) {
        return;
    }

    ty::PrimvarSampler const* normalSampler = nullptr;
    auto normalIt = ctx->primvarMap.find(HdTokens->normals);
    if (normalIt != ctx->primvarMap.end()) {
        normalSampler = normalIt->second.get();
    }

    struct Accumulator
    {
        GfVec3f tangent = GfVec3f(0.0f);
        GfVec3f bitangent = GfVec3f(0.0f);
    };

    std::unordered_map<_SmoothingKey, size_t, _SmoothingKeyHash> groupMap;
    std::vector<Accumulator> accumulators;
    std::vector<GfVec3f> cornerNormals(faceVertexIndices.size(), GfVec3f(0.0f));
    std::vector<size_t> cornerGroups(faceVertexIndices.size(), size_t(-1));

    for (size_t triIndex = 0; triIndex < _triangulatedIndices.size(); ++triIndex) {
        GfVec3i const& tri = _triangulatedIndices[triIndex];
        if (tri[0] >= static_cast<int>(_points.size()) ||
            tri[1] >= static_cast<int>(_points.size()) ||
            tri[2] >= static_cast<int>(_points.size())) {
            continue;
        }

        GfVec3f triNormals[3];
        const GfVec3f geometricNormal = _BuildGeometricFaceNormal(
            _points[tri[0]], _points[tri[1]], _points[tri[2]]);
        if (!_SampleTriangleCorners(
                normalSampler, static_cast<unsigned int>(triIndex),
                &triNormals[0], &triNormals[1], &triNormals[2])) {
            triNormals[0] = geometricNormal;
            triNormals[1] = geometricNormal;
            triNormals[2] = geometricNormal;
        }

        for (size_t corner = 0; corner < 3; ++corner) {
            const int authoredCorner = triangulatedCornerIds[triIndex * 3 + corner];
            if (authoredCorner < 0 ||
                authoredCorner >= static_cast<int>(faceVertexIndices.size())) {
                continue;
            }

            GfVec3f normal = triNormals[corner];
            if (normal.GetLengthSq() < 1e-18f) {
                normal = geometricNormal;
            } else {
                normal.Normalize();
            }

            cornerNormals[authoredCorner] = normal;

            GfVec3f rawTangent =
                _triangleDPdu[triIndex] - normal * GfDot(normal, _triangleDPdu[triIndex]);
            if (rawTangent.GetLengthSq() < 1e-18f) {
                rawTangent = GfCross(_triangleDPdv[triIndex], normal);
            }

            if (rawTangent.GetLengthSq() < 1e-18f) {
                GfVec3f fallbackBitangent;
                GfBuildOrthonormalFrame(normal, &rawTangent, &fallbackBitangent);
            } else {
                rawTangent.Normalize();
            }

            GfVec3f rawBitangent = GfCross(normal, rawTangent);
            if (rawBitangent.GetLengthSq() < 1e-18f) {
                GfVec3f fallbackTangent;
                GfBuildOrthonormalFrame(normal, &fallbackTangent, &rawBitangent);
                rawTangent = fallbackTangent;
            } else {
                rawBitangent.Normalize();
                if (GfDot(rawBitangent, _triangleDPdv[triIndex]) < 0.0f) {
                    rawBitangent *= -1.0f;
                }
            }

            const _SmoothingKey key = _MakeSmoothingKey(
                faceVertexIndices[authoredCorner], normal);
            auto [it, inserted] = groupMap.emplace(key, accumulators.size());
            if (inserted) {
                accumulators.emplace_back();
            }

            const size_t groupIndex = it->second;
            cornerGroups[authoredCorner] = groupIndex;
            Accumulator& accum = accumulators[groupIndex];
            if (accum.tangent.GetLengthSq() > 1e-18f &&
                GfDot(rawTangent, accum.tangent) < 0.0f) {
                rawTangent *= -1.0f;
                rawBitangent *= -1.0f;
            }
            accum.tangent += rawTangent;
            accum.bitangent += rawBitangent;
        }
    }

    _computedTangents.resize(faceVertexIndices.size(), GfVec3f(1.0f, 0.0f, 0.0f));
    _computedBitangents.resize(faceVertexIndices.size(), GfVec3f(0.0f, 1.0f, 0.0f));

    for (size_t authoredCorner = 0; authoredCorner < faceVertexIndices.size(); ++authoredCorner) {
        if (cornerGroups[authoredCorner] == size_t(-1)) {
            continue;
        }

        const Accumulator& accum = accumulators[cornerGroups[authoredCorner]];
        GfVec3f normal = cornerNormals[authoredCorner];
        if (normal.GetLengthSq() < 1e-18f) {
            normal = GfVec3f(0.0f, 1.0f, 0.0f);
        } else {
            normal.Normalize();
        }

        GfVec3f tangent = accum.tangent - normal * GfDot(normal, accum.tangent);
        if (tangent.GetLengthSq() < 1e-18f) {
            tangent = GfCross(accum.bitangent, normal);
        }
        if (tangent.GetLengthSq() < 1e-18f) {
            GfVec3f fallbackBitangent;
            GfBuildOrthonormalFrame(normal, &tangent, &fallbackBitangent);
            _computedTangents[authoredCorner] = tangent;
            _computedBitangents[authoredCorner] = fallbackBitangent;
            continue;
        }

        tangent.Normalize();

        GfVec3f bitangent =
            accum.bitangent - normal * GfDot(normal, accum.bitangent);
        bitangent -= tangent * GfDot(tangent, bitangent);
        if (bitangent.GetLengthSq() < 1e-18f) {
            bitangent = GfCross(normal, tangent);
        }
        if (bitangent.GetLengthSq() < 1e-18f) {
            GfVec3f fallbackTangent, fallbackBitangent;
            GfBuildOrthonormalFrame(normal, &fallbackTangent, &fallbackBitangent);
            _computedTangents[authoredCorner] = fallbackTangent;
            _computedBitangents[authoredCorner] = fallbackBitangent;
            continue;
        }

        bitangent.Normalize();

        _computedTangents[authoredCorner] = tangent;
        _computedBitangents[authoredCorner] = bitangent;
    }

    _tangentFrameValid = true;
}

void
HdEmbreeMesh::_CreatePrimvarSampler(TfToken const& name, VtValue const& data,
                                    HdInterpolation interpolation,
                                    bool refined,
                                    unsigned int topologyId)
{
    // Replace the old sampler, if it exists.
    ty::PrototypeContext *ctx = _prototypeContext.get();
    ctx->primvarMap.erase(name);
    if (name == HdTokens->normals) {
        ctx->triangleNormalSampler = nullptr;
        ctx->triangleNormalSamplerKind =
            ty::TriangleCornerSamplerKind::none;
    } else if (name == _tokensTangent) {
        ctx->tangentSampler = nullptr;
    } else if (name == _tokensBitangent) {
        ctx->bitangentSampler = nullptr;
    } else if (name == _tokensComputedTangent) {
        ctx->computedTangentSampler = nullptr;
    } else if (name == _tokensComputedBitangent) {
        ctx->computedBitangentSampler = nullptr;
    }

    HdVtBufferSource buffer(name, data);
    const HdTupleType tupleType = buffer.GetTupleType();
    if (_IsMatrixPrimvarType(tupleType) &&
        interpolation != HdInterpolationConstant &&
        interpolation != HdInterpolationUniform) {
        _WarnUnsupportedMatrixPrimvarOnce(name, interpolation);
        return;
    }

    // Construct the correct type of sampler from the interpolation mode and
    // geometry mode.
    std::unique_ptr<ty::PrimvarSampler> sampler;
    switch(interpolation) {
        case HdInterpolationConstant:
            sampler = std::make_unique<ty::ConstantSampler>(name, data);
            break;
        case HdInterpolationUniform:
            if (refined) {
                sampler = std::make_unique<ty::UniformSampler>(name, data);
            } else {
                sampler = std::make_unique<ty::UniformSampler>(
                    name, data, _trianglePrimitiveParams);
            }
            break;
        case HdInterpolationVertex:
            if (refined) {
                sampler = std::make_unique<ty::SubdivVertexSampler>(
                    name, data, _geometry, &_embreeBufferAllocator);
            } else {
                sampler = std::make_unique<ty::TriangleVertexSampler>(
                    name, data, _triangulatedIndices);
            }
            break;
        case HdInterpolationVarying:
            if (refined) {
                sampler = std::make_unique<ty::SubdivVaryingSampler>(
                    name, data, _geometry, &_embreeBufferAllocator);
            } else {
                sampler = std::make_unique<ty::TriangleVertexSampler>(
                    name, data, _triangulatedIndices);
            }
            break;
        case HdInterpolationFaceVarying:
            if (refined) {
                sampler = std::make_unique<ty::SubdivFaceVaryingSampler>(
                    name, data, _geometry, topologyId,
                    &_embreeBufferAllocator);
            } else {
                HdMeshUtil meshUtil(&_topology, GetId());
                sampler =
                    std::make_unique<ty::TriangleFaceVaryingSampler>(
                        name, data, meshUtil);
            }
            break;
        default:
            TF_CODING_ERROR("Unrecognized interpolation mode");
            break;
    }

    // Put the new sampler back in the primvar map.
    if (sampler) {
        if (name == HdTokens->normals && !refined) {
            ctx->triangleNormalSampler = sampler.get();
            if (interpolation == HdInterpolationVertex ||
                interpolation == HdInterpolationVarying) {
                ctx->triangleNormalSamplerKind =
                    ty::TriangleCornerSamplerKind::vertex;
            } else if (interpolation == HdInterpolationFaceVarying) {
                ctx->triangleNormalSamplerKind =
                    ty::TriangleCornerSamplerKind::faceVarying;
            }
        } else if (name == _tokensTangent) {
            ctx->tangentSampler = sampler.get();
        } else if (name == _tokensBitangent) {
            ctx->bitangentSampler = sampler.get();
        } else if (name == _tokensComputedTangent) {
            ctx->computedTangentSampler = sampler.get();
        } else if (name == _tokensComputedBitangent) {
            ctx->computedBitangentSampler = sampler.get();
        }
        ctx->primvarMap[name] = std::move(sampler);
    }
}

void
HdEmbreeMesh::_UpdateInstances(HdSceneDelegate* sceneDelegate,
                               RTCScene scene,
                               RTCDevice device)
{
    std::vector<HdEmbreeInstanceData> instances;
    if (!GetInstancerId().IsEmpty()) {
        // Retrieve instance transforms from the instancer.
        HdRenderIndex &renderIndex = sceneDelegate->GetRenderIndex();
        HdInstancer *instancer =
            renderIndex.GetInstancer(GetInstancerId());
        instances = static_cast<HdEmbreeInstancer*>(instancer)->
            ComputeInstanceData(GetId());
        for (HdEmbreeInstanceData& instance : instances) {
            ty::MergeCategories(
                _categories, &instance.categories);
        }
    } else {
        // If there's no instancer, add a single instance with transform I.
        instances.emplace_back();
        instances.back().categories = _categories;
    }

    const size_t oldSize = _instances.size();
    const size_t newSize = instances.size();

    // Release removed instance geometry before destroying the stable user
    // data address owned by its record.
    for(size_t i = newSize; i < oldSize; ++i) {
        _Instance& instance = _instances[i];
        rtcDetachGeometry(scene, instance.rtcId);
        rtcReleaseGeometry(instance.geometry);
        instance.geometry = nullptr;
        instance.rtcId = RTC_INVALID_GEOMETRY_ID;
        instance.context.reset();
    }
    _instances.resize(newSize);

    // Size up (if necessary).
    for(size_t i = oldSize; i < newSize; ++i) {
        _Instance& instance = _instances[i];

        // Create the new instance.
        RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_INSTANCE);
        rtcSetGeometryInstancedScene(geom,_rtcMeshScene);
        rtcSetGeometryTimeStepCount(geom,1);
        rtcSetGeometryMask(geom, ty::RayMask::Scene);
        instance.rtcId = rtcAttachGeometry(scene,geom);
        instance.geometry = geom;

        // Embree borrows the context address owned by this record.
        instance.context =
            std::make_unique<ty::InstanceContext>();
        instance.context->rootScene = _rtcMeshScene;
        instance.context->instanceId = i;
        rtcSetGeometryUserData(geom, instance.context.get());
    }

    // Update transform
    for (size_t i = 0; i < instances.size(); ++i) {
        _Instance& instance = _instances[i];

        // Combine the local transform and the instance transform.
        GfMatrix4f matf =
            _transform * GfMatrix4f(instances[i].transform);

        // Update the transform in the BVH.
        rtcSetGeometryTransform(instance.geometry,
            0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, matf.GetArray());
        // Update the renderer data exposed through Embree user data.
        instance.context->objectToWorldMatrix = matf;
        instance.context->worldToObjectMatrix = matf.GetInverse();
        instance.context->categories = instances[i].categories;
    }
}

void
HdEmbreeMesh::_PopulateRtMesh(HdSceneDelegate* sceneDelegate,
                              RTCScene         scene,
                              RTCDevice        device,
                              ty::MaterialEvalServices const*
                                  materialEvalServices,
                              HdDirtyBits*     dirtyBits,
                              HdMeshReprDesc const &desc)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();

    ////////////////////////////////////////////////////////////////////////
    // 1. Pull scene data.
    bool requiresRefinedGeometryRebuild = false;
    TfTokenVector computedPrimvars =
        _UpdateComputedPrimvarSources(
            sceneDelegate, *dirtyBits,
            &requiresRefinedGeometryRebuild);

    bool pointsIsComputed =
        std::find(computedPrimvars.begin(), computedPrimvars.end(),
                  HdTokens->points) != computedPrimvars.end();
    if (!pointsIsComputed &&
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        VtValue value = sceneDelegate->Get(id, HdTokens->points);
        _points = value.Get<VtVec3fArray>();
        _normalsValid = false;
        _surfaceDerivativesValid = false;
        _tangentFrameValid = false;
    }

    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        // When pulling a new topology, we don't want to overwrite the
        // refine level or subdiv tags, which are provided separately by the
        // scene delegate, so we save and restore them.
        PxOsdSubdivTags subdivTags = _topology.GetSubdivTags();
        int refineLevel = _topology.GetRefineLevel();
        _topology = HdMeshTopology(GetMeshTopology(sceneDelegate), refineLevel);
        _topology.SetSubdivTags(subdivTags);
        _adjacencyValid = false;
        _surfaceDerivativesValid = false;
        _tangentFrameValid = false;
    }
    if (HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id)) {
        _topology.SetSubdivTags(sceneDelegate->GetSubdivTags(id));
    }
    if (HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id)) {
        HdDisplayStyle const displayStyle = sceneDelegate->GetDisplayStyle(id);
        _topology = HdMeshTopology(_topology,
            displayStyle.refineLevel);
        _displacementEnabled = displayStyle.displacementEnabled;
    }

    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
        _transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
    }

    if (HdChangeTracker::IsCullStyleDirty(*dirtyBits, id)) {
        _cullStyle = GetCullStyle(sceneDelegate);
    }
    if (HdChangeTracker::IsDoubleSidedDirty(*dirtyBits, id)) {
        _doubleSided = IsDoubleSided(sceneDelegate);
    }
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->widths) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->primvar)) {
        _UpdatePrimvarSources(
            sceneDelegate, *dirtyBits,
            &requiresRefinedGeometryRebuild);
        if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals) ||
            HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, _tokensSt)) {
            _surfaceDerivativesValid = false;
            _tangentFrameValid = false;
        }
    }

    ////////////////////////////////////////////////////////////////////////
    // 2. Resolve drawstyles

    // The repr defines a set of geometry styles for drawing the mesh
    // (see hd/enums.h). HdEmbree does not implement edge/point drawing, but
    // unsupported mesh reprs still fall back to surface rendering, and that
    // fallback should honor the active refine level.
    bool doRefine = _GeomStyleShouldHonorRefineLevel(desc.geomStyle);

    // If the subdivision scheme is "none", force us to not refine.
    doRefine = doRefine && (_topology.GetScheme() != PxOsdOpenSubdivTokens->none);

    // Low complexity is the control cage, matching Hydra expectations and
    // avoiding subdivision cost when refinement was explicitly disabled.
    // Higher levels remain subdivision geometry whose rates are chosen later
    // from projected edge length.
    doRefine = doRefine && (_topology.GetRefineLevel() > 0);

    // The repr defines whether we should compute smooth normals for this mesh:
    // per-vertex normals taken as an average of adjacent faces, and
    // interpolated smoothly across faces.
    _smoothNormals = !desc.flatShadingEnabled;

    // If the subdivision scheme is "none" or "bilinear", force us not to use
    // smooth normals.
    _smoothNormals = _smoothNormals &&
        (_topology.GetScheme() != PxOsdOpenSubdivTokens->none) &&
        (_topology.GetScheme() != PxOsdOpenSubdivTokens->bilinear);

    // If the scene delegate has provided authored normals, force us to not use
    // smooth normals.
    bool authoredNormals = false;
    if (_primvarSourceMap.count(HdTokens->normals) > 0) {
        authoredNormals = true;
    }
    _smoothNormals = _smoothNormals && !authoredNormals;

    ////////////////////////////////////////////////////////////////////////
    // 3. Populate embree prototype object.

    // If the topology has changed, or the value of doRefine has changed, we
    // need to create or recreate the embree mesh object.
    // _GetInitialDirtyBits() ensures that the topology is dirty the first time
    // this function is called, so that the embree mesh is always created.
    bool newMesh = false;
    if (_rtcMeshId == RTC_INVALID_GEOMETRY_ID ||
        HdChangeTracker::IsTopologyDirty(*dirtyBits, id) ||
        (doRefine &&
         HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id)) ||
        (doRefine && requiresRefinedGeometryRebuild) ||
        doRefine != _refined) {

        newMesh = true;

        // Detach and release the old geometry before destroying the samplers
        // and stable context address that Embree borrowed.
        if (_rtcMeshId != RTC_INVALID_GEOMETRY_ID) {
            rtcDetachGeometry(_rtcMeshScene, _rtcMeshId);
            rtcReleaseGeometry(_geometry);
            _geometry = nullptr;
            _rtcMeshId = RTC_INVALID_GEOMETRY_ID;
            _prototypeContext.reset();
        }
        // Create the prototype mesh scene, if it doesn't exist yet.
        if (_rtcMeshScene == nullptr) {
            _rtcMeshScene = rtcNewScene(device);
            // Robust traversal is particularly important along neighboring
            // displaced subdivision patches; the scene remains dynamic.
            rtcSetSceneFlags(
                _rtcMeshScene,
                static_cast<RTCSceneFlags>(
                    RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST));

            // RTC_BUILD_QUALITY_LOW: Create lower quality data structures,
            // e.g. for dynamic scenes. A two-level spatial index structure is built
            // when enabling this mode, which supports fast partial scene updates,
            // and allows for setting a per-geometry build quality through
            // the rtcSetGeometryBuildQuality function.
            rtcSetSceneBuildQuality(_rtcMeshScene, RTC_BUILD_QUALITY_LOW);
        }

        // Populate either a subdivision or triangle prototype. A failed
        // replacement leaves the shared prototype scene committed and empty,
        // so existing instances never reference a half-built geometry.
        RTCGeometry geometry = nullptr;
        unsigned rtcId = RTC_INVALID_GEOMETRY_ID;
        const bool created = doRefine
            ? _CreateEmbreeSubdivMesh(
                _rtcMeshScene, device, &geometry, &rtcId)
            : _CreateEmbreeTriangleMesh(
                _rtcMeshScene, device, &geometry, &rtcId);
        if (!created) {
            rtcCommitScene(_rtcMeshScene);
            _CommitPrototypeInstances();
            _refined = false;
            return;
        }
        _geometry = geometry;
        _rtcMeshId = rtcId;

        _refined = doRefine;
        // In both cases, RTC_VERTEX_BUFFER will be populated below.

        // Prototype geometry gets tagged with a prototype context, that the
        // ray-hit algorithm can use to look up data.
        _prototypeContext =
            std::make_unique<ty::PrototypeContext>();
        rtcSetGeometryUserData(_geometry, _prototypeContext.get());
        _prototypeContext->primId = GetPrimId();
        _prototypeContext->geometryKind = _refined
            ? ty::GeometryKind::subdivisionMesh
            : ty::GeometryKind::triangleMesh;
        _prototypeContext->cullStyle = _cullStyle;
        _prototypeContext->doubleSided = _doubleSided;
        _prototypeContext->refined = _refined;
        _prototypeContext->orientationSign =
            _refined && _topology.GetOrientation() != HdTokens->rightHanded
                ? -1.0f
                : 1.0f;
        _prototypeContext->triangleDPdu = &_triangleDPdu;
        _prototypeContext->triangleDPdv = &_triangleDPdv;
        _prototypeContext->triangleIndices = &_triangulatedIndices;
        _prototypeContext->points = &_points;
        _prototypeContext->primitiveParams = (_refined ?
            _trianglePrimitiveParams : VtIntArray());
        _prototypeContext->faceVertexCounts =
            _topology.GetFaceVertexCounts();
        _prototypeContext->faceVertexOffsets.clear();
        _prototypeContext->faceVertexOffsets.reserve(
            _prototypeContext->faceVertexCounts.size() + 1);
        size_t faceVertexOffset = 0;
        _prototypeContext->faceVertexOffsets.push_back(
            faceVertexOffset);
        for (const int faceVertexCount :
                _prototypeContext->faceVertexCounts) {
            if (faceVertexCount > 0) {
                faceVertexOffset += static_cast<size_t>(faceVertexCount);
            }
            _prototypeContext->faceVertexOffsets.push_back(
                faceVertexOffset);
        }
        _prototypeContext->subdivisionLevels = &_subdivisionLevels;
        _prototypeContext->material = nullptr;

        // Camera and shadow traversal share culling semantics. The same
        // dispatch also handles open curve endpoints for curve records.
        ty::BindPrototypeGeometryFilter(_geometry);

        // Force the smooth normals code to rebuild the "normals" primvar the
        // next time smooth normals is enabled.
        _normalsValid = false;
        _surfaceDerivativesValid = false;
        _tangentFrameValid = false;
    }

    // The prototype scene is shared by all instances, so displacement is
    // evaluated in the rprim's prototype transform. Point-instancer transforms
    // are intentionally unavailable at prototype-commit time.
    {
        ty::PrototypeContext* const context = _prototypeContext.get();
        context->materialEvalServices = materialEvalServices;
        context->displacementEnabled =
            _displacementEnabled && desc.useCustomDisplacement;
        context->displacementObjectToWorldMatrix = _transform;
        context->displacementWorldToObjectMatrix = _transform.GetInverse();
    }

    // If the subdiv tags changed or the mesh was recreated, update the
    // subdivision interpolation mode.
    if (newMesh || HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id)) {
        if (doRefine) {
            if (_topology.GetScheme() == PxOsdOpenSubdivTokens->bilinear) {
                // Keep bilinear patches piecewise linear while retaining
                // Embree subdivision geometry for adaptive tessellation and
                // displacement.
                rtcSetGeometrySubdivisionMode(
                    _geometry, 0, RTC_SUBDIVISION_MODE_PIN_ALL);
            } else {
                TfToken const vertexRule =
                    _topology.GetSubdivTags().GetVertexInterpolationRule();

                if (vertexRule == PxOsdOpenSubdivTokens->none) {
                    rtcSetGeometrySubdivisionMode(
                        _geometry, 0,
                        RTC_SUBDIVISION_MODE_NO_BOUNDARY);
                } else if (vertexRule == PxOsdOpenSubdivTokens->edgeOnly) {
                    rtcSetGeometrySubdivisionMode(
                        _geometry, 0,
                        RTC_SUBDIVISION_MODE_SMOOTH_BOUNDARY);
                } else if (
                    vertexRule == PxOsdOpenSubdivTokens->edgeAndCorner) {
                    rtcSetGeometrySubdivisionMode(
                        _geometry, 0,
                        RTC_SUBDIVISION_MODE_PIN_CORNERS);
                } else if (!vertexRule.IsEmpty()) {
                    TF_WARN("Unknown vertex interpolation rule: %s",
                            vertexRule.GetText());
                }
            }
        }
    }

    bool subdivAttributeTopologiesDirty = newMesh;
    if (doRefine && !subdivAttributeTopologiesDirty) {
        TF_FOR_ALL(it, _primvarSourceMap) {
            if (it->second.interpolation == HdInterpolationFaceVarying &&
                HdChangeTracker::IsPrimvarDirty(
                    *dirtyBits, id, it->first)) {
                subdivAttributeTopologiesDirty = true;
                break;
            }
        }
    }
    if (doRefine && subdivAttributeTopologiesDirty) {
        _ConfigureSubdivAttributeTopologies(_geometry);
    }

    // Update the smooth normals in steps:
    // 1. If the topology is dirty, update the adjacency table, a processed
    //    form of the topology that helps calculate smooth normals quickly.
    // 2. If the points are dirty, update the smooth normal buffer itself.
    if (_smoothNormals && !_adjacencyValid) {
        _adjacency.BuildAdjacencyTable(&_topology);
        _adjacencyValid = true;
        // If we rebuilt the adjacency table, force a rebuild of normals.
        _normalsValid = false;
        _tangentFrameValid = false;
    }
    if (_smoothNormals && !_normalsValid) {
        _computedNormals = Hd_SmoothNormals::ComputeSmoothNormals(
            &_adjacency, _points.size(), _points.cdata());
        _normalsValid = true;

        // Create a sampler for the "normals" primvar. If there are authored
        // normals, the smooth normals flag has been suppressed, so it won't
        // be overwritten by the primvar population below.
        _CreatePrimvarSampler(HdTokens->normals, VtValue(_computedNormals),
            HdInterpolationVertex, _refined);
        _tangentFrameValid = false;
    }

    // If smooth normals are off and there are no authored normals, make sure
    // there's no "normals" sampler so the renderpass can use its fallback
    // behavior.
    if (!_smoothNormals && !authoredNormals) {
        ty::PrototypeContext *ctx = _prototypeContext.get();
        ctx->primvarMap.erase(HdTokens->normals);
        ctx->triangleNormalSampler = nullptr;
        ctx->triangleNormalSamplerKind =
            ty::TriangleCornerSamplerKind::none;

        // Force the smooth normals code to rebuild the "normals" primvar the
        // next time smooth normals is enabled.
        _normalsValid = false;
        _tangentFrameValid = false;
    }

    // Populate primvars if they've changed or we recreated the mesh.
    TF_FOR_ALL(it, _primvarSourceMap) {
        if (newMesh ||
            HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, it->first)) {
            // For subdivision meshes, skip face-varying authored normals.
            // The limit surface normal from Embree (via geometric normal or
            // rtcInterpolate1) is smooth and correct; authored face-varying
            // normals from the control mesh would create discontinuities
            // at face boundaries due to the PIN_CORNERS subdivision mode.
            if (_refined &&
                it->first == HdTokens->normals &&
                it->second.interpolation == HdInterpolationFaceVarying) {
                ty::PrototypeContext* const context =
                    _prototypeContext.get();
                auto samplerIt = context->primvarMap.find(it->first);
                if (samplerIt != context->primvarMap.end()) {
                    context->primvarMap.erase(samplerIt);
                }
                context->triangleNormalSampler = nullptr;
                context->triangleNormalSamplerKind =
                    ty::TriangleCornerSamplerKind::none;
                continue;
            }
            // Only refined face-varying samplers consume authored indices
            // directly through an Embree attribute topology. Triangle-cage
            // samplers and all other interpolation modes require the
            // conventional flattened Hydra value stream.
            VtValue samplerData = it->second.data;
            if (!it->second.indices.empty() &&
                (!_refined || it->second.interpolation !=
                    HdInterpolationFaceVarying)) {
                samplerData = VtVisitValue(
                    samplerData, _FlattenIndexedPrimvar(it->second.indices));
            }
            _CreatePrimvarSampler(
                it->first, samplerData, it->second.interpolation,
                _refined, it->second.topologyId);
        }
    }

    if (!_refined && (!_surfaceDerivativesValid || newMesh)) {
        _UpdateSurfaceDerivativeCache();
    }

    if (!_refined && (!_tangentFrameValid || newMesh)) {
        _UpdateTangentFrameCache();
    }

    if (!_refined && _tangentFrameValid) {
        _CreatePrimvarSampler(_tokensComputedTangent, VtValue(_computedTangents),
                              HdInterpolationFaceVarying, false);
        _CreatePrimvarSampler(_tokensComputedBitangent, VtValue(_computedBitangents),
                              HdInterpolationFaceVarying, false);
    } else {
        ty::PrototypeContext* ctx = _prototypeContext.get();
        if (ctx) {
            ctx->computedTangentSampler = nullptr;
            ctx->computedBitangentSampler = nullptr;
            for (TfToken const& name :
                     {_tokensComputedTangent, _tokensComputedBitangent}) {
                auto it = ctx->primvarMap.find(name);
                if (it != ctx->primvarMap.end()) {
                    ctx->primvarMap.erase(it);
                }
            }
        }
    }

    // Bind material before committing the prototype scene: Embree invokes
    // the displacement callback during scene commit.
    if ((*dirtyBits & HdChangeTracker::DirtyMaterialId) || newMesh) {
        if (_geometry) {
            HdEmbreeMaterial *mat = nullptr;
            SdfPath materialId = sceneDelegate->GetMaterialId(id);
            if (!materialId.IsEmpty()) {
                HdSprim *sprim =
                    sceneDelegate->GetRenderIndex().GetSprim(
                        HdPrimTypeTokens->material, materialId);
                mat = dynamic_cast<HdEmbreeMaterial*>(sprim);
            }
            ty::PrototypeContext* prototypeContext =
                _prototypeContext.get();
            prototypeContext->material =
                mat ? mat->GetRenderMaterial() : nullptr;
        }
    }
    _ResolveGeomPropBindings();
    ty::PrototypeContext* const prototypeContext =
        _prototypeContext.get();
    const bool displacementStateChanged = _RefreshDisplacementState();

    // Populate points in the RTC mesh.
    const bool pointsDirty = newMesh ||
        HdChangeTracker::IsPrimvarDirty(
            *dirtyBits, id, HdTokens->points);
    if (pointsDirty) {
        // Embree accesses RTC_FORMAT_FLOAT3 vertices using 16-byte loads, so
        // the last item also needs one readable padding float. An Embree-owned
        // buffer with an explicit 16-byte stride provides that guarantee and
        // decouples its lifetime from the Hydra point array. Rebind on every
        // DirtyPoints update: in-place updates produced allocation-dependent
        // collapsed bounds in Embree's instanced dynamic-scene path.
        constexpr size_t vertexStride = 4 * sizeof(float);
        float* vertices = nullptr;
        if (!_points.empty()) {
            vertices = static_cast<float*>(rtcSetNewGeometryBuffer(
                _geometry,
                RTC_BUFFER_TYPE_VERTEX,
                0, /* unsigned int slot */
                RTC_FORMAT_FLOAT3,
                vertexStride,
                _points.size()));
        }
        if (!vertices && !_points.empty()) {
            TF_CODING_ERROR(
                "Failed to allocate Embree vertex buffer for mesh <%s>",
                id.GetText());
            // rtcSetNewGeometryBuffer may have replaced the old binding before
            // reporting failure. Publish a committed empty prototype rather
            // than claiming the previously committed geometry is still valid.
            rtcDetachGeometry(_rtcMeshScene, _rtcMeshId);
            rtcReleaseGeometry(_geometry);
            _geometry = nullptr;
            _rtcMeshId = RTC_INVALID_GEOMETRY_ID;
            _prototypeContext.reset();
            _refined = false;
            rtcCommitScene(_rtcMeshScene);
            _CommitPrototypeInstances();
            return;
        }
        for (size_t i = 0; i < _points.size(); ++i) {
            vertices[4 * i + 0] = _points[i][0];
            vertices[4 * i + 1] = _points[i][1];
            vertices[4 * i + 2] = _points[i][2];
            vertices[4 * i + 3] = 0.0f;
        }
        if (vertices) {
            rtcUpdateGeometryBuffer(_geometry, RTC_BUFFER_TYPE_VERTEX, 0);
        } else {
            // Retain the last valid buffer so Embree can still validate the
            // authored topology, but make an empty points prim explicitly
            // non-intersectable. A later non-empty update always binds a
            // fresh buffer.
            rtcDisableGeometry(_geometry);
        }
    }

    // Update visibility by pulling the object into/out of the embree BVH.
    if (newMesh || pointsDirty ||
        HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        if (_sharedData.visible && !_points.empty()) {
            rtcEnableGeometry(_geometry);
        } else {
            rtcDisableGeometry(_geometry);
        }
    }

    // Instance-only edits must not retessellate a displaced prototype. A
    // direct rprim transform is the exception because graph-facing world
    // transforms change even though the Embree prototype remains shared.
    constexpr HdDirtyBits instanceOnlyBits =
        HdChangeTracker::Varying |
        HdChangeTracker::DirtyExtent |
        HdChangeTracker::DirtyInstancer |
        HdChangeTracker::DirtyInstanceIndex |
        HdChangeTracker::DirtyTransform |
        HdChangeTracker::DirtyCategories;
    const bool prototypeDirty = newMesh ||
        displacementStateChanged ||
        ((*dirtyBits & ~instanceOnlyBits) != HdChangeTracker::Clean) ||
        (HdChangeTracker::IsTransformDirty(*dirtyBits, id) &&
         prototypeContext->displaced);
    if (prototypeDirty) {
        // Primvar samplers and subdivision topology/rule changes also mutate
        // the geometry. Commit once after all buffers are current.
        prototypeContext->displacementExceptionReported.store(false);
        rtcCommitGeometry(_geometry);
        rtcCommitScene(_rtcMeshScene);
    }

    ////////////////////////////////////////////////////////////////////////
    // 4. Populate embree instance objects.

    // First, update our own instancer data.
    _UpdateInstancer(sceneDelegate, dirtyBits);

    // Make sure we call sync on parent instancers.
    // XXX: In theory, this should be done automatically by the render index.
    // At the moment, it's done by rprim-reference.  The helper function on
    // HdInstancer needs to use a mutex to guard access, if there are actually
    // updates pending, so this might be a contention point.
    HdInstancer::_SyncInstancerAndParents(
        sceneDelegate->GetRenderIndex(), GetInstancerId());

    if (*dirtyBits & HdChangeTracker::DirtyCategories) {
        _categories = sceneDelegate->GetCategories(id);
    }

    // If the instance topology changes, we need to update the instance
    // geometries. Un-instanced prims are treated here as a special case.
    // Instance geometries read from the instancer (for per-instance transform)
    // and the rprim transform, which gets added to the per instance transform.
    const bool instancesDirty =
        HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id) ||
        HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
        (*dirtyBits & HdChangeTracker::DirtyCategories);
    if (instancesDirty) {
        _UpdateInstances(sceneDelegate, scene, device);
    }

    // A changed prototype invalidates every referencing instance bound; a
    // direct instance edit must likewise be committed before the root scene.
    if (prototypeDirty || instancesDirty) {
        _CommitPrototypeInstances();
    }

    //
    // We are relying on the code calling this to commit the scene
    // since there are a bunch of commits to instances of geom
    // in the root scene
    //

    if (_geometry) {
        ty::PrototypeContext* const context = _prototypeContext.get();
        context->cullStyle = _cullStyle;
        context->doubleSided = _doubleSided;
        context->refined = _refined;
        context->wireframeMode = _GetWireframeMode(desc.geomStyle);
        context->blendWireframeColor = desc.blendWireframeColor;
        context->wireframeLineWidth = desc.lineWidth;
    }

    // Clean all dirty bits.
    *dirtyBits &= ~(HdChangeTracker::AllSceneDirtyBits |
                    HdChangeTracker::NewRepr);
}

PXR_NAMESPACE_CLOSE_SCOPE
