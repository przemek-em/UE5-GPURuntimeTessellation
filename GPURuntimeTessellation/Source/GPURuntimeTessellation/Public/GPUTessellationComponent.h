// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "Materials/MaterialInterface.h"
#include "GPUTessellationComponent.generated.h"

class FGPUTessellationSceneProxy;
class UBodySetup;
class UStaticMesh;
struct FGPUTessellatedMeshData;

/** Collision representation built for gameplay/physics queries. */
UENUM(BlueprintType)
enum class EGPUTessellationCollisionMode : uint8
{
	Disabled UMETA(DisplayName = "Disabled"),
	HeightFieldTraceOnly UMETA(DisplayName = "Height Field Trace Only"),
	CoarseHeightFieldMesh UMETA(DisplayName = "Coarse Height Field Mesh"),
	VertexPerfectMesh UMETA(DisplayName = "Vertex Perfect Mesh (GPU Readback)"),
	CollisionLODRingsMesh UMETA(DisplayName = "Collision LOD Rings Mesh")
};

/** Sample layout used by the water interaction volume when applying buoyancy to physics bodies. */
UENUM(BlueprintType)
enum class EGPUTessellationWaterBuoyancySampleMode : uint8
{
	Center UMETA(DisplayName = "Center Pontoon"),
	Bounds5Point UMETA(DisplayName = "Bounds 5 Point"),
	Bounds9Point UMETA(DisplayName = "Bounds 9 Point"),
	CustomPontoons UMETA(DisplayName = "Custom Pontoons")
};

/** Artist-authored buoyancy sample point in the overlapped primitive component's local space. */
USTRUCT(BlueprintType)
struct FGPUTessellationWaterPontoon
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	FVector RelativeLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "1.0", UIMin = "1.0", UIMax = "1000.0", Units = "cm"))
	float Radius = 100.0f;
};

/**
 * Normal calculation methods for tessellated geometry
 */
UENUM(BlueprintType)
enum class EGPUTessellationNormalMethod : uint8
{
	Disabled UMETA(DisplayName = "Disabled (Use Up Vector)"),
	FiniteDifference UMETA(DisplayName = "Finite Difference (Fast)"),
	GeometryBased UMETA(DisplayName = "Geometry Based (Accurate)"),
	Hybrid UMETA(DisplayName = "Hybrid (Best Quality)"),
	FromNormalMap UMETA(DisplayName = "From Normal Map Texture (Highest Quality)"),
	GeometryHeightTextureBlend UMETA(DisplayName = "Geometry + Height Texture Detail (Dynamic)"),
	FromHeightTexture UMETA(DisplayName = "From Height Texture / Render Target (Dynamic)")
};

/**
 * LOD modes for dynamic tessellation
 * Compute shader generates fixed-resolution meshes, so LOD works by regenerating mesh at different resolutions
 */
UENUM(BlueprintType)
enum class EGPUTessellationLODMode : uint8
{
	Disabled UMETA(DisplayName = "No LOD (Static Resolution)"),
	DistanceBased UMETA(DisplayName = "Distance-Based LOD (Smooth Transition)"),
	DistanceBasedDiscrete UMETA(DisplayName = "Distance-Based LOD (Discrete Levels)"),
	DistanceBasedPatches UMETA(DisplayName = "Spatial Patches (Per-Tile LOD)"),
	DensityTexture UMETA(DisplayName = "Density Texture Based - WIP"),
	DistanceBasedQuadtree UMETA(DisplayName = "Quadtree Patches (Experimental GPU LOD Target)")
};

/**
 * LOD patch levels for discrete tessellation
 */
UENUM(BlueprintType)
enum class EGPUTessellationPatchLevel : uint8
{
	Patch_4 UMETA(DisplayName = "4 (Very Low)"),
	Patch_8 UMETA(DisplayName = "8 (Low)"),
	Patch_16 UMETA(DisplayName = "16 (Medium)"),
	Patch_32 UMETA(DisplayName = "32 (High)"),
	Patch_64 UMETA(DisplayName = "64 (Very High)"),
	Patch_128 UMETA(DisplayName = "128 (Ultra)")
};

/**
 * Procedural ocean wave models that can drive the displacement pass instead of
 * (or on top of) a displacement texture.
 */
UENUM(BlueprintType)
enum class EGPUOceanWaveMode : uint8
{
	Disabled       UMETA(DisplayName = "Disabled (Use Texture / Sine)"),
	Gerstner       UMETA(DisplayName = "Gerstner (Sum of N Trochoidal Waves)"),
	FFT            UMETA(DisplayName = "FFT / Tessendorf"),
	PerlinFBM      UMETA(DisplayName = "Perlin fBm (Cheap Procedural)")
};

/** Secondary FFT motion styling. Classic preserves the current Tessendorf phase animation. */
UENUM(BlueprintType)
enum class EGPUOceanFFTMotionMode : uint8
{
	ClassicTessendorf UMETA(DisplayName = "Current Tessendorf"),
	NaturalSway       UMETA(DisplayName = "Natural Sway")
};

/** Coordinate space used by vector displacement maps before the component transform is applied. */
UENUM(BlueprintType)
enum class EGPUVectorDisplacementSpace : uint8
{
	LocalSpace   UMETA(DisplayName = "Local Space"),
	WorldSpace   UMETA(DisplayName = "World Space"),
	TangentSpace UMETA(DisplayName = "Tangent Space")
};

/** Encoding used by vector displacement texture RGB values. */
UENUM(BlueprintType)
enum class EGPUVectorDisplacementDecodeMode : uint8
{
	SignedFloat      UMETA(DisplayName = "Signed Float / Direct Units"),
	ZeroToOne       UMETA(DisplayName = "0..1 Encoded (-1..1)"),
	MinusOneToOne   UMETA(DisplayName = "-1..1 Normalized"),
	CustomScaleBias UMETA(DisplayName = "Custom Scale / Bias")
};

/**
 * Single Gerstner wave descriptor. The component sums up to MaxOceanGerstnerWaves of these.
 * For most ocean spectra 4-8 waves with random directions inside +/- 30deg of the wind
 * direction look believable. Direction must be a unit vector; Wavelength and Speed are in
 * Unreal units (cm); Amplitude is the peak crest height in cm; Steepness in [0,1] makes
 * the wave more trochoidal (sharper crests, broader troughs) but is currently applied as
 * a vertical-only approximation - we do not move XY in this version to keep the existing
 * patch/grid pipeline untouched.
 */
USTRUCT(BlueprintType)
struct FGPUOceanGerstnerWave
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean")
	FVector2D Direction = FVector2D(1.0, 0.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean", meta = (ClampMin = "1.0"))
	float Wavelength = 400.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean", meta = (ClampMin = "0.0"))
	float Amplitude = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean", meta = (ClampMin = "0.0"))
	float Speed = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Steepness = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean")
	float PhaseOffset = 0.0f;
};

/** Hard cap on simultaneous Gerstner waves; matches the shader-side fixed array. */
#define GPU_OCEAN_MAX_GERSTNER_WAVES 8

/**
 * Ocean wave parameters. Embedded inside FGPUTessellationSettings; if WaveMode is
 * Disabled the displacement pass behaves exactly as before. UGPUOceanComponent fills
 * this in automatically and animates Time per-tick.
 */
