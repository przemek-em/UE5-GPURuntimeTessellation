// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "GPUTessellationComponent.generated.h"

class FGPUTessellationSceneProxy;

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
	FromNormalMap UMETA(DisplayName = "From Normal Map Texture (Highest Quality)")
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
	DensityTexture UMETA(DisplayName = "Density Texture Based - WIP")
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
 * Settings structure for GPU tessellation
 */
USTRUCT(BlueprintType)
struct FGPUTessellationSettings
{
	GENERATED_BODY()

	/** Base tessellation factor (grid resolution multiplier) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tessellation", meta = (ClampMin = "1", ClampMax = "512", UIMin = "1", UIMax = "512", EditCondition = "LODMode == EGPUTessellationLODMode::Disabled", EditConditionHides))
	int32 TessellationFactor = 16;

	/** Size of the plane in X direction (local space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", meta = (ClampMin = "1.0", ClampMax = "10000.0"))
	float PlaneSizeX = 1000.0f;

	/** Size of the plane in Y direction (local space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", meta = (ClampMin = "1.0", ClampMax = "10000.0"))
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

	/** Maximum tessellation factor at close range (LOD Mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1", ClampMax = "512", UIMin = "8", UIMax = "512", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased", EditConditionHides))
	int32 MaxTessellationFactor = 64;

	/** Minimum tessellation factor at max distance (LOD Mode only) */
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "LODMode == EGPUTessellationLODMode::DistanceBased || LODMode == EGPUTessellationLODMode::DistanceBasedPatches", EditConditionHides))
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
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent), hidecategories = (Object, LOD, Physics, Collision))
class GPURUNTIMETESSELLATION_API UGPUTessellationComponent : public UMeshComponent
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
	//~ End UPrimitiveComponent Interface

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

	/** Displacement texture (R channel = height) - Supports both UTexture2D and UTextureRenderTarget2D */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation")
	TObjectPtr<UTexture> DisplacementTexture;

	/** Subtract/Mask texture (optional) - Texture mask where white = no displacement, black = full displacement. Supports both UTexture2D and UTextureRenderTarget2D for realtime effects like snow melting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation")
	TObjectPtr<UTexture> SubtractTexture;

	/** Normal map texture (RGB = tangent space normal, optional - used when NormalCalculationMethod = FromNormalMap) - Supports both UTexture2D and UTextureRenderTarget2D */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation", meta = (EditCondition = "TessellationSettings.NormalCalculationMethod == 4", EditConditionHides))
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

	/** Enable debug logging (throttled to every 2 seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Debug")
	bool bEnableDebugLogging = false;

	/** Show debug visualization (patch bounds boxes and centers) - Only visible in editor, not in shipping builds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tessellation|Debug")
	bool bShowPatchDebugVisualization = false;

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

	/** Get current tessellation resolution */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation")
	FIntPoint GetTessellationResolution() const;

	/** Get current vertex count */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation")
	int32 GetVertexCount() const;

	/** Get current triangle count */
	UFUNCTION(BlueprintPure, Category = "GPU Tessellation")
	int32 GetTriangleCount() const;

private:
	/** Mark render state dirty and request update */
	void MarkRenderStateDirty();

	/** Calculate grid resolution from tessellation factor */
	FIntPoint CalculateGridResolution() const;

	/** Update LOD based on distance to camera */
	void UpdateDistanceBasedLOD(float DeltaTime);

	/** Update LOD based on distance with discrete levels */
	void UpdateDiscreteLOD(float DeltaTime);

	/** Update LOD based on distance to camera with discrete patches */
	void UpdatePatchBasedLOD(float DeltaTime);

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

	/** Last patch configuration for change detection (Instance-specific, not static!) */
	int32 LastPatchCountX = 1;
	int32 LastPatchCountY = 1;

	friend class FGPUTessellationSceneProxy;
};
