//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "displacementEvaluation.h"
#include "context.h"
#include "primvarSampling.h"

#include <renderer/materials/MaterialXCpp/graph.h>
#include <renderer/materials/MaterialXCpp/shadingContext.h>

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/tf/token.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// The displaced normal derivative is a second finite difference of a
// float-valued material graph. A smaller patch step visibly amplifies graph
// rounding; 2e-3 keeps the curvature stable while remaining local enough for
// the existing first-derivative frame reconstruction.
constexpr float _finiteDifferenceStep = 2.0e-3f;
constexpr float _normalDerivativeRingSpan = 2.0f * _finiteDifferenceStep;
constexpr float _minimumRelativeAreaSquared = 1.0e-18f;
static const TfToken _tokensSt("st", TfToken::Immortal);

float
_GetFiniteDifferenceOffset(float encodedCoordinate, bool isQuad)
{
    if (isQuad) {
        return encodedCoordinate + _normalDerivativeRingSpan <= 1.0f
            ? _finiteDifferenceStep
            : -_finiteDifferenceStep;
    }

    // Non-quad subdivision faces encode a subpatch ID and a local coordinate
    // together. Embree guarantees the local coordinate range [-0.5, 1.5),
    // within which adding a small offset stays on the same subpatch encoding.
    const float halfCoordinate = 0.5f * encodedCoordinate;
    const float localCoordinate =
        2.0f * (halfCoordinate - std::floor(halfCoordinate)) - 0.5f;
    return localCoordinate + _normalDerivativeRingSpan < 1.5f
        ? _finiteDifferenceStep
        : -_finiteDifferenceStep;
}