USTRUCT(BlueprintType)
struct FGPUOceanSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean")
	EGPUOceanWaveMode WaveMode = EGPUOceanWaveMode::Disabled;

	/** Animated by the component each tick. Wraps every ~10000s to keep float precision. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ocean")
	float Time = 0.0f;

	// ---- Gerstner -------------------------------------------------------------------

	/** Active Gerstner waves. Up to GPU_OCEAN_MAX_GERSTNER_WAVES will be uploaded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Gerstner", meta = (EditCondition = "WaveMode == EGPUOceanWaveMode::Gerstner || WaveMode == EGPUOceanWaveMode::FFT", EditConditionHides))
	TArray<FGPUOceanGerstnerWave> GerstnerWaves;

	// ---- Perlin / fBm ---------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Perlin", meta = (ClampMin = "0.0001", EditCondition = "WaveMode == EGPUOceanWaveMode::PerlinFBM", EditConditionHides))
	float PerlinFrequency = 0.005f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Perlin", meta = (ClampMin = "1", ClampMax = "8", EditCondition = "WaveMode == EGPUOceanWaveMode::PerlinFBM", EditConditionHides))
	int32 PerlinOctaves = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Perlin", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "WaveMode == EGPUOceanWaveMode::PerlinFBM", EditConditionHides))
	float PerlinPersistence = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Perlin", meta = (ClampMin = "1.0", EditCondition = "WaveMode == EGPUOceanWaveMode::PerlinFBM", EditConditionHides))
	float PerlinLacunarity = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Perlin", meta = (EditCondition = "WaveMode == EGPUOceanWaveMode::PerlinFBM", EditConditionHides))
	FVector2D PerlinFlowDirection = FVector2D(1.0, 0.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Perlin", meta = (ClampMin = "0.0", EditCondition = "WaveMode == EGPUOceanWaveMode::PerlinFBM", EditConditionHides))
	float PerlinFlowSpeed = 50.0f;

	// ---- FFT (Tessendorf) ----------------------------------------------------------
	// These fields are populated by UGPUOceanComponent (not exposed in the base inspector).
	// They are read by the FFT compute pipeline each frame.

	/** Physical period of the FFT spectrum in cm (the heightmap tiles every TileSize). */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	float FFTTileSize = 5000.0f;

	/** Master amplitude for the Phillips spectrum. */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	float FFTAmplitude = 1.0f;

	/** Crest sharpness bias (0 = smooth, ~1 = peaky). */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	float FFTChoppiness = 0.5f;

	/** Secondary FFT motion styling. */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	EGPUOceanFFTMotionMode FFTMotionMode = EGPUOceanFFTMotionMode::ClassicTessendorf;

	/** Strength of Natural Sway local wave-group domain warp and amplitude modulation. */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	float FFTSwayIntensity = 0.65f;

	/** Natural Sway cycle rate in cycles per second. */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	float FFTSwayRate = 0.045f;

	/** Fraction of wind speed used to drift Natural Sway's local noise groups. */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	float FFTSwayDrift = 0.03f;

	/** Wind direction (set internally by UGPUOceanComponent from its top-level UPROPERTY). */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	FVector2D Wind = FVector2D(1.0, 0.0);

	/** Wind speed in cm/s (drives Phillips spectrum length scale). */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	float WindSpeed = 200.0f;

	/** Deterministic seed for the per-frame H0 Gaussian field. */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean|FFT")
	int32 FFTSpectrumSeed = 1234;

	FGPUOceanSettings() {}
};

/**
 * Settings structure for GPU tessellation
 */
USTRUCT(BlueprintType)
struct FGPUTessellationSettings
{
	GENERATED_BODY()

