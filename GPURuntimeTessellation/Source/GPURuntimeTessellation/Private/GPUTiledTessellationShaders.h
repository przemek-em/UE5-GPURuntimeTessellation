// Licensed under the MIT License. See LICENSE file in the project root.
#pragma once
#include "GPUTessellationMeshBuilder.h"

struct FGPUTiledTessellationInput
{
	FIntPoint Coordinate;
	FTextureRHIRef Heightmap;
};

// Render-thread entry point. Inputs are RHI snapshots, never UObjects.
void GenerateGPUTiledTessellationGeometry(
	FRHICommandListImmediate& RHICmdList, ERHIFeatureLevel::Type FeatureLevel,
	TConstArrayView<FGPUTiledTessellationInput> Tiles, const FVector2f& TileSize,
	int32 QuadsPerTile, float HeightScale, float HeightOffset,
	TArray<TUniquePtr<FGPUTessellationBuffers>>& OutBuffers);
