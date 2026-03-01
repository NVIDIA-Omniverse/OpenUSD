"""Minimal headless Embree render test — avoids GL compositing."""
import sys, os, traceback

USD_INSTALL = os.environ.get("USD_INSTALL",
    os.path.join(os.path.dirname(__file__), "../../../../../../../_usd_install"))
sys.path.insert(0, os.path.join(USD_INSTALL, "lib", "python"))

from pxr import Usd, UsdGeom, Sdf, Gf, Tf

def main():
    if len(sys.argv) < 2:
        print("Usage: python debug_render.py <scene.usda>")
        sys.exit(1)

    scene_path = sys.argv[1]
    print(f"Opening: {scene_path}")

    stage = Usd.Stage.Open(scene_path)
    if not stage:
        print("ERROR: Could not open stage")
        sys.exit(1)

    for prim in stage.Traverse():
        print(f"  Prim: {prim.GetPath()} ({prim.GetTypeName()})")

    # Test material resource retrieval (simulate what HdEmbreeMaterial::Sync does)
    print("\nTesting material resource retrieval...")
    for prim in stage.Traverse():
        if prim.GetTypeName() == "Material":
            mat_path = prim.GetPath()
            print(f"\n  Material: {mat_path}")
            mat = UsdGeom.Subset(prim) if False else None

            from pxr import UsdShade
            mat_prim = UsdShade.Material(prim)
            surface_output = mat_prim.GetSurfaceOutput()
            print(f"    Surface output: {surface_output}")
            if surface_output:
                source_info = surface_output.GetConnectedSources()
                print(f"    Connected sources: {source_info}")

            mtlx_output = mat_prim.GetSurfaceOutput("mtlx")
            print(f"    MtlX surface output: {mtlx_output}")
            if mtlx_output:
                source_info = mtlx_output.GetConnectedSources()
                print(f"    Connected sources: {source_info}")

        elif prim.GetTypeName() == "Shader":
            from pxr import UsdShade
            shader = UsdShade.Shader(prim)
            impl = shader.GetImplementationSourceAttr()
            shader_id = shader.GetIdAttr()
            print(f"  Shader: {prim.GetPath()}")
            print(f"    id: {shader_id.Get() if shader_id else 'none'}")

    # Test binding resolution
    print("\nTesting material bindings...")
    for prim in stage.Traverse():
        from pxr import UsdShade
        binding = UsdShade.MaterialBindingAPI(prim)
        if binding:
            mat, rel = binding.ComputeBoundMaterial()
            if mat:
                print(f"  {prim.GetPath()} -> {mat.GetPath()}")

    print("\nDone. No crash in USD layer.")
    print("If usdview crashes, the issue is in Hydra sync/render.")

if __name__ == "__main__":
    main()