	/** Base tessellation factor (grid resolution multiplier).
	 *  Each unit produces 4 grid edges, so the resulting vertex grid is (Factor*4 + 1) per axis
	 *  (e.g. Factor 64 -> 257x257 verts, Factor 128 -> 513x513). The +1 is the closing vertex
	 *  required to form the last column/row of quads - removing it would leave a gap.
	 *
	 *  Recommended values are powers of two (8, 16, 32, 64, 128, 256, 512). Non-pow2 values
	 *  are accepted but adjacent LOD/patch tiles only stitch crack-free when their factors
	 *  differ by a power of two (e.g. 32<->64). Visual subdivision can multiply the effective
	 *  render factor above this base value, with protected caps in the mesh builder.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tessellation", meta = (ClampMin = "1", ClampMax = "512", UIMin = "1", UIMax = "512", EditCondition = "LODMode == EGPUTessellationLODMode::Disabled", EditConditionHides))
	int32 TessellationFactor = 16;

	/** Uniformly multiply generated render tessellation before displacement. This is not height-adaptive; it does not refine only mountain peaks or sharp height features. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tessellation|Subdivision", meta = (DisplayName = "Uniform Geometry Subdivision"))
	bool bSubdivideHardEdges = false;

	/** Multiplies the visual tessellation factor when Uniform Geometry Subdivision is enabled. High values are intentionally expensive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tessellation|Subdivision", meta = (ClampMin = "2", ClampMax = "8", UIMin = "2", UIMax = "8", EditCondition = "bSubdivideHardEdges", EditConditionHides, DisplayName = "Subdivision Multiplier"))
	int32 SubdivisionMultiplier = 2;

	/** Size of the plane in X direction (local space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", meta = (ClampMin = "1.0", ClampMax = "1000000.0", UIMin = "100.0", UIMax = "100000.0"))
	float PlaneSizeX = 1000.0f;

	/** Size of the plane in Y direction (local space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", meta = (ClampMin = "1.0", ClampMax = "1000000.0", UIMin = "100.0", UIMax = "100000.0"))
	float PlaneSizeY = 1000.0f;

	/** Displacement intensity (height multiplier) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Displacement", meta = (ClampMin = "0.0"))
	float DisplacementIntensity = 100.0f;

	/** Displacement offset (vertical shift) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Displacement")
	float DisplacementOffset = 0.0f;

	/** Use procedural sine wave displacement for testing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Displacement")
	bool bUseSineWaveDisplacement = true;

	/** LOD mode for dynamic tessellation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	EGPUTessellationLODMode LODMode = EGPUTessellationLODMode::Disabled;

	/** Use distance to bounds instead of pivot for LOD (more accurate, slight overhead) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased || LODMode == EGPUTessellationLODMode::DistanceBasedDiscrete", EditConditionHides))
	bool bUseDistanceToBounds = true;

	// ============ Discrete LOD Settings ============
	
	/** Discrete tessellation levels (ordered from closest to farthest) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Discrete", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedDiscrete", EditConditionHides))
	TArray<EGPUTessellationPatchLevel> DiscreteLODLevels = {
		EGPUTessellationPatchLevel::Patch_64,
		EGPUTessellationPatchLevel::Patch_32,
		EGPUTessellationPatchLevel::Patch_16,
		EGPUTessellationPatchLevel::Patch_8
	};

	/** Distance thresholds for each discrete level (in unscaled units, ordered from closest to farthest) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Discrete", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedDiscrete", EditConditionHides))
	TArray<float> DiscreteLODDistances = { 2000.0f, 5000.0f, 10000.0f, 20000.0f };

	// ============ Spatial Patch Settings (WIP - Not Fully Implemented) ============

	/** Number of patch subdivisions in X direction (creates PatchCountX * PatchCountY patches) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Patches", meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "16", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
	int32 PatchCountX = 4;

	/** Number of patch subdivisions in Y direction (creates PatchCountX * PatchCountY patches) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Patches", meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "16", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
	int32 PatchCountY = 4;

	/** Patch levels for distance-based LOD (ordered from closest to farthest) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Patches", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
	TArray<EGPUTessellationPatchLevel> PatchLevels = {
		EGPUTessellationPatchLevel::Patch_64,
		EGPUTessellationPatchLevel::Patch_32,
		EGPUTessellationPatchLevel::Patch_16,
		EGPUTessellationPatchLevel::Patch_8,
		EGPUTessellationPatchLevel::Patch_4
	};

	/** Distance thresholds for each patch level (in unscaled units, ordered from closest to farthest) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Patches", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
	TArray<float> PatchDistances = { 2000.0f, 5000.0f, 10000.0f, 20000.0f, 40000.0f };

	/** Enable frustum culling for patches (skip patches outside view) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Patches", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
	bool bEnablePatchCulling = true;

	/** Experimental: collapse high-detail patch edge indices toward lower-detail neighbors. Leave disabled if mixed LOD creates spike triangles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Patches|Experimental", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
	bool bEnablePatchEdgeStitching = false;

	/** Experimental: keep generated patch geometry buffers persistent and update only patch LOD state when the camera moves. Structural changes still rebuild the scene proxy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Patches|Experimental", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
	bool bUsePersistentPatchBuffers = false;

	// ============ Quadtree LOD Settings (Experimental) ============

	/** Root quadtree tiles in X before recursive subdivision. Keep low until the GPU-compacted path exists. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree", meta = (ClampMin = "1", ClampMax = "8", UIMin = "1", UIMax = "4", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	int32 QuadtreeRootTileCountX = 1;

	/** Root quadtree tiles in Y before recursive subdivision. Keep low until the GPU-compacted path exists. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree", meta = (ClampMin = "1", ClampMax = "8", UIMin = "1", UIMax = "4", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	int32 QuadtreeRootTileCountY = 1;

	/** Maximum recursive subdivision depth for the prototype quadtree leaf generator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree", meta = (ClampMin = "0", ClampMax = "8", UIMin = "0", UIMax = "6", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	int32 QuadtreeMaxDepth = 4;

	/** Safety cap for prototype quadtree leaves rendered through CPU mesh-batch submission. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree", meta = (ClampMin = "1", ClampMax = "2048", UIMin = "16", UIMax = "512", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	int32 QuadtreeMaxVisibleLeaves = 256;

	/** Quadtree tessellation levels from closest to farthest. The level index also drives target subdivision depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	TArray<EGPUTessellationPatchLevel> QuadtreeLevels = {
		EGPUTessellationPatchLevel::Patch_64,
		EGPUTessellationPatchLevel::Patch_32,
		EGPUTessellationPatchLevel::Patch_16,
		EGPUTessellationPatchLevel::Patch_8,
		EGPUTessellationPatchLevel::Patch_4
	};

	/** Quadtree distance thresholds from closest to farthest. Closer buckets subdivide deeper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	TArray<float> QuadtreeDistances = { 2000.0f, 5000.0f, 10000.0f, 20000.0f, 40000.0f };

	/** Enable per-view frustum culling for generated quadtree leaves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	bool bEnableQuadtreeCulling = true;

	/** Critical: collapse high-density quadtree leaf edge indices toward lower-density neighbor edges to hide T-junction cracks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree|Critical", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	bool bEnableQuadtreeEdgeStitching = true;

	/** Critical: split coarse quadtree leaves near much deeper neighbors so visible adjacent leaves differ by at most one depth level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree|Critical", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	bool bBalanceQuadtreeLeaves = true;

	/** Safety cap for quadtree edge-collapse stitching. Lower values reduce rare long stitch triangles; higher values hide larger LOD jumps but can create very long triangles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Quadtree|Critical", meta = (ClampMin = "1", ClampMax = "64", UIMin = "2", UIMax = "16", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree && bEnableQuadtreeEdgeStitching", EditConditionHides))
	int32 QuadtreeMaxEdgeCollapseFactor = 8;

	/** Maximum tessellation factor at close range (LOD Mode only).
	 *  Same grid math as TessellationFactor: vertex grid = (Factor*4 + 1) per axis.
	 *  Use a power of two (8, 16, 32, 64, 128, 256, 512) so distance-based LOD steps stay on
	 *  pow2 boundaries with MinTessellationFactor - mismatched, non-pow2 transitions can
	 *  produce visible cracks at LOD boundaries.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1", ClampMax = "512", UIMin = "8", UIMax = "512", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased", EditConditionHides))
	int32 MaxTessellationFactor = 64;

	/** Minimum tessellation factor at max distance (LOD Mode only).
	 *  Should be a power of two and divide evenly into MaxTessellationFactor (e.g. Min=8 with
	 *  Max=64). This guarantees adjacent LOD rings share grid divisors and stitch without
	 *  cracks. Non-pow2 values are clamped to the (Factor*4 + 1) grid as usual.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1", ClampMax = "512", UIMin = "1", UIMax = "128", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased", EditConditionHides))
	int32 MinTessellationFactor = 8;

	/** Minimum distance for LOD transitions (within this distance, uses MaxTessellationFactor) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0", ClampMax = "500000.0", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased", EditConditionHides))
	float MinTessellationDistance = 1000.0f;

	/** Maximum distance for LOD transitions (beyond this distance, uses MinTessellationFactor) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "100.0", ClampMax = "500000.0", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased", EditConditionHides))
	float MaxTessellationDistance = 50000.0f;

	/** Smooth transition speed between LOD levels (higher = faster transitions) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.1", ClampMax = "10.0", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased", EditConditionHides))
	float LODTransitionSpeed = 2.0f;

	/** Hysteresis to prevent LOD oscillation (minimum difference before triggering regeneration) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased || LODMode == EGPUTessellationLODMode::DistanceBasedPatches || LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree", EditConditionHides))
	int32 LODHysteresis = 2;

	/** Density texture for spatially-varying tessellation (R channel: 0=low detail, 1=high detail) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "LODMode == EGPUTessellationLODMode::DensityTexture", EditConditionHides))
	TObjectPtr<UTexture2D> DensityTexture = nullptr;

	/** Normal calculation method */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals")
	EGPUTessellationNormalMethod NormalCalculationMethod = EGPUTessellationNormalMethod::FiniteDifference;

	/** Invert calculated normals */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals")
	bool bInvertNormals = false;

	/** Normal smoothing factor (0 = sharp detail from texture, 1 = smooth averaged normals) - Blends between finite difference and geometry-based normal calculation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float NormalSmoothingFactor = 0.0f;

	/** Generated vertex normal intensity (0 = flat up normals, 1 = physically generated slope, >1 exaggerates generated slopes). Does not affect authored normal-map texture mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0", DisplayName = "Vertex Normal Intensity"))
	float NormalIntensity = 1.0f;

	/** Strength for height texture/render-target normal modes. For blended mode this controls residual detail strength; for pure height mode this controls full height normal strength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0", DisplayName = "Height Texture Normal Strength", EditCondition = "NormalCalculationMethod == EGPUTessellationNormalMethod::GeometryHeightTextureBlend || NormalCalculationMethod == EGPUTessellationNormalMethod::FromHeightTexture", EditConditionHides))
	float HeightTextureNormalDetailStrength = 1.0f;

	/** Height texture texel radius used for dynamic height-derived normals. Values near 1 sample the source height field at texture resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (ClampMin = "0.25", ClampMax = "16.0", UIMin = "0.5", UIMax = "4.0", DisplayName = "Height Texture Normal Texel Step", EditCondition = "NormalCalculationMethod == EGPUTessellationNormalMethod::GeometryHeightTextureBlend || NormalCalculationMethod == EGPUTessellationNormalMethod::FromHeightTexture", EditConditionHides))
	float HeightTextureNormalTexelStep = 1.0f;

	// Vector displacement values are populated by UGPUVectorDisplacementComponent. They are
	// intentionally not EditAnywhere here so the standard heightfield component stays clean.
	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	bool bUseVectorDisplacement = false;

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	EGPUVectorDisplacementSpace VectorDisplacementSpace = EGPUVectorDisplacementSpace::LocalSpace;

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	EGPUVectorDisplacementDecodeMode VectorDisplacementDecodeMode = EGPUVectorDisplacementDecodeMode::SignedFloat;

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	FVector VectorDisplacementScale = FVector(1.0, 1.0, 1.0);

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	FVector VectorDisplacementBias = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	bool bUseVectorDisplacementAlphaAsStrength = false;

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	float VectorDisplacementIntensity = 1.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	bool bAddScalarHeightDisplacementToVector = false;

	UPROPERTY(BlueprintReadWrite, Category = "Vector Displacement")
	FVector VectorDisplacementBoundsPadding = FVector::ZeroVector;

	/** Procedural ocean wave parameters. NOT exposed in the inspector for the base component
	 *  on purpose - ocean configuration lives on UGPUOceanComponent (Add Component -> GPU Ocean),
	 *  which writes into this struct internally. Kept BlueprintReadWrite so advanced users can
	 *  still drive it from script if they really want to. */
	UPROPERTY(BlueprintReadWrite, Category = "Ocean")
	FGPUOceanSettings OceanSettings;

