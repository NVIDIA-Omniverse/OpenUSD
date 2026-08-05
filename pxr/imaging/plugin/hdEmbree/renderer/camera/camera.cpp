//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Camera and lens sampling, primary-ray construction, and ray differentials.

#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

PXR_NAMESPACE_OPEN_SCOPE

static bool
_IsCameraDepthOfFieldEnabled(ty::CameraDepthOfField const& dof,
                             bool isOrthographic)
{
    return !isOrthographic &&
           std::isfinite(dof.fStop) &&
           std::isfinite(dof.focusDistance) &&
           std::isfinite(dof.focalLength) &&
           dof.fStop > 0.0f &&
           dof.focusDistance > 0.0f &&
           dof.focalLength > 0.0f;
}

static float
_GetLensRadius(ty::CameraDepthOfField const& dof)
{
    return dof.focalLength / (2.0f * dof.fStop);
}

static GfVec2f
_SampleUniformDiskConcentric(GfVec2f const& sample)
{
    const float x = 2.0f * sample[0] - 1.0f;
    const float y = 2.0f * sample[1] - 1.0f;

    if (x == 0.0f && y == 0.0f) {
        return GfVec2f(0.0f);
    }

    float r;
    float angleAzimuth;
    if (std::abs(x) > std::abs(y)) {
        r = x;
        angleAzimuth = (ty::Pi / 4.0f) * (y / x);
    } else {
        r = y;
        angleAzimuth = (ty::Pi / 2.0f) - (ty::Pi / 4.0f) * (x / y);
    }

    return GfVec2f(r * std::cos(angleAzimuth), r * std::sin(angleAzimuth));
}

static bool
_ApplyCameraDepthOfField(ty::CameraDepthOfField const& dof,
                         GfVec2f const& lensPoint, GfVec3f* posRayOrgCamera,
                         GfVec3f* dirRayCamera)
{
    constexpr float eps = 1.0e-7f;

    if (!posRayOrgCamera || !dirRayCamera ||
        !ty::IsFinite(*posRayOrgCamera) || !ty::IsFinite(*dirRayCamera)) {
        return false;
    }

    const float dz = (*dirRayCamera)[2];
    if (!std::isfinite(dz) || std::abs(dz) < eps) {
        return false;
    }

    const float focusT = -dof.focusDistance / dz;
    if (!std::isfinite(focusT) || focusT <= 0.0f) {
        return false;
    }

    const GfVec3f posFocusCamera =
        *posRayOrgCamera + (*dirRayCamera) * focusT;
    const GfVec3f posLensCamera(lensPoint[0], lensPoint[1], 0.0f);
    const GfVec3f dirDofCamera = posFocusCamera - posLensCamera;
    if (!ty::IsFinite(dirDofCamera) ||
        dirDofCamera.GetLengthSq() <= eps * eps) {
        return false;
    }

    *posRayOrgCamera = posLensCamera;
    *dirRayCamera = dirDofCamera;
    return true;
}

void
ty::Renderer::_SampleCameraRay(
    unsigned int x, unsigned int y,
    unsigned int imageMinX, unsigned int imageMinY,
    ty::Sampler& sampler,
    GfVec3f& outPosRayOrgWld, GfVec3f& outDirRayWld,
    ty::RayDifferential& outDiffRay) const
{
    // Jitter the camera ray direction.
    const GfVec2f jitter = sampler.RootDomain()
        .Fork(ty::SampleDomainKey::CameraJitter)
        .Draw2D();

    // Un-transform the pixel's NDC coordinates through the
    // projection matrix to get the trace of the camera ray in the
    // near plane.
    const float imageWidth(_dataWindow.GetWidth());
    const float imageHeight(_dataWindow.GetHeight());

    const GfVec3f ndc(
        2.0f * ((x + jitter[0] - imageMinX) / imageWidth) - 1.0f,
        2.0f * ((y + jitter[1] - imageMinY) / imageHeight) - 1.0f,
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
                .Fork(ty::SampleDomainKey::CameraLens)
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
    GfVec3f posRayOrgWld =
        GfVec3f(_inverseViewMatrix.Transform(originCamera));
    GfVec3f dirRayWld = GfVec3f(
        _inverseViewMatrix.TransformDir(dirCamera)).GetNormalized();

    // --- Ray differential ---
    ty::RayDifferential diffRay;
    {
        const GfVec3f ndcDx(
            2.0f * ((x + 1.0f + jitter[0] - imageMinX) / imageWidth) - 1.0f,
            2.0f * ((y + jitter[1] - imageMinY) / imageHeight) - 1.0f,
            -1.0f);
        const GfVec3f ndcDy(
            2.0f * ((x + jitter[0] - imageMinX) / imageWidth) - 1.0f,
            2.0f * ((y + 1.0f + jitter[1] - imageMinY) / imageHeight) - 1.0f,
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

        bool diffRayValid = true;
        if (appliedCameraDof) {
            diffRayValid =
                _ApplyCameraDepthOfField(
                    _cameraDepthOfField, lensPoint,
                    &originDxCamera, &dirDxCamera) &&
                _ApplyCameraDepthOfField(
                    _cameraDepthOfField, lensPoint,
                    &originDyCamera, &dirDyCamera);
        }

        if (diffRayValid) {
            diffRay.rxOrigin = GfVec3f(
                _inverseViewMatrix.Transform(originDxCamera));
            diffRay.ryOrigin = GfVec3f(
                _inverseViewMatrix.Transform(originDyCamera));
            diffRay.rxDirection = GfVec3f(
                _inverseViewMatrix.TransformDir(dirDxCamera))
                .GetNormalized();
            diffRay.ryDirection = GfVec3f(
                _inverseViewMatrix.TransformDir(dirDyCamera))
                .GetNormalized();
            diffRay.hasDifferentials = true;
        }

        // Scale by 1/sqrt(spp) to match sampling rate.
        if (diffRay.hasDifferentials &&
            _settings.samplesToConvergence > 1) {
            float scale = 1.0f / std::sqrt(
                static_cast<float>(_settings.samplesToConvergence));
            if (isOrthographic) {
                GfVec3f dPosOrgdx = diffRay.rxOrigin - posRayOrgWld;
                GfVec3f dPosOrgdy = diffRay.ryOrigin - posRayOrgWld;
                diffRay.rxOrigin = posRayOrgWld + dPosOrgdx * scale;
                diffRay.ryOrigin = posRayOrgWld + dPosOrgdy * scale;
            } else {
                GfVec3f dDirRaydx = diffRay.rxDirection - dirRayWld;
                GfVec3f dDirRaydy = diffRay.ryDirection - dirRayWld;
                diffRay.rxDirection =
                    (dirRayWld + dDirRaydx * scale).GetNormalized();
                diffRay.ryDirection =
                    (dirRayWld + dDirRaydy * scale).GetNormalized();
            }
        }
    }

    outPosRayOrgWld = posRayOrgWld;
    outDirRayWld = dirRayWld;
    outDiffRay = diffRay;
}

PXR_NAMESPACE_CLOSE_SCOPE
