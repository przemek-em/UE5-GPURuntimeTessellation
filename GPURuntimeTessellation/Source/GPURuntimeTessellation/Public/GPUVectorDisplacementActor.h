// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "GPUVectorDisplacementComponent.h"
#include "GameFramework/Actor.h"
#include "GPUVectorDisplacementActor.generated.h"

class UStaticMesh;

/** Placeable actor wrapper for the vector displacement tessellated plane component. */
UCLASS(
	BlueprintType,
	Blueprintable,
	ComponentWrapperClass,
	meta = (DisplayName = "GPU Vector Displacement Actor", ChildCanTick),
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
class GPURUNTIMETESSELLATION_API AGPUVectorDisplacementActor : public AActor
{
	GENERATED_BODY()

public:
	AGPUVectorDisplacementActor(const FObjectInitializer& ObjectInitializer);

	UFUNCTION(BlueprintPure, Category = "GPU Vector Displacement")
	UGPUVectorDisplacementComponent* GetVectorDisplacementComponent() const { return VectorDisplacementComponent.Get(); }

#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Vector Displacement", meta = (DisplayName = "Bake Current Vector Displacement To Static Mesh"))
	void BakeCurrentVectorDisplacementToStaticMesh();

	UFUNCTION(BlueprintCallable, Category = "GPU Vector Displacement")
	UStaticMesh* BakeCurrentVectorDisplacementToStaticMeshAsset();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Vector Displacement", meta = (DisplayName = "Regenerate Vector Displacement Mesh"))
	void UpdateVectorDisplacementMesh();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Vector Displacement", meta = (DisplayName = "Validate Vector Displacement Texture"))
	void ValidateVectorDisplacementTexture() const;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Tessellation", meta = (DisplayName = "Rebuild Collision Mesh"))
	void RebuildCollisionMesh();
#endif

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GPU Vector Displacement", meta = (ExposeFunctionCategories = "GPU Tessellation,GPU Vector Displacement,Rendering,Physics,Collision,Components|Mesh", AllowPrivateAccess = "true"))
	TObjectPtr<UGPUVectorDisplacementComponent> VectorDisplacementComponent;
};