	// Internal runtime values (not exposed to editor)
	/** UV offset for patch rendering (used internally for spatial subdivision) */
	FVector2f UVOffset = FVector2f(0.0f, 0.0f);
	
	/** UV scale for patch rendering (used internally for spatial subdivision) */
	FVector2f UVScale = FVector2f(1.0f, 1.0f);

	FGPUTessellationSettings() {}
};

/**
 * GPU Tessellation Component
 * 
 * Pure compute shader-based tessellation component that replaces Hull/Domain shaders.
 * Generates a tessellated plane with displacement mapping entirely on the GPU.
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent, DisplayName = "GPU Tessellation"), hidecategories = (Object, LOD, Physics))
class GPURUNTIMETESSELLATION_API UGPUTessellationComponent : public UMeshComponent, public IInterface_CollisionDataProvider
{
	GENERATED_BODY()

public:
	UGPUTessellationComponent(const FObjectInitializer& ObjectInitializer);

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	virtual int32 GetNumMaterials() const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual bool LineTraceComponent(FHitResult& OutHit, const FVector Start, const FVector End, const FCollisionQueryParams& Params) override;
	virtual bool LineTraceComponent(FHitResult& OutHit, const FVector Start, const FVector End, ECollisionChannel TraceChannel, const FCollisionQueryParams& Params, const FCollisionResponseParams& ResponseParams, const FCollisionObjectQueryParams& ObjectParams) override;
	virtual UBodySetup* GetBodySetup() override;
	//~ End UPrimitiveComponent Interface

	//~ Begin Interface_CollisionDataProvider Interface
	virtual bool GetTriMeshSizeEstimates(FTriMeshCollisionDataEstimates& OutTriMeshEstimates, bool bInUseAllTriData) const override;
	virtual bool GetPhysicsTriMeshData(FTriMeshCollisionData* CollisionData, bool InUseAllTriData) override;
	virtual bool ContainsPhysicsTriMeshData(bool InUseAllTriData) const override;
	virtual bool WantsNegXTriMesh() override { return false; }
	//~ End Interface_CollisionDataProvider Interface

	//~ Begin USceneComponent Interface
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	//~ End USceneComponent Interface

	//~ Begin UActorComponent Interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	//~ End UActorComponent Interface

#if WITH_EDITOR
	//~ Begin UObject Interface
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	//~ End UObject Interface
#endif

public:
	/** Tessellation settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation")
	FGPUTessellationSettings TessellationSettings;

	/**
	 * Allow the embedded OceanSettings inside TessellationSettings to affect rendering and gameplay queries.
	 * GPU Ocean enables this automatically. The standard tessellation component keeps it off so hidden
	 * serialized ocean settings from duplicated/derived blueprints cannot accidentally turn terrain meshes into oceans.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Advanced", meta = (AdvancedDisplay))
	bool bEnableProceduralOceanSettings = false;

	/** Displacement texture (R channel = height) - Supports both UTexture2D and UTextureRenderTarget2D */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation", meta = (ToolTip = "Height texture sampled from the R channel. For imported heightmaps, prefer Greyscale G8/16 from source R with sRGB off and a true 16-bit source. Use R16F mainly for float/HDR sources or render targets; it cannot recover precision from an 8-bit source."))
	TObjectPtr<UTexture> DisplacementTexture;

	/** Subtract/Mask texture (optional) - Texture mask where white = no displacement, black = full displacement. Supports both UTexture2D and UTextureRenderTarget2D for realtime effects like snow melting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation")
	TObjectPtr<UTexture> SubtractTexture;

	/** Normal map texture (RGB = tangent space normal, optional - used when TessellationSettings.NormalCalculationMethod = FromNormalMap) - Supports both UTexture2D and UTextureRenderTarget2D */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation")
	TObjectPtr<UTexture> NormalMapTexture;

	/** Material to render the tessellated mesh */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation")
	TObjectPtr<UMaterialInterface> Material;

	/** Enable automatic updates based on camera movement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation")
	bool bAutoUpdate = true;

	/** Enable automatic updates for render target textures - Updates mesh every frame when render targets are used */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Render Target")
	bool bAutoUpdateRenderTargets = true;

	/** Limit render target update rate (FPS) - 0 means unlimited (update every frame) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Render Target", meta = (ClampMin = "0", ClampMax = "120", UIMin = "0", UIMax = "120", EditCondition = "bAutoUpdateRenderTargets", EditConditionHides))
	int32 RenderTargetUpdateFPS = 60;

	/** When render targets are used, update only after their render-time marker changes. This avoids blind polling but is not a pixel hash. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Render Target", meta = (EditCondition = "bAutoUpdateRenderTargets", EditConditionHides))
	bool bAutoDetectRenderTargetChanges = false;

	/** Enable debug logging (throttled to every 2 seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Debug")
	bool bEnableDebugLogging = false;

	/** Show debug visualization (patch bounds boxes and centers) - Only visible in editor, not in shipping builds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Debug")
	bool bShowPatchDebugVisualization = false;

#if WITH_EDITORONLY_DATA
	/** Content Browser folder for baked static mesh assets. Must be under /Game. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	FString BakeAssetDirectory = TEXT("/Game/GPUTessellationBakes");

	/** Optional baked static mesh asset name. Empty auto-generates from the actor/component name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	FString BakeAssetName;

	/** Bake the active visual LOD factor or patch topology. Disable to bake the configured base tessellation settings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	bool bBakeMeshUseCurrentVisualLOD = true;

	/** In patch/quadtree modes, bake one full mesh at the highest selected patch density instead of the current camera-dependent patch layout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	bool bBakeMeshFullPatchMesh = false;

	/** Clamp for full-patch static mesh bakes. Example: 4x4 patches at patch factor 32 request full factor 128. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake", meta = (ClampMin = "1", ClampMax = "1024", UIMin = "64", UIMax = "1024", EditCondition = "bBakeMeshFullPatchMesh", EditConditionHides))
	int32 BakeFullPatchMeshTessellationCap = 1024;

	/** Allow CPU access on the generated static mesh render buffers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	bool bBakeMeshAllowCPUAccess = false;

	/** Use the generated render triangles as simple collision on the baked static mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	bool bBakeMeshUseComplexCollision = true;

	/** Save the generated asset package immediately. Disabled leaves the asset dirty for manual saving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	bool bBakeMeshAutoSaveAsset = false;

	/** Also bake a tangent-space normal map texture from the current height texture or render target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake")
	bool bBakeNormalMapTexture = false;

	/** Skip static mesh creation and bake only the normal map texture when pressing the bake button. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake", meta = (EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	bool bBakeNormalMapOnly = false;

	/** Optional normal map asset name. Empty derives a name from the baked mesh, or actor/component when baking only normals. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake", meta = (EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	FString BakeNormalMapAssetName;

	/** Height texture texel distance used for central-difference normal generation. Matches Height Texture Normal Texel Step. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake", meta = (ClampMin = "0.25", ClampMax = "16.0", UIMin = "0.5", UIMax = "4.0", EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	float BakeNormalMapTexelStep = 1.0f;

	/** Slope multiplier applied to the baked normal map. Matches Height Texture Normal Strength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Bake", meta = (ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0", EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	float BakeNormalMapStrength = 1.0f;
#endif

	/** Collision/query representation for the generated surface. Coarse and LOD ring meshes use CPU heights when possible and GPU readback for texture/FFT surfaces; Vertex Perfect Mesh reads back the final GPU-generated grid and is expensive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh")
	EGPUTessellationCollisionMode CollisionMode = EGPUTessellationCollisionMode::HeightFieldTraceOnly;

	/** Enable component-level line traces against the CPU-evaluable height field. This is a gameplay query fallback, not Chaos physics collision. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (EditCondition = "CollisionMode != EGPUTessellationCollisionMode::Disabled"))
	bool bEnableHeightFieldLineTrace = true;

	/** Step count used by component-level height-field line traces. Higher values catch steeper/high-frequency displacement at greater CPU cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (ClampMin = "1", ClampMax = "512", UIMin = "8", UIMax = "128", EditCondition = "CollisionMode != EGPUTessellationCollisionMode::Disabled && bEnableHeightFieldLineTrace", EditConditionHides))
	int32 HeightFieldLineTraceSteps = 64;

	/** Number of quads per side in the cooked coarse collision mesh. Keep this far below render resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (ClampMin = "2", ClampMax = "256", UIMin = "8", UIMax = "128", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CoarseHeightFieldMesh", EditConditionHides))
	int32 CollisionResolution = 32;

	/** Match Vertex Perfect Mesh collision to the currently active render LOD. Single-grid modes use the active visual tessellation factor; patch and quadtree modes read back their current patch/leaf layout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (EditCondition = "CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh", EditConditionHides, DisplayName = "Match Actual LOD"))
	bool bVertexPerfectCollisionMatchVisualLOD = false;

	/** In patch/quadtree LOD modes, bake Vertex Perfect collision as one full-plane mesh using the highest selected patch density instead of assembling current patch/leaf readbacks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (EditCondition = "CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh", EditConditionHides, DisplayName = "Bake Full Mesh Patch Collision"))
	bool bVertexPerfectCollisionBakeFullPatchMesh = false;

	/** Clamp for the computed full-plane patch collision factor. Example: 4x4 patches at patch factor 32 request full factor 128. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (ClampMin = "1", ClampMax = "1024", UIMin = "64", UIMax = "1024", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh && bVertexPerfectCollisionBakeFullPatchMesh", EditConditionHides, DisplayName = "Full Mesh Patch Collision Cap"))
	int32 VertexPerfectCollisionFullPatchMeshTessellationCap = 1024;

	/** Rebuild matched Vertex Perfect Mesh collision automatically in game worlds when camera-driven visual LOD topology changes. Editor viewport auto-recook remains opt-in below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (EditCondition = "CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh && bVertexPerfectCollisionMatchVisualLOD", EditConditionHides, DisplayName = "Auto Recook Matched LOD Collision"))
	bool bAutoRecookVertexPerfectCollisionForLODChanges = true;

	/** Also allow automatic matched LOD collision recooks while moving the editor viewport. Keep disabled for heavy quadtree scenes unless actively debugging collision. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (EditCondition = "CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh && bVertexPerfectCollisionMatchVisualLOD && bAutoRecookVertexPerfectCollisionForLODChanges", EditConditionHides, DisplayName = "Auto Recook Matched LOD In Editor"))
	bool bAutoRecookVertexPerfectCollisionInEditor = false;

	/** Minimum camera/player movement before automatic matched LOD collision recooks can follow a new quadtree focus area. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10000.0", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh && bVertexPerfectCollisionMatchVisualLOD && bAutoRecookVertexPerfectCollisionForLODChanges", EditConditionHides, DisplayName = "Matched LOD Recook Distance", Units = "cm"))
	float VertexPerfectCollisionMatchedLODRecookDistance = 500.0f;

	/** Tessellation factor used by Vertex Perfect Mesh collision readback when Match Visual LOD is disabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (ClampMin = "1", ClampMax = "1024", UIMin = "8", UIMax = "1024", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh && !bVertexPerfectCollisionMatchVisualLOD", EditConditionHides, DisplayName = "Vertex Perfect Collision Tessellation"))
	int32 VertexPerfectCollisionTessellationFactor = 64;

	/** World-space ring boundaries in cm around the active camera for Collision LOD Rings Mesh. Each entry starts a farther, coarser band. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|LOD Rings", meta = (EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides))
	TArray<float> CollisionLODRingDistances = { 5000.0f, 20000.0f, 50000.0f };

	/** Target world-space quad size in cm for each collision ring band. Use one more entry than Ring Distances; the final value covers the rest of the plane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|LOD Rings", meta = (EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides))
	TArray<float> CollisionLODRingQuadSizes = { 250.0f, 1000.0f, 4000.0f, 16000.0f };

	/** Rebuild Collision LOD Rings Mesh after the camera has moved this far in world space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|LOD Rings", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10000.0", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides, Units = "cm"))
	float CollisionLODRingUpdateDistance = 500.0f;

	/** Maximum per-axis source vertices selected for the nonuniform Collision LOD Rings Mesh. Raises quality but increases Chaos cook cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|LOD Rings", meta = (ClampMin = "8", ClampMax = "2049", UIMin = "64", UIMax = "1025", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides))
	int32 CollisionLODRingMaxSamplesPerAxis = 1025;

	/** Tessellation factor for the dense GPU-readback source cache used by Collision LOD Rings Mesh. Higher values give nearer rings more exact source vertices; 512 reaches the current 2048-vertex-per-edge readback cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|LOD Rings", meta = (ClampMin = "1", ClampMax = "512", UIMin = "16", UIMax = "512", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides))
	int32 CollisionLODRingReadbackTessellationFactor = 128;

	/** Maximum recook rate for animated collision surfaces. 0 means manual/settings-driven rebuilds only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "10.0", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CoarseHeightFieldMesh || CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh || CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides))
	float CollisionUpdateRate = 2.0f;

	/** Cook the collision mesh asynchronously in game worlds. Vertex readback still happens synchronously before cooking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CoarseHeightFieldMesh || CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh || CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides))
	bool bUseAsyncCollisionCooking = true;

	/** Force the first physics collision cook to block when no previous cooked body exists. This avoids a no-collision window at startup, at the cost of a hitch for large meshes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (EditCondition = "(CollisionMode == EGPUTessellationCollisionMode::CoarseHeightFieldMesh || CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh || CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh) && bUseAsyncCollisionCooking", EditConditionHides))
	bool bUseSynchronousInitialCollisionCook = true;

	/** Adds a lower backing surface below the generated collision mesh. Keep at 0 for exact surface collision; raise for fast/small rigid bodies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "100.0", Units = "cm", EditCondition = "CollisionMode == EGPUTessellationCollisionMode::CoarseHeightFieldMesh || CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh || CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh", EditConditionHides))
	float CollisionThickness = 0.0f;

	/** Draw the cached Chaos collision mesh in the editor/game viewport for debugging. Red bounds mean no collision mesh is currently available. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|Debug")
	bool bShowCollisionMeshDebug = false;

	/** Color used for valid Chaos collision mesh debug lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|Debug", meta = (EditCondition = "bShowCollisionMeshDebug", EditConditionHides))
	FColor CollisionMeshDebugColor = FColor::Green;

	/** Draw every Nth collision triangle. Raise this for high collision resolutions to keep debug drawing cheap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|Debug", meta = (ClampMin = "1", ClampMax = "64", UIMin = "1", UIMax = "16", EditCondition = "bShowCollisionMeshDebug", EditConditionHides))
	int32 CollisionMeshDebugTriangleStride = 1;

	/** Thickness for collision mesh debug lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Collision Mesh|Debug", meta = (ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "4.0", EditCondition = "bShowCollisionMeshDebug", EditConditionHides))
	float CollisionMeshDebugLineThickness = 0.0f;

	/** Enable a non-blocking water interaction volume for buoyancy/gameplay. This is separate from Chaos surface collision. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction")
	bool bEnableWaterInteraction = false;

	/** Depth below the tessellation plane covered by the water interaction volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "50000.0", Units = "cm", EditCondition = "bEnableWaterInteraction", EditConditionHides))
	float WaterInteractionDepth = 5000.0f;

	/** Extra height above the tessellation plane so bodies crossing the surface are detected before their origin is underwater. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "5000.0", Units = "cm", EditCondition = "bEnableWaterInteraction", EditConditionHides))
	float WaterInteractionHeightAboveSurface = 200.0f;

	/** Object types queried by the water interaction volume. Defaults to PhysicsBody. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction", meta = (EditCondition = "bEnableWaterInteraction", EditConditionHides))
	TArray<TEnumAsByte<EObjectTypeQuery>> WaterInteractionObjectTypes;

	/** Apply pontoon-style buoyancy and drag to overlapping simulating physics bodies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (EditCondition = "bEnableWaterInteraction", EditConditionHides))
	bool bWaterApplyBuoyancyToPhysicsBodies = true;

	/** Automatic pontoon sample layout generated from each overlapped body's bounds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	EGPUTessellationWaterBuoyancySampleMode WaterBuoyancySampleMode = EGPUTessellationWaterBuoyancySampleMode::Bounds5Point;

	/** Custom local-space pontoon points used when Sample Mode is Custom Pontoons. These are interpreted relative to each overlapping primitive component. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies && WaterBuoyancySampleMode == EGPUTessellationWaterBuoyancySampleMode::CustomPontoons", EditConditionHides))
	TArray<FGPUTessellationWaterPontoon> WaterCustomPontoons;

	/** Buoyancy multiplier. 1 roughly cancels gravity at full submersion; values above 1 float higher. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "5.0", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterBuoyancyStrength = 1.15f;

	/** Vertical damping applied at each submerged pontoon point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "20.0", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterBuoyancyDamping = 2.5f;

	/** Per-pontoon horizontal velocity drag. Unlike global linear drag, this creates torque when only part of the body is submerged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "20.0", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterPointDrag = 1.0f;

	/** Lateral force from the sampled water surface slope. Helps waves push and roll/tilt bodies instead of only lifting them upward. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterSurfaceNormalForce = 0.35f;

	/** Linear drag applied while the body is submerged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterLinearDrag = 0.8f;

	/** Angular drag applied while the body is submerged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterAngularDrag = 0.4f;

	/** Pontoon radius in cm. 0 derives a radius from the overlapped body's bounds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "1000.0", Units = "cm", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterBuoyancySampleRadius = 0.0f;

	/** Optional cap for total buoyancy force per body. 0 means unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Buoyancy", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10000000.0", EditCondition = "bEnableWaterInteraction && bWaterApplyBuoyancyToPhysicsBodies", EditConditionHides))
	float WaterMaxBuoyantForce = 0.0f;

	/** Use the cached collision/readback mesh as the water surface sampler when the CPU evaluator cannot represent the surface, such as FFT or texture displacement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Surface Sampling", meta = (EditCondition = "bEnableWaterInteraction", EditConditionHides))
	bool bWaterUseCachedCollisionMeshSurface = true;

	/** Read back the final GPU-displaced surface for water sampling when CPU evaluation cannot represent the surface, such as FFT. Does not cook Chaos collision. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Surface Sampling", meta = (EditCondition = "bEnableWaterInteraction", EditConditionHides))
	bool bWaterUseGPUSurfaceReadback = true;

	/** Maximum readback rate for the water surface cache while overlapping physics bodies are present. 0 means every water-interaction tick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Surface Sampling", meta = (ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "30.0", EditCondition = "bEnableWaterInteraction && bWaterUseGPUSurfaceReadback", EditConditionHides))
	float WaterSurfaceReadbackUpdateRate = 15.0f;

	/** Tessellation factor used by the water readback cache. Lower values are cheaper but less accurate for short FFT waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Surface Sampling", meta = (ClampMin = "1", ClampMax = "256", UIMin = "8", UIMax = "128", EditCondition = "bEnableWaterInteraction && bWaterUseGPUSurfaceReadback", EditConditionHides))
	int32 WaterSurfaceReadbackTessellationFactor = 64;

	/** Fall back to the undisplaced tessellation plane if no CPU or cached collision surface sample is available. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Surface Sampling", meta = (EditCondition = "bEnableWaterInteraction", EditConditionHides))
	bool bWaterFallbackToPlaneSurface = true;

	/** Draw the water interaction volume and active buoyancy sample points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Debug", meta = (EditCondition = "bEnableWaterInteraction", EditConditionHides))
	bool bShowWaterInteractionDebug = false;

	/** Color used for water interaction volume debug lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Debug", meta = (EditCondition = "bEnableWaterInteraction && bShowWaterInteractionDebug", EditConditionHides))
	FColor WaterInteractionDebugColor = FColor::Cyan;

	/** Thickness for water interaction debug lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Water Interaction|Debug", meta = (ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "4.0", EditCondition = "bEnableWaterInteraction && bShowWaterInteractionDebug", EditConditionHides))
	float WaterInteractionDebugLineThickness = 0.0f;

	/** Cooked Chaos collision for coarse mesh mode. */
	UPROPERTY(Instanced)
	TObjectPtr<UBodySetup> CollisionBodySetup;

