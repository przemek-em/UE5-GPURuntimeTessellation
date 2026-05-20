// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "GPUTessellationComponent.h"
#include "GPUVectorDisplacementComponent.generated.h"

class UStaticMesh;

/**
 * Plane tessellation component that applies RGB/RGBA vector displacement maps.
 *
 * This reuses the standard GPU Runtime Tessellation grid, LOD, quadtree, material,
 * and GPU-readback collision paths, but feeds a vector displacement texture into
 * the displacement compute shader instead of treating the texture as a scalar height.
 */
UCLASS(
	ClassGroup = (Rendering),
	meta = (BlueprintSpawnableComponent, DisplayName = "GPU Vector Displacement"),
	hidecategories = (Object),
	showcategories = (
		"GPU Vector Displacement",
		"GPU Tessellation",
		"GPU Tessellation|Collision Mesh",
		"GPU Tessellation|Collision Mesh|LOD Rings",
		"GPU Tessellation|Bake",
		"GPU Tessellation|Render Target",
		"GPU Tessellation|Debug",
		"Tessellation",
		"Tessellation|Subdivision",
		"Geometry",
		"Displacement",
		"LOD",
		"LOD|Discrete",
		"LOD|Patches",
		"LOD|Patches|Experimental",
		"LOD|Quadtree",
		"LOD|Quadtree|Critical",
		"Normals"))
class GPURUNTIMETESSELLATION_API UGPUVectorDisplacementComponent : public UGPUTessellationComponent
{
	GENERATED_BODY()

public:
	UGPUVectorDisplacementComponent(const FObjectInitializer& ObjectInitializer);

	virtual FGPUTessellationSettings GetEffectiveTessellationSettings() const override;
	virtual UTexture* GetVectorDisplacementTexture() const override { return VectorDisplacementTexture.Get(); }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** RGB/RGBA vector displacement texture. Recommended source: linear OpenEXR, imported as HDR/float data. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement", meta = (ToolTip = "RGB stores X/Y/Z displacement. Use linear EXR input, sRGB off, and HDR/float-compatible compression for high precision."))
	TObjectPtr<UTexture> VectorDisplacementTexture;

	/** Coordinate space of RGB vector values before the component transform is applied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement")
	EGPUVectorDisplacementSpace VectorDisplacementSpace = EGPUVectorDisplacementSpace::LocalSpace;

	/** How RGB values are decoded before scale, bias, and intensity are applied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Decode")
	EGPUVectorDisplacementDecodeMode DecodeMode = EGPUVectorDisplacementDecodeMode::SignedFloat;

	/** Per-axis multiplier applied after decode. For 0..1 encoded maps this is usually the displacement range in centimeters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Decode")
	FVector VectorDisplacementScale = FVector(1.0, 1.0, 1.0);

	/** Per-axis offset applied after decode and scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Decode")
	FVector VectorDisplacementBias = FVector::ZeroVector;

	/** Global multiplier applied to the final vector offset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Decode", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0"))
	float GlobalVectorDisplacementIntensity = 1.0f;

	/** Multiply the decoded vector offset by the texture alpha channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Decode")
	bool bUseAlphaAsStrength = false;

	/** Also apply the inherited scalar height displacement after vector displacement. Usually leave disabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Advanced")
	bool bAddScalarHeightDisplacement = false;

	/** Force vector-displaced surfaces to use normals derived from final GPU geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Normals")
	bool bForceGeometryNormalsForVectorDisplacement = true;

	/** Extra local-space bounds padding in X/Y/Z. Increase this if vector displacement is clipped by culling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Bounds", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	FVector VectorDisplacementBoundsPadding = FVector(100.0, 100.0, 100.0);

	/** Automatically warn about common texture import mistakes when properties change. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Validation")
	bool bValidateTextureImportSettings = true;

	/** Warn unless the runtime texture is truly RGBA32F. This is expensive and rarely needed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Vector Displacement|Validation")
	bool bRequire32BitRuntimeTexture = false;

	UFUNCTION(BlueprintCallable, Category = "GPU Vector Displacement")
	void SetVectorDisplacementTexture(UTexture* InTexture);

#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Vector Displacement", meta = (DisplayName = "Bake Current Vector Displacement To Static Mesh"))
	void BakeCurrentVectorDisplacementToStaticMesh();

	UFUNCTION(BlueprintCallable, Category = "GPU Vector Displacement")
	UStaticMesh* BakeCurrentVectorDisplacementToStaticMeshAsset();
#endif

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Vector Displacement")
	void ValidateVectorDisplacementTexture() const;
};
