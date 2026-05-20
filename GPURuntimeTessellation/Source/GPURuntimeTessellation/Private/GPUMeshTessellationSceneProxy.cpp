// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUMeshTessellationSceneProxy.h"

#include "GPUMeshTessellationMeshBuilder.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Engine/Engine.h"
#include "MeshBatch.h"
#include "PrimitiveUniformShaderParameters.h"
#include "RenderGraphBuilder.h"
#include "RenderUtils.h"
#include "SceneManagement.h"
#include "TextureResource.h"

FGPUMeshTessellationSceneProxy::FGPUMeshTessellationSceneProxy(
	UGPUMeshTessellationComponent* Component,
	TArray<FGPUMeshTessellationBuildData>&& InBuildDataLODs,
	TArray<float>&& InLODDistanceThresholds,
	bool bInUseDistanceToBoundsForLOD)
	: FPrimitiveSceneProxy(Component)
	, BuildDataLODs(MoveTemp(InBuildDataLODs))
	, LODDistanceThresholds(MoveTemp(InLODDistanceThresholds))
	, DisplacementTexture(Component->DisplacementTexture)
	, LocalBounds(BuildDataLODs.Num() > 0 ? BuildDataLODs[0].LocalBounds : FBoxSphereBounds(EForceInit::ForceInit))
	, bUseDistanceToBoundsForLOD(bInUseDistanceToBoundsForLOD)
{
	bVFRequiresPrimitiveUniformBuffer = true;
	bSupportsGPUScene = false;
	bWillEverBeLit = true;
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = true;
	bAffectDistanceFieldLighting = true;

	const int32 MaterialCount = FMath::Max(Component->GetNumMaterials(), 1);
	MaterialProxies.Reserve(MaterialCount);
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
	{
		UMaterialInterface* Material = Component->GetMaterial(MaterialIndex);
		if (!Material)
		{
			Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}

		MaterialProxies.Add(Material ? Material->GetRenderProxy() : nullptr);
		if (Material)
		{
			MaterialRelevance |= Material->GetRelevance(GetScene().GetShaderPlatform());
		}
	}

	if (MaterialProxies.Num() == 0 || MaterialProxies[0] == nullptr)
	{
		if (UMaterial* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface))
		{
			MaterialProxies.Add(DefaultMaterial->GetRenderProxy());
			MaterialRelevance = DefaultMaterial->GetRelevance(GetScene().GetShaderPlatform());
		}
	}

	const int32 LODCount = FMath::Max(BuildDataLODs.Num(), 1);
	GPUBuffersLODs.Reserve(LODCount);
	VertexFactoryLODs.Reserve(LODCount);
	ShadowVertexFactoryLODs.Reserve(LODCount);
	MeshValidLODs.SetNumZeroed(LODCount);

	for (int32 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
	{
		GPUBuffersLODs.Add(MakeUnique<FGPUTessellationBuffers>());
		VertexFactoryLODs.Add(MakeUnique<FGPUTessellationVertexFactory>(GetScene().GetFeatureLevel()));
		ShadowVertexFactoryLODs.Add(MakeUnique<FGPUTessellationGPUSceneVertexFactory>(GetScene().GetFeatureLevel()));
	}

	ENQUEUE_RENDER_COMMAND(GenerateGPUMeshTessellation)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FGPUMeshTessellationMeshBuilder MeshBuilder;

			for (int32 LODIndex = 0; LODIndex < BuildDataLODs.Num(); ++LODIndex)
			{
				if (GPUBuffersLODs.IsValidIndex(LODIndex) && GPUBuffersLODs[LODIndex])
				{
					MeshBuilder.ExecuteTessellationPipeline(GraphBuilder, BuildDataLODs[LODIndex], DisplacementTexture.Get(), *GPUBuffersLODs[LODIndex]);
				}
			}

			GraphBuilder.Execute();

			bAnyMeshValid = false;
			for (int32 LODIndex = 0; LODIndex < BuildDataLODs.Num(); ++LODIndex)
			{
				const bool bLODValid = GPUBuffersLODs.IsValidIndex(LODIndex) &&
					GPUBuffersLODs[LODIndex] &&
					GPUBuffersLODs[LODIndex]->IsValid() &&
					VertexFactoryLODs.IsValidIndex(LODIndex) &&
					VertexFactoryLODs[LODIndex] &&
					ShadowVertexFactoryLODs.IsValidIndex(LODIndex) &&
					ShadowVertexFactoryLODs[LODIndex];

				MeshValidLODs[LODIndex] = bLODValid ? 1 : 0;
				if (bLODValid)
				{
					bAnyMeshValid = true;

					FGPUTessellationBuffers& LODBuffers = *GPUBuffersLODs[LODIndex];
					FGPUTessellationVertexFactory& LODVertexFactory = *VertexFactoryLODs[LODIndex];
					FGPUTessellationGPUSceneVertexFactory& LODShadowVertexFactory = *ShadowVertexFactoryLODs[LODIndex];

					LODVertexFactory.SetBuffers(LODBuffers.PositionSRV, LODBuffers.NormalSRV, LODBuffers.UVSRV, LODBuffers.TangentSRV);
					LODShadowVertexFactory.SetBuffers(LODBuffers.PositionSRV, LODBuffers.NormalSRV, LODBuffers.UVSRV, LODBuffers.TangentSRV);
					LODVertexFactory.SetHeightNormalParameters(false, FTextureRHIRef(), FTextureRHIRef(), false, 1.0f, 1.0f, 1.0f, 1000.0f, 1000.0f);
					LODShadowVertexFactory.SetHeightNormalParameters(false, FTextureRHIRef(), FTextureRHIRef(), false, 1.0f, 1.0f, 1.0f, 1000.0f, 1000.0f);
					LODVertexFactory.InitResource(RHICmdList);
					LODShadowVertexFactory.InitResource(RHICmdList);
				}

				BuildDataLODs[LODIndex].Vertices.Empty();
				BuildDataLODs[LODIndex].Indices.Empty();
				BuildDataLODs[LODIndex].SeamEdges.Empty();
			}
		});
}

