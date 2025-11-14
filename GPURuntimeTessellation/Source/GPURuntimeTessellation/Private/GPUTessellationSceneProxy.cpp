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
#include "MeshBatch.h"
#include "RayTracingDefinitions.h"
#include "RayTracingInstance.h"
#include "DrawDebugHelpers.h"
#include "PrimitiveUniformShaderParameters.h"

FGPUTessellationSceneProxy::FGPUTessellationSceneProxy(UGPUTessellationComponent* Component)
	: FPrimitiveSceneProxy(Component)
	, MaterialProxy(nullptr)
	, Settings(Component->TessellationSettings)
	, CachedLocalToWorld(Component->GetComponentTransform().ToMatrixWithScale())
	, CachedDisplacementTexture(Component->DisplacementTexture)
	, CachedSubtractTexture(Component->SubtractTexture)
	, CachedNormalMapTexture(Component->NormalMapTexture)
	, VertexFactory(GetScene().GetFeatureLevel())
	, bMeshValid(false)
	, bUsePatchMode(Settings.LODMode == EGPUTessellationLODMode::DistanceBasedPatches)
	, bEnableDebugLogging(Component->bEnableDebugLogging)
	, bShowPatchDebugVisualization(Component->bShowPatchDebugVisualization)
	, LastLogTime(0.0)
	, LastCameraPosition(FVector::ZeroVector)
{
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
		MaterialRelevance = Component->Material->GetRelevance(GetScene().GetFeatureLevel());
	}
	else if (UMaterial::GetDefaultMaterial(MD_Surface))
	{
		MaterialProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
		MaterialRelevance = UMaterial::GetDefaultMaterial(MD_Surface)->GetRelevance(GetScene().GetFeatureLevel());
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
	if (CameraPosition.IsZero() || !CameraPosition.ContainsNaN())
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
	if (bUsePatchMode)
	{
		// SPATIAL PATCH MODE: Generate multiple patches with per-patch LOD
		ENQUEUE_RENDER_COMMAND(GeneratePatchedMesh)(
			[this, EffectiveSettings, LocalToWorld = Component->GetComponentTransform().ToMatrixWithScale(), CameraPosition,
			 DisplacementTexture = Component->DisplacementTexture, SubtractTexture = Component->SubtractTexture,
			 NormalMapTexture = Component->NormalMapTexture,
			 bDebugLog = this->bEnableDebugLogging]
			(FRHICommandListImmediate& RHICmdList)
			{
				if (bDebugLog)
				{
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Starting PATCH generation on render thread - Patches:%dx%d"),
						EffectiveSettings.PatchCountX, EffectiveSettings.PatchCountY);
				}
				
				FGPUTessellationMeshBuilder MeshBuilder;
				FRDGBuilder GraphBuilder(RHICmdList);
				
				// For frustum culling, we'd need the view frustum here
				// For now, pass nullptr (culling will be disabled in first frame)
				const FConvexVolume* ViewFrustum = nullptr;
				
				// Execute patch tessellation pipeline
				MeshBuilder.ExecutePatchTessellationPipeline(
					GraphBuilder,
					EffectiveSettings,
					LocalToWorld,
					CameraPosition,
					ViewFrustum,
					EffectiveSettings.PatchCountX,
					EffectiveSettings.PatchCountY,
					DisplacementTexture,
					SubtractTexture,
					NormalMapTexture,
					GPUPatchBuffers
				);
				
				GraphBuilder.Execute();
				
				if (bDebugLog)
				{
					int32 TotalPatches = GPUPatchBuffers.GetTotalPatchCount();
					int32 ValidPatches = 0;
					for (const FGPUTessellationBuffers& Patch : GPUPatchBuffers.PatchBuffers)
					{
						if (Patch.IsValid()) ValidPatches++;
					}
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Patches generated - Total:%d Valid:%d"),
						TotalPatches, ValidPatches);
				}
				
				// Initialize vertex factories for all patches
				InitializePatchVertexFactories(RHICmdList);
				
				bMeshValid = GPUPatchBuffers.IsValid();
				
				if (bDebugLog)
				{
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Patch mode initialized - MeshValid:%d"), bMeshValid);
				}
			});
	}
	else
	{
		// SINGLE MESH MODE: Generate one mesh (original behavior)
		ENQUEUE_RENDER_COMMAND(GenerateTessellatedMesh)(
			[this, EffectiveSettings, LocalToWorld = Component->GetComponentTransform().ToMatrixWithScale(), CameraPosition, 
			 DisplacementTexture = Component->DisplacementTexture, SubtractTexture = Component->SubtractTexture,
			 NormalMapTexture = Component->NormalMapTexture,
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
					DisplacementTexture, SubtractTexture, NormalMapTexture, GPUBuffers);
				
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
					VertexFactory.InitResource(RHICmdList);
					
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

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];

			// Draw mesh (pure GPU rendering!)
			FMeshBatch& Mesh = Collector.AllocateMesh();
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			
			// Use GPU index buffer wrapper
			BatchElement.IndexBuffer = &GPUBuffers.IndexBuffer;
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = GPUBuffers.IndexCount / 3;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = GPUBuffers.VertexCount - 1;
			
			// Set up primitive uniform buffer for GPU Scene
			BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
			BatchElement.PrimitiveIdMode = PrimID_ForceZero;

			Mesh.bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
			Mesh.VertexFactory = &VertexFactory;
			Mesh.MaterialRenderProxy = Mesh.bWireframe ? WireframeMaterialInstance : MaterialProxy;
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = true;
			Mesh.CastShadow = IsShadowCast(View);

			Collector.AddMesh(ViewIndex, Mesh);

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

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];

			// Render each patch
			for (int32 PatchIndex = 0; PatchIndex < TotalPatches; ++PatchIndex)
			{
				const FGPUTessellationPatchInfo& PatchInfo = GPUPatchBuffers.PatchInfo[PatchIndex];
				
				// Skip culled patches
				if (!PatchInfo.bVisible)
				{
					if (bEnableDebugLogging)
					{
						UE_LOG(LogTemp, Warning, TEXT("    RenderPatch[%d]: SKIPPED - not visible"), PatchIndex);
					}
					continue;
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
				
			// CRITICAL FIX: Create per-patch uniform buffer with correct bounds!
			// Each patch has vertices in absolute world-local space, but we need to tell
			// the renderer about this specific patch's bounds for proper culling.
			
			// PatchInfo.WorldBounds is already in world space (calculated in CalculatePatchInfo)
			FBoxSphereBounds PatchWorldBounds(PatchInfo.WorldBounds);
			
			// Transform world bounds back to local space for the uniform buffer
			// We need to use the inverse transform properly
			FTransform InverseTransform = FTransform(GetLocalToWorld()).Inverse();
			FBox LocalBoundsBox = PatchInfo.WorldBounds.TransformBy(InverseTransform);
			FBoxSphereBounds PatchLocalBounds(LocalBoundsBox);
			
			// Create a dynamic primitive uniform buffer for this patch
			// that includes the correct bounds for culling
			FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
			DynamicPrimitiveUniformBuffer.Set(
				Collector.GetRHICommandList(),
				GetLocalToWorld(),        // LocalToWorld
				GetLocalToWorld(),        // PreviousLocalToWorld (same for now)
				PatchWorldBounds,        // WorldBounds - use patch-specific world bounds!
				PatchLocalBounds,        // LocalBounds - transformed to local space
				false,                   // bReceivesDecals
				false,                   // bHasPrecomputedVolumetricLightmap
				false                    // bOutputVelocity
			);
			BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
			BatchElement.PrimitiveIdMode = PrimID_ForceZero;				// Setup mesh batch
				Mesh.bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
				Mesh.VertexFactory = PatchVertexFactories[PatchIndex];
				Mesh.MaterialRenderProxy = Mesh.bWireframe ? WireframeMaterialInstance : MaterialProxy;
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = true;
				Mesh.CastShadow = IsShadowCast(View);

				Collector.AddMesh(ViewIndex, Mesh);
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
					
					// Color: green for visible, red for culled, blue for different LODs
					FColor PatchColor = PatchInfo.bVisible ? FColor::Green : FColor::Red;
					if (PatchInfo.bVisible && PatchInfo.TessellationLevel > 0)
					{
						// Vary color by LOD level for better visibility
						int32 ColorVariation = (PatchInfo.TessellationLevel * 40) % 255;
						PatchColor = FColor(0, 255 - ColorVariation, ColorVariation);
					}
					
					// Draw wire box for patch bounds
					DrawWireBox(PDI, PatchInfo.WorldBounds, PatchColor, SDPG_World, 3.0f);
					
					// Draw a sphere at the patch center
					DrawWireSphere(PDI, PatchInfo.WorldCenter, PatchColor, 10.0f, 8, SDPG_World, 2.0f);
				}
			}
#endif
		}
	}
	
	// Debug logging for patch rendering
	if (RenderedPatches > 0)
	{
		static int32 FrameCounter = 0;
		static int32 LastRenderedCount = 0;
		FrameCounter++;
		
		if (FrameCounter % 60 == 0 || LastRenderedCount != RenderedPatches)
		{
			LastRenderedCount = RenderedPatches;
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Rendered %d/%d patches (Frame %d)"), 
				RenderedPatches, TotalPatches, FrameCounter);
			
			// Log first few patch positions
			if (GPUPatchBuffers.PatchInfo.Num() >= 4)
			{
				for (int32 i = 0; i < FMath::Min(4, GPUPatchBuffers.PatchInfo.Num()); ++i)
				{
					const FGPUTessellationPatchInfo& PatchInf = GPUPatchBuffers.PatchInfo[i];
					UE_LOG(LogTemp, Warning, TEXT("  Patch[%d] Center: %s Visible:%d"), 
						i, *PatchInf.WorldCenter.ToString(), PatchInf.bVisible);
				}
			}
		}
	}
}

void FGPUTessellationSceneProxy::InitializePatchVertexFactories(FRHICommandListImmediate& RHICmdList)
{
	int32 TotalPatches = GPUPatchBuffers.GetTotalPatchCount();
	
	// Release and delete old factories
	for (FGPUTessellationVertexFactory* VF : PatchVertexFactories)
	{
		if (VF)
		{
			if (VF->IsInitialized())
			{
				VF->ReleaseResource();
			}
			delete VF;
		}
	}
	
	// Allocate new factories
	PatchVertexFactories.Empty(TotalPatches);
	
	// Create and initialize each factory
	for (int32 i = 0; i < TotalPatches; ++i)
	{
		if (GPUPatchBuffers.PatchBuffers[i].IsValid())
		{
			FGPUTessellationVertexFactory* VF = new FGPUTessellationVertexFactory(GetScene().GetFeatureLevel());
			VF->SetBuffers(
				GPUPatchBuffers.PatchBuffers[i].PositionSRV,
				GPUPatchBuffers.PatchBuffers[i].NormalSRV,
				GPUPatchBuffers.PatchBuffers[i].UVSRV
			);
			VF->InitResource(RHICmdList);
			PatchVertexFactories.Add(VF);
		}
		else
		{
			PatchVertexFactories.Add(nullptr);
		}
	}
}

