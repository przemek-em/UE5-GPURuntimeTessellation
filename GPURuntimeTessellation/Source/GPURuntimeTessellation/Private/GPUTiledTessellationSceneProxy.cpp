// Licensed under the MIT License. See LICENSE file in the project root.
#include "GPUTiledTessellationSceneProxy.h"
#include "GPUTiledTessellationComponent.h"
#include "GPUTessellationRendering.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Materials/MaterialRenderProxy.h"
#include "TextureResource.h"
#include "RenderUtils.h"

FGPUTiledTessellationSceneProxy::FGPUTiledTessellationSceneProxy(const UGPUTiledTessellationComponent* Component)
	: FPrimitiveSceneProxy(Component)
	, TileSize(Component->TileSize)
	, QuadsPerTile(Component->QuadsPerTile)
	, HeightScale(Component->HeightScale)
	, HeightOffset(Component->HeightOffset)
	, LocalBounds(Component->CalcBounds(FTransform::Identity))
{
	bVFRequiresPrimitiveUniformBuffer = true;
	bSupportsGPUScene = false;
	bAffectDistanceFieldLighting = false;
	UMaterialInterface* Material = Component->GetMaterial(0);
	if (!Material) { Material = UMaterial::GetDefaultMaterial(MD_Surface); }
	MaterialProxy = Material->GetRenderProxy();
	MaterialRelevance = Material->GetRelevance(GetScene().GetShaderPlatform());
	const double MinHeight = FMath::Min(HeightOffset, HeightOffset + HeightScale) - 1.0;
	const double MaxHeight = FMath::Max(HeightOffset, HeightOffset + HeightScale) + 1.0;
	for (const FGPUTiledHeightmap& Tile : Component->Tiles)
	{
		FTile& RenderTile = Tiles.AddDefaulted_GetRef();
		RenderTile.Coordinate = Tile.Coordinate;
		// Resource initialization/destruction is ordered on the render thread by UTexture.
		// No UObjects are dereferenced in render-thread generation or draw submission.
		RenderTile.TextureResource = Tile.Heightmap->GetResource();
		const double X = double(Tile.Coordinate.X) * Component->TileSize.X;
		const double Y = double(Tile.Coordinate.Y) * Component->TileSize.Y;
		RenderTile.LocalBounds = FBoxSphereBounds(FBox(FVector(X, Y, MinHeight),
			FVector(X + Component->TileSize.X, Y + Component->TileSize.Y, MaxHeight)));
	}
}

void FGPUTiledTessellationSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	TArray<FGPUTiledTessellationInput> Inputs;
	for (const FTile& Tile : Tiles)
	{
		FGPUTiledTessellationInput& Input = Inputs.AddDefaulted_GetRef();
		Input.Coordinate = Tile.Coordinate;
		Input.Heightmap = Tile.TextureResource ? Tile.TextureResource->TextureRHI : FTextureRHIRef();
		if (!Input.Heightmap.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("GPU Tiled Tessellation: heightmap resource (%d,%d) is not ready; rebuild after initialization."), Tile.Coordinate.X, Tile.Coordinate.Y);
		}
	}
	GenerateGPUTiledTessellationGeometry(FRHICommandListExecutor::GetImmediateCommandList(), GetScene().GetFeatureLevel(),
		Inputs, TileSize, QuadsPerTile, HeightScale, HeightOffset, Buffers);
	for (const TUniquePtr<FGPUTessellationBuffers>& TileBuffers : Buffers)
	{
		auto& VF = VertexFactories.Emplace_GetRef(MakeUnique<FGPUTessellationVertexFactory>(GetScene().GetFeatureLevel()));
		auto& ShadowVF = ShadowVertexFactories.Emplace_GetRef(MakeUnique<FGPUTessellationGPUSceneVertexFactory>(GetScene().GetFeatureLevel()));
		if (!TileBuffers->IsValid()) { continue; }
		VF->SetBuffers(TileBuffers->PositionSRV, TileBuffers->NormalSRV, TileBuffers->UVSRV);
		ShadowVF->SetBuffers(TileBuffers->PositionSRV, TileBuffers->NormalSRV, TileBuffers->UVSRV);
		VF->InitResource(RHICmdList);
		ShadowVF->InitResource(RHICmdList);
	}
}

FGPUTiledTessellationSceneProxy::~FGPUTiledTessellationSceneProxy()
{
	for (auto& VF : VertexFactories) { VF->ReleaseResource(); }
	for (auto& VF : ShadowVertexFactories) { VF->ReleaseResource(); }
	for (auto& TileBuffers : Buffers) { TileBuffers->Reset(); }
}

void FGPUTiledTessellationSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	const bool bVSM = UseNonNaniteVirtualShadowMaps(GetScene().GetShaderPlatform(), GetScene().GetFeatureLevel());
	const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
	FMaterialRenderProxy* WireframeProxy = nullptr;
	if (bWireframe)
	{
		auto* Wireframe = new FColoredMaterialRenderProxy(GEngine->WireframeMaterial->GetRenderProxy(), FLinearColor(0.2f, 0.7f, 1.0f));
		Collector.RegisterOneFrameMaterialProxy(Wireframe);
		WireframeProxy = Wireframe;
	}
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (!(VisibilityMap & (1u << ViewIndex))) { continue; }
		const FSceneView* View = Views[ViewIndex];
		const FConvexVolume* ShadowFrustum = View->GetDynamicMeshElementsShadowCullFrustum();
		const bool bShadowView = ShadowFrustum != nullptr;
		auto& Uniform = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		SetGPUTessellationPrimitiveUniformBuffer(*this, Collector, GetBounds(), LocalBounds, Uniform);
		for (int32 Index = 0; Index < Buffers.Num(); ++Index)
		{
			const FGPUTessellationBuffers& TileBuffers = *Buffers[Index];
			const FBoxSphereBounds WorldBounds = Tiles[Index].LocalBounds.TransformBy(FTransform(GetLocalToWorld()));
			if (!TileBuffers.IsValid() || !IsGPUTessellationBoxVisible(*View, WorldBounds.Origin, WorldBounds.BoxExtent)) { continue; }
			FMeshBatch& Mesh = Collector.AllocateMesh();
			FMeshBatchElement& Element = Mesh.Elements[0];
			Element.IndexBuffer = &TileBuffers.IndexBuffer;
			Element.FirstIndex = 0;
			Element.NumPrimitives = TileBuffers.IndexCount / 3;
			Element.MinVertexIndex = 0;
			Element.MaxVertexIndex = TileBuffers.VertexCount - 1;
			Element.PrimitiveUniformBufferResource = &Uniform.UniformBuffer;
			Element.PrimitiveIdMode = PrimID_ForceZero;
			Mesh.VertexFactory = VertexFactories[Index].Get();
			Mesh.bWireframe = bWireframe && !bShadowView;
			Mesh.MaterialRenderProxy = Mesh.bWireframe ? WireframeProxy : MaterialProxy;
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = !bShadowView;
			Mesh.CastShadow = IsShadowCast(View);
			Collector.AddMesh(ViewIndex, Mesh);
			// Match the existing plugin's GPUScene shadow batch for non-Nanite VSM rasterization.
			if (bShadowView && bVSM && ShadowVertexFactories[Index]->IsInitialized())
			{
				FMeshBatch& ShadowMesh = Collector.AllocateMesh();
				ShadowMesh = Mesh;
				ShadowMesh.VertexFactory = ShadowVertexFactories[Index].Get();
				ShadowMesh.bWireframe = false;
				ShadowMesh.bCanApplyViewModeOverrides = false;
				ShadowMesh.bUseForMaterial = false;
				ShadowMesh.bUseForDepthPass = false;
				ShadowMesh.bUseAsOccluder = false;
				ShadowMesh.Elements[0].PrimitiveIdMode = PrimID_DynamicPrimitiveShaderData;
				Collector.AddMesh(ViewIndex, ShadowMesh);
			}
		}
	}
}

FPrimitiveViewRelevance FGPUTiledTessellationSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
	return Result;
}

uint32 FGPUTiledTessellationSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize() + Tiles.GetAllocatedSize() + Buffers.GetAllocatedSize()
		+ VertexFactories.GetAllocatedSize() + ShadowVertexFactories.GetAllocatedSize()
		+ Buffers.Num() * (sizeof(FGPUTessellationBuffers) + sizeof(FGPUTessellationVertexFactory) + sizeof(FGPUTessellationGPUSceneVertexFactory));
}

SIZE_T FGPUTiledTessellationSceneProxy::GetTypeHash() const
{
	static size_t Unique;
	return reinterpret_cast<SIZE_T>(&Unique);
}
