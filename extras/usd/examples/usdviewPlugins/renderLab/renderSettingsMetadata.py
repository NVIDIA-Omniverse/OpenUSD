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
            "renderingColorSpace": {
                "category": "Scene",
                "order": 5,
                "allowedTokens": [
                    "lin_rec709_scene",
                    "lin_ap1_scene",
                    "data",
                ],
            },
            "ty:convergedSamplesPerPixel": {
                "category": "Sampling",
                "order": 10,
            },
            "ty:randomNumberSeed": {
                "category": "Sampling",
                "order": 20,
            },
            "ty:enableAdaptiveSampling": {
                "category": "Sampling",
                "order": 40,
            },
            "ty:adaptiveThreshold": {
                "category": "Sampling",
                "order": 50,
            },
            "ty:minSamplesBeforeAdaptive": {
                "category": "Sampling",
                "order": 60,
            },
            "ty:maxBounces": {
                "category": "Path Tracing",
                "order": 10,
            },
            "ty:minBouncesBeforeRR": {
                "category": "Path Tracing",
                "order": 20,
            },
            "ty:lightSamplesPerHit": {
                "category": "Path Tracing",
                "order": 30,
            },
            "ty:enableCaustics": {
                "category": "Path Tracing",
                "order": 50,
            },
            "ty:fireflyClampThreshold": {
                "category": "Path Tracing",
                "order": 60,
            },
            "ty:causticsClampThreshold": {
                "category": "Path Tracing",
                "order": 70,
            },
            "ty:materialRenderContext": {
                "category": "Materials",
                "order": 20,
                "allowedTokens": [
                    "mtlx",
                    "default",
                ],
            },
            "ty:useAdobeOpenPBR": {
                "category": "Materials",
                "order": 30,
            },
            "ty:dielectricLayerThroughputMode": {
                "category": "Materials",
                "order": 40,
                "allowedTokens": [
                    "bsdl",
                    "materialxGlsl",
                ],
            },
            "ty:showAdaptiveHeatmap": {
                "category": "Diagnostics",
                "order": 10,
            },
            "ty:tileSize": {
                "category": "Diagnostics",
                "order": 20,
            },
            "ty:enableLighting": {
                "category": "Scene",
                "order": 20,
            },
            "domeLightCameraVisibility": {
                "category": "Scene",
                "order": 30,
            },
            "ty:enableExposureCompensation": {
                "category": "Scene",
                "order": 40,
            },
            "ty:dynamicSubdvTesselation": {
                "category": "Scene",
                "order": 50,
            },
            "ty:enableAmbientOcclusion": {
                "category": "Ambient Occlusion",
                "order": 10,
            },
            "ty:ambientOcclusionSamples": {
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
