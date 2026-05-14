from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer


class RenderLabContainer(PluginContainer):

    def registerPlugins(self, plugRegistry, plugCtx):
        from . import domeLightManipulator
        renderLab = self.deferredImport(".renderLab")

        domeLightManipulator.Install(plugCtx)

        self._openOverview = plugRegistry.registerCommandPlugin(
            "RenderLabContainer.openOverview",
            "Open RenderLab",
            renderLab.OpenOverview,
            "Open the RenderLab overview window.")
        self._reloadRenderLab = plugRegistry.registerCommandPlugin(
            "RenderLabContainer.reloadRenderLab",
            "Reload RenderLab",
            renderLab.ReloadRenderLab,
            "Reload RenderLab Python modules and reopen the window.")

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("RenderLab")
        menu.addItem(self._openOverview)
        menu.addItem(self._reloadRenderLab, "Ctrl+Alt+R")


Tf.Type.Define(RenderLabContainer)
