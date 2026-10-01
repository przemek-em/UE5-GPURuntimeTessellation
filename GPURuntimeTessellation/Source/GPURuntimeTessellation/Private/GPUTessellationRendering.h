// Licensed under the MIT License. See LICENSE file in the project root.
#pragma once

#include "GPUTessellationVertexFactory.h"
#include "Components/MeshComponent.h"
#include "Materials/Material.h"
#include "PSOPrecache.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "PrimitiveUniformShaderParametersBuilder.h"

// Shadow views retain camera state, but their caster volume can include offscreen
// geometry. Local-light volumes also use translated coordinates (see ShadowSetup).
inline bool IsGPUTessellationBoxVisible(const FSceneView& View, const FVector& WorldCenter, const FVector& WorldExtent)
{
	if (const FConvexVolume* ShadowFrustum = View.GetDynamicMeshElementsShadowCullFrustum())
	{
		return ShadowFrustum->IntersectBox(WorldCenter + View.GetPreShadowTranslation(), WorldExtent);
	}
	return View.GetCullingFrustum().IntersectBox(WorldCenter, WorldExtent);
}

// Use the scene's transform history, including the engine's teleport/reset policy.
// This supplies rigid motion only; compute deformation needs previous position buffers.
inline void SetGPUTessellationPrimitiveUniformBuffer(
	const FPrimitiveSceneProxy& Proxy, FMeshElementCollector& Collector,
	const FBoxSphereBounds& WorldBounds, const FBoxSphereBounds& LocalBounds,
	FDynamicPrimitiveUniformBuffer& UniformBuffer)
{
	FPrimitiveUniformShaderParametersBuilder Builder;
	Proxy.BuildUniformShaderParameters(Builder);
	// Preserve the generated mesh's bounds while retaining all engine primitive state:
	// transform history, decals, lighting channels, capture index, custom data and WPO.
	Builder.WorldBounds(WorldBounds).LocalBounds(LocalBounds).PreSkinnedLocalBounds(LocalBounds);
	UniformBuffer.Set(Collector.GetRHICommandList(), Builder);
}

inline void CollectGPUTessellationPSOPrecacheData(
	const UMeshComponent& Component, const FPSOPrecacheParams& BaseParams,
	FMaterialInterfacePSOPrecacheParamsList& OutParams)
{
	TArray<UMaterialInterface*> Materials;
	Component.GetUsedMaterials(Materials);
	Materials.AddUnique(UMaterial::GetDefaultMaterial(MD_Surface));
	for (UMaterialInterface* Material : Materials)
	{
		if (!Material) { continue; }
		FMaterialInterfacePSOPrecacheParams Params;
		Params.MaterialInterface = Material;
		Params.PSOPrecacheParams = BaseParams;
		Params.PSOPrecacheParams.bStaticLighting = false;
		Params.VertexFactoryDataList.Add(&FGPUTessellationVertexFactory::StaticType);
		Params.VertexFactoryDataList.Add(&FGPUTessellationGPUSceneVertexFactory::StaticType);
		AddMaterialInterfacePSOPrecacheParamsToList(Params, OutParams);
	}
}
