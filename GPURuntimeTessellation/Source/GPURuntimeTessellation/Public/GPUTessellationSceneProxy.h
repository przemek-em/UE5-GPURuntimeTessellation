// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GPUTessellationMeshBuilder.h"
#include "GPUTessellationVertexFactory.h"
#include "GPUTessellationComponent.h"

class FMaterialRenderProxy;

/**
 * Dynamic data for patch updates (camera position)
 */
struct FGPUTessellationDynamicData
{
	FVector CameraPosition;
	FMatrix LocalToWorld;
	
	FGPUTessellationDynamicData()
		: CameraPosition(FVector::ZeroVector)
		, LocalToWorld(FMatrix::Identity)
	{}
};

/**
 * GPU Tessellation Scene Proxy
 * 
 * Manages rendering representation of the tessellated mesh.
 * Uses pure GPU buffers without CPU readback for rendering.
 */
class FGPUTessellationSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FGPUTessellationSceneProxy(UGPUTessellationComponent* Component);
	virtual ~FGPUTessellationSceneProxy();

	//~ Begin FPrimitiveSceneProxy Interface
	virtual SIZE_T GetTypeHash() const override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }
	uint32 GetAllocatedSize() const { return FPrimitiveSceneProxy::GetAllocatedSize(); }
	//~ End FPrimitiveSceneProxy Interface

	/**
	 * Update mesh buffers (called from render thread)
	 */
	void UpdateMeshBuffers_RenderThread(const FGPUTessellationBuffers& Buffers);

	/**
	 * Update dynamic data (camera position for patch LOD)
	 */
	void UpdateDynamicData_RenderThread(FGPUTessellationDynamicData* DynamicData);

private:
	/** Render single mesh (original mode) */
	void RenderSingleMesh(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector,
		FMaterialRenderProxy* WireframeMaterialInstance) const;

	/** Render all patches (spatial patch mode) */
	void RenderPatches(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector,
		FMaterialRenderProxy* WireframeMaterialInstance) const;

	/** Initialize vertex factories for all patches */
	void InitializePatchVertexFactories(FRHICommandListImmediate& RHICmdList);

	/** Apply height-source pixel normal parameters to a vertex factory. */
	void ConfigureVertexFactoryHeightNormals(FGPUTessellationVertexFactory& InVertexFactory) const;

	/** Update patch metadata/desired LOD state without reallocating patch geometry buffers. */
	bool UpdatePersistentPatchState_RenderThread(const FVector& CameraPosition, const FMatrix& ComponentTransform);

	/** Emit experimental persistent-patch stats when debug logging is enabled. */
	void LogPersistentPatchStats_RenderThread(const TCHAR* Reason) const;

private:
	/** Material render proxy */
	FMaterialRenderProxy* MaterialProxy;

	/** Tessellation settings */
	FGPUTessellationSettings Settings;

	/** Cached transforms and textures for dynamic patch rebuild/state updates */
	FMatrix CachedLocalToWorld;
	TObjectPtr<UTexture> CachedDisplacementTexture;
	TObjectPtr<UTexture> CachedVectorDisplacementTexture;
	TObjectPtr<UTexture> CachedSubtractTexture;
	TObjectPtr<UTexture> CachedNormalMapTexture;

	/** GPU buffers (persistent, no CPU copy) - for single mesh mode */
	mutable FGPUTessellationBuffers GPUBuffers;

	/** GPU patch buffers - for spatial patch mode */
	mutable FGPUTessellationPatchBuffers GPUPatchBuffers;

	/** Most recent camera-derived patch LOD state. In persistent mode this can differ from generated geometry. */
	mutable TArray<FGPUTessellationPatchInfo> LatestPatchLODState;

	/** Vertex factory for GPU buffer rendering - single mesh */
	mutable FGPUTessellationVertexFactory VertexFactory;
	mutable FGPUTessellationGPUSceneVertexFactory ShadowVertexFactory;

	/** Vertex factories for patch rendering - one per patch (array of pointers since vertex factory requires constructor args) */
	mutable TArray<FGPUTessellationVertexFactory*> PatchVertexFactories;
	mutable TArray<FGPUTessellationGPUSceneVertexFactory*> PatchShadowVertexFactories;

	/** Is mesh data valid and ready to render */
	mutable bool bMeshValid;

	/** Are we using spatial patch mode? */
	bool bUsePatchMode;

	/** Material relevance */
	FMaterialRelevance MaterialRelevance;

	/** Enable debug logging */
	bool bEnableDebugLogging;

	/** Show patch debug visualization (bounds boxes and centers) */
	bool bShowPatchDebugVisualization;

	/** Last log time for throttling */
	mutable double LastLogTime;

	/** Last camera position used for patch generation (to detect movement) */
	mutable FVector LastCameraPosition;

	/** Experimental patch persistence debug counters. */
	uint64 PatchGeometryRebuildCount = 0;
	uint64 PatchStateUpdateCount = 0;
	uint64 PatchVertexFactoryReinitCount = 0;
	mutable double LastPersistentPatchStatsLogTime = 0.0;

	friend class UGPUTessellationComponent;
};
