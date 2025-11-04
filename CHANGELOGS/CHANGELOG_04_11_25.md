# GPU Runtime Tessellation - Changelog

## 2025-11-04

### Added
- **Render Target Support**: Changed all texture parameters from `UTexture2D*` to `UTexture*` to support both `UTexture2D` and `UTextureRenderTarget2D`
  - `DisplacementTexture` now accepts render targets for dynamic displacement
  - `SubtractTexture` now accepts render targets for realtime painting/snow melting effects
  - `NormalMapTexture` now accepts render targets for dynamic normal maps
  
- **Automatic Render Target Updates**: System now automatically detects render targets and updates mesh every frame
  - New property: `bAutoUpdateRenderTargets` (default: true) - Enable/disable automatic render target updates
  - New property: `RenderTargetUpdateFPS` (default: 60) - Limit update rate
  - FPS limiter prevents performance issues with high-frequency updates
  - Categorized under "GPU Tessellation|Render Target" section

- **Subtract Texture in Normal Calculation**: Normal calculation shader now properly considers the subtract/mask texture
  - Added `SubtractTexture` parameter to normal calculation pipeline
  - Shader now applies mask: `Height *= (1.0 - Mask)` when calculating normals
  - Normals now correctly match displaced geometry when using subtract textures
  - Fixes incorrect lighting when using dynamic render targets for effects

### Changed
- **Function Signatures Updated** (Breaking Change):
  - `ExecuteTessellationPipeline()` - Added `UTexture*` parameters (both overloads)
  - `ExecutePatchTessellationPipeline()` - Changed to `UTexture*` parameters
  - `GenerateMeshSync()` - Changed to `UTexture*` parameters
  - `DispatchDisplacement()` - Changed to `UTexture*` parameters
  - `DispatchNormalCalculation()` - Changed to `UTexture*` parameters AND added `SubtractTexture` parameter
  - `GenerateSinglePatch()` - Changed to `UTexture*` parameters
  - `CreateRDGTextureFromUTexture2D()` - Renamed to `CreateRDGTextureFromUTexture()`
  - `SetDisplacementTexture()` - Changed to accept `UTexture*`
  - `SetSubtractTexture()` - Changed to accept `UTexture*`
  - `SetNormalMapTexture()` - Changed to accept `UTexture*`
  - `SetTessellationTextures()` (Blueprint) - Changed to accept `UTexture*`

- **Component Properties**:
  - `DisplacementTexture`: Changed from `TObjectPtr<UTexture2D>` to `TObjectPtr<UTexture>`
  - `SubtractTexture`: Changed from `TObjectPtr<UTexture2D>` to `TObjectPtr<UTexture>`
  - `NormalMapTexture`: Changed from `TObjectPtr<UTexture2D>` to `TObjectPtr<UTexture>`
  - Updated tooltips to clarify render target support

- **Scene Proxy Cached Textures**:
  - `CachedDisplacementTexture`: Changed from `TObjectPtr<UTexture2D>` to `TObjectPtr<UTexture>`
  - `CachedSubtractTexture`: Changed from `TObjectPtr<UTexture2D>` to `TObjectPtr<UTexture>`
  - `CachedNormalMapTexture`: Changed from `TObjectPtr<UTexture2D>` to `TObjectPtr<UTexture>`

- **Shader Parameters** (`GPUTessellationComputeShaders.h`):
  - `FGPUNormalCalculationCS::FParameters` - Added subtract texture parameters:
    - `bHasSubtractTexture` (uint32)
    - `SubtractTexture` (Texture2D SRV)
    - `SubtractSampler` (SamplerState)

### Fixed
- **Normal Calculation Bug**: Normals now correctly account for subtract/mask texture
  - Previously, normals were calculated only from displacement texture
  - Subtract texture was applied to geometry but ignored in normal calculation
  - This caused incorrect lighting/shading when using dynamic effects
  - Now both displacement and normals use identical masking logic

- **Render Target Update Detection**: Mesh now regenerates when render target content changes
  - Previously required manual `UpdateTessellatedMesh()` calls
  - Now automatically detects render targets in `TickComponent()`
  - Calls `MarkRenderStateDirty()` every frame when render targets are present
  - Can be controlled via `bAutoUpdateRenderTargets` flag

### Implementation Details

#### Files Modified
**Headers (.h)**:
- `GPUTessellationComponent.h` - Component properties and public API
- `GPUTessellationMeshBuilder.h` - Pipeline function signatures
- `GPUTessellationComputeShaders.h` - Shader parameter structures
- `GPUTessellationSceneProxy.h` - Scene proxy cached textures
- `GPUTessellationExamples.h` - Example/helper functions

**Source (.cpp)**:
- `GPUTessellationComponent.cpp` - Component implementation and render target detection
- `GPUTessellationMeshBuilder.cpp` - Pipeline implementation and parameter passing

**Shaders (.usf)**:
- `GPUNormalCalculation.usf` - Normal calculation shader logic

#### Performance Considerations
- **Render Target Auto-Update**: Can be disabled via `bAutoUpdateRenderTargets = false`
- **FPS Limiting**: Set `RenderTargetUpdateFPS` to limit update frequency (default: 60 FPS)
- **Unlimited Updates**: Set `RenderTargetUpdateFPS = 0` for every-frame updates
- **Recommended Settings**:
  - Static textures: Keep auto-update enabled (no overhead when not using render targets)
  - Dynamic painting: 30-60 FPS for good balance
  - High-frequency effects: Set to 0 for unlimited updates
  - Performance mode: Set to 15-30 FPS

### Technical Notes

#### Render Target Detection
The system uses `UTexture::IsA<UTextureRenderTarget>()` to detect render targets at runtime in `TickComponent()`. This check is performed only when `bAutoUpdateRenderTargets` is enabled.

#### Normal Calculation Pipeline
The normal calculation now mirrors the displacement logic:
1. Sample displacement texture → get height
2. If subtract texture exists, sample it → get mask
3. Apply mask: `Height *= (1.0 - Mask)`
4. Calculate normals using finite difference on masked heights

This ensures normals match the actual displaced geometry for correct lighting.

#### Zero Performance Overhead
When not using render targets, there is zero performance overhead:
- Render target detection check is O(1)
- Only executes when `bAutoUpdate = true`
- FPS limiter only runs when render targets are detected