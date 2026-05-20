// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUTessellationSceneProxy.h"
#include "GPUTessellationComponent.h"
#include "GPUTessellationMeshBuilder.h"
#include "GPUTessellationVertexFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Engine/Engine.h"
#include "RenderingThread.h"
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"
#include "RenderUtils.h"
#include "MeshBatch.h"
#include "RayTracingDefinitions.h"
#include "RayTracingInstance.h"
#include "DrawDebugHelpers.h"
#include "PrimitiveUniformShaderParameters.h"
#include "TextureResource.h"

namespace
{
	FTextureRHIRef GetGPUTessellationTextureRHI(UTexture* Texture)
	{
		if (!Texture)
		{
			return FTextureRHIRef();
		}

		FTextureResource* TextureResource = Texture->GetResource();
		return TextureResource ? TextureResource->TextureRHI : FTextureRHIRef();
	}

	FVector GetGPUTessellationVectorDisplacementBoundsExtent(const FGPUTessellationSettings& Settings)
	{
		if (!Settings.bUseVectorDisplacement)
		{
			return FVector::ZeroVector;
		}

		const FVector Scale = Settings.VectorDisplacementScale * Settings.VectorDisplacementIntensity;
		const FVector Bias = Settings.VectorDisplacementBias * Settings.VectorDisplacementIntensity;
		return FVector(
			FMath::Abs(Scale.X) + FMath::Abs(Bias.X) + FMath::Max(0.0, Settings.VectorDisplacementBoundsPadding.X),
			FMath::Abs(Scale.Y) + FMath::Abs(Bias.Y) + FMath::Max(0.0, Settings.VectorDisplacementBoundsPadding.Y),
			FMath::Abs(Scale.Z) + FMath::Abs(Bias.Z) + FMath::Max(0.0, Settings.VectorDisplacementBoundsPadding.Z));
	}

	float GetGPUTessellationScalarDisplacementBoundsExtent(const FGPUTessellationSettings& Settings)
	{
		if (Settings.bUseVectorDisplacement && !Settings.bAddScalarHeightDisplacementToVector)
		{
			return 1.0f;
		}

		return Settings.DisplacementIntensity + FMath::Abs(Settings.DisplacementOffset) + 1.0f;
	}
}

FGPUTessellationSceneProxy::FGPUTessellationSceneProxy(UGPUTessellationComponent* Component)
	: FPrimitiveSceneProxy(Component)
	, MaterialProxy(nullptr)
	, Settings(Component->GetEffectiveTessellationSettings())
	, CachedLocalToWorld(Component->GetComponentTransform().ToMatrixWithScale())
	, CachedDisplacementTexture(Component->DisplacementTexture)
	, CachedVectorDisplacementTexture(Component->GetVectorDisplacementTexture())
	, CachedSubtractTexture(Component->SubtractTexture)
	, CachedNormalMapTexture(Component->NormalMapTexture)
	, VertexFactory(GetScene().GetFeatureLevel())
	, ShadowVertexFactory(GetScene().GetFeatureLevel())
	, bMeshValid(false)
	, bUsePatchMode(Settings.LODMode == EGPUTessellationLODMode::DistanceBasedPatches || Settings.LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree)
	, bEnableDebugLogging(Component->bEnableDebugLogging)
	, bShowPatchDebugVisualization(Component->bShowPatchDebugVisualization)
	, LastLogTime(0.0)
	, LastCameraPosition(FVector::ZeroVector)
{
	// This proxy's vertex factory fetches positions from custom SRVs and uses a
	// per-batch primitive uniform buffer for the component transform. Keeping it
	// off the GPUScene primitive-id stream avoids instance-culling/view-path
	// differences corrupting manual vertex fetches on large scaled planes.
	bVFRequiresPrimitiveUniformBuffer = true;
	bSupportsGPUScene = false;

	// Throttled debug logging
	if (bEnableDebugLogging)
	{
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastLogTime >= 2.0)
		{
			LastLogTime = CurrentTime;
			// Log component bounds and transform
			const FTransform ComponentTransform = Component->GetComponentTransform();
			const FBoxSphereBounds CompBounds = Component->Bounds;
			const FBoxSphereBounds RecalcBounds = Component->CalcBounds(ComponentTransform);
			
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Scene Proxy Constructor:"));
			UE_LOG(LogTemp, Warning, TEXT("  Component->Bounds: %s"), *CompBounds.ToString());
			UE_LOG(LogTemp, Warning, TEXT("  CalcBounds(Transform): %s"), *RecalcBounds.ToString());
			UE_LOG(LogTemp, Warning, TEXT("  Transform Location: %s Scale: %s"), 
				*ComponentTransform.GetLocation().ToString(), *ComponentTransform.GetScale3D().ToString());
			const float TotalDisp = Settings.DisplacementIntensity + FMath::Abs(Settings.DisplacementOffset);
			UE_LOG(LogTemp, Warning, TEXT("  Settings: PlaneSizeX:%.1f PlaneSizeY:%.1f Disp:%.1f"),
				Settings.PlaneSizeX, Settings.PlaneSizeY, TotalDisp);
		}
	}
	
	// Get material
	if (Component->Material)
	{
		MaterialProxy = Component->Material->GetRenderProxy();
		MaterialRelevance = Component->Material->GetRelevance(GetScene().GetShaderPlatform());
	}
	else if (UMaterial::GetDefaultMaterial(MD_Surface))
	{
		MaterialProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
		MaterialRelevance = UMaterial::GetDefaultMaterial(MD_Surface)->GetRelevance(GetScene().GetShaderPlatform());
	}
	
	if (bEnableDebugLogging)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Material setup - HasMaterial:%d"), MaterialProxy != nullptr);
	}

	// Generate initial mesh data (PURE GPU - NO CPU READBACK!)
	FGPUTessellationMeshBuilder MeshBuilder;
	FVector CameraPosition = FVector::ZeroVector;

	// Get camera position if available
	if (UWorld* World = Component->GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager)
			{
				CameraPosition = PC->PlayerCameraManager->GetCameraLocation();
			}
		}
	}
	
	// CRITICAL: If camera position is not available, DO NOT use component location!
	// Using component location makes patches appear sorted by distance from plane center, not camera.
	// Better to use a reasonable default camera position (above and away from plane)
	// FIX (CRITICAL #1): The previous condition `!CameraPosition.ContainsNaN()` was inverted –
	// it was true for every healthy vector and silently overwrote the real camera position.
	if (CameraPosition.IsZero() || CameraPosition.ContainsNaN())
	{
		// Use a reasonable default: above the component, looking down
		FVector ComponentLocation = Component->GetComponentLocation();
		CameraPosition = ComponentLocation + FVector(0, 0, 2000.0f); // 2000 units above
		
		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Camera position unavailable, using default position above component: %s"), 
				*CameraPosition.ToString());
		}
	}
	else if (bEnableDebugLogging)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Camera position: %s (Component at: %s)"), 
			*CameraPosition.ToString(), *Component->GetComponentLocation().ToString());
	}

	// Prepare settings with effective tessellation factor for LOD mode
	FGPUTessellationSettings EffectiveSettings = Settings;
	if (Settings.LODMode != EGPUTessellationLODMode::Disabled)
	{
		// When LOD is enabled, use LastAppliedTessFactor instead of TessellationFactor
		EffectiveSettings.TessellationFactor = Component->LastAppliedTessFactor;
		
		if (bEnableDebugLogging)
		{
			const int32 OriginalFactor = Settings.TessellationFactor;
			const int32 MinFactor = Settings.MinTessellationFactor;
			const int32 MaxFactor = Settings.MaxTessellationFactor;
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: SceneProxy using LOD-adjusted TessellationFactor: %d (Original: %d, Min: %d, Max: %d)"),
				Component->LastAppliedTessFactor, OriginalFactor, MinFactor, MaxFactor);
		}
	}
	
	// Choose mesh generation path based on mode
	// FIX (CRITICAL #3): Capture UTexture pointers via TWeakObjectPtr so a GC pass between
	// the game-thread enqueue and the render-thread execution cannot leave us with dangling raw
	// UObject pointers inside the render-thread lambda.
	TWeakObjectPtr<UTexture> WeakDisplacement(Component->DisplacementTexture);
	TWeakObjectPtr<UTexture> WeakVectorDisplacement(Component->GetVectorDisplacementTexture());
	TWeakObjectPtr<UTexture> WeakSubtract(Component->SubtractTexture);
	TWeakObjectPtr<UTexture> WeakNormalMap(Component->NormalMapTexture);

	if (bUsePatchMode)
	{
		// SPATIAL PATCH/QUADTREE MODE: Generate multiple leaves with per-leaf LOD
		ENQUEUE_RENDER_COMMAND(GeneratePatchedMesh)(
			[this, EffectiveSettings, LocalToWorld = Component->GetComponentTransform().ToMatrixWithScale(), CameraPosition,
			 WeakDisplacement, WeakVectorDisplacement, WeakSubtract, WeakNormalMap,
			 bDebugLog = this->bEnableDebugLogging]
			(FRHICommandListImmediate& RHICmdList)
			{
				if (bDebugLog)
				{
					if (EffectiveSettings.LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree)
					{
						UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Starting QUADTREE generation on render thread - Roots:%dx%d MaxDepth:%d"),
							EffectiveSettings.QuadtreeRootTileCountX, EffectiveSettings.QuadtreeRootTileCountY, EffectiveSettings.QuadtreeMaxDepth);
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Starting PATCH generation on render thread - Patches:%dx%d"),
							EffectiveSettings.PatchCountX, EffectiveSettings.PatchCountY);
					}
				}

				FGPUTessellationMeshBuilder MeshBuilder;
				FRDGBuilder GraphBuilder(RHICmdList);

				// For frustum culling, we'd need the view frustum here
				// For now, pass nullptr (per-frame culling happens in RenderPatches)
				const FConvexVolume* ViewFrustum = nullptr;

				if (EffectiveSettings.LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree)
				{
					MeshBuilder.ExecuteQuadtreeTessellationPipeline(
						GraphBuilder,
						EffectiveSettings,
						LocalToWorld,
						CameraPosition,
						ViewFrustum,
						WeakDisplacement.Get(),
						WeakSubtract.Get(),
						WeakNormalMap.Get(),
						GPUPatchBuffers,
						WeakVectorDisplacement.Get()
					);
				}
				else
				{
					MeshBuilder.ExecutePatchTessellationPipeline(
						GraphBuilder,
						EffectiveSettings,
						LocalToWorld,
						CameraPosition,
						ViewFrustum,
						EffectiveSettings.PatchCountX,
						EffectiveSettings.PatchCountY,
						WeakDisplacement.Get(),
						WeakSubtract.Get(),
						WeakNormalMap.Get(),
						GPUPatchBuffers,
						WeakVectorDisplacement.Get()
					);
				}

				GraphBuilder.Execute();
				PatchGeometryRebuildCount++;
				LatestPatchLODState = GPUPatchBuffers.PatchInfo;
				
				if (bDebugLog)
				{
					int32 TotalPatches = GPUPatchBuffers.GetTotalPatchCount();
					int32 ValidPatches = 0;
					for (const FGPUTessellationBuffers& Patch : GPUPatchBuffers.PatchBuffers)
					{
						if (Patch.IsValid()) ValidPatches++;
					}
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Patch leaves generated - Total:%d Valid:%d"),
						TotalPatches, ValidPatches);
				}
				
				// Initialize vertex factories for all patches
				InitializePatchVertexFactories(RHICmdList);
				
				bMeshValid = GPUPatchBuffers.IsValid();

				if (bDebugLog && EffectiveSettings.bUsePersistentPatchBuffers && EffectiveSettings.LODMode == EGPUTessellationLODMode::DistanceBasedPatches)
				{
					LogPersistentPatchStats_RenderThread(TEXT("InitialBuild"));
				}
				
				if (bDebugLog)
				{
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Patch/quadtree mode initialized - MeshValid:%d"), bMeshValid);
				}
			});
	}
	else
	{
		// SINGLE MESH MODE: Generate one mesh (original behavior)
		ENQUEUE_RENDER_COMMAND(GenerateTessellatedMesh)(
			[this, EffectiveSettings, LocalToWorld = Component->GetComponentTransform().ToMatrixWithScale(), CameraPosition,
			 WeakDisplacement, WeakVectorDisplacement, WeakSubtract, WeakNormalMap,
			 bDebugLog = this->bEnableDebugLogging]
			(FRHICommandListImmediate& RHICmdList)
			{
				if (bDebugLog)
				{
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Starting mesh generation on render thread with TessFactor:%d"), 
						EffectiveSettings.TessellationFactor);
				}
				
				FGPUTessellationMeshBuilder MeshBuilder;
				FRDGBuilder GraphBuilder(RHICmdList);
				
				// Execute tessellation pipeline
				MeshBuilder.ExecuteTessellationPipeline(GraphBuilder, EffectiveSettings, LocalToWorld, CameraPosition, 
					WeakDisplacement.Get(), WeakSubtract.Get(), WeakNormalMap.Get(), GPUBuffers, WeakVectorDisplacement.Get());
				
				GraphBuilder.Execute();
				
				if (bDebugLog)
				{
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: After Execute - VertexCount:%d IndexCount:%d PositionBuffer:%d NormalBuffer:%d"),
						GPUBuffers.VertexCount, GPUBuffers.IndexCount, 
						GPUBuffers.PositionBuffer.IsValid(), GPUBuffers.NormalBuffer.IsValid());
				}
				
				// Initialize vertex factory if buffers are valid
				if (GPUBuffers.IsValid())
				{
					bMeshValid = true;
					VertexFactory.SetBuffers(GPUBuffers.PositionSRV, GPUBuffers.NormalSRV, GPUBuffers.UVSRV);
					ShadowVertexFactory.SetBuffers(GPUBuffers.PositionSRV, GPUBuffers.NormalSRV, GPUBuffers.UVSRV);
					ConfigureVertexFactoryHeightNormals(VertexFactory);
					ConfigureVertexFactoryHeightNormals(ShadowVertexFactory);
					VertexFactory.InitResource(RHICmdList);
					ShadowVertexFactory.InitResource(RHICmdList);
					
					if (bDebugLog)
					{
						UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Mesh initialized - %d vertices, %d indices, Resolution: %dx%d"), 
							GPUBuffers.VertexCount, GPUBuffers.IndexCount, GPUBuffers.ResolutionX, GPUBuffers.ResolutionY);
					}
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("GPUTessellation: Failed to initialize - buffers invalid"));
				}
			});
	}
	// Set primitive properties
	bWillEverBeLit = true;
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = true;
	bAffectDistanceFieldLighting = true;
	
	if (bEnableDebugLogging)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Scene proxy created - WillEverBeLit:%d CastShadow:%d"), 
			bWillEverBeLit, bCastDynamicShadow);
	}
}

FGPUTessellationSceneProxy::~FGPUTessellationSceneProxy()
{
	GPUBuffers.Reset();
	GPUPatchBuffers.Reset();
	VertexFactory.ReleaseResource();
	ShadowVertexFactory.ReleaseResource();
	
	// Release and delete all patch vertex factories
	for (FGPUTessellationVertexFactory* VF : PatchVertexFactories)
	{
		if (VF)
		{
			VF->ReleaseResource();
			delete VF;
		}
	}
	PatchVertexFactories.Empty();

	for (FGPUTessellationGPUSceneVertexFactory* VF : PatchShadowVertexFactories)
	{
		if (VF)
		{
			VF->ReleaseResource();
			delete VF;
		}
	}
	PatchShadowVertexFactories.Empty();
}

SIZE_T FGPUTessellationSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FGPUTessellationSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_GPUTessellationSceneProxy_GetDynamicMeshElements);
	
	// Throttled debug logging
	if (bEnableDebugLogging)
	{
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastLogTime >= 2.0)
		{
			LastLogTime = CurrentTime;
			if (bUsePatchMode)
			{
				UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: GetDynamicMeshElements PATCH MODE - Valid:%d Material:%d TotalPatches:%d VisibilityMap:0x%X"), 
					bMeshValid, MaterialProxy != nullptr, GPUPatchBuffers.GetTotalPatchCount(), VisibilityMap);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: GetDynamicMeshElements SINGLE MESH - Valid:%d Material:%d Buffers:%d VertexCount:%d IndexCount:%d"), 
					bMeshValid, MaterialProxy != nullptr, GPUBuffers.IsValid(), GPUBuffers.VertexCount, GPUBuffers.IndexCount);
			}
		}
	}
	
	if (!bMeshValid || !MaterialProxy)
	{
		return;
	}

	// CRITICAL FIX: Get camera position from the current View!
	// For patch LOD, we need the ACTUAL camera position from the view being rendered
	FVector CurrentCameraPosition = FVector::ZeroVector;
	const FConvexVolume* ViewFrustum = nullptr;
	if (Views.Num() > 0 && Views[0])
	{
		// Use View's actual camera position (ViewMatrices.GetViewOrigin())
		CurrentCameraPosition = Views[0]->ViewMatrices.GetViewOrigin();
		ViewFrustum = &Views[0]->ViewFrustum;
		
		// Store for potential future use
		LastCameraPosition = CurrentCameraPosition;
		
		if (bEnableDebugLogging)
		{
			static double LastCameraPosLogTime = 0.0;
			double CurrentTime = FPlatformTime::Seconds();
			if (CurrentTime - LastCameraPosLogTime >= 2.0)
			{
				LastCameraPosLogTime = CurrentTime;
				UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Current Camera Position from View: %s"), 
					*CurrentCameraPosition.ToString());
			}
		}
	}

	// NOTE: Patch regeneration per-frame causes RDG nesting issues
	// The patches are generated once in constructor, but LOD calculation uses camera position
	// TODO: Implement proper per-frame LOD update without nested RDG builders
	
	// Set up wireframe material (if needed)
	auto WireframeMaterialInstance = new FColoredMaterialRenderProxy(
		GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
		FLinearColor(0, 0.5f, 1.f)
	);
	Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);

	// Render based on mode
	if (bUsePatchMode)
	{
		// SPATIAL PATCH RENDERING: Render each visible patch
		RenderPatches(Views, ViewFamily, VisibilityMap, Collector, WireframeMaterialInstance);
	}
	else
	{
		// SINGLE MESH RENDERING: Original behavior
		RenderSingleMesh(Views, ViewFamily, VisibilityMap, Collector, WireframeMaterialInstance);
	}
}

void FGPUTessellationSceneProxy::RenderSingleMesh(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector,
	FMaterialRenderProxy* WireframeMaterialInstance) const
{
	if (!GPUBuffers.IsValid())
	{
		return;
	}

	const bool bNonNaniteVSMEnabled = UseNonNaniteVirtualShadowMaps(GetScene().GetShaderPlatform(), GetScene().GetFeatureLevel());

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];
			const bool bIsShadowView = View && View->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
			const bool bSubmitVSMShadowMesh = bIsShadowView && bNonNaniteVSMEnabled && ShadowVertexFactory.IsInitialized();

			// Draw mesh (pure GPU rendering!)
			FMeshBatch& Mesh = Collector.AllocateMesh();
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			
			// Use GPU index buffer wrapper
			BatchElement.IndexBuffer = &GPUBuffers.IndexBuffer;
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = GPUBuffers.IndexCount / 3;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = GPUBuffers.VertexCount - 1;
			BatchElement.BaseVertexIndex = 0;

			const FVector VectorDisplacementExtent = GetGPUTessellationVectorDisplacementBoundsExtent(Settings);
			const float HalfSizeX = Settings.PlaneSizeX * 0.5f + VectorDisplacementExtent.X;
			const float HalfSizeY = Settings.PlaneSizeY * 0.5f + VectorDisplacementExtent.Y;
			const float MaxDisplacement = GetGPUTessellationScalarDisplacementBoundsExtent(Settings) + VectorDisplacementExtent.Z;
			const FBoxSphereBounds ComponentLocalBounds(FBox(
				FVector(-HalfSizeX, -HalfSizeY, -MaxDisplacement),
				FVector(HalfSizeX, HalfSizeY, MaxDisplacement)));
			const FBoxSphereBounds ComponentWorldBounds = ComponentLocalBounds.TransformBy(FTransform(GetLocalToWorld()));

			FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
			DynamicPrimitiveUniformBuffer.Set(
				Collector.GetRHICommandList(),
				GetLocalToWorld(),
				GetLocalToWorld(),
				ComponentWorldBounds,
				ComponentLocalBounds,
				false,
				false,
				false
			);
			BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
			BatchElement.PrimitiveIdMode = PrimID_ForceZero;

			Mesh.bWireframe = !bIsShadowView && AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
			Mesh.VertexFactory = &VertexFactory;
			Mesh.MaterialRenderProxy = Mesh.bWireframe ? WireframeMaterialInstance : MaterialProxy;
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
				VSMShadowMesh.VertexFactory = &ShadowVertexFactory;
				VSMShadowMesh.bWireframe = false;
				VSMShadowMesh.bCanApplyViewModeOverrides = false;
				VSMShadowMesh.bUseForMaterial = false;
				VSMShadowMesh.bUseForDepthPass = false;
				VSMShadowMesh.bUseAsOccluder = false;
				VSMShadowMesh.Elements[0].PrimitiveIdMode = PrimID_DynamicPrimitiveShaderData;
				Collector.AddMesh(ViewIndex, VSMShadowMesh);
			}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// Render bounds
			RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
#endif
		}
	}
}

void FGPUTessellationSceneProxy::RenderPatches(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector,
	FMaterialRenderProxy* WireframeMaterialInstance) const
{
	if (!GPUPatchBuffers.IsValid())
	{
		return;
	}

	int32 TotalPatches = GPUPatchBuffers.GetTotalPatchCount();
	int32 RenderedPatches = 0;
	const bool bNonNaniteVSMEnabled = UseNonNaniteVirtualShadowMaps(GetScene().GetShaderPlatform(), GetScene().GetFeatureLevel());

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];
			const bool bIsShadowView = View && View->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
			const bool bSubmitVSMShadowMesh = bIsShadowView && bNonNaniteVSMEnabled;

			// FIX (CRITICAL #5): Per-frame frustum rejection. Patch generation passes
			// nullptr for ViewFrustum (the patch path runs off the game thread without a
			// view), so PatchInfo.bVisible is always true. We do a cheap per-view test
			// here using the view we actually have, honouring the user-facing
			// bEnablePatchCulling setting.
			const bool bDoFrustumCull = (Settings.LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree ? Settings.bEnableQuadtreeCulling : Settings.bEnablePatchCulling) && View != nullptr;

			// Render each patch
			for (int32 PatchIndex = 0; PatchIndex < TotalPatches; ++PatchIndex)
			{
				const FGPUTessellationPatchInfo& PatchInfo = GPUPatchBuffers.PatchInfo[PatchIndex];
				
				// Skip culled patches (legacy regen-time culling)
				if (!PatchInfo.bVisible)
				{
					if (bEnableDebugLogging)
					{
						UE_LOG(LogTemp, Verbose, TEXT("    RenderPatch[%d]: SKIPPED - not visible"), PatchIndex);
					}
					continue;
				}

				// Per-frame frustum reject against this view (FIX CRITICAL #5)
				if (bDoFrustumCull)
				{
					const FVector PatchCenter = PatchInfo.WorldBounds.GetCenter();
					const FVector PatchExtent = PatchInfo.WorldBounds.GetExtent();
					if (!View->ViewFrustum.IntersectBox(PatchCenter, PatchExtent))
					{
						continue;
					}
				}
				
				const FGPUTessellationBuffers& PatchBuffer = GPUPatchBuffers.PatchBuffers[PatchIndex];
				
				// Skip invalid patches
				if (!PatchBuffer.IsValid())
				{
					// Only log error once per patch to avoid spam
					static TSet<int32> LoggedInvalidPatches;
					if (!LoggedInvalidPatches.Contains(PatchIndex))
					{
						LoggedInvalidPatches.Add(PatchIndex);
						UE_LOG(LogTemp, Error, TEXT("GPUTessellation: Patch[%d] has INVALID buffer! Verts:%d Indices:%d PosBuffer:%d NormalBuffer:%d UVBuffer:%d IndexBuffer:%d"),
							PatchIndex, 
							PatchBuffer.VertexCount, 
							PatchBuffer.IndexCount, 
							PatchBuffer.PositionBuffer.IsValid(),
							PatchBuffer.NormalBuffer.IsValid(),
							PatchBuffer.UVBuffer.IsValid(),
							PatchBuffer.IndexBufferRHI.IsValid());
					}
					continue;
				}
				
				// Make sure we have a vertex factory for this patch
				if (!PatchVertexFactories.IsValidIndex(PatchIndex) || !PatchVertexFactories[PatchIndex])
				{
					// Only log error once per patch to avoid spam
					static TSet<int32> LoggedMissingVF;
					if (!LoggedMissingVF.Contains(PatchIndex))
					{
						LoggedMissingVF.Add(PatchIndex);
						UE_LOG(LogTemp, Error, TEXT("GPUTessellation: Patch[%d] has NO vertex factory! ArraySize:%d TotalPatches:%d"),
							PatchIndex, PatchVertexFactories.Num(), TotalPatches);
					}
					continue;
				}
				
				// Additional safety check: verify vertex factory is initialized
				if (!PatchVertexFactories[PatchIndex]->IsInitialized())
				{
					static TSet<int32> LoggedUninitializedVF;
					if (!LoggedUninitializedVF.Contains(PatchIndex))
					{
						LoggedUninitializedVF.Add(PatchIndex);
						UE_LOG(LogTemp, Error, TEXT("GPUTessellation: Patch[%d] vertex factory NOT INITIALIZED!"), PatchIndex);
					}
					continue;
				}

				// Draw this patch
				FMeshBatch& Mesh = Collector.AllocateMesh();
				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				
				// Use patch's GPU index buffer
				BatchElement.IndexBuffer = &PatchBuffer.IndexBuffer;
				BatchElement.FirstIndex = 0;
				BatchElement.NumPrimitives = PatchBuffer.IndexCount / 3;
				BatchElement.MinVertexIndex = 0;
				BatchElement.MaxVertexIndex = PatchBuffer.VertexCount - 1;
				BatchElement.BaseVertexIndex = 0;
				BatchElement.PrimitiveIdMode = PrimID_ForceZero;
				
				// Use stable component-level bounds for material primitive data. Per-patch bounds
				// make ObjectPosition/ObjectRadius-driven materials change as quadtree leaves split,
				// which can look like UV corruption at large actor scales. Patch culling is handled
				// explicitly above against PatchInfo.WorldBounds, so the material data can stay stable.
				const FVector VectorDisplacementExtent = GetGPUTessellationVectorDisplacementBoundsExtent(Settings);
				const float HalfSizeX = Settings.PlaneSizeX * 0.5f + VectorDisplacementExtent.X;
				const float HalfSizeY = Settings.PlaneSizeY * 0.5f + VectorDisplacementExtent.Y;
				const float MaxDisplacement = GetGPUTessellationScalarDisplacementBoundsExtent(Settings) + VectorDisplacementExtent.Z;
				const FBox LocalComponentBox(
					FVector(-HalfSizeX, -HalfSizeY, -MaxDisplacement),
					FVector(HalfSizeX, HalfSizeY, MaxDisplacement));
				const FBoxSphereBounds ComponentLocalBounds(LocalComponentBox);
				const FBoxSphereBounds ComponentWorldBounds = ComponentLocalBounds.TransformBy(FTransform(GetLocalToWorld()));

				FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
				DynamicPrimitiveUniformBuffer.Set(
					Collector.GetRHICommandList(),
					GetLocalToWorld(),        // LocalToWorld
					GetLocalToWorld(),        // PreviousLocalToWorld (same for now)
					ComponentWorldBounds,    // WorldBounds - stable for material object data
					ComponentLocalBounds,    // LocalBounds - stable for material object data
					false,                   // bReceivesDecals
					false,                   // bHasPrecomputedVolumetricLightmap
					false                    // bOutputVelocity
				);
				BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
				// The VF reads transform data from the primitive uniform buffer directly.
				// Setup mesh batch
				Mesh.bWireframe = !bIsShadowView && AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
				Mesh.VertexFactory = PatchVertexFactories[PatchIndex];
				Mesh.MaterialRenderProxy = Mesh.bWireframe ? WireframeMaterialInstance : MaterialProxy;
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = !bIsShadowView;
				Mesh.CastShadow = IsShadowCast(View);

				Collector.AddMesh(ViewIndex, Mesh);

				if (bSubmitVSMShadowMesh)
				{
					if (!PatchShadowVertexFactories.IsValidIndex(PatchIndex) || !PatchShadowVertexFactories[PatchIndex] || !PatchShadowVertexFactories[PatchIndex]->IsInitialized())
					{
						static TSet<int32> LoggedMissingShadowVF;
						if (!LoggedMissingShadowVF.Contains(PatchIndex))
						{
							LoggedMissingShadowVF.Add(PatchIndex);
							UE_LOG(LogTemp, Error, TEXT("GPUTessellation: Patch[%d] shadow vertex factory is not initialized!"), PatchIndex);
						}
					}
					else
					{
						FMeshBatch& VSMShadowMesh = Collector.AllocateMesh();
						VSMShadowMesh = Mesh;
						VSMShadowMesh.VertexFactory = PatchShadowVertexFactories[PatchIndex];
						VSMShadowMesh.bWireframe = false;
						VSMShadowMesh.bCanApplyViewModeOverrides = false;
						VSMShadowMesh.bUseForMaterial = false;
						VSMShadowMesh.bUseForDepthPass = false;
						VSMShadowMesh.bUseAsOccluder = false;
						VSMShadowMesh.Elements[0].PrimitiveIdMode = PrimID_DynamicPrimitiveShaderData;
						Collector.AddMesh(ViewIndex, VSMShadowMesh);
					}
				}

				RenderedPatches++;
			}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// Render bounds
			RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
			
			// Debug: Draw patch boundaries (only if explicitly enabled via checkbox)
			if (bShowPatchDebugVisualization)
			{
				FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
				
				// Draw each patch's bounds
				for (int32 PatchIndex = 0; PatchIndex < TotalPatches; ++PatchIndex)
				{
					const FGPUTessellationPatchInfo& PatchInfo = GPUPatchBuffers.PatchInfo[PatchIndex];
					const FGPUTessellationPatchInfo& DebugPatchInfo = LatestPatchLODState.IsValidIndex(PatchIndex) ? LatestPatchLODState[PatchIndex] : PatchInfo;
					
					// Color: green for visible, red for culled, blue for different LODs
					FColor PatchColor = DebugPatchInfo.bVisible ? FColor::Green : FColor::Red;
					if (DebugPatchInfo.bVisible && DebugPatchInfo.TessellationLevel > 0)
					{
						// Vary color by LOD level for better visibility
						int32 ColorVariation = (DebugPatchInfo.TessellationLevel * 40) % 255;
						PatchColor = FColor(0, 255 - ColorVariation, ColorVariation);
					}
					
					// Draw wire box for patch bounds
					DrawWireBox(PDI, DebugPatchInfo.WorldBounds, PatchColor, SDPG_World, 3.0f);
					
					// Draw a sphere at the patch center
					DrawWireSphere(PDI, DebugPatchInfo.WorldCenter, PatchColor, 10.0f, 8, SDPG_World, 2.0f);
				}
			}
#endif
		}
	}
	
	// Debug logging for patch rendering (FIX CRITICAL #4: gated by bEnableDebugLogging)
	if (bEnableDebugLogging && RenderedPatches > 0)
	{
		static int32 FrameCounter = 0;
		static int32 LastRenderedCount = 0;
		FrameCounter++;

		if (FrameCounter % 60 == 0 || LastRenderedCount != RenderedPatches)
		{
			LastRenderedCount = RenderedPatches;
			UE_LOG(LogTemp, Verbose, TEXT("GPUTessellation: Rendered %d/%d patches (Frame %d)"),
				RenderedPatches, TotalPatches, FrameCounter);

			// Log first few patch positions
			if (GPUPatchBuffers.PatchInfo.Num() >= 4)
			{
				for (int32 i = 0; i < FMath::Min(4, GPUPatchBuffers.PatchInfo.Num()); ++i)
				{
					const FGPUTessellationPatchInfo& PatchInf = GPUPatchBuffers.PatchInfo[i];
					UE_LOG(LogTemp, Verbose, TEXT("  Patch[%d] Center: %s Visible:%d"),
						i, *PatchInf.WorldCenter.ToString(), PatchInf.bVisible);
				}
			}
		}
	}
}

void FGPUTessellationSceneProxy::InitializePatchVertexFactories(FRHICommandListImmediate& RHICmdList)
{
	PatchVertexFactoryReinitCount++;

	int32 TotalPatches = GPUPatchBuffers.GetTotalPatchCount();

	while (PatchVertexFactories.Num() < TotalPatches)
	{
		FGPUTessellationVertexFactory* VF = new FGPUTessellationVertexFactory(GetScene().GetFeatureLevel());
		VF->InitResource(RHICmdList);
		PatchVertexFactories.Add(VF);
	}

	while (PatchShadowVertexFactories.Num() < TotalPatches)
	{
		FGPUTessellationGPUSceneVertexFactory* VF = new FGPUTessellationGPUSceneVertexFactory(GetScene().GetFeatureLevel());
		VF->InitResource(RHICmdList);
		PatchShadowVertexFactories.Add(VF);
	}

	for (int32 i = 0; i < TotalPatches; ++i)
	{
		FGPUTessellationVertexFactory* VF = PatchVertexFactories[i];
		if (!VF)
		{
			VF = new FGPUTessellationVertexFactory(GetScene().GetFeatureLevel());
			VF->InitResource(RHICmdList);
			PatchVertexFactories[i] = VF;
		}

		FGPUTessellationGPUSceneVertexFactory* ShadowVF = PatchShadowVertexFactories[i];
		if (!ShadowVF)
		{
			ShadowVF = new FGPUTessellationGPUSceneVertexFactory(GetScene().GetFeatureLevel());
			ShadowVF->InitResource(RHICmdList);
			PatchShadowVertexFactories[i] = ShadowVF;
		}

		if (GPUPatchBuffers.PatchBuffers[i].IsValid())
		{
			VF->SetBuffers(
				GPUPatchBuffers.PatchBuffers[i].PositionSRV,
				GPUPatchBuffers.PatchBuffers[i].NormalSRV,
				GPUPatchBuffers.PatchBuffers[i].UVSRV
			);
			ShadowVF->SetBuffers(
				GPUPatchBuffers.PatchBuffers[i].PositionSRV,
				GPUPatchBuffers.PatchBuffers[i].NormalSRV,
				GPUPatchBuffers.PatchBuffers[i].UVSRV
			);
			ConfigureVertexFactoryHeightNormals(*VF);
			ConfigureVertexFactoryHeightNormals(*ShadowVF);
		}
		else
		{
			VF->SetBuffers(FShaderResourceViewRHIRef(), FShaderResourceViewRHIRef(), FShaderResourceViewRHIRef());
			VF->SetHeightNormalParameters(false, FTextureRHIRef(), FTextureRHIRef(), false, 1.0f, 1.0f, 1.0f, 1000.0f, 1000.0f);
			ShadowVF->SetBuffers(FShaderResourceViewRHIRef(), FShaderResourceViewRHIRef(), FShaderResourceViewRHIRef());
			ShadowVF->SetHeightNormalParameters(false, FTextureRHIRef(), FTextureRHIRef(), false, 1.0f, 1.0f, 1.0f, 1000.0f, 1000.0f);
		}
	}
}

void FGPUTessellationSceneProxy::ConfigureVertexFactoryHeightNormals(FGPUTessellationVertexFactory& InVertexFactory) const
{
	const bool bUseHeightPixelNormals = !Settings.bUseVectorDisplacement && Settings.NormalCalculationMethod == EGPUTessellationNormalMethod::FromHeightTexture && CachedDisplacementTexture.Get() != nullptr;
	const FTextureRHIRef HeightTextureRHI = bUseHeightPixelNormals ? GetGPUTessellationTextureRHI(CachedDisplacementTexture.Get()) : FTextureRHIRef();
	const FTextureRHIRef SubtractTextureRHI = bUseHeightPixelNormals ? GetGPUTessellationTextureRHI(CachedSubtractTexture.Get()) : FTextureRHIRef();

	InVertexFactory.SetHeightNormalParameters(
		bUseHeightPixelNormals,
		HeightTextureRHI,
		SubtractTextureRHI,
		CachedSubtractTexture.Get() != nullptr,
		Settings.DisplacementIntensity,
		Settings.HeightTextureNormalDetailStrength,
		Settings.HeightTextureNormalTexelStep,
		Settings.PlaneSizeX,
		Settings.PlaneSizeY);
}

bool FGPUTessellationSceneProxy::UpdatePersistentPatchState_RenderThread(const FVector& CameraPosition, const FMatrix& ComponentTransform)
{
	check(IsInRenderingThread());

	if (!Settings.bUsePersistentPatchBuffers || Settings.LODMode != EGPUTessellationLODMode::DistanceBasedPatches || !GPUPatchBuffers.IsValid())
	{
		return false;
	}

	if (GPUPatchBuffers.PatchCountX != Settings.PatchCountX || GPUPatchBuffers.PatchCountY != Settings.PatchCountY)
	{
		return false;
	}

	FGPUTessellationMeshBuilder MeshBuilder;
	TArray<FGPUTessellationPatchInfo> UpdatedPatchState;
		MeshBuilder.CalculatePatchState(
			Settings,
			ComponentTransform,
			CameraPosition,
			nullptr,
			Settings.PatchCountX,
			Settings.PatchCountY,
			UpdatedPatchState);

	if (UpdatedPatchState.Num() != GPUPatchBuffers.PatchInfo.Num())
	{
		return false;
	}

	LatestPatchLODState = UpdatedPatchState;

	// Keep geometry-compatible fields from the initial build, but refresh bounds/visibility for
	// per-view culling and debug. The rendered buffers stay persistent in this experimental mode.
	for (int32 PatchIndex = 0; PatchIndex < UpdatedPatchState.Num(); ++PatchIndex)
	{
		FGPUTessellationPatchInfo& RenderPatchInfo = GPUPatchBuffers.PatchInfo[PatchIndex];
		const FGPUTessellationPatchInfo& DesiredPatchInfo = UpdatedPatchState[PatchIndex];

		RenderPatchInfo.WorldCenter = DesiredPatchInfo.WorldCenter;
		RenderPatchInfo.WorldBounds = DesiredPatchInfo.WorldBounds;
		RenderPatchInfo.bVisible = DesiredPatchInfo.bVisible;
	}

	CachedLocalToWorld = ComponentTransform;
	LastCameraPosition = CameraPosition;
	PatchStateUpdateCount++;

	LogPersistentPatchStats_RenderThread(TEXT("StateOnlyCameraUpdate"));
	return true;
}

void FGPUTessellationSceneProxy::LogPersistentPatchStats_RenderThread(const TCHAR* Reason) const
{
	if (!Settings.bUsePersistentPatchBuffers || Settings.LODMode != EGPUTessellationLODMode::DistanceBasedPatches || !bEnableDebugLogging)
	{
		return;
	}

	const double CurrentTime = FPlatformTime::Seconds();
	if (CurrentTime - LastPersistentPatchStatsLogTime < 2.0)
	{
		return;
	}
	LastPersistentPatchStatsLogTime = CurrentTime;

	auto BuildBucketSummary = [](const TArray<FGPUTessellationPatchInfo>& PatchInfo) -> FString
	{
		TMap<int32, int32> Buckets;
		for (const FGPUTessellationPatchInfo& Patch : PatchInfo)
		{
			Buckets.FindOrAdd(Patch.TessellationLevel)++;
		}

		FString Summary;
		for (const TPair<int32, int32>& Bucket : Buckets)
		{
			if (!Summary.IsEmpty())
			{
				Summary += TEXT(" ");
			}
			Summary += FString::Printf(TEXT("%d:%d"), Bucket.Key, Bucket.Value);
		}
		return Summary.IsEmpty() ? FString(TEXT("none")) : Summary;
	};

	int32 ValidPatchBuffers = 0;
	for (const FGPUTessellationBuffers& PatchBuffer : GPUPatchBuffers.PatchBuffers)
	{
		if (PatchBuffer.IsValid())
		{
			ValidPatchBuffers++;
		}
	}

	const FString GeneratedBuckets = BuildBucketSummary(GPUPatchBuffers.PatchInfo);
	const FString DesiredBuckets = BuildBucketSummary(LatestPatchLODState.Num() > 0 ? LatestPatchLODState : GPUPatchBuffers.PatchInfo);
	UE_LOG(LogTemp, Warning, TEXT("GPUTessellation PersistentPatch[%s]: GeometryRebuilds=%llu StateUpdates=%llu VertexFactoryReinits=%llu ValidBuffers=%d/%d GeneratedLOD={%s} DesiredLOD={%s}"),
		Reason ? Reason : TEXT("Unknown"),
		static_cast<unsigned long long>(PatchGeometryRebuildCount),
		static_cast<unsigned long long>(PatchStateUpdateCount),
		static_cast<unsigned long long>(PatchVertexFactoryReinitCount),
		ValidPatchBuffers,
		GPUPatchBuffers.PatchBuffers.Num(),
		*GeneratedBuckets,
		*DesiredBuckets);
}

void FGPUTessellationSceneProxy::UpdateDynamicData_RenderThread(FGPUTessellationDynamicData* DynamicData)
{
	check(IsInRenderingThread());
	
	if (!DynamicData || !bUsePatchMode)
	{
		delete DynamicData;
		return;
	}
	
	// Legacy path regenerates patches with the updated camera position. The experimental
	// persistent path below updates patch state only and keeps geometry buffers intact.
	// This is called from SendRenderDynamicData_Concurrent, NOT during GetDynamicMeshElements
	// So we can safely create an RDGBuilder here!
	
	FVector CameraPosition = DynamicData->CameraPosition;
	FMatrix ComponentTransform = DynamicData->LocalToWorld;
	delete DynamicData;
	
	const bool bAllowPersistentStateOnly = Settings.bUsePersistentPatchBuffers && Settings.LODMode == EGPUTessellationLODMode::DistanceBasedPatches;

	if (bEnableDebugLogging)
	{
		static double LastUpdateLogTime = 0.0;
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastUpdateLogTime >= 2.0)
		{
			LastUpdateLogTime = CurrentTime;
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: UpdateDynamicData - %s with camera at: %s"),
				bAllowPersistentStateOnly ? TEXT("updating persistent patch state") : (Settings.LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree ? TEXT("regenerating quadtree leaves") : TEXT("regenerating patches")),
				*CameraPosition.ToString());
		}
	}

	if (bAllowPersistentStateOnly && UpdatePersistentPatchState_RenderThread(CameraPosition, ComponentTransform))
	{
		return;
	}

	if (bAllowPersistentStateOnly && bEnableDebugLogging)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Persistent patch state update was not possible; falling back to full patch geometry rebuild."));
	}
	
	// Regenerate patch geometry on the render thread when persistent state-only update is disabled
	// or unavailable.
	// FIX (CRITICAL #3): We are already on the render thread here. The previous nested
	// ENQUEUE_RENDER_COMMAND added a frame of latency and captured `this` by raw pointer,
	// which could outlive the proxy if it was being destroyed concurrently. Execute the
	// RDG work directly using the immediate command list we can grab right now.
	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
	{
		FGPUTessellationMeshBuilder MeshBuilder;
		FRDGBuilder GraphBuilder(RHICmdList);

		if (Settings.LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree)
		{
			MeshBuilder.ExecuteQuadtreeTessellationPipeline(
				GraphBuilder,
				Settings,
				ComponentTransform,
				CameraPosition,
				nullptr,
				CachedDisplacementTexture.Get(),
				CachedSubtractTexture.Get(),
				CachedNormalMapTexture.Get(),
				GPUPatchBuffers,
				CachedVectorDisplacementTexture.Get()
			);
		}
		else
		{
			MeshBuilder.ExecutePatchTessellationPipeline(
				GraphBuilder,
				Settings,
				ComponentTransform,
				CameraPosition,
				nullptr,
				Settings.PatchCountX,
				Settings.PatchCountY,
				CachedDisplacementTexture.Get(),
				CachedSubtractTexture.Get(),
				CachedNormalMapTexture.Get(),
				GPUPatchBuffers,
				CachedVectorDisplacementTexture.Get()
			);
		}

		GraphBuilder.Execute();
		PatchGeometryRebuildCount++;
		LatestPatchLODState = GPUPatchBuffers.PatchInfo;

		// Reinitialize vertex factories
		InitializePatchVertexFactories(RHICmdList);
		bMeshValid = GPUPatchBuffers.IsValid();
		LogPersistentPatchStats_RenderThread(TEXT("FullRebuildFallback"));
	}
}

FPrimitiveViewRelevance FGPUTessellationSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && bMeshValid;
	Result.bShadowRelevance = IsShadowCast(View) && bMeshValid;
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;

	// FIX (CRITICAL #19): Without bRenderInDepthPass the proxy is invisible to the
	// depth-only / pre-pass and therefore never reaches the virtual shadow map
	// rasterizer. Velocity relevance is needed for TSR/TAA temporal stability and
	// for VSM dynamic-page invalidation when the mesh moves.
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;

	MaterialRelevance.SetPrimitiveViewRelevance(Result);

	if (bEnableDebugLogging)
	{
		static bool bLoggedRelevance = false;
		if (!bLoggedRelevance && bMeshValid)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: GetViewRelevance - Draw:%d Shadow:%d Dynamic:%d RenderInMain:%d Depth:%d Velocity:%d"),
				Result.bDrawRelevance, Result.bShadowRelevance, Result.bDynamicRelevance, Result.bRenderInMainPass,
				Result.bRenderInDepthPass, Result.bVelocityRelevance);
			bLoggedRelevance = true;
		}
	}

	return Result;
}

void FGPUTessellationSceneProxy::UpdateMeshBuffers_RenderThread(const FGPUTessellationBuffers& Buffers)
{
	check(IsInRenderingThread());

	GPUBuffers = Buffers;
	bMeshValid = GPUBuffers.IsValid();

	if (bMeshValid)
	{
		// Update vertex factory with new buffers
		VertexFactory.SetBuffers(GPUBuffers.PositionSRV, GPUBuffers.NormalSRV, GPUBuffers.UVSRV);
		ShadowVertexFactory.SetBuffers(GPUBuffers.PositionSRV, GPUBuffers.NormalSRV, GPUBuffers.UVSRV);
		ConfigureVertexFactoryHeightNormals(VertexFactory);
		ConfigureVertexFactoryHeightNormals(ShadowVertexFactory);

		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		if (!VertexFactory.IsInitialized())
		{
			VertexFactory.InitResource(RHICmdList);
		}
		if (!ShadowVertexFactory.IsInitialized())
		{
			ShadowVertexFactory.InitResource(RHICmdList);
		}
	}
	
	// Pure GPU - no CPU buffer uploads!
}
