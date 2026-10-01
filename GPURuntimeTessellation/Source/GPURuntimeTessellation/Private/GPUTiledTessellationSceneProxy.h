// Licensed under the MIT License. See LICENSE file in the project root.
#pragma once
#include "PrimitiveSceneProxy.h"
#include "GPUTiledTessellationShaders.h"
#include "GPUTessellationVertexFactory.h"
#include "MaterialShared.h"

class UGPUTiledTessellationComponent;
class FTextureResource;

class FGPUTiledTessellationSceneProxy final : public FPrimitiveSceneProxy
{
public:
	explicit FGPUTiledTessellationSceneProxy(const UGPUTiledTessellationComponent* Component);
	virtual ~FGPUTiledTessellationSceneProxy();
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual SIZE_T GetTypeHash() const override;

private:
	struct FTile
	{
		FIntPoint Coordinate;
		FTextureResource* TextureResource = nullptr;
		FBoxSphereBounds LocalBounds;
	};
	TArray<FTile> Tiles;
	TArray<TUniquePtr<FGPUTessellationBuffers>> Buffers;
	TArray<TUniquePtr<FGPUTessellationVertexFactory>> VertexFactories;
	TArray<TUniquePtr<FGPUTessellationGPUSceneVertexFactory>> ShadowVertexFactories;
	FVector2f TileSize;
	int32 QuadsPerTile;
	float HeightScale;
	float HeightOffset;
	FBoxSphereBounds LocalBounds;
	FMaterialRenderProxy* MaterialProxy;
	FMaterialRelevance MaterialRelevance;
};
