from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer


class RenderLabContainer(PluginContainer):

    def registerPlugins(self, plugRegistry, plugCtx):
        renderLab = self.deferredImport(".renderLab")

        self._openOverview = plugRegistry.registerCommandPlugin(
            "RenderLabContainer.openOverview",
            "Open RenderLab",
            renderLab.OpenOverview,
            "Open the RenderLab overview window.")
        self._openRenderSettings = plugRegistry.registerCommandPlugin(
            "RenderLabContainer.openRenderSettings",
            "Render Settings",
            renderLab.OpenRenderSettings,
            "Open RenderLab render settings.")
        self._openCameraTools = plugRegistry.registerCommandPlugin(
            "RenderLabContainer.openCameraTools",
            "Camera Tools",
            renderLab.OpenCameraTools,
            "Open RenderLab camera controls.")
        self._openLightTools = plugRegistry.registerCommandPlugin(
            "RenderLabContainer.openLightTools",
            "Light Tools",
            renderLab.OpenLightTools,
            "Open RenderLab light controls.")
        self._openMaterialTools = plugRegistry.registerCommandPlugin(
            "RenderLabContainer.openMaterialTools",
            "Material Tools",
            renderLab.OpenMaterialTools,
            "Open RenderLab material controls.")

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("RenderLab")
        menu.addItem(self._openOverview)
        menu.addSeparator()
        menu.addItem(self._openRenderSettings)

        lightsMenu = menu.findOrCreateSubmenu("Lights")
        lightsMenu.addItem(self._openLightTools)

        camerasMenu = menu.findOrCreateSubmenu("Cameras")
        camerasMenu.addItem(self._openCameraTools)

        materialsMenu = menu.findOrCreateSubmenu("Materials")
        materialsMenu.addItem(self._openMaterialTools)


Tf.Type.Define(RenderLabContainer)