public:
	/** Blueprint callable function to force mesh update */
	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation")
	void UpdateTessellatedMesh();

	/** Set displacement texture */
	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation")
	void SetDisplacementTexture(UTexture* InTexture);

	/** Set subtract/mask texture (accepts regular textures or RenderTargets for realtime painting) */
	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation")
	void SetSubtractTexture(UTexture* InTexture);

	/** Set normal map texture */
	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation")
	void SetNormalMapTexture(UTexture* InTexture);

	/** Set material (overrides parent method) */
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial) override;

	/** Update tessellation settings */
	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation")
	void UpdateSettings(const FGPUTessellationSettings& NewSettings);

#if WITH_EDITOR
	/** Bake the current GPU-generated tessellated geometry into a new Static Mesh asset. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Tessellation", meta = (DisplayName = "Bake Current Tessellation To Static Mesh"))
	void BakeCurrentTessellationToStaticMesh();

	/** Bake the current GPU-generated tessellated geometry and return the created Static Mesh asset. */
	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation")
	UStaticMesh* BakeCurrentTessellationToStaticMeshAsset();
#endif

	/** Return tessellation settings sanitized for this component type before render/query use. */
	virtual FGPUTessellationSettings GetEffectiveTessellationSettings() const;

	/** Optional vector displacement texture supplied by derived component types. */
	virtual UTexture* GetVectorDisplacementTexture() const { return nullptr; }

	/** Get current tessellation resolution */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation")
	FIntPoint GetTessellationResolution() const;

	/** Get current vertex count */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation")
	int32 GetVertexCount() const;

	/** Get current triangle count */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation")
	int32 GetTriangleCount() const;

	/** Project a world position onto the component's finite tessellation plane and return normalized UV plus local position. */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation|Collision Mesh")
	bool ProjectWorldToTessellationUV(const FVector& WorldPosition, FVector2D& OutUV, FVector& OutLocalPosition) const;

	/** Sample the CPU-evaluable height field at a world XY position. Supports sine, Gerstner, Perlin, and default flat texture fallback. */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation|Collision Mesh")
	bool SampleHeightAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const;

	/** Trace a line against the CPU-evaluable height field. This is a gameplay query fallback, not Chaos physics collision. */
	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation|Collision Mesh", meta = (AdvancedDisplay = "NumSteps"))
	bool LineTraceHeightField(const FVector& TraceStart, const FVector& TraceEnd, FHitResult& OutHit, int32 NumSteps = 32) const;

	/** Rebuild the coarse Chaos collision mesh immediately when CollisionMode is Coarse Height Field Mesh. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Tessellation")
	void RebuildCollisionMesh();

	/** Sample the water interaction surface at a world XY position. Uses CPU waves, cached collision mesh, then optional flat-plane fallback. */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation|Water Interaction")
	bool SampleWaterSurfaceAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const;

