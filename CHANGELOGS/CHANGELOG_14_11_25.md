# GPU Runtime Tessellation - Changelog

## 2025-11-14 - Patch LOD Fixes & Seamless Stitching

### Overview
Major update fixing visual gaps (T-junctions) between patches of different LOD levels. The system now generates watertight meshes by intelligently collapsing high-resolution edge vertices to match lower-resolution neighboring patches.

### Added

#### **Per-Edge Transition Analysis**
- New `ComputePatchEdgeTransitions()` function analyzes neighboring patch LOD levels
- Computes collapse ratios for each edge (West, East, South, North)
- Collapse factors determine how many high-res segments merge into one coarse segment
- Factors clamped to [1, 64] range for stability

#### **Edge Collapse Metadata in Patches**
- `FGPUTessellationPatchInfo` now includes:
  - `ResolutionX` (int32) - Cached grid resolution for the patch
  - `ResolutionY` (int32) - Cached grid resolution for the patch  
  - `EdgeCollapseFactors` (FIntVector4) - Per-edge collapse ratios:
    - X component = West edge collapse factor
    - Y component = East edge collapse factor
    - Z component = South edge collapse factor
    - W component = North edge collapse factor
  - Default initialization: `FIntVector4(1, 1, 1, 1)` (no collapse)

#### **GPU Index Generation with Seam Stitching**
- New shader functions in `GPUIndexGeneration.usf`:
  - `ClampStride()` - Safely clamps stride values to valid segment counts
  - `ApplyEdgeCollapse()` - Remaps vertex indices along edges to eliminate T-junctions
- Edge vertices automatically snap to coarser grid while preserving corner anchors
- Works entirely on GPU without duplicating vertex data

### Changed

#### **Resolution Calculation Algorithm** (Critical Fix)
- **Old behavior**: Padded total vertex count to thread-group size
- **New behavior**: Pads segment count, then adds closing vertex
- **Why this matters**: Ensures adjacent patches share compatible divisors
  ```cpp
  // Old (broken for LOD stitching):
  int32 Resolution = TessellationFactor * 4;
  Resolution = RoundUpToMultiple(Resolution, 8);
  
  // New (correct for LOD stitching):
  int32 Segments = TessellationFactor * 4;
  Segments = RoundUpToMultiple(Segments, 8);
  int32 Resolution = Segments + 1;  // Add closing vertex
  ```
- This guarantees neighboring grids can collapse cleanly without micro-gaps

#### **Function Signature Updates**

**GPUTessellationMeshBuilder.h/cpp:**
- `DispatchIndexGeneration()` - Added `EdgeCollapseFactors` parameter:
  ```cpp
  // Old:
  void DispatchIndexGeneration(FRDGBuilder& GraphBuilder, FIntPoint Resolution, FRDGBufferRef& OutIndexBuffer);
  
  // New:
  void DispatchIndexGeneration(FRDGBuilder& GraphBuilder, FIntPoint Resolution, const FIntVector4& EdgeCollapseFactors, FRDGBufferRef& OutIndexBuffer);
  ```

- `GenerateSinglePatch()` - Simplified to accept full patch info:
  ```cpp
  // Old:
  void GenerateSinglePatch(..., const FVector2f& PatchUVOffset, const FVector2f& PatchUVSize, int32 TessellationLevel, ...);
  
  // New:
  void GenerateSinglePatch(..., const FGPUTessellationPatchInfo& PatchInfo, ...);
  ```

#### **Shader Parameter Updates**

**FGPUIndexGenerationCS::FParameters:**
- Added `EdgeCollapseFactors` (FIntVector4) to pass collapse ratios to GPU
- Shader now receives and applies edge-specific stitching logic

#### **Pipeline Integration**
- `ExecuteTessellationPipeline()` (single mesh mode):
  - Calls `DispatchIndexGeneration()` with `FIntVector4(1,1,1,1)` (no collapsing)
  
- `ExecutePatchTessellationPipeline()`:
  - Now calls `ComputePatchEdgeTransitions()` after `CalculatePatchInfo()`
  - Each patch receives its computed collapse factors
  
- `GenerateSinglePatch()`:
  - Extracts tessellation level, UV offset/size from `PatchInfo` parameter
  - Passes `PatchInfo.EdgeCollapseFactors` to index generation

### Fixed

