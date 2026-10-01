// Licensed under the MIT License. See LICENSE file in the project root.
#include "GPUTiledTessellationActor.h"
#include "GPUTiledTessellationComponent.h"

AGPUTiledTessellationActor::AGPUTiledTessellationActor()
{
	TessellationComponent = CreateDefaultSubobject<UGPUTiledTessellationComponent>(TEXT("TiledTessellation"));
	SetRootComponent(TessellationComponent);
}