protected:
	/** Mark render state dirty and request update.
	 *  NOTE: this intentionally shadows UActorComponent::MarkRenderStateDirty so this class can
	 *  add its own bookkeeping. Kept protected so subclasses (e.g. UGPUOceanComponent) can call it. */
	void MarkRenderStateDirty();

	/** Mark coarse collision as needing a recook after settings that affect CPU height evaluation change. */
	void MarkCollisionMeshDirty();

	/** Mark the water surface readback cache dirty after settings, textures, or time-dependent inputs change. */
	void MarkWaterSurfaceReadbackDirty();

private:
	/** Calculate grid resolution from tessellation factor */
	FIntPoint CalculateGridResolution() const;

	/** Apply optional visual subdivision to a render tessellation factor. */
	int32 ApplyGeometrySubdivisionMultiplier(int32 TessellationFactor) const;

	/** Update LOD based on distance to camera */
	void UpdateDistanceBasedLOD(float DeltaTime);

	/** Update LOD based on distance with discrete levels */
	void UpdateDiscreteLOD(float DeltaTime);

	/** Update LOD based on distance to camera with discrete patches */
	void UpdatePatchBasedLOD(float DeltaTime);

	/** Update experimental quadtree LOD leaf state */
	void UpdateQuadtreeLOD(float DeltaTime);

	/** Update LOD based on density texture */
	void UpdateDensityBasedLOD(float DeltaTime);

	/** Calculate distance from camera to component (pivot or bounds) */
	float CalculateDistanceToCamera(const FVector& CameraPos, FVector& OutComponentPos) const;

	/** Send dynamic data (camera position) to scene proxy for patch updates */
	void SendRenderDynamicData_Concurrent();

	/** Current LOD level (smoothly interpolated) */
	int32 CalculateLODFactorScaled(float Distance, float ScaledMinDistance, float ScaledMaxDistance) const;

	/** Calculate target tessellation factor based on distance (legacy - not scale-aware) */
	int32 CalculateLODFactor(float Distance) const;

	/** True when local XY lies inside the finite plane footprint. */
	bool IsLocalXYInsidePlane(const FVector2D& LocalXY) const;

	/** Evaluate local-space surface height for CPU-supported displacement modes. */
	bool EvaluateLocalSurfaceHeight(const FVector2D& LocalXY, float& OutLocalHeight) const;

	/** Calculate local-space height-field normal by finite difference. */
	FVector CalculateLocalSurfaceNormal(const FVector2D& LocalXY) const;

	/** True when coarse Chaos collision should be generated. */
	bool IsCoarseCollisionMeshEnabled() const;

	/** True when camera-centered collision LOD rings should be generated. */
	bool IsCollisionLODRingsMeshEnabled() const;

	/** True when vertex-perfect GPU readback collision should be generated. */
	bool IsVertexPerfectCollisionMeshEnabled() const;

	/** True when any Chaos collision mesh mode should be generated. */
	bool IsPhysicsCollisionMeshEnabled() const;

	/** True when CPU-supported displacement can change with time and should recook at CollisionUpdateRate. */
	bool IsCollisionSurfaceAnimated() const;

	/** Generate the cached low-resolution local-space collision mesh. */
	bool BuildCoarseCollisionMeshData();

	/** Generate a camera-centered nonuniform collision mesh with dense near rings and coarse far rings. */
	bool BuildCollisionLODRingsMeshData();

	/** Refresh the dense GPU-readback source mesh cache used by Collision LOD Rings Mesh. */
	bool BuildCollisionLODRingSourceCache();

	/** Build collision LOD rings by selecting exact vertices from the dense source mesh cache. */
	bool BuildCollisionLODRingsMeshDataFromSourceCache();

	/** Resolve the current camera used by collision LOD rings. Falls back to component center. */
	bool GetCollisionLODRingCameraPosition(FVector& OutCameraPosition) const;

	/** True when the ring collision camera center moved enough to recook. */
	bool UpdateCollisionLODRingCameraState(bool bForceUpdate);

	/** Generate a local-space collision mesh by reading a fixed whole-plane GPU grid back to CPU. */
	bool BuildGPUReadbackCollisionMeshData(int32 RequestedTessellationFactor);

	/** True when the current visual LOD mode is patch or quadtree based. */
	bool IsPatchBasedLODMode(EGPUTessellationLODMode LODMode) const;

	/** Compute the full-plane factor needed to match the highest selected patch density, clamped by the supplied cap. */
	int32 CalculateFullPatchMeshTessellationFactor(const FGPUTessellationSettings& EffectiveSettings, int32 FullMeshCapValue) const;

	/** Compute the full-plane factor needed to match the highest selected patch density for collision. */
	int32 CalculateFullPatchMeshCollisionTessellationFactor(const FGPUTessellationSettings& EffectiveSettings) const;

	/** Generate Vertex Perfect collision as one full-plane bake for patch/quadtree LOD modes. */
	bool BuildFullPatchMeshVertexPerfectCollisionMeshData(const FGPUTessellationSettings& EffectiveSettings);

	/** Clamp the requested Vertex Perfect collision readback factor. Visual subdivision is applied before this when Match Actual LOD is enabled. */
	int32 GetEffectiveVertexPerfectCollisionTessellationFactor(int32 BaseTessellationFactor) const;

	/** Return the readback resolution cap needed for a given Vertex Perfect collision factor. */
	int32 GetVertexPerfectCollisionReadbackMaxResolution(int32 TessellationFactor) const;

	/** Append one GPU-readback grid to the collision mesh using explicit patch settings. */
	bool AppendGPUReadbackCollisionMeshData(const FGPUTessellationSettings& CollisionSettings, const FVector& CameraPosition, const FIntVector4& EdgeCollapseFactors, int32 MaxResolutionOverride);

