// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "GPUMeshTessellationComponent.h"
#include "GameFramework/Actor.h"
#include "GPUMeshTessellationActor.generated.h"

class UStaticMesh;

/**
 * Placeable actor wrapper for arbitrary static meshes rendered through the GPU
 * Runtime Tessellation compute path.
 */
UCLASS(BlueprintType, Blueprintable, ComponentWrapperClass, meta = (DisplayName = "GPU Mesh Tessellation Actor", ChildCanTick))
class GPURUNTIMETESSELLATION_API AGPUMeshTessellationActor : public AActor
{
	GENERATED_BODY()

public:
	AGPUMeshTessellationActor(const FObjectInitializer& ObjectInitializer);

	UFUNCTION(BlueprintPure, Category = "GPU Mesh Tessellation")
	UGPUMeshTessellationComponent* GetMeshTessellationComponent() const { return MeshTessellationComponent.Get(); }

#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Mesh Tessellation", meta = (DisplayName = "Bake Current Tessellation To Static Mesh"))
	void BakeCurrentTessellationToStaticMesh();

	UFUNCTION(BlueprintCallable, Category = "GPU Mesh Tessellation")
	UStaticMesh* BakeCurrentTessellationToStaticMeshAsset();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Mesh Tessellation", meta = (DisplayName = "Regenerate Tessellated Mesh"))
	void UpdateTessellatedMesh();
#endif

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GPU Mesh Tessellation", meta = (ExposeFunctionCategories = "GPU Mesh Tessellation,Mesh,Rendering,Physics,Components|StaticMesh", AllowPrivateAccess = "true"))
	TObjectPtr<UGPUMeshTessellationComponent> MeshTessellationComponent;
};
