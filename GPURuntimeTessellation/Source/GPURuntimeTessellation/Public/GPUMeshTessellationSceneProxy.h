// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GPUMeshTessellationComponent.h"
#include "GPUTessellationMeshBuilder.h"
#include "GPUTessellationVertexFactory.h"

class FMaterialRenderProxy;

/** Scene proxy for UGPUMeshTessellationComponent. */
class FGPUMeshTessellationSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FGPUMeshTessellationSceneProxy(
		UGPUMeshTessellationComponent* Component,
		TArray<FGPUMeshTessellationBuildData>&& InBuildDataLODs,
		TArray<float>&& InLODDistanceThresholds,
		bool bInUseDistanceToBoundsForLOD);
	virtual ~FGPUMeshTessellationSceneProxy();

	virtual SIZE_T GetTypeHash() const override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }
	uint32 GetAllocatedSize() const { return FPrimitiveSceneProxy::GetAllocatedSize(); }

private:
	void RenderMesh(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector,
		FMaterialRenderProxy* WireframeMaterialInstance) const;

	FMaterialRenderProxy* GetSectionMaterialProxy(int32 MaterialIndex) const;
	int32 SelectLODIndex(const FSceneView* View, const FBoxSphereBounds& WorldBounds, bool bIsShadowView) const;
	bool HasAnyValidLOD() const;

private:
	TArray<FGPUMeshTessellationBuildData> BuildDataLODs;
	TArray<float> LODDistanceThresholds;
	TWeakObjectPtr<UTexture> DisplacementTexture;
	TArray<FMaterialRenderProxy*> MaterialProxies;
	FMaterialRelevance MaterialRelevance;
	FBoxSphereBounds LocalBounds;
	bool bUseDistanceToBoundsForLOD = true;

	mutable TArray<TUniquePtr<FGPUTessellationBuffers>> GPUBuffersLODs;
	mutable TArray<TUniquePtr<FGPUTessellationVertexFactory>> VertexFactoryLODs;
	mutable TArray<TUniquePtr<FGPUTessellationGPUSceneVertexFactory>> ShadowVertexFactoryLODs;
	mutable TArray<uint8> MeshValidLODs;
	mutable bool bAnyMeshValid = false;
};
