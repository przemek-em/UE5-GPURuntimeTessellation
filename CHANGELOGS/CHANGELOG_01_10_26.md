# GPU Runtime Tessellation - Changelog

## 2026-10-01 - Tiled heightfields and renderer integration

Adds an experimental terrain component for multiple heightmaps and fixes shadow submission, normal handling, primitive rendering state, editor lightmap UVs, and SM5 ocean shader compilation. Validated against the local Unreal Engine 5.8 source on Windows/DX12. The plugin remains experimental.

This entry is based on the supplied `GPURuntimeTessellation-BeforeChanges` folder compared with `Engine/Plugins/Experimental/GPURuntimeTessellation`. The source/shader comparison contains **14 modified files, 13 added files, and no removals**. The plugin manifest and module build rules are unchanged. Earlier changes already present in that baseline, including the RDG buffer-lifetime and grid-dispatch fixes, are covered by older entries.

### Added

- **Tiled heightfield component and actor:** `UGPUTiledTessellationComponent`, `AGPUTiledTessellationActor`, and the Blueprint-editable `FGPUTiledHeightmap` structure. Each heightmap has an integer tile coordinate; negative and sparse coordinates are supported.
- **GPU tile generation:** a dedicated compute shader generates positions, normals, and continuous UV0 coordinates. Tiles retain their generated GPU buffers and share one index buffer per component. Each tile has its own bounds and draw batch for camera/shadow culling.
- **Shared-border sampling:** heightmaps use duplicated border samples and texel-center mapping. Neighboring tiles supply finite-difference samples for border normals; missing neighbors use one-sided derivatives. Rectangular heightmaps and negative height scales are supported.
- **Tile configuration and diagnostics:** `TileSize`, `QuadsPerTile`, `HeightScale`, `HeightOffset`, and `GeometryBudgetMB`, plus `RebuildTiles`, `ValidateTiles`, and `GetEstimatedGeometryBytes`. Invalid configurations refuse the proxy build with a diagnostic. The default geometry budget is 256 MiB; supported output density is 1..1024 quads per tile edge.
- **PSO precache collection:** planar, arbitrary-mesh, and tiled components collect their materials, default-material fallback, and both tessellation vertex factories. Vertex factories advertise precache support and provide declarations matching their manual-fetch streams. Planar and tiled registration request precaching; the mesh component retains the inherited engine registration path. Vector/ocean components inherit planar support.
- **Nine automation tests** covering tile validation, GPU seams, baked lightmap UVs, rendered tile depth, normal transforms, height-mode shadow normals, large quadtree shadow coverage, offscreen patch shadows, and translated shadow culling volumes.

### Fixed

- **Offscreen terrain shadow casters:** spatial patches and quadtree patches now use the light's caster volume during shadow submission. Camera-frustum rejection and camera-only visibility metadata no longer discard existing patch buffers needed to shadow visible terrain.
- **Translated shadow volumes:** tiled and planar patch culling apply the engine's pre-shadow translation when testing local-light caster volumes.
- **Component lighting flags:** planar and arbitrary-mesh proxies preserve engine-derived Cast Shadow, Dynamic Shadow, indirect-only, and dynamic indirect-lighting state instead of forcing dynamic shadow/indirect flags on.
- **Normals under nonuniform scale:** the shared vertex factory applies inverse-transpose transforms to material, height-texture pixel, vertex-lighting, and normal-only paths. Tangents are orthogonalized against the corrected normals, and handedness includes the transform determinant and source tangent sign.
- **From Height Texture shadow-bias normals:** this mode previously left vertex normals flat while generating detailed pixel normals. Single-grid, spatial-patch, quadtree, and CPU-readback generation now calculate geometric vertex normals from displaced positions with intensity 1. Conventional directional/spot-light slope bias and vertex-normal material expressions receive mesh slopes; height-texture pixel-normal strength and texel-step behavior are preserved.
- **Rigid transform velocity state:** primitive uniforms now use the engine's previous transform and output-velocity state. Material opacity relevance is applied before velocity relevance is evaluated. Previous positions for animated compute deformation remain future work.
- **Primitive material/lighting state:** planar, arbitrary-mesh, and tiled proxies use the engine primitive-uniform builder, preserving custom primitive data, decal reception, lighting channels, capture selection, and volumetric-lightmap state while retaining generated bounds.
- **Editor-baked lightmap UVs:** the shared static-mesh baker now requests UV0-to-UV1 chart repacking, selects lightmap coordinate index 1, and uses a minimum packing resolution of 64. Existing source charts still determine packing quality.
- **Ocean FFT SM5 compilation:** row and column transforms read RG32F textures through SRVs instead of unsupported typed UAV loads. The row transform writes separate output textures, giving RDG explicit dependencies and avoiding same-pass SRV/UAV aliasing.

### Changed

