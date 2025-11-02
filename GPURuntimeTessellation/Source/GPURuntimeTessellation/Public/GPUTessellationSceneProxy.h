// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GPUTessellationMeshBuilder.h"
#include "GPUTessellationVertexFactory.h"

class UGPUTessellationComponent;
class FMaterialRenderProxy;
struct FGPUTessellationSettings;

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

private:
	/** Material render proxy */
	FMaterialRenderProxy* MaterialProxy;

	/** Tessellation settings */
	FGPUTessellationSettings Settings;

	/** Cached transforms and textures for patch regeneration */
	FMatrix CachedLocalToWorld;
	TObjectPtr<UTexture2D> CachedDisplacementTexture;
	TObjectPtr<UTexture2D> CachedSubtractTexture;
	TObjectPtr<UTexture2D> CachedNormalMapTexture;

	/** GPU buffers (persistent, no CPU copy) - for single mesh mode */
	mutable FGPUTessellationBuffers GPUBuffers;

	/** GPU patch buffers - for spatial patch mode */
	mutable FGPUTessellationPatchBuffers GPUPatchBuffers;

	/** Vertex factory for GPU buffer rendering - single mesh */
	mutable FGPUTessellationVertexFactory VertexFactory;

	/** Vertex factories for patch rendering - one per patch (array of pointers since vertex factory requires constructor args) */
	mutable TArray<FGPUTessellationVertexFactory*> PatchVertexFactories;

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

	friend class UGPUTessellationComponent;
};
