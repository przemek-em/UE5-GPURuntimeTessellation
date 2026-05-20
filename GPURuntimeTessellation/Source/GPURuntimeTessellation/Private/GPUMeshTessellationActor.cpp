// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUMeshTessellationActor.h"

#include "GPUMeshTessellationComponent.h"

AGPUMeshTessellationActor::AGPUMeshTessellationActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;

	MeshTessellationComponent = CreateDefaultSubobject<UGPUMeshTessellationComponent>(TEXT("MeshTessellationComponent"));
	SetRootComponent(MeshTessellationComponent);
}

#if WITH_EDITOR
void AGPUMeshTessellationActor::BakeCurrentTessellationToStaticMesh()
{
	if (MeshTessellationComponent)
	{
		MeshTessellationComponent->BakeCurrentTessellationToStaticMesh();
	}
}

UStaticMesh* AGPUMeshTessellationActor::BakeCurrentTessellationToStaticMeshAsset()
{
	return MeshTessellationComponent ? MeshTessellationComponent->BakeCurrentTessellationToStaticMeshAsset() : nullptr;
}

void AGPUMeshTessellationActor::UpdateTessellatedMesh()
{
	if (MeshTessellationComponent)
	{
		MeshTessellationComponent->UpdateTessellatedMesh();
	}
}
#endif