- Runtime planar and arbitrary-mesh proxies explicitly disable unsupported distance-field-lighting flags. Generated geometry still needs a separate representation for distance-field shadows, DFAO, or software distance-field tracing.
- Added [tiled terrain and renderer findings](../TILED_TESSELLATION_RENDERER_FINDINGS.md) and [terrain shadow/lighting findings](../TERRAIN_SHADOW_LIGHTING_FINDINGS.md), and refreshed the README renderer compatibility section. Older tiled-design and release-audit documents now identify their historical scope.
- From Height Texture incurs one normal compute pass per geometry rebuild, using the existing normal buffer. The FFT correction adds approximately 1.5 MiB of temporary row spectra at the current 256x256 FFT size.

### Validation

- UnrealHeaderTool and Win64 Development Editor plugin compilation/linking passed in the local UE 5.8 workspace. Normal unity and plugin-scoped separate-file compilation were checked during the investigation; original build settings were restored. Existing deprecated-API compiler warnings remain.
- **All 9 automation tests passed on both DX12 SM5 and DX12 SM6**, with zero test warnings, failures, or skipped tests, on an NVIDIA GeForce RTX 5070 Ti. SM5 exercises conventional shadows. SM6 confirms non-Nanite VSM and temporarily switches to conventional shadows for the large-quadtree comparison, restoring VSM afterward.
- Test actors, textures, materials, and baked mesh objects are transient; the tests do not save terrain assets or levels.

Test names below use the `GPURuntimeTessellation.` prefix:

| Test | Coverage |
| --- | --- |
| `Tiled.Validation` | Coordinates, rectangular maps, import settings, bounds, topology limits, memory estimates, and budget refusal. |
| `Tiled.GPUSeams` | Actual GPU positions/normals/UVs for four matching rectangular tiles, shared borders, and index-buffer ownership. |
| `Renderer.BakedLightmapUVs` | In-memory static mesh UV1 packing and lightmap coordinate index. |
| `Renderer.TiledDepthCapture` | Rendered displaced depth and actor movement through the tiled scene proxy/vertex factory. |
| `Renderer.NormalTransforms` | Rendered GBuffer normals under scale `(2,1,0.25)` for Finite Difference, Geometry Based, and From Height Texture. |
| `Renderer.HeightTextureShadowNormals` | GPU normal-buffer readback for a single grid, spatial patches, and 4096-scale quadtree leaves. All nine sampled flat normals failed before correction and match the analytical slope afterward. |
| `Renderer.LargeQuadtreeShadowCoverage` | Broad conventional occlusion at scale 4096, insufficient near coverage, a 20 km range with one/four cascades, and Far Shadow flags. Uses cheaper flat geometry than the reported terrain. |
| `Renderer.OffscreenPatchShadow` | Offscreen caster retention, component Cast Shadow, conventional/VSM paths, and distant VSM Far Shadow controls. |
| `Renderer.ShadowCullVolumes` | Camera rejection, translated light-volume acceptance/rejection, and unconstrained caster volumes. |

These tests cover controlled compute and rendering cases. Exact terrain/material appearance, deformation velocity, animated shadow-cache behavior, packaged PSO hitch rates, complex baked UV charts, and Vulkan/Metal/mobile/console rendering still require validation.

### Upgrade and terrain setup

- Rebuild the plugin, restart the editor, and allow changed vertex-factory/FFT shaders to compile. Check component shadow flags when refreshing existing actors; the proxies now respect those flags.
- Tiled heightmaps must have equal dimensions, sRGB and Virtual Texture Streaming disabled, Never Stream enabled, and matching duplicated border samples. `ValidateTiles` checks configuration rather than border-pixel equality. Tile `(0,0)` starts at local XY `(0,0)`; runtime edits require `RebuildTiles`.
- High-resolution heightmaps and dense geometry are separate controls. The new component uses fixed density and keeps all configured textures/geometry resident; culling reduces draw work rather than resident memory.
- In the reported scene, a 1000x1000 plane at uniform actor scale 4096 spans **40.96 km**, with an **8.192 km** height span for normalized height data at intensity 200. The user confirmed VSM works after the earlier fixes and that changing conventional dynamic cascades from **1 to 4**, while keeping the **20 km** distance, significantly improves lighting. This scene setting is separate from the vertex-normal code fix; the plugin does not change lights or global shadow CVars automatically.
- For conventional distant shadows, configure sufficient dynamic/far cascade coverage and enable the terrain component's **Far Shadow** flag when using far cascades. Fine height-texture normals can still exceed triangle and shadow-map resolution. See the lighting findings for quadtree density, bias, and cascade guidance.

### Remaining limitations

- Tiled terrain has no asynchronous tile/mip streaming, adaptive tile/patch LOD, cross-LOD stitching, skirts, collision, or tile-set data asset. Source-border mismatches require correction in the input data.
- Runtime geometry has no Nanite, generated distance fields, ray-tracing acceleration structure, Lumen mesh cards, or baked static-lighting representation. The lightmap UV improvement applies to editor-baked static meshes.
- Transform motion is integrated, but animated displacement/remeshing still lacks previous generated positions for complete deformation velocity.
- `bUsePersistentPatchBuffers` currently optimizes spatial patches; quadtree updates still regenerate leaves.
- PSO collection is implemented; packaged hitch behavior and delaying draws until precaching completes remain follow-up work.
