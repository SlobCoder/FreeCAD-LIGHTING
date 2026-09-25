# FreeCAD-LIGHTING — Fork Changes

**FreeCAD-LIGHTING** is an unofficial community fork of
[FreeCAD](https://github.com/FreeCAD/FreeCAD) carrying a local rendering
and performance stack. It is **not affiliated with or endorsed by** the
FreeCAD project or the FreeCAD Project Association.

All code remains licensed under the **LGPL-2.1-or-later** (see `LICENSE`);
this fork imposes no additional restrictions.

## Modification notices (LGPL-2.1, Section 2b)

Per LGPL-2.1 §2b, every file modified relative to upstream carries a
prominent header notice:

> `Modified 2026 by SlobCoder for the FreeCAD-LIGHTING fork`

Files added entirely by this fork (e.g. `src/Mod/Part/Gui/MeshCache.*`)
carry their own copyright headers and need no modification notice.

## Base

Branched from FreeCAD/FreeCAD `main` @ `c2b56826` (2026-09-21,
"CAM: LeadInOut - Skip zero length plunge moves (#32257)").

## What this fork changes

The stack is published here as a single squashed commit plus this notice
commit; the original per-commit history is preserved in the maintainer's
local repository. Logical changes, in original order:

### 1. Transparency rendering via SORTED_OBJECT_BLEND
`View3DInventorViewer::init()` switches the transparency type from
`SORTED_OBJECT_SORTED_TRIANGLE_BLEND` to `SORTED_OBJECT_BLEND`. The former
sorts every triangle of every transparent shape on the CPU each frame
(`SoPrimitiveVertexCache::depthSortTriangles`), which pegs the GUI thread
on complex models; object-level sorting keeps correct ordering between
bodies and only forgoes within-body triangle order.

### 2. Coin vertex-array path for per-part colors
`SoBrepFaceSet` publishes per-part colors such that Coin can use its fast
vertex-array render path instead of immediate-mode per-face color sends.

### 3. Color VBO keeps transparent bodies on the vertex-array path
Transparent bodies previously fell back from the vertex-array path to a
per-face immediate-mode color remap. Colors are now published through an
internal `SoMaterial` node so Coin registers a color VBO
(`SoGLVBOElement`), keeping the fast path for transparent bodies too.

### 4. BVH-accelerated ray picking for SoBrepFaceSet
`SoBrepFaceSet::rayPick()` builds a cached median-split BVH over the
triangles (leaf size 6, depth cap 34, invalidated by field sensors and
rebuilt lazily) instead of sweeping all triangles through
`generatePrimitives`. Small shapes and malformed data keep the stock path.
Measured on complex models: face-picking cost essentially eliminated from
the frame profile.

### 5. Coarser interior meshing
Face interiors are meshed with OCCT's separate `DeflectionInterior` /
`AngleInterior` parameters (default 4x / 2x coarser than the boundary
deflection; configurable via the `MeshInteriorDeflection` and
`MeshInteriorAngle` parameters in `Mod/Part`). Boundary edges keep full
precision.

### 6. Optional embedded triangulation cache (CacheTriangulation)
`TopoShape::exportBrep` can write triangulations into the BRep stream
(`BRepTools_ShapeSet` with triangles enabled) when the
`CacheTriangulation` preference is set — portable, opt-in, trades file
size for load speed.

### 7. Sidecar mesh cache (enabled by default in this fork)
`PartGui::MeshCache`: a machine-local triangulation cache (default under
the XDG cache location, size-capped with mtime eviction) keyed by SHA-256
over the meshing parameters, the OCCT version and a triangulation-free
BRep serialization of the shape. Warm document loads skip BRepMesh
entirely (measured 16.6s -> 0.8s on a 494k-triangle, 2137-face model);
intra-load deduplication also speeds up cold loads with identical shapes.
Enabled by default in this fork (an upstream-ready variant would default
to off). See `src/Mod/Part/Gui/MeshCache.h` for the preference knobs
(`MeshCacheEnabled`, `MeshCacheDirectory`, `MeshCacheMaxSize`).

Stored blobs are transparently compressed as a whole-file zstd frame
(`MeshCacheCompressionLevel`, default 3; 0 = uncompressed; reading always
supports both formats). Measured: level 3 shrinks blobs to ~23-33% of raw
size at >1.4 GB/s decompression, so warm loads gain 3x cache capacity
under the size cap for tens of milliseconds of decompression. Restore
validation is structural (every face meshed); the deflection-based gate
was dropped because OCCT reports the achieved chordal deviation on
angular-deflection-governed faces (e.g. analytic spheres), which legitimately
exceeds the requested linear deflection and would otherwise cause a
remesh on every load.

### 8. Configurable transparency type (incl. PPLL order-independent transparency)

`BaseApp/Preferences/View/TransparencyType` (int, matching
`SoGLRenderAction::TransparencyType`) selects the 3D view transparency
mode, with a combo box under Preferences -> Display -> 3D View
(default and fallback: `SORTED_OBJECT_BLEND`, unchanged from upstream).

The list includes `Per-pixel linked list (OIT)` (PPLL_BLEND, 11):
order-independent transparency provided by the Coin fork (per-pixel
linked list / A-buffer with per-sample MSAA compositing and per-sample
depth rejection), published at
<https://github.com/SlobCoder/coin> (branch `lighting-main`). It
requires the fork's libCoin at runtime (OpenGL
4.3+; unsupported contexts fall back to SORTED_OBJECT_BLEND with a
warning). The `freecad-ppll` launcher script starts FreeCAD with the
fork Coin preloaded.