bool
_IsFinite(GfVec3f const& value)
{
    return std::isfinite(value[0]) &&
           std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool
_IsFinite(GfVec3d const& value)
{
    return std::isfinite(value[0]) &&
           std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool
_TryConvertToVec3f(GfVec3d const& value, GfVec3f* result)
{
    // Double-precision transforms, sums, and finite differences can exceed
    // float range even when every input is finite. Reject those results before
    // narrowing so displacement never introduces infinities.
    constexpr double maxFloat =
        static_cast<double>(std::numeric_limits<float>::max());
    if (!result || !_IsFinite(value) ||
        std::abs(value[0]) > maxFloat ||
        std::abs(value[1]) > maxFloat ||
        std::abs(value[2]) > maxFloat) {
        return false;
    }

    *result = GfVec3f(
        static_cast<float>(value[0]),
        static_cast<float>(value[1]),
        static_cast<float>(value[2]));
    return _IsFinite(*result);
}

bool
_TryNormalize(GfVec3d const& value, GfVec3d* result)
{
    if (!result || !_IsFinite(value)) {
        return false;
    }

    const double maximumComponent = std::max({
        std::abs(value[0]), std::abs(value[1]), std::abs(value[2])});
    if (!std::isfinite(maximumComponent) || maximumComponent == 0.0) {
        return false;
    }

    const GfVec3d scaled = value / maximumComponent;
    const double scaledLength = std::sqrt(scaled.GetLengthSq());
    if (!std::isfinite(scaledLength) || scaledLength == 0.0) {
        return false;
    }

    *result = scaled / scaledLength;
    return _IsFinite(*result);
}

bool
_TryNormalize(GfVec3f const& value, GfVec3f* result)
{
    if (!result || !_IsFinite(value)) {
        return false;
    }

    GfVec3d normalized;
    if (!_TryNormalize(
            GfVec3d(value[0], value[1], value[2]), &normalized)) {
        return false;
    }
    return _TryConvertToVec3f(normalized, result);
}

bool
_AreTangentsIndependent(
    GfVec3f const& dPdu,
    GfVec3f const& dPdv)
{
    GfVec3f normalizedU;
    GfVec3f normalizedV;
    if (!_TryNormalize(dPdu, &normalizedU) ||
        !_TryNormalize(dPdv, &normalizedV)) {
        return false;
    }
    const GfVec3f relativeArea = GfCross(normalizedU, normalizedV);
    const float relativeAreaSquared = relativeArea.GetLengthSq();
    return std::isfinite(relativeAreaSquared) &&
        relativeAreaSquared > _minimumRelativeAreaSquared;
}

bool
_TryBuildNormalFromTangents(
    GfVec3f const& dPdu,
    GfVec3f const& dPdv,
    GfVec3f* normal)
{
    if (!normal || !_AreTangentsIndependent(dPdu, dPdv)) {
        return false;
    }
    const GfVec3d cross = GfCross(
        GfVec3d(dPdu[0], dPdu[1], dPdu[2]),
        GfVec3d(dPdv[0], dPdv[1], dPdv[2]));
    GfVec3d normalized;
    if (!_TryNormalize(cross, &normalized)) {
        return false;
    }
    return _TryConvertToVec3f(normalized, normal);
}

bool
_TryBuildWorldFrame(
    HdEmbreePrototypeContext const& context,
    GfVec3f const& objectNormal,
    GfVec3f const& objectDPdu,
    GfVec3f const& objectDPdv,
    GfVec3f* worldNormal,
    GfVec3f* worldDPdu,
    GfVec3f* worldDPdv,
    GfVec3f* worldTangent,
    GfVec3f* worldBitangent)
{
    if (!worldNormal || !worldDPdu || !worldDPdv ||
        !worldTangent || !worldBitangent ||
        !_IsFinite(objectNormal) || !_IsFinite(objectDPdu) ||
        !_IsFinite(objectDPdv) ||
        !_AreTangentsIndependent(objectDPdu, objectDPdv)) {
        return false;
    }

    const double objectToWorldDeterminant =
        context.displacementObjectToWorldMatrix.GetDeterminant3();
    if (!std::isfinite(objectToWorldDeterminant) ||
        objectToWorldDeterminant == 0.0) {
        return false;
    }

    const GfMatrix4f normalTransform =
        context.displacementWorldToObjectMatrix.GetTranspose();
    if (!_TryNormalize(
            normalTransform.TransformDir(objectNormal), worldNormal)) {
        return false;
    }

    *worldDPdu = context.displacementObjectToWorldMatrix.TransformDir(
        objectDPdu);
    *worldDPdv = context.displacementObjectToWorldMatrix.TransformDir(
        objectDPdv);
    if (!_IsFinite(*worldDPdu) || !_IsFinite(*worldDPdv) ||
        !_AreTangentsIndependent(*worldDPdu, *worldDPdv)) {
        return false;
    }

    const GfVec3f tangentCandidate =
        *worldDPdu - *worldNormal * GfDot(*worldNormal, *worldDPdu);
    if (!_TryNormalize(tangentCandidate, worldTangent)) {
        return false;
    }

    const GfVec3f bitangentCandidate =
        *worldDPdv - *worldNormal * GfDot(*worldNormal, *worldDPdv) -
        *worldTangent * GfDot(*worldTangent, *worldDPdv);
    if (!_TryNormalize(bitangentCandidate, worldBitangent)) {
        // A valid but highly skewed parameterization can lose its independent
        // component to float precision. The oriented cross product remains a
        // stable orthogonal fallback.
        if (!_TryNormalize(
                GfCross(*worldNormal, *worldTangent), worldBitangent)) {
            return false;
        }
    }

    if (GfDot(*worldBitangent, *worldDPdv) < 0.0f) {
        *worldBitangent = -*worldBitangent;
    }
    return true;
}

bool
_InterpolateBaseFrame(
    RTCGeometry geometry,
    unsigned int primID,
    float u,
    float v,
    float orientationSign,
    GfVec3f* position,
    GfVec3f* normal,
    GfVec3f* dPdu,
    GfVec3f* dPdv)
{
    if (!geometry || !position || !normal || !dPdu || !dPdv ||
        !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    alignas(16) float sampled[4] = {};
    alignas(16) float sampledDu[4] = {};
    alignas(16) float sampledDv[4] = {};
    rtcInterpolate1(
        geometry, primID, u, v, RTC_BUFFER_TYPE_VERTEX, 0,
        sampled, sampledDu, sampledDv, 3);

    *position = GfVec3f(sampled[0], sampled[1], sampled[2]);
    *dPdu = GfVec3f(sampledDu[0], sampledDu[1], sampledDu[2]);
    *dPdv = GfVec3f(sampledDv[0], sampledDv[1], sampledDv[2]);
    if (!_IsFinite(*position) || !_IsFinite(*dPdu) || !_IsFinite(*dPdv) ||
        !_TryBuildNormalFromTangents(*dPdu, *dPdv, normal)) {
        return false;
    }
    *normal *= orientationSign;
    return true;
}

bool
_TryAddOffset(
    GfVec3f const& position,
    GfVec3f const& offset,
    GfVec3f* displacedPosition)
{
    if (!displacedPosition || !_IsFinite(position) || !_IsFinite(offset)) {
        return false;
    }
    const GfVec3d sum(
        static_cast<double>(position[0]) + offset[0],
        static_cast<double>(position[1]) + offset[1],
        static_cast<double>(position[2]) + offset[2]);
    return _TryConvertToVec3f(sum, displacedPosition);
}

bool
_TryFiniteDifference(
    GfVec3f const& probe,
    GfVec3f const& center,
    float parameterOffset,
    GfVec3f* derivative)
{
    if (!derivative || !_IsFinite(probe) || !_IsFinite(center) ||
        !std::isfinite(parameterOffset) || parameterOffset == 0.0f) {
        return false;
    }
    const double inverseOffset =
        1.0 / static_cast<double>(parameterOffset);
    const GfVec3d result(
        (static_cast<double>(probe[0]) - center[0]) * inverseOffset,
        (static_cast<double>(probe[1]) - center[1]) * inverseOffset,
        (static_cast<double>(probe[2]) - center[2]) * inverseOffset);
    return _TryConvertToVec3f(result, derivative);
}

bool
_EvaluateDisplacedSubdivProbe(
    RTCGeometry geometry,
    HdEmbreePrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    float orientationSign,
    GfVec3f* outObjectOffset,
    GfVec3f* outBaseDPdu,
    GfVec3f* outBaseDPdv)
{
    if (!geometry || !context || !outObjectOffset ||
        !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    GfVec3f position;
    GfVec3f normal;
    GfVec3f dPdu;
    GfVec3f dPdv;
    if (!_InterpolateBaseFrame(
            geometry, primID, u, v, orientationSign,
            &position, &normal, &dPdu, &dPdv)) {
        return false;
    }

    float displacement = 0.0f;
    if (!HdEmbreeEvaluateDisplacement(
            context, primID, u, v,
            position, normal, dPdu, dPdv, &displacement)) {
        return false;
    }
    if (!HdEmbreeComputeObjectSpaceDisplacementOffset(
            context, normal, displacement, outObjectOffset)) {
        return false;
    }

    if (outBaseDPdu) {
        *outBaseDPdu = dPdu;
    }
    if (outBaseDPdv) {
        *outBaseDPdv = dPdv;
    }
    return true;
}

bool
_TryBuildDisplacedNormal(
    GfVec3f const& baseDPdu,
    GfVec3f const& baseDPdv,
    GfVec3f const& centerObjectOffset,
    GfVec3f const& uObjectOffset,
    GfVec3f const& vObjectOffset,
    float du,
    float dv,
    float orientationSign,
    GfVec3f* outNormal,
    GfVec3f* outDPdu,
    GfVec3f* outDPdv)
{
    // Every caller needs the normal; callers that need only curvature may
    // omit either displaced-tangent output.
    if (!outNormal) {
        return false;
    }

    GfVec3f objectOffsetDu;
    GfVec3f objectOffsetDv;
    GfVec3f displacedDPdu;
    GfVec3f displacedDPdv;
    if (!_TryFiniteDifference(
            uObjectOffset, centerObjectOffset, du, &objectOffsetDu) ||
        !_TryFiniteDifference(
            vObjectOffset, centerObjectOffset, dv, &objectOffsetDv) ||
        !_TryAddOffset(baseDPdu, objectOffsetDu, &displacedDPdu) ||
        !_TryAddOffset(baseDPdv, objectOffsetDv, &displacedDPdv)) {
        return false;
    }

    GfVec3f displacedNormal;
    if (!_TryBuildNormalFromTangents(
            displacedDPdu, displacedDPdv, &displacedNormal)) {
        return false;
    }
    displacedNormal *= orientationSign;
    if (!_IsFinite(displacedNormal)) {
        return false;
    }

    *outNormal = displacedNormal;
    if (outDPdu) {
        *outDPdu = displacedDPdu;
    }
    if (outDPdv) {
        *outDPdv = displacedDPdv;
    }
    return true;
}

} // namespace

bool
HdEmbreeEvaluateDisplacement(
    HdEmbreePrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& dPdu,
    GfVec3f const& dPdv,
    float* displacement)
{
    if (!context || !context->material ||
        !context->material->displacementGraph || !displacement ||
        !std::isfinite(u) || !std::isfinite(v) || !_IsFinite(position)) {
        return false;
    }

    // MaterialX surface derivatives are expressed in authored `st`, not in
    // Embree's patch coordinates. Use the same inverse Jacobian as hit-time
    // shading so geometric nodes see one consistent frame.
    GfVec3f objectDPdu = dPdu;
    GfVec3f objectDPdv = dPdv;
    auto const stIt = context->primvarMap.find(_tokensSt);
    if (stIt != context->primvarMap.end()) {
        const HdEmbreeSubdivTexcoordJacobian stJacobian =
            HdEmbreeComputeSubdivTexcoordJacobian(
                stIt->second.get(), primID, u, v);
        if (stJacobian.valid) {
            objectDPdu =
                stJacobian.duDs * dPdu + stJacobian.dvDs * dPdv;
            objectDPdv =
                stJacobian.duDt * dPdu + stJacobian.dvDt * dPdv;
        }
    }

    GfVec3f worldNormal;
    GfVec3f worldDPdu;
    GfVec3f worldDPdv;
    GfVec3f worldTangent;
    GfVec3f worldBitangent;
    if (!_TryBuildWorldFrame(
            *context, normal, objectDPdu, objectDPdv,
            &worldNormal, &worldDPdu, &worldDPdv,
            &worldTangent, &worldBitangent)) {
        return false;
    }

    mxcpp::ShadingContext shadingContext;
    shadingContext.position = mxcpp::Vec3f(
        position[0], position[1], position[2]);
    shadingContext.normal = mxcpp::Vec3f(
        worldNormal[0], worldNormal[1], worldNormal[2]);
    shadingContext.tangent = mxcpp::Vec3f(
        worldTangent[0], worldTangent[1], worldTangent[2]);
    shadingContext.bitangent = mxcpp::Vec3f(
        worldBitangent[0], worldBitangent[1], worldBitangent[2]);
    shadingContext.faceId = static_cast<int>(primID);
    shadingContext.baryU = u;
    shadingContext.baryV = v;
    shadingContext.dPdu = mxcpp::Vec3f(
        worldDPdu[0], worldDPdu[1], worldDPdu[2]);
    shadingContext.dPdv = mxcpp::Vec3f(
        worldDPdv[0], worldDPdv[1], worldDPdv[2]);
    shadingContext.dPositiondu = mxcpp::Vec3f(
        objectDPdu[0], objectDPdu[1], objectDPdu[2]);
    shadingContext.dPositiondv = mxcpp::Vec3f(
        objectDPdv[0], objectDPdv[1], objectDPdv[2]);

    // Geometry displacement has no screen-space footprint. Leaving all dx/dy
    // derivatives at their zero defaults makes callback-time and hit-time
    // image filtering identical.
    if (stIt != context->primvarMap.end()) {
        HdEmbreeSampleTexcoord(
            stIt->second.get(), primID, u, v, &shadingContext.texcoord);
    }

    HdEmbreePrimvarLookup primvarLookup{
        &context->geomPropSamplers, primID, u, v};
    shadingContext.geomPropLookup = &HdEmbreeSamplePrimvar;
    shadingContext.geomPropUserData = &primvarLookup;
    shadingContext.uniformProps = &context->geomPropUniformValues;

    if (context->materialEvalServices) {
        shadingContext.textureSystem =
            context->materialEvalServices->textureSystem;
        shadingContext.frame = context->materialEvalServices->frame;
        shadingContext.time = context->materialEvalServices->time;
    }

    shadingContext.objectToWorldMatrix =
        HdEmbreePrimvarSamplingDetail::ToMxMatrix(
            context->displacementObjectToWorldMatrix);
    shadingContext.worldToObjectMatrix =
        HdEmbreePrimvarSamplingDetail::ToMxMatrix(
            context->displacementWorldToObjectMatrix);
    shadingContext.hasObjectToWorldTransform = true;
    shadingContext.hasWorldToObjectTransform = true;

    float evaluated = 0.0f;
    if (!context->material->displacementGraph->EvaluateDisplacement(
            shadingContext, &evaluated) || !std::isfinite(evaluated)) {
        return false;
    }

    *displacement = evaluated;
    return true;
}

bool
HdEmbreeComputeObjectSpaceDisplacementOffset(
    HdEmbreePrototypeContext const* context,
    GfVec3f const& objectNormal,
    float displacement,
    GfVec3f* objectOffset)
{
    if (!context || !objectOffset || !_IsFinite(objectNormal) ||
        !std::isfinite(displacement)) {
        return false;
    }

    GfVec3f worldNormal;
    if (!_TryNormalize(
            context->displacementWorldToObjectMatrix.GetTranspose()
                .TransformDir(objectNormal),
            &worldNormal)) {
        return false;
    }

    const GfVec3d worldOffset =
        static_cast<double>(displacement) *
        GfVec3d(worldNormal[0], worldNormal[1], worldNormal[2]);
    const GfVec3d offset =
        GfMatrix4d(context->displacementWorldToObjectMatrix)
            .TransformDir(worldOffset);
    return _TryConvertToVec3f(offset, objectOffset);
}

bool
HdEmbreeComputeDisplacedSubdivPosition(
    RTCGeometry geometry,
    HdEmbreePrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* outPosition)
{
    if (!geometry || !context || !outPosition ||
        !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    const float orientationSign = context->orientationSign;
    GfVec3f position;
    GfVec3f normal;
    GfVec3f dPdu;
    GfVec3f dPdv;
    if (!_InterpolateBaseFrame(
            geometry, primID, u, v, orientationSign,
            &position, &normal, &dPdu, &dPdv)) {
        return false;
    }

    float displacement = 0.0f;
    if (!HdEmbreeEvaluateDisplacement(
            context, primID, u, v,
            position, normal, dPdu, dPdv, &displacement)) {
        return false;
    }

    GfVec3f objectOffset;
    GfVec3f displacedPosition;
    if (!HdEmbreeComputeObjectSpaceDisplacementOffset(
            context, normal, displacement, &objectOffset) ||
        !_TryAddOffset(position, objectOffset, &displacedPosition)) {
        return false;
    }

    *outPosition = displacedPosition;
    return true;
}

bool
HdEmbreeComputeDisplacedSubdivFrame(
    RTCGeometry geometry,
    HdEmbreePrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    HdEmbreeDisplacedSubdivFrame* outFrame)
{
    if (!geometry || !context || !outFrame ||
        !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    const float orientationSign = context->orientationSign;

    GfVec3f centerObjectOffset;
    GfVec3f centerBaseDPdu;
    GfVec3f centerBaseDPdv;
    if (!_EvaluateDisplacedSubdivProbe(
            geometry, context, primID, u, v, orientationSign,
            &centerObjectOffset, &centerBaseDPdu, &centerBaseDPdv)) {
        return false;
    }

    auto const* faceVertexCounts = static_cast<uint32_t const*>(
        rtcGetGeometryBufferData(geometry, RTC_BUFFER_TYPE_FACE, 0));
    if (!faceVertexCounts) {
        return false;
    }
    const bool isQuad = faceVertexCounts[primID] == 4;
    const float du = _GetFiniteDifferenceOffset(u, isQuad);
    const float dv = _GetFiniteDifferenceOffset(v, isQuad);

    GfVec3f uObjectOffset;
    GfVec3f uBaseDPdu;
    GfVec3f uBaseDPdv;
    if (!_EvaluateDisplacedSubdivProbe(
            geometry, context, primID, u + du, v, orientationSign,
            &uObjectOffset, &uBaseDPdu, &uBaseDPdv)) {
        return false;
    }

    GfVec3f vObjectOffset;
    GfVec3f vBaseDPdu;
    GfVec3f vBaseDPdv;
    if (!_EvaluateDisplacedSubdivProbe(
            geometry, context, primID, u, v + dv, orientationSign,
            &vObjectOffset, &vBaseDPdu, &vBaseDPdv)) {
        return false;
    }

    GfVec3f displacedNormal;
    GfVec3f displacedDPdu;
    GfVec3f displacedDPdv;
    if (!_TryBuildDisplacedNormal(
            centerBaseDPdu, centerBaseDPdv,
            centerObjectOffset, uObjectOffset, vObjectOffset,
            du, dv, orientationSign,
            &displacedNormal, &displacedDPdu, &displacedDPdv)) {
        return false;
    }

    HdEmbreeDisplacedSubdivFrame result;
    result.normal = displacedNormal;
    result.dPdu = displacedDPdu;
    result.dPdv = displacedDPdv;
    result.uObjectOffset = uObjectOffset;
    result.vObjectOffset = vObjectOffset;
    result.uBaseDPdu = uBaseDPdu;
    result.uBaseDPdv = uBaseDPdv;
    result.vBaseDPdu = vBaseDPdu;
    result.vBaseDPdv = vBaseDPdv;
    result.primID = primID;
    result.u = u;
    result.v = v;
    result.du = du;
    result.dv = dv;
    result.valid = true;
    *outFrame = result;
    return true;
}

bool
HdEmbreeComputeDisplacedSubdivFrame(
    RTCGeometry geometry,
    HdEmbreePrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* outNormal,
    GfVec3f* outDPdu,
    GfVec3f* outDPdv)
{
    if (!outNormal || !outDPdu || !outDPdv) {
        return false;
    }

    HdEmbreeDisplacedSubdivFrame frame;
    if (!HdEmbreeComputeDisplacedSubdivFrame(
            geometry, context, primID, u, v, &frame)) {
        return false;
    }
    *outNormal = frame.normal;
    *outDPdu = frame.dPdu;
    *outDPdv = frame.dPdv;
    return true;
}

bool
HdEmbreeComputeDisplacedSubdivNormalDerivatives(
    RTCGeometry geometry,
    HdEmbreePrototypeContext const* context,
    HdEmbreeDisplacedSubdivFrame const& frame,
    GfVec3f* outDndu,
    GfVec3f* outDndv)
{
    if (!geometry || !context || !frame.valid || !outDndu || !outDndv ||
        !std::isfinite(frame.u) || !std::isfinite(frame.v) ||
        !std::isfinite(frame.du) || !std::isfinite(frame.dv) ||
        frame.du == 0.0f || frame.dv == 0.0f ||
        !_IsFinite(frame.normal) ||
        !_IsFinite(frame.uObjectOffset) ||
        !_IsFinite(frame.vObjectOffset) ||
        !_IsFinite(frame.uBaseDPdu) || !_IsFinite(frame.uBaseDPdv) ||
        !_IsFinite(frame.vBaseDPdu) || !_IsFinite(frame.vBaseDPdv)) {
        return false;
    }

    const float orientationSign = context->orientationSign;
    GfVec3f uuObjectOffset;
    if (!_EvaluateDisplacedSubdivProbe(
            geometry, context, frame.primID,
            frame.u + 2.0f * frame.du, frame.v, orientationSign,
            &uuObjectOffset, nullptr, nullptr)) {
        return false;
    }

    GfVec3f uvObjectOffset;
    if (!_EvaluateDisplacedSubdivProbe(
            geometry, context, frame.primID,
            frame.u + frame.du, frame.v + frame.dv, orientationSign,
            &uvObjectOffset, nullptr, nullptr)) {
        return false;
    }

    GfVec3f vvObjectOffset;
    if (!_EvaluateDisplacedSubdivProbe(
            geometry, context, frame.primID,
            frame.u, frame.v + 2.0f * frame.dv, orientationSign,
            &vvObjectOffset, nullptr, nullptr)) {
        return false;
    }

    GfVec3f normalU;
    if (!_TryBuildDisplacedNormal(
            frame.uBaseDPdu, frame.uBaseDPdv,
            frame.uObjectOffset, uuObjectOffset, uvObjectOffset,
            frame.du, frame.dv, orientationSign,
            &normalU, nullptr, nullptr)) {
        return false;
    }

    GfVec3f normalV;
    if (!_TryBuildDisplacedNormal(
            frame.vBaseDPdu, frame.vBaseDPdv,
            frame.vObjectOffset, uvObjectOffset, vvObjectOffset,
            frame.du, frame.dv, orientationSign,
            &normalV, nullptr, nullptr)) {
        return false;
    }

    if (GfDot(normalU, frame.normal) < 0.0f) {
        normalU = -normalU;
    }
    if (GfDot(normalV, frame.normal) < 0.0f) {
        normalV = -normalV;
    }

    GfVec3f dNdu;
    GfVec3f dNdv;
    if (!_TryFiniteDifference(normalU, frame.normal, frame.du, &dNdu) ||
        !_TryFiniteDifference(normalV, frame.normal, frame.dv, &dNdv)) {
        return false;
    }
    dNdu -= frame.normal * GfDot(frame.normal, dNdu);
    dNdv -= frame.normal * GfDot(frame.normal, dNdv);
    if (!_IsFinite(dNdu) || !_IsFinite(dNdv)) {
        return false;
    }

    *outDndu = dNdu;
    *outDndv = dNdv;
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