#if WITH_EDITOR
	/** Append one GPU-readback grid to an editor static-mesh bake. */
	bool AppendGPUReadbackBakeMeshData(FGPUTessellatedMeshData& InOutMeshData, const FGPUTessellationSettings& BakeSettings, const FVector& CameraPosition, const FIntVector4& EdgeCollapseFactors, int32 MaxResolutionOverride) const;

	/** Generate the readback mesh used by BakeCurrentTessellationToStaticMesh. */
	bool BuildBakeMeshData(FGPUTessellatedMeshData& OutMeshData) const;
#endif

	/** Generate Vertex Perfect collision using the current visual render LOD topology. */
	bool BuildVisualLODMatchedVertexPerfectCollisionMeshData();

	/** Calculate the active visual LOD signature used to decide when matched collision should recook. */
	uint32 CalculateVertexPerfectCollisionVisualLODSignature() const;

	/** Generate the cached full render-grid collision mesh by reading final GPU vertices back to CPU. */
	bool BuildVertexPerfectCollisionMeshData();

	/** Generate collision mesh data for the active Chaos collision mode. */
	bool BuildCollisionMeshData();

	/** Remove invalid triangles and orient surface triangles consistently for Chaos contacts. */
	void NormalizeCollisionMeshTriangles();

	/** Add a lower backing surface when CollisionThickness is greater than zero. */
	void ApplyCollisionMeshThickness();

	/** Finalize cached mesh data before Chaos cooking/debug rendering. */
	void FinalizeCollisionMeshData();

	/** True when any assigned texture input is a render target. */
	bool HasRenderTargetInputs() const;

	/** Max render-time marker across assigned render target inputs. */
	double GetRenderTargetInputsLastRenderTime() const;

	/** Should render-target-driven render/collision resources update this tick? */
	bool ShouldUpdateRenderTargetDrivenResources();

	/** True when the water surface needs a GPU readback cache rather than CPU height evaluation. */
	bool ShouldUseWaterSurfaceReadbackCache() const;

	/** Refresh the water surface readback cache if dirty or due by rate limit. */
	void UpdateWaterSurfaceReadbackCache();

	/** Generate the cached local-space water surface mesh by reading back the final GPU-generated grid. */
	bool BuildWaterSurfaceReadbackCache();

	/** Sample a local-space height mesh in world XY. */
	bool SampleCachedMeshHeightAtWorldPosition(const TArray<FVector>& MeshVertices, const TArray<int32>& MeshIndices, const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const;

	/** Sample the water-specific GPU readback mesh in local XY. */
	bool SampleCachedWaterSurfaceHeightAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const;

	/** Sample the cached collision/readback mesh in local XY. */
	bool SampleCachedCollisionMeshHeightAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const;

	/** Build world-space center/extents/rotation for the finite water interaction overlap box. */
	bool GetWaterInteractionBox(FVector& OutCenter, FVector& OutExtent, FQuat& OutRotation) const;

	/** Query the water interaction volume and apply configured gameplay forces. */
	void UpdateWaterInteraction();

	/** Apply pontoon-style buoyancy to one overlapping physics body. */
	void ApplyWaterBuoyancyToComponent(UPrimitiveComponent* PrimitiveComponent) const;

	/** Generate automatic world-space pontoon points for an overlapping physics body. */
	void BuildWaterBuoyancySamplePoints(const UPrimitiveComponent* PrimitiveComponent, TArray<FVector>& OutSamplePoints, TArray<float>& OutSampleRadii) const;

	/** Draw water interaction volume debug visualization. */
	void DrawWaterInteractionDebug() const;

	/** Cook or clear the current coarse collision body. */
	void UpdateCoarseCollisionMesh(bool bForceUpdate);

	/** Create and configure a body setup for coarse collision cooking. */
	UBodySetup* CreateCollisionBodySetupHelper();

	/** Ensure the active collision body setup exists. */
	void CreateCollisionBodySetup();

	/** Finish an async Chaos cook and swap in the cooked body if it is still current. */
	void FinishCollisionAsyncCook(bool bSuccess, UBodySetup* FinishedBodySetup);

	/** Commit the matched visual LOD bookkeeping after a collision body is actually cooked and active. */
	void CommitPendingVertexPerfectCollisionLODState();

	/** Draw current collision mesh/bounds debug visualization. */
	void DrawCollisionMeshDebug() const;

	/** Current LOD level (smoothly interpolated) */
	float CurrentLODLevel = 16.0f;

	/** Last applied tessellation factor (for hysteresis) */
	int32 LastAppliedTessFactor = 16;

	/** Last known camera position for LOD */
	FVector LastCameraPosition = FVector::ZeroVector;

	/** Current grid resolution */
	mutable FIntPoint CurrentResolution;

	/** Last log time for throttling */
	mutable double LastLogTime = 0.0;

	/** Last render target update time for FPS limiting */
	double LastRenderTargetUpdateTime = 0.0;

	/** Last observed render-time marker across render target inputs. */
	double LastObservedRenderTargetRenderTime = -TNumericLimits<double>::Max();

	/**
	 * Last RHI texture pointers observed for the three sampled inputs. Used to detect
	 * texture streaming completion: when an asset is loaded, its FTextureResource RHI
	 * may not be created yet (or is a placeholder) when the proxy is first built. Once
	 * the real RHI shows up, these pointers change and the proxy needs to be rebuilt
	 * so the compute pipeline samples the fully-resident texture instead of a 2x2 stub.
	 */
	void* LastObservedDisplacementTextureRHI = nullptr;
	void* LastObservedSubtractTextureRHI = nullptr;
	void* LastObservedNormalMapTextureRHI = nullptr;
	void* LastObservedVectorDisplacementTextureRHI = nullptr;

	/** Last time the coarse collision mesh was cooked. */
	double LastCollisionUpdateTime = -1.0;

	/** Cached local-space vertices for the coarse collision mesh. */
	TArray<FVector> CollisionMeshVertices;

	/** Cached triangle indices for the coarse collision mesh. */
	TArray<int32> CollisionMeshIndices;

	/** Cached local-space vertices for water surface sampling. */
	TArray<FVector> WaterSurfaceCacheVertices;

	/** Cached triangle indices for water surface sampling. */
	TArray<int32> WaterSurfaceCacheIndices;

	/** Last time the water surface readback cache was refreshed. */
	double LastWaterSurfaceReadbackTime = -1.0;

	/** Whether water readback cache settings or displacement inputs changed since the last refresh. */
	bool bWaterSurfaceReadbackDirty = true;

	/** Pending async body setups that are still being cooked. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBodySetup>> AsyncCollisionBodySetupQueue;

	/** Whether collision settings or displacement inputs changed since the last coarse cook. */
	bool bCollisionMeshDirty = true;

	/** True once this component has an active cooked Chaos collision body. */
	bool bHasCookedCollisionBody = false;

	/** Last visual LOD signature used to cook Vertex Perfect collision when Match Visual LOD is enabled. */
	uint32 LastVertexPerfectCollisionVisualLODSignature = 0;

	/** Matched visual LOD signature waiting for the active sync/async collision cook to finish. */
	uint32 PendingVertexPerfectCollisionVisualLODSignature = 0;

	/** True when PendingVertexPerfectCollisionVisualLODSignature belongs to the in-flight collision cook. */
	bool bPendingVertexPerfectCollisionVisualLODSignatureValid = false;

	/** Camera/player focus position used to generate the in-flight matched visual LOD collision cook. */
	FVector PendingVertexPerfectCollisionLODRecookPosition = FVector::ZeroVector;

	/** True when PendingVertexPerfectCollisionLODRecookPosition belongs to the in-flight collision cook. */
	bool bPendingVertexPerfectCollisionLODRecookPositionValid = false;

	/** Whether the last generated Vertex Perfect collision mesh used the matched visual LOD path. */
	bool bLastVertexPerfectCollisionBuildUsedMatchedVisualLOD = false;

	/** Last camera/player focus position whose matched visual LOD collision cook became active. */
	FVector LastVertexPerfectCollisionLODRecookPosition = FVector::ZeroVector;

	/** True once matched visual LOD collision has an active cooked focus position. */
	bool bVertexPerfectCollisionLODRecookPositionInitialized = false;

	/** Last camera center used to build Collision LOD Rings Mesh. */
	FVector LastCollisionLODRingCameraPosition = FVector::ZeroVector;

	/** True once Collision LOD Rings Mesh has captured an initial camera center. */
	bool bCollisionLODRingCameraInitialized = false;

	/** Dense source vertices used as the authoritative mesh for Collision LOD Rings Mesh. */
	TArray<FVector> CollisionLODRingSourceVertices;

	/** Source cache grid dimensions for Collision LOD Rings Mesh. */
	int32 CollisionLODRingSourceResolutionX = 0;
	int32 CollisionLODRingSourceResolutionY = 0;

	/** True when the dense Collision LOD Rings source cache must be regenerated. */
	bool bCollisionLODRingSourceDirty = true;

	/** Last patch configuration for change detection (Instance-specific, not static!) */
	int32 LastPatchCountX = 1;
	int32 LastPatchCountY = 1;
	uint32 LastPatchTopologySignature = 0;

	/**
	 * Per-instance LOD init flag (FIX CRITICAL #2).
	 * Was previously a function-scope `static bool` shared by every component in the process,
	 * which meant only the first ticking component ever ran the LOD init and every other
	 * instance (and every PIE session after the first) inherited the constructor defaults.
	 */
	bool bLODInitialized = false;

	friend class FGPUTessellationSceneProxy;
};