FGPUMeshTessellationSceneProxy::~FGPUMeshTessellationSceneProxy()
{
	for (TUniquePtr<FGPUTessellationBuffers>& LODBuffers : GPUBuffersLODs)
	{
		if (LODBuffers)
		{
			LODBuffers->Reset();
		}
	}
	for (TUniquePtr<FGPUTessellationVertexFactory>& LODVertexFactory : VertexFactoryLODs)
	{
		if (LODVertexFactory)
		{
			LODVertexFactory->ReleaseResource();
		}
	}
	for (TUniquePtr<FGPUTessellationGPUSceneVertexFactory>& LODShadowVertexFactory : ShadowVertexFactoryLODs)
	{
		if (LODShadowVertexFactory)
		{
			LODShadowVertexFactory->ReleaseResource();
		}
	}
}

SIZE_T FGPUMeshTessellationSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FGPUMeshTessellationSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	if (!HasAnyValidLOD() || MaterialProxies.Num() == 0)
	{
		return;
	}

	FColoredMaterialRenderProxy* WireframeMaterialInstance = new FColoredMaterialRenderProxy(
		GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
		FLinearColor(0.0f, 0.5f, 1.0f));
	Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);

	RenderMesh(Views, ViewFamily, VisibilityMap, Collector, WireframeMaterialInstance);
}

