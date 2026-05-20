// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GPUTessellationComponent.h"
#include "GPUTessellationActor.generated.h"

class UStaticMesh;

/** Placeable actor wrapper for the standard GPU Runtime Tessellation plane component. */
UCLASS(BlueprintType, Blueprintable, ComponentWrapperClass, meta = (DisplayName = "GPU Tessellation Actor", ChildCanTick))
class GPURUNTIMETESSELLATION_API AGPUTessellationActor : public AActor
{
	GENERATED_BODY()

public:
	AGPUTessellationActor(const FObjectInitializer& ObjectInitializer);

	UFUNCTION(BlueprintPure, Category = "GPU Tessellation")
	UGPUTessellationComponent* GetTessellationComponent() const { return TessellationComponent.Get(); }

#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Tessellation", meta = (DisplayName = "Bake Current Tessellation To Static Mesh"))
	void BakeCurrentTessellationToStaticMesh();

	UFUNCTION(BlueprintCallable, Category = "GPU Tessellation")
	UStaticMesh* BakeCurrentTessellationToStaticMeshAsset();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Tessellation", meta = (DisplayName = "Regenerate Tessellated Mesh"))
	void UpdateTessellatedMesh();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Tessellation", meta = (DisplayName = "Rebuild Collision Mesh"))
	void RebuildCollisionMesh();
#endif

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GPU Tessellation", meta = (ExposeFunctionCategories = "GPU Tessellation,Rendering,Physics,Collision,Components|Mesh", AllowPrivateAccess = "true"))
	TObjectPtr<UGPUTessellationComponent> TessellationComponent;
};
