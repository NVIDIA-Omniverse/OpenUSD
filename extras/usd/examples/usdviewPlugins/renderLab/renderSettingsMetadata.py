"""Renderer-specific metadata for RenderLab render settings."""


_DEFAULT_CATEGORY = "Other"


_RENDERERS = {
    "hdEmbree": {
        "displayName": "Embree",
        "rendererIds": {
            "HdEmbreeRendererPlugin",
            "Embree",
        },
        "categories": [
            "Sampling",
            "Path Tracing",
            "Transparency",
            "Materials",
            "Diagnostics",
            "Scene",
            "Ambient Occlusion",
            _DEFAULT_CATEGORY,
        ],
        "settings": {
            "convergedSamplesPerPixel": {
                "category": "Sampling",
                "order": 10,
            },
            "randomNumberSeed": {
                "category": "Sampling",
                "order": 20,
            },
            "samplerSequence": {
                "category": "Sampling",
                "order": 30,
                "allowedTokens": [
                    "sobol",
                    "random",
                    "openqmc_sobol",
                    "openqmc_sobolbn",
                    "openqmc_pmj",
                    "openqmc_pmjbn",
                    "openqmc_lattice",
                    "openqmc_latticebn",
                ],
            },
            "enableAdaptiveSampling": {
                "category": "Sampling",
                "order": 40,
            },
            "adaptiveThreshold": {
                "category": "Sampling",
                "order": 50,
            },
            "minSamplesBeforeAdaptive": {
                "category": "Sampling",
                "order": 60,
            },
            "maxBounces": {
                "category": "Path Tracing",
                "order": 10,
            },
            "minBouncesBeforeRR": {
                "category": "Path Tracing",
                "order": 20,
            },
            "lightSamplesPerHit": {
                "category": "Path Tracing",
                "order": 30,
            },
            "stratifyLightSamples": {
                "category": "Path Tracing",
                "order": 40,
            },
            "enableCaustics": {
                "category": "Path Tracing",
                "order": 50,
            },
            "approxTransparentShadows": {
                "category": "Path Tracing",
                "order": 55,
            },
            "fireflyClampThreshold": {
                "category": "Path Tracing",
                "order": 60,
            },
            "causticsClampThreshold": {
                "category": "Path Tracing",
                "order": 70,
            },
            "enableGgxMicrofacetMultipleScattering": {
                "category": "Materials",
                "order": 10,
            },
            "materialRenderContext": {
                "category": "Materials",
                "order": 20,
                "allowedTokens": [
                    "mtlx",
                    "default",
                ],
            },
            "useAdobeOpenPBR": {
                "category": "Materials",
                "order": 30,
            },
            "dielectricLayerThroughputMode": {
                "category": "Materials",
                "order": 40,
                "allowedTokens": [
                    "bsdl",
                    "materialxGlsl",
                ],
            },
            "showAdaptiveHeatmap": {
                "category": "Diagnostics",
                "order": 10,
            },
            "enableSceneColors": {
                "category": "Scene",
                "order": 10,
            },
            "enableLighting": {
                "category": "Scene",
                "order": 20,
            },
            "domeLightCameraVisibility": {
                "category": "Scene",
                "order": 30,
            },
            "enableAmbientOcclusion": {
                "category": "Ambient Occlusion",
                "order": 10,
            },
            "ambientOcclusionSamples": {
                "category": "Ambient Occlusion",
                "order": 20,
            },
        },
    },
    "storm": {
        "displayName": "Storm",
        "rendererIds": {
            "HdStormRendererPlugin",
            "Storm",
            "GL",
        },
        "categories": [
            "Culling",
            "Volumes",
            "Lighting",
            _DEFAULT_CATEGORY,
        ],
        "settings": {
            "enableTinyPrimCulling": {
                "category": "Culling",
                "order": 10,
            },
            "volumeRaymarchingStepSize": {
                "category": "Volumes",
                "order": 10,
            },
            "volumeRaymarchingStepSizeLighting": {
                "category": "Volumes",
                "order": 20,
            },
            "volumeMaxTextureMemoryPerField": {
                "category": "Volumes",
                "order": 30,
            },
            "maxLights": {
                "category": "Lighting",
                "order": 10,
            },
            "domeLightCameraVisibility": {
                "category": "Lighting",
                "order": 20,
            },
            "domeLightCubemapTargetMemory": {
                "category": "Lighting",
                "order": 30,
            },
            "enableExposureCompensation": {
                "category": "Lighting",
                "order": 40,
            },
        },
    },
}


def _rendererConfig(rendererId):
    rendererId = str(rendererId or "")
    for config in _RENDERERS.values():
        if rendererId in config["rendererIds"]:
            return config
    return None


def hasRendererMetadata(rendererId):
    return _rendererConfig(rendererId) is not None


def getRendererDisplayName(rendererId):
    config = _rendererConfig(rendererId)
    if config:
        return config.get("displayName", str(rendererId or ""))
    return str(rendererId or "<none>")


def getSettingMetadata(rendererId, settingKey):
    config = _rendererConfig(rendererId)
    if not config:
        return {}
    metadata = config["settings"].get(str(settingKey), {})
    return dict(metadata)


def getSettingCategory(rendererId, settingKey):
    metadata = getSettingMetadata(rendererId, settingKey)
    return metadata.get("category", _DEFAULT_CATEGORY)


def orderSettings(rendererId, settings):
    config = _rendererConfig(rendererId)
    if not config:
        return list(settings)

    categories = config["categories"]
    categoryIndices = {
        category: index
        for index, category in enumerate(categories)
    }

    def sortKey(setting):
        key = str(setting.key)
        metadata = config["settings"].get(key, {})
        category = metadata.get("category", _DEFAULT_CATEGORY)
        return (
            categoryIndices.get(category, len(categories)),
            metadata.get("order", 1000),
            str(getattr(setting, "name", key)),
        )

    return sorted(settings, key=sortKey)
