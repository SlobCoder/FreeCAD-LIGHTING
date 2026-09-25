# FreeCAD-LIGHTING

**FreeCAD-LIGHTING** is an unofficial community fork of
[FreeCAD](https://github.com/FreeCAD/FreeCAD) (26.3) carrying a rendering
and performance stack aimed at large assemblies and heavy models.
It is **not affiliated with or endorsed by** the FreeCAD project or the
FreeCAD Project Association.

[![Release](https://img.shields.io/badge/release-v26.3--LIGHTING-blue)](https://github.com/SlobCoder/FreeCAD-LIGHTING/releases/tag/v26.3-LIGHTING)
&nbsp; [AppImage download](https://github.com/SlobCoder/FreeCAD-LIGHTING/releases/tag/v26.3-LIGHTING)
&nbsp;·&nbsp; [FORK-CHANGES.md](FORK-CHANGES.md)
&nbsp;·&nbsp; [Companion Coin fork](https://github.com/SlobCoder/coin)

## What this fork changes

| # | Change | Default |
|---|--------|---------|
| 1 | Transparency via `SORTED_OBJECT_BLEND` — no per-triangle CPU sorting each frame | on |
| 2 | Coin vertex-array render path for per-part colors (no immediate-mode color sends) | on |
| 3 | Color VBO keeps *transparent* bodies on the vertex-array path too | on |
| 4 | BVH-accelerated ray picking for `SoBrepFaceSet` — face-picking cost essentially eliminated from the frame profile | on |
| 5 | Coarser interior meshing (`DeflectionInterior` / `AngleInterior`, boundary edges keep full precision) | on (4x / 2x) |
| 6 | Optional embedded triangulation cache inside the BRep stream (`CacheTriangulation`) | off |
| 7 | Sidecar mesh cache with zstd compression — warm loads skip BRepMesh entirely (measured 16.6 s → 0.8 s on a 494k-triangle, 2137-face model; blobs shrink to ~23–33 % at >1.4 GB/s decompression) | on |
| 8 | Configurable transparency type, incl. **PPLL order-independent transparency** (`TransparencyType=11`) via the [Coin fork](https://github.com/SlobCoder/coin) | `SORTED_OBJECT_BLEND` |
| 9 | **TSSAA 2TX** temporal supersampling (`AntiAliasing=6`): jittered MSAA accumulation with a direct-GL present path; edit mode automatically falls back to sorted transparency (driver-race workaround) | off |

Full details, measured numbers and file lists: [FORK-CHANGES.md](FORK-CHANGES.md).

## Preferences cheat sheet

| Preference | Values |
|------------|--------|
| `BaseApp/Preferences/View/TransparencyType` | 0 = `SORTED_OBJECT_BLEND` (default) · 11 = PPLL OIT (needs fork Coin, OpenGL 4.3+) |
| `BaseApp/Preferences/View/AntiAliasing` | 0–5 = stock modes · 6 = TSSAA 2TX |
| `Mod/Part/MeshCacheEnabled` / `MeshCacheDirectory` / `MeshCacheMaxSize` / `MeshCacheCompressionLevel` | sidecar mesh cache (default: on, XDG cache, zstd level 3) |
| `Mod/Part/MeshInteriorDeflection` / `MeshInteriorAngle` | interior meshing coarseness (default 4x / 2x) |
| `Mod/Part/CacheTriangulation` | embedded BRep triangulation cache (opt-in) |

Diagnostic knobs: `FREECAD_DEBUG_TSSAA=1` (TSSAA/GL-present logging) and the
Coin fork's `COIN_PPLL_*` environment variables.

## PPLL and the Coin fork

PPLL order-independent transparency is implemented in the companion fork
[SlobCoder/coin](https://github.com/SlobCoder/coin) (branch `lighting-main`).
When you build FreeCAD-LIGHTING, that Coin is bundled automatically via the
`src/3rdParty/coin` submodule. For setups linking a system Coin, the
`freecad-ppll` launcher script starts FreeCAD with the fork Coin preloaded.
Unsupported GL contexts fall back to `SORTED_OBJECT_BLEND` with a warning.

## Download

Prebuilt **AppImage** (Linux x86_64, conda-based toolchain for a portable
glibc baseline, includes the fork Coin with PPLL and TSSAA):
[v26.3-LIGHTING release](https://github.com/SlobCoder/FreeCAD-LIGHTING/releases/tag/v26.3-LIGHTING)
— `.AppImage`, `.zsync` (delta updates) and `SHA256` provided. Smoke-tested
(`freecadcmd` + Pivy/Coin import) before upload.

## Building

Standard FreeCAD build; the bundled Coin/Pivy submodules are used:

```sh
git submodule update --init --recursive
# then configure & build as usual (see the FreeCAD Developers Handbook)
```

For a reproducible release bundle matching the published AppImage, use the
pixi-based packaging: `package/bundle/build.sh` followed by
`package/bundle/linux/create_bundle.sh`.

## Base

Branched from FreeCAD/FreeCAD `main` @ `c2b56826` (2026-09-21).

## License & trademark

All code remains licensed under the **LGPL-2.1-or-later**. Every file
modified relative to upstream carries a prominent header notice per
LGPL-2.1 §2b (`Modified 2026 by SlobCoder for the FreeCAD-LIGHTING fork`);
files added by this fork carry their own headers. See `FORK-CHANGES.md`.

"FreeCAD" and the FreeCAD logo are trademarks of the FreeCAD Project
Association. This fork uses the name referentially to indicate provenance
and is not an official FreeCAD distribution.