void FGPUMeshTessellationSceneProxy::RenderMesh(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector,
	FMaterialRenderProxy* WireframeMaterialInstance) const
{
	const bool bNonNaniteVSMEnabled = UseNonNaniteVirtualShadowMaps(GetScene().GetShaderPlatform(), GetScene().GetFeatureLevel());
	const FBoxSphereBounds WorldBounds = LocalBounds.TransformBy(FTransform(GetLocalToWorld()));

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0)
		{
			continue;
		}

		const FSceneView* View = Views[ViewIndex];
		const bool bIsShadowView = View && View->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
		const int32 LODIndex = SelectLODIndex(View, WorldBounds, bIsShadowView);
		if (!BuildDataLODs.IsValidIndex(LODIndex) ||
			!GPUBuffersLODs.IsValidIndex(LODIndex) ||
			!GPUBuffersLODs[LODIndex] ||
			!VertexFactoryLODs.IsValidIndex(LODIndex) ||
			!VertexFactoryLODs[LODIndex] ||
			!ShadowVertexFactoryLODs.IsValidIndex(LODIndex) ||
			!ShadowVertexFactoryLODs[LODIndex] ||
			!MeshValidLODs.IsValidIndex(LODIndex) ||
			MeshValidLODs[LODIndex] == 0)
		{
			continue;
		}

		const FGPUMeshTessellationBuildData& SelectedBuildData = BuildDataLODs[LODIndex];
		FGPUTessellationBuffers& SelectedGPUBuffers = *GPUBuffersLODs[LODIndex];
		FGPUTessellationVertexFactory& SelectedVertexFactory = *VertexFactoryLODs[LODIndex];
		FGPUTessellationGPUSceneVertexFactory& SelectedShadowVertexFactory = *ShadowVertexFactoryLODs[LODIndex];
		const bool bSubmitVSMShadowMesh = bIsShadowView && bNonNaniteVSMEnabled && SelectedShadowVertexFactory.IsInitialized();

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		DynamicPrimitiveUniformBuffer.Set(
			Collector.GetRHICommandList(),
			GetLocalToWorld(),
			GetLocalToWorld(),
			WorldBounds,
			LocalBounds,
			false,
			false,
			false);

		for (const FGPUMeshTessellationSection& Section : SelectedBuildData.Sections)
		{
			if (Section.NumTriangles == 0)
			{
				continue;
			}

			FMeshBatch& Mesh = Collector.AllocateMesh();
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			BatchElement.IndexBuffer = &SelectedGPUBuffers.IndexBuffer;
			BatchElement.FirstIndex = Section.FirstIndex;
			BatchElement.NumPrimitives = Section.NumTriangles;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = SelectedGPUBuffers.VertexCount - 1;
			BatchElement.BaseVertexIndex = 0;
			BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
			BatchElement.PrimitiveIdMode = PrimID_ForceZero;

			Mesh.bWireframe = !bIsShadowView && AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
			Mesh.VertexFactory = &SelectedVertexFactory;
			Mesh.MaterialRenderProxy = Mesh.bWireframe ? WireframeMaterialInstance : GetSectionMaterialProxy(Section.MaterialIndex);
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = !bIsShadowView;
			Mesh.CastShadow = IsShadowCast(View);

			Collector.AddMesh(ViewIndex, Mesh);

			if (bSubmitVSMShadowMesh)
			{
				FMeshBatch& VSMShadowMesh = Collector.AllocateMesh();
				VSMShadowMesh = Mesh;
				VSMShadowMesh.VertexFactory = &SelectedShadowVertexFactory;
				VSMShadowMesh.bWireframe = false;
				VSMShadowMesh.bCanApplyViewModeOverrides = false;
				VSMShadowMesh.bUseForMaterial = false;
				VSMShadowMesh.bUseForDepthPass = false;
				VSMShadowMesh.bUseAsOccluder = false;
				VSMShadowMesh.Elements[0].PrimitiveIdMode = PrimID_DynamicPrimitiveShaderData;
				Collector.AddMesh(ViewIndex, VSMShadowMesh);
			}
		}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
#endif
	}
}

FMaterialRenderProxy* FGPUMeshTessellationSceneProxy::GetSectionMaterialProxy(int32 MaterialIndex) const
{
	if (MaterialProxies.IsValidIndex(MaterialIndex) && MaterialProxies[MaterialIndex])
	{
		return MaterialProxies[MaterialIndex];
	}

	return MaterialProxies.Num() > 0 ? MaterialProxies[0] : nullptr;
}

int32 FGPUMeshTessellationSceneProxy::SelectLODIndex(const FSceneView* View, const FBoxSphereBounds& WorldBounds, bool bIsShadowView) const
{
	if (BuildDataLODs.Num() <= 1 || LODDistanceThresholds.Num() == 0 || !View || bIsShadowView)
	{
		return 0;
	}

	const FVector ViewOrigin = View->ViewMatrices.GetViewOrigin();
	const float Distance = bUseDistanceToBoundsForLOD
		? FMath::Sqrt(WorldBounds.ComputeSquaredDistanceFromBoxToPoint(ViewOrigin))
		: FVector::Dist(WorldBounds.Origin, ViewOrigin);

	const FVector Scale3D = FTransform(GetLocalToWorld()).GetScale3D();
	const float MaxScale = FMath::Max3(FMath::Abs(Scale3D.X), FMath::Abs(Scale3D.Y), FMath::Abs(Scale3D.Z));
	const float ScaledDistance = Distance / FMath::Max(MaxScale, 0.0001f);
	int32 LODIndex = 0;
	const int32 MaxThresholds = FMath::Min(LODDistanceThresholds.Num(), BuildDataLODs.Num() - 1);
	for (int32 ThresholdIndex = 0; ThresholdIndex < MaxThresholds; ++ThresholdIndex)
	{
		if (ScaledDistance > LODDistanceThresholds[ThresholdIndex])
		{
			LODIndex = ThresholdIndex + 1;
		}
		else
		{
			break;
		}
	}

	return FMath::Clamp(LODIndex, 0, BuildDataLODs.Num() - 1);
}

bool FGPUMeshTessellationSceneProxy::HasAnyValidLOD() const
{
	if (bAnyMeshValid)
	{
		return true;
	}

	for (uint8 bLODValid : MeshValidLODs)
	{
		if (bLODValid != 0)
		{
			return true;
		}
	}

	return false;
}

FPrimitiveViewRelevance FGPUMeshTessellationSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && HasAnyValidLOD();
	Result.bShadowRelevance = IsShadowCast(View) && HasAnyValidLOD();
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;

	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}
