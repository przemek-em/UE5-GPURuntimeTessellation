// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUVectorDisplacementActor.h"

#include "GPUVectorDisplacementComponent.h"

AGPUVectorDisplacementActor::AGPUVectorDisplacementActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;

	VectorDisplacementComponent = CreateDefaultSubobject<UGPUVectorDisplacementComponent>(TEXT("VectorDisplacementComponent"));
	SetRootComponent(VectorDisplacementComponent);
}

#if WITH_EDITOR
void AGPUVectorDisplacementActor::BakeCurrentVectorDisplacementToStaticMesh()
{
	if (VectorDisplacementComponent)
	{
		VectorDisplacementComponent->BakeCurrentVectorDisplacementToStaticMesh();
	}
}

UStaticMesh* AGPUVectorDisplacementActor::BakeCurrentVectorDisplacementToStaticMeshAsset()
{
	return VectorDisplacementComponent ? VectorDisplacementComponent->BakeCurrentVectorDisplacementToStaticMeshAsset() : nullptr;
}

void AGPUVectorDisplacementActor::UpdateVectorDisplacementMesh()
{
	if (VectorDisplacementComponent)
	{
		VectorDisplacementComponent->UpdateTessellatedMesh();
	}
}

void AGPUVectorDisplacementActor::ValidateVectorDisplacementTexture() const
{
	if (VectorDisplacementComponent)
	{
		VectorDisplacementComponent->ValidateVectorDisplacementTexture();
	}
}

void AGPUVectorDisplacementActor::RebuildCollisionMesh()
{
	if (VectorDisplacementComponent)
	{
		VectorDisplacementComponent->RebuildCollisionMesh();
	}
}
#endif