void FGPUTessellationSceneProxy::UpdateDynamicData_RenderThread(FGPUTessellationDynamicData* DynamicData)
{
	check(IsInRenderingThread());
	
	if (!DynamicData || !bUsePatchMode)
	{
		delete DynamicData;
		return;
	}
	
	// PROPER SOLUTION: Regenerate patches with updated camera position
	// This is called from SendRenderDynamicData_Concurrent, NOT during GetDynamicMeshElements
	// So we can safely create an RDGBuilder here!
	
	FVector CameraPosition = DynamicData->CameraPosition;
	FMatrix ComponentTransform = DynamicData->LocalToWorld;
	delete DynamicData;
	
	if (bEnableDebugLogging)
	{
		static double LastUpdateLogTime = 0.0;
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastUpdateLogTime >= 2.0)
		{
			LastUpdateLogTime = CurrentTime;
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: UpdateDynamicData - Regenerating patches with camera at: %s"), 
				*CameraPosition.ToString());
		}
	}
	
	// Enqueue render command to regenerate patches
	// This is SAFE because we're not inside GetDynamicMeshElements!
	FGPUTessellationSceneProxy* SceneProxy = this;
	ENQUEUE_RENDER_COMMAND(UpdatePatchesWithCamera)(
		[SceneProxy, CameraPosition, ComponentTransform](FRHICommandListImmediate& RHICmdList)
		{
			FGPUTessellationMeshBuilder MeshBuilder;
			FRDGBuilder GraphBuilder(RHICmdList);
			
			// Execute patch tessellation pipeline with updated camera position
			MeshBuilder.ExecutePatchTessellationPipeline(
				GraphBuilder,
				SceneProxy->Settings,
				ComponentTransform,
				CameraPosition,  // Updated camera position from component!
				nullptr,  // ViewFrustum - could pass from component if needed
				SceneProxy->Settings.PatchCountX,
				SceneProxy->Settings.PatchCountY,
				SceneProxy->CachedDisplacementTexture.Get(),
				SceneProxy->CachedSubtractTexture.Get(),
				SceneProxy->CachedNormalMapTexture.Get(),
				SceneProxy->GPUPatchBuffers
			);
			
			GraphBuilder.Execute();
			
			// Reinitialize vertex factories
			SceneProxy->InitializePatchVertexFactories(RHICmdList);
			SceneProxy->bMeshValid = SceneProxy->GPUPatchBuffers.IsValid();
		});
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

	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	
	if (bEnableDebugLogging)
	{
		static bool bLoggedRelevance = false;
		if (!bLoggedRelevance && bMeshValid)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: GetViewRelevance - Draw:%d Shadow:%d Dynamic:%d RenderInMain:%d"), 
				Result.bDrawRelevance, Result.bShadowRelevance, Result.bDynamicRelevance, Result.bRenderInMainPass);
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
	}
	
	// Pure GPU - no CPU buffer uploads!
}
