//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "intersectionFilter.h"

#include "context.h"

#include "pxr/base/tf/diagnostic.h"

PXR_NAMESPACE_OPEN_SCOPE

bool
ty::IsOpenCurveEndpoint(
    ty::PrototypeContext const& context,
    unsigned int primitiveId,
    float localU) noexcept
{
    if (context.geometryKind != ty::GeometryKind::roundCurve ||
        context.curveRepresentation !=
            ty::CurveGeometryRepresentation::roundLinear ||
        primitiveId >= context.curvePrimitiveMetadata.size()) {
        return false;
    }

    ty::CurveSegmentMetadata const& metadata =
        context.curvePrimitiveMetadata[primitiveId];
    return (localU == 0.0f &&
            metadata.isAuthoredCurveStart &&
            metadata.authoredU0 == 0.0f) ||
        (localU == 1.0f &&
         metadata.isAuthoredCurveEnd &&
         metadata.authoredU1 == 1.0f);
}

void
ty::PrototypeGeometryFilter(
    RTCFilterFunctionNArguments const* arguments)
{
    if (arguments == nullptr) {
        TF_CODING_ERROR("PrototypeGeometryFilter got a null argument");
        return;
    }
    if (arguments->valid == nullptr ||
        arguments->ray == nullptr ||
        arguments->hit == nullptr) {
        TF_CODING_ERROR("PrototypeGeometryFilter got incomplete arguments");
        return;
    }

    ty::PrototypeContext const* const context =
        static_cast<ty::PrototypeContext const*>(
            arguments->geometryUserPtr);
    if (context == nullptr) {
        TF_CODING_ERROR("PrototypeGeometryFilter got no prototype context");
        return;
    }

    // Embree may call this function for scalar or packet candidates. Never
    // revive a lane that an earlier filter or intersector already rejected.
    for (unsigned int lane = 0; lane < arguments->N; ++lane) {
        if (arguments->valid[lane] != -1) {
            continue;
        }

        unsigned int const primitiveId =
            RTCHitN_primID(arguments->hit, arguments->N, lane);
        float const localU =
            RTCHitN_u(arguments->hit, arguments->N, lane);
        if (ty::IsOpenCurveEndpoint(*context, primitiveId, localU)) {
            arguments->valid[lane] = 0;
            continue;
        }

        if (RTCRayN_id(arguments->ray, arguments->N, lane) ==
            ty::FaceCullBypassRayId) {
            continue;
        }

        float const facing = context->orientationSign * (
            RTCHitN_Ng_x(arguments->hit, arguments->N, lane) *
                RTCRayN_dir_x(arguments->ray, arguments->N, lane) +
            RTCHitN_Ng_y(arguments->hit, arguments->N, lane) *
                RTCRayN_dir_y(arguments->ray, arguments->N, lane) +
            RTCHitN_Ng_z(arguments->hit, arguments->N, lane) *
                RTCRayN_dir_z(arguments->ray, arguments->N, lane));
        bool const isFrontFace = facing < 0.0f;

        bool cull = false;
        switch (context->cullStyle) {
        case HdCullStyleBack:
            cull = !isFrontFace;
            break;
        case HdCullStyleFront:
            cull = isFrontFace;
            break;
        case HdCullStyleBackUnlessDoubleSided:
            cull = !isFrontFace && !context->doubleSided;
            break;
        case HdCullStyleFrontUnlessDoubleSided:
            cull = isFrontFace && !context->doubleSided;
            break;
        default:
            break;
        }
        if (cull) {
            arguments->valid[lane] = 0;
        }
    }
}

void
ty::BindPrototypeGeometryFilter(RTCGeometry geometry)
{
    if (geometry == nullptr) {
        TF_CODING_ERROR("Cannot bind a filter to a null Embree geometry");
        return;
    }
    ty::SetGeometryFilterFunctions(
        geometry, ty::PrototypeGeometryFilter);
}

PXR_NAMESPACE_CLOSE_SCOPE
