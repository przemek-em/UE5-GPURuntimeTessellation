// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUTessellationActor.h"

#include "GPUTessellationComponent.h"

AGPUTessellationActor::AGPUTessellationActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;

	TessellationComponent = CreateDefaultSubobject<UGPUTessellationComponent>(TEXT("TessellationComponent"));
	SetRootComponent(TessellationComponent);
}

#if WITH_EDITOR
void AGPUTessellationActor::BakeCurrentTessellationToStaticMesh()
{
	if (TessellationComponent)
	{
		TessellationComponent->BakeCurrentTessellationToStaticMesh();
	}
}

UStaticMesh* AGPUTessellationActor::BakeCurrentTessellationToStaticMeshAsset()
{
	return TessellationComponent ? TessellationComponent->BakeCurrentTessellationToStaticMeshAsset() : nullptr;
}

void AGPUTessellationActor::UpdateTessellatedMesh()
{
	if (TessellationComponent)
	{
		TessellationComponent->UpdateTessellatedMesh();
	}
}

void AGPUTessellationActor::RebuildCollisionMesh()
{
	if (TessellationComponent)
	{
		TessellationComponent->RebuildCollisionMesh();
	}
}
#endif