The preference applies to all open 3D views immediately. Files changed:
`src/Gui/View3DInventorViewer.cpp`,
`src/Gui/PreferencePages/DlgSettings3DView.ui`,
`src/Gui/PreferencePages/DlgSettings3DViewImp.cpp`; added:
`freecad-ppll`.

### 9. TSSAA temporal supersampling (optional, AntiAliasing = 6)

Setting `BaseApp/Preferences/View/AntiAliasing` to 6 enables "TSSAA 2TX":
temporal supersampling antialiasing. Each frame is rendered into an
offscreen MSAA target at a jittered subpixel offset and accumulated into a
history buffer (2 samples per pixel, convergence over ~15 frames); the
resolved image is presented through a direct-GL blit path (bypassing
`QOpenGLWidget::blitFramebuffer`, whose framebuffer-state side effects
caused GL errors and invisible models with some drivers).

While any document is in edit mode (e.g. sketch editing), the viewer
temporarily renders the frame with `SORTED_OBJECT_BLEND` transparency
instead of PPLL and skips TSSAA accumulation — a workaround for a
Mesa/RADEONSI driver race in the PPLL machinery that could crash during
edit-mode graph churn; normal rendering resumes after leaving edit mode.

Also includes: audited (SoPath-audited) copies for `So3DAnnotation`
deferred paths (fixes a dangling-pointer crash on thumbnail saves), an
updated bundled `glext.h`/`khrplatform.h` (GL extension definitions
needed by the present path), and `FREECAD_DEBUG_TSSAA=1` /
`COIN_PPLL_*` diagnostic environment knobs. Files added:
`src/Gui/TemporalAA.cpp/h`; files changed:
`src/Gui/View3DInventorViewer.cpp/h`, `src/Gui/Multisample.cpp/h`,
`src/Gui/SoDevicePixelRatioElement.cpp/h`, `src/Gui/SoFCDB.cpp`,
`src/Gui/Inventor/So3DAnnotation.cpp`, `src/Gui/CMakeLists.txt`,
`src/3rdParty/OpenGL/api/**`.

## Intentionally not included

- The async-recompute work (FreeCAD PR 29757) that the original local
  stack was based on; this branch is a clean port onto current upstream
  `main` instead.
- Local OCCT 8.0 compatibility fixes that upstream has since resolved
  itself (`TopoShapeCache.h` include cleanup, `modelRefine.cpp`
  exception usage).

## Trademark note

"FreeCAD" and the FreeCAD logo are trademarks of the FreeCAD Project
Association. This fork uses the name referentially to indicate provenance
and is not an official FreeCAD distribution.
