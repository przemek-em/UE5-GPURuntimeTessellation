// Licensed under the MIT License. See LICENSE file in the project root.
#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "GPUTiledTessellationComponent.generated.h"

class UTexture2D;

/** One heightmap including the duplicated border samples shared with its neighbors. */
USTRUCT(BlueprintType)
struct GPURUNTIMETESSELLATION_API FGPUTiledHeightmap
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tile")
	FIntPoint Coordinate = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tile")
	TObjectPtr<UTexture2D> Heightmap = nullptr;
};

/** Experimental fixed-resolution tiled heightfield. Call RebuildTiles after runtime edits.
 * Heightmaps must be linear, non-virtual, non-streaming textures with matching border samples.
 * GPU geometry is bounded by GeometryBudgetMB. Texture memory is additional.
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent, DisplayName = "GPU Tiled Tessellation"), hidecategories = (Physics, Collision))
class GPURUNTIMETESSELLATION_API UGPUTiledTessellationComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UGPUTiledTessellationComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tiled Tessellation")
	TArray<FGPUTiledHeightmap> Tiles;

	/** Coordinate (0,0) starts at local XY (0,0); negative coordinates are supported. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tiled Tessellation", meta = (ClampMin = "1.0", Units = "cm"))
	FVector2D TileSize = FVector2D(10000.0, 10000.0);

	/** Output geometry resolution, independent of input heightmap resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tiled Tessellation", meta = (ClampMin = "1", ClampMax = "1024"))
	int32 QuadsPerTile = 128;

	/** Height = R * HeightScale + HeightOffset, with R in [0,1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tiled Tessellation")
	float HeightScale = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tiled Tessellation")
	float HeightOffset = 0.0f;

	/** Entire rebuild is refused if generated geometry exceeds this budget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Tiled Tessellation", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 GeometryBudgetMB = 256;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Tiled Tessellation")
	void RebuildTiles();

	/** Checks coordinates, dimensions, import settings and GPU geometry budget, not border pixel equality. */
	UFUNCTION(BlueprintCallable, Category = "GPU Tiled Tessellation")
	bool ValidateTiles(FString& OutIssues) const;

	UFUNCTION(BlueprintPure, Category = "GPU Tiled Tessellation")
	int64 GetEstimatedGeometryBytes() const;

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual void OnRegister() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual void CollectPSOPrecacheData(const FPSOPrecacheParams& BaseParams, FMaterialInterfacePSOPrecacheParamsList& OutParams) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
