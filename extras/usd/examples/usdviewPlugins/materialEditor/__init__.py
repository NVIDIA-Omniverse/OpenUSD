from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer


class MaterialEditorContainer(PluginContainer):

    def registerPlugins(self, plugRegistry, plugCtx):
        self._openEditor = plugRegistry.registerCommandPlugin(
            "MaterialEditorContainer.openEditor",
            "Open Material Editor",
            _OpenEditor)

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("Material")
        menu.addItem(self._openEditor)


Tf.Type.Define(MaterialEditorContainer)


def _OpenEditor(usdviewApi):
    from .materialEditor import OpenMaterialEditor
    OpenMaterialEditor(usdviewApi)