#### **T-Junction Elimination**
- **Problem**: High-resolution patch edges created vertices that didn't align with low-resolution neighbors
- **Solution**: Edge vertices on patch boundaries now collapse to match neighboring patch resolution
- **Result**: Perfectly watertight mesh across all LOD transitions

#### **Grid Alignment Issues**
- **Problem**: Padding vertex counts caused incompatible grid divisions between LODs
- **Solution**: Padding segment counts ensures adjacent patches share common factors
- **Example**: LOD 2 (64 segments) perfectly divides into LOD 1 (32 segments) at 2:1 ratio

#### **Corner Vertex Preservation**
- **Problem**: Early implementations collapsed corner vertices incorrectly
- **Solution**: `ApplyEdgeCollapse()` explicitly excludes corner vertices from collapsing
  ```cpp
  // Corners are preserved by checking != LastX/LastY
  if (VertexY != LastY) { /* only non-corner vertices collapse */ }
  ```

### Implementation Details

#### **Files Modified**

**Shaders (.usf):**
- `GPUIndexGeneration.usf`
  - Added `EdgeCollapseFactors` parameter (uint4)
  - Implemented `ClampStride()` helper function
  - Implemented `ApplyEdgeCollapse()` with per-edge logic
  - Modified `GenerateIndices()` to apply collapsing before triangle emission

**Headers (.h):**
- `GPUTessellationComputeShaders.h`
  - Added `EdgeCollapseFactors` to `FGPUIndexGenerationCS::FParameters`
  
- `GPUTessellationMeshBuilder.h`
  - Added fields to `FGPUTessellationPatchInfo` struct
  - Updated `DispatchIndexGeneration()` signature
  - Updated `GenerateSinglePatch()` signature
  - Added `ComputePatchEdgeTransitions()` declaration

**Source (.cpp):**
- `GPUTessellationMeshBuilder.cpp`
  - Rewrote `CalculateResolution()` to pad segments instead of vertices
  - Implemented `ComputePatchEdgeTransitions()` with neighbor analysis
  - Updated `DispatchIndexGeneration()` to pass collapse factors to shader
  - Modified `GenerateSinglePatch()` to use `PatchInfo` parameter
  - Updated `CalculatePatchInfo()` to cache resolutions in patch metadata
  - Modified pipeline functions to integrate transition computation

#### **Edge Collapse Algorithm**

For each vertex on a patch edge:
1. **Identify edge location**: Check if vertex is on West/East/South/North boundary
2. **Skip if corner**: Corners must remain fixed for proper patch alignment
3. **Calculate stride**: Determine collapse factor for that edge
4. **Snap to coarse grid**: Round vertex position to nearest coarse vertex
   ```cpp
   CollapsedY = (VertexY / Stride) * Stride;  // Integer division snaps to grid
   ```
5. **Remap index**: Update vertex index to point to collapsed position

**Example**: If West edge has factor 4, vertices at Y={1,2,3} snap to Y=0, Y={5,6,7} snap to Y=4, etc.

#### **Collapse Factor Computation**

For each patch edge:
1. **Find neighbor patch** in that direction (may be null at boundaries)
2. **Compare resolutions**: If neighbor has equal/higher resolution, no collapse needed
3. **Calculate ratio**: `Factor = MySegments / NeighborSegments`
4. **Clamp safely**: Ensure factor is in valid range [1, 64]

### Performance Considerations

- **Zero CPU overhead**: All stitching logic runs on GPU in index generation
- **No vertex duplication**: Edge vertices are simply remapped, not duplicated
- **Minimal shader cost**: 4 conditional checks per vertex (one per edge)
- **Compatible with existing features**: Works seamlessly with render targets, dynamic displacement, etc.

### Technical Notes

#### **Thread-Group Alignment**
The resolution padding ensures compute dispatches always cover full thread groups. This prevents driver-specific issues where partial thread groups might be optimized away or produce undefined results.

#### **Segment vs Vertex Padding**
Critical distinction for LOD compatibility:
- **Segments** = quads = spaces between vertices
- **Vertices** = segments + 1
- Padding segments ensures divisibility: 32 segments / 16 segments = exactly 2
- Padding vertices breaks divisibility: 33 vertices / 17 vertices ≠ integer

---

## Previous Changes

For changes prior to 2025-11-14, see `CHANGELOG_04_11_25.md`.
