//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Camera and lens sampling, primary-ray construction, and ray differentials.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "../rendererImpl.h"

PXR_NAMESPACE_OPEN_SCOPE

void
HdEmbreeRenderer::_SampleCameraRay(
    unsigned int x, unsigned int y,
    unsigned int imageMinX, unsigned int imageMinY,
    HdEmbreeSampler& sampler,
    GfVec3f& rayOrigin, GfVec3f& rayDirection,
    HdEmbreeRayDifferential& rayDifferential) const
{
    // Jitter the camera ray direction.
    GfVec2f jitter(0.0f, 0.0f);
    if (_settings.jitterCamera) {
        jitter = sampler.RootDomain()
            .Fork(HdEmbreeSampleDomainKey::CameraJitter)
            .Draw2D();
    }

    // Un-transform the pixel's NDC coordinates through the
    // projection matrix to get the trace of the camera ray in the
    // near plane.
    const float w(_dataWindow.GetWidth());
    const float h(_dataWindow.GetHeight());

    const GfVec3f ndc(
        2.0f * ((x + jitter[0] - imageMinX) / w) - 1.0f,
        2.0f * ((y + jitter[1] - imageMinY) / h) - 1.0f,
        -1.0f);
    const GfVec3f nearPlaneTrace(_inverseProjMatrix.Transform(ndc));

    const bool isOrthographic = round(_projMatrix[3][3]) == 1.0;
    GfVec3f originCamera;
    GfVec3f dirCamera;
    if (isOrthographic) {
        // During orthographic projection: trace parallel rays
        // from the near plane trace.
        originCamera = nearPlaneTrace;
        dirCamera = GfVec3f(0.0f, 0.0f, -1.0f);
    } else {
        // Otherwise, assume this is a perspective projection;
        // project from the camera origin through the
        // near plane trace.
        originCamera = GfVec3f(0.0f, 0.0f, 0.0f);
        dirCamera = nearPlaneTrace;
    }

    const bool cameraDofEnabled =
        _IsCameraDepthOfFieldEnabled(
            _cameraDepthOfField, isOrthographic);
    GfVec2f lensPoint(0.0f);
    bool appliedCameraDof = false;
    if (cameraDofEnabled) {
        const GfVec2f lensSample =
            sampler.RootDomain()
                .Fork(HdEmbreeSampleDomainKey::CameraLens)
                .Draw2D();
        const GfVec2f lensDisk =
            _SampleUniformDiskConcentric(lensSample);
        const float lensRadius =
            _GetLensRadius(_cameraDepthOfField);
        lensPoint = GfVec2f(
            lensDisk[0] * lensRadius,
            lensDisk[1] * lensRadius);
        appliedCameraDof = _ApplyCameraDepthOfField(
            _cameraDepthOfField, lensPoint,
            &originCamera, &dirCamera);
    }

    // Transform camera rays to world space.
    GfVec3f origin =
        GfVec3f(_inverseViewMatrix.Transform(originCamera));
    GfVec3f dir = GfVec3f(
        _inverseViewMatrix.TransformDir(dirCamera)).GetNormalized();

    // --- Ray differential ---
    HdEmbreeRayDifferential rayDiff;
    {
        const GfVec3f ndcDx(
            2.0f * ((x + 1.0f + jitter[0] - imageMinX) / w) - 1.0f,
            2.0f * ((y + jitter[1] - imageMinY) / h) - 1.0f,
            -1.0f);
        const GfVec3f ndcDy(
            2.0f * ((x + jitter[0] - imageMinX) / w) - 1.0f,
            2.0f * ((y + 1.0f + jitter[1] - imageMinY) / h) - 1.0f,
            -1.0f);
        const GfVec3f nearDx(
            _inverseProjMatrix.Transform(ndcDx));
        const GfVec3f nearDy(
            _inverseProjMatrix.Transform(ndcDy));

        GfVec3f originDxCamera;
        GfVec3f originDyCamera;
        GfVec3f dirDxCamera;
        GfVec3f dirDyCamera;
        if (isOrthographic) {
            originDxCamera = nearDx;
            originDyCamera = nearDy;
            dirDxCamera = dirCamera;
            dirDyCamera = dirCamera;
        } else {
            originDxCamera = GfVec3f(0.0f);
            originDyCamera = GfVec3f(0.0f);
            dirDxCamera = nearDx;
            dirDyCamera = nearDy;
        }

        bool rayDiffValid = true;
        if (appliedCameraDof) {
            rayDiffValid =
                _ApplyCameraDepthOfField(
                    _cameraDepthOfField, lensPoint,
                    &originDxCamera, &dirDxCamera) &&
                _ApplyCameraDepthOfField(
                    _cameraDepthOfField, lensPoint,
                    &originDyCamera, &dirDyCamera);
        }

        if (rayDiffValid) {
            rayDiff.rxOrigin = GfVec3f(
                _inverseViewMatrix.Transform(originDxCamera));
            rayDiff.ryOrigin = GfVec3f(
                _inverseViewMatrix.Transform(originDyCamera));
            rayDiff.rxDirection = GfVec3f(
                _inverseViewMatrix.TransformDir(dirDxCamera))
                .GetNormalized();
            rayDiff.ryDirection = GfVec3f(
                _inverseViewMatrix.TransformDir(dirDyCamera))
                .GetNormalized();
            rayDiff.hasDifferentials = true;
        }

        // Scale by 1/sqrt(spp) to match sampling rate.
        if (rayDiff.hasDifferentials &&
            _settings.samplesToConvergence > 1) {
            float scale = 1.0f / std::sqrt(
                static_cast<float>(_settings.samplesToConvergence));
            if (isOrthographic) {
                GfVec3f dOx = rayDiff.rxOrigin - origin;
                GfVec3f dOy = rayDiff.ryOrigin - origin;
                rayDiff.rxOrigin = origin + dOx * scale;
                rayDiff.ryOrigin = origin + dOy * scale;
            } else {
                GfVec3f dDx = rayDiff.rxDirection - dir;
                GfVec3f dDy = rayDiff.ryDirection - dir;
                rayDiff.rxDirection =
                    (dir + dDx * scale).GetNormalized();
                rayDiff.ryDirection =
                    (dir + dDy * scale).GetNormalized();
            }
        }
    }

    rayOrigin = origin;
    rayDirection = dir;
    rayDifferential = rayDiff;
}

PXR_NAMESPACE_CLOSE_SCOPE
