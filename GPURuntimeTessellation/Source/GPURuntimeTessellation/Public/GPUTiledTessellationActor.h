// Licensed under the MIT License. See LICENSE file in the project root.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GPUTiledTessellationActor.generated.h"

class UGPUTiledTessellationComponent;

UCLASS(BlueprintType, Blueprintable, ComponentWrapperClass, meta = (DisplayName = "GPU Tiled Tessellation Actor"))
class GPURUNTIMETESSELLATION_API AGPUTiledTessellationActor : public AActor
{
	GENERATED_BODY()
public:
	AGPUTiledTessellationActor();

	UFUNCTION(BlueprintPure, Category = "GPU Tiled Tessellation")
	UGPUTiledTessellationComponent* GetTessellationComponent() const { return TessellationComponent; }

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GPU Tiled Tessellation", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGPUTiledTessellationComponent> TessellationComponent;
};
