// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUTessellationComponent.h"
#include "GPUTessellationSceneProxy.h"
#include "GPUTessellationMeshBuilder.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget.h"
#include "BodySetupEnums.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "PrimitiveSceneProxy.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderingThread.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"

#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#include "GPUTessellationStaticMeshBaker.h"
#endif

namespace
{
	// Force a UTexture to keep all of its mips resident in GPU memory so the compute
	// shaders that sample it deterministically see the full-resolution data on the very
	// first dispatch after the level loads. Without this, the proxy is built before the
	// streamer has had a chance to upload the high mips and the displacement bake sees
	// a near-flat 2x2 placeholder, which is the "plane stays flat / blurred until I open
	// the texture asset" symptom. Idempotent and safe to call repeatedly.
	void GPUTessellationForceTextureResident(UTexture* Texture)
	{
		if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
		{
			Texture2D->bForceMiplevelsToBeResident = true;
			Texture2D->SetForceMipLevelsToBeResident(30.0f /* seconds */, 0);
#if WITH_EDITOR
			// In the editor we want the result to be visible immediately on level open;
			// in cooked builds WaitForStreaming would block the game thread, so skip it.
			Texture2D->WaitForStreaming();
#endif
		}
	}

	// Returns the current RHI pointer of the texture's resource as an opaque void* so
	// the component can detect changes without touching RHI types. Returns nullptr if
	// the resource isn't created yet (texture is still streaming in / async-loading).
	void* GPUTessellationGetTextureRHIIdentity(UTexture* Texture)
	{
		if (!Texture)
		{
			return nullptr;
		}
		FTextureResource* TextureResource = Texture->GetResource();
		if (!TextureResource)
		{
			return nullptr;
		}
		return TextureResource->TextureRHI.GetReference();
	}

	int32 GPUTessellationPatchLevelToTessellation(EGPUTessellationPatchLevel Level)
	{
		switch (Level)
		{
			case EGPUTessellationPatchLevel::Patch_4: return 4;
			case EGPUTessellationPatchLevel::Patch_8: return 8;
			case EGPUTessellationPatchLevel::Patch_16: return 16;
			case EGPUTessellationPatchLevel::Patch_32: return 32;
			case EGPUTessellationPatchLevel::Patch_64: return 64;
			case EGPUTessellationPatchLevel::Patch_128: return 128;
			default: return 16;
		}
	}

	int32 GPUTessellationHighestPatchLevelTessellation(const TArray<EGPUTessellationPatchLevel>& Levels, int32 DefaultTessellation)
	{
		int32 HighestTessellation = DefaultTessellation;
		for (EGPUTessellationPatchLevel Level : Levels)
		{
			HighestTessellation = FMath::Max(HighestTessellation, GPUTessellationPatchLevelToTessellation(Level));
		}
		return HighestTessellation;
	}

	uint32 GPUTessellationGradHash(uint32 GridX, uint32 GridY, uint32 GridZ, uint32 Salt)
	{
		uint32 Hash = GridX * 374761393u + GridY * 668265263u + GridZ * 1274126177u + Salt * 2147483647u;
		Hash = (Hash ^ (Hash >> 13u)) * 1274126177u;
		Hash ^= Hash >> 16u;
		return Hash;
	}

	FVector GPUTessellationGrad3D(int32 GridX, int32 GridY, int32 GridZ)
	{
		const uint32 Hash = GPUTessellationGradHash((uint32)GridX, (uint32)GridY, (uint32)GridZ, 0u) & 15u;
		switch (Hash)
		{
			case 0u:  return FVector( 1.0,  1.0,  0.0);
			case 1u:  return FVector(-1.0,  1.0,  0.0);
			case 2u:  return FVector( 1.0, -1.0,  0.0);
			case 3u:  return FVector(-1.0, -1.0,  0.0);
			case 4u:  return FVector( 1.0,  0.0,  1.0);
			case 5u:  return FVector(-1.0,  0.0,  1.0);
			case 6u:  return FVector( 1.0,  0.0, -1.0);
			case 7u:  return FVector(-1.0,  0.0, -1.0);
			case 8u:  return FVector( 0.0,  1.0,  1.0);
			case 9u:  return FVector( 0.0, -1.0,  1.0);
			case 10u: return FVector( 0.0,  1.0, -1.0);
			case 11u: return FVector( 0.0, -1.0, -1.0);
			case 12u: return FVector( 1.0,  1.0,  0.0);
			case 13u: return FVector(-1.0,  1.0,  0.0);
			case 14u: return FVector( 0.0, -1.0,  1.0);
			default:  return FVector( 0.0, -1.0, -1.0);
		}
	}

	double GPUTessellationFrac(double Value)
	{
		return Value - FMath::FloorToDouble(Value);
	}

	double GPUTessellationPerlinNoise3D(const FVector& Position)
	{
		const double FloorX = FMath::FloorToDouble(Position.X);
		const double FloorY = FMath::FloorToDouble(Position.Y);
		const double FloorZ = FMath::FloorToDouble(Position.Z);

		const int32 GridX = (int32)FloorX;
		const int32 GridY = (int32)FloorY;
		const int32 GridZ = (int32)FloorZ;

		const FVector Fraction(
			GPUTessellationFrac(Position.X),
			GPUTessellationFrac(Position.Y),
			GPUTessellationFrac(Position.Z));
		const FVector Weight = Fraction * Fraction * Fraction * (Fraction * (Fraction * 6.0 - FVector(15.0)) + FVector(10.0));

		const double Noise000 = FVector::DotProduct(GPUTessellationGrad3D(GridX,     GridY,     GridZ    ), Fraction - FVector(0.0, 0.0, 0.0));
		const double Noise100 = FVector::DotProduct(GPUTessellationGrad3D(GridX + 1, GridY,     GridZ    ), Fraction - FVector(1.0, 0.0, 0.0));
		const double Noise010 = FVector::DotProduct(GPUTessellationGrad3D(GridX,     GridY + 1, GridZ    ), Fraction - FVector(0.0, 1.0, 0.0));
		const double Noise110 = FVector::DotProduct(GPUTessellationGrad3D(GridX + 1, GridY + 1, GridZ    ), Fraction - FVector(1.0, 1.0, 0.0));
		const double Noise001 = FVector::DotProduct(GPUTessellationGrad3D(GridX,     GridY,     GridZ + 1), Fraction - FVector(0.0, 0.0, 1.0));
		const double Noise101 = FVector::DotProduct(GPUTessellationGrad3D(GridX + 1, GridY,     GridZ + 1), Fraction - FVector(1.0, 0.0, 1.0));
		const double Noise011 = FVector::DotProduct(GPUTessellationGrad3D(GridX,     GridY + 1, GridZ + 1), Fraction - FVector(0.0, 1.0, 1.0));
		const double Noise111 = FVector::DotProduct(GPUTessellationGrad3D(GridX + 1, GridY + 1, GridZ + 1), Fraction - FVector(1.0, 1.0, 1.0));

		const double NoiseX00 = FMath::Lerp(Noise000, Noise100, Weight.X);
		const double NoiseX10 = FMath::Lerp(Noise010, Noise110, Weight.X);
		const double NoiseX01 = FMath::Lerp(Noise001, Noise101, Weight.X);
		const double NoiseX11 = FMath::Lerp(Noise011, Noise111, Weight.X);
		const double NoiseXY0 = FMath::Lerp(NoiseX00, NoiseX10, Weight.Y);
		const double NoiseXY1 = FMath::Lerp(NoiseX01, NoiseX11, Weight.Y);
		return FMath::Lerp(NoiseXY0, NoiseXY1, Weight.Z);
	}

	double GetGPUTessellationRenderTargetLastRenderTime(const UTexture* Texture)
	{
		if (!Texture || !Texture->IsA<UTextureRenderTarget>())
		{
			return -TNumericLimits<double>::Max();
		}

		double LastRenderTime = Texture->TextureReference.GetLastRenderTime();
		if (const FTextureResource* TextureResource = Texture->GetResource())
		{
			LastRenderTime = FMath::Max(LastRenderTime, TextureResource->LastRenderTime);
		}
		return LastRenderTime;
	}

	bool GPUTessellationComputeBarycentric2D(const FVector2D& Point, const FVector2D& A, const FVector2D& B, const FVector2D& C, FVector& OutBarycentric)
	{
		const double Denominator = ((B.Y - C.Y) * (A.X - C.X)) + ((C.X - B.X) * (A.Y - C.Y));
		if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
		{
			OutBarycentric = FVector::ZeroVector;
			return false;
		}

		const double WeightA = (((B.Y - C.Y) * (Point.X - C.X)) + ((C.X - B.X) * (Point.Y - C.Y))) / Denominator;
		const double WeightB = (((C.Y - A.Y) * (Point.X - C.X)) + ((A.X - C.X) * (Point.Y - C.Y))) / Denominator;
		const double WeightC = 1.0 - WeightA - WeightB;
		const double Tolerance = 1.0e-4;
		if (WeightA < -Tolerance || WeightB < -Tolerance || WeightC < -Tolerance)
		{
			OutBarycentric = FVector::ZeroVector;
			return false;
		}

		OutBarycentric = FVector(WeightA, WeightB, WeightC);
		return true;
	}

	float GPUTessellationGetCollisionRingQuadSize(const TArray<float>& QuadSizes, int32 BandIndex)
	{
		if (QuadSizes.Num() == 0)
		{
			return 1000.0f;
		}

		const int32 ClampedIndex = FMath::Clamp(BandIndex, 0, QuadSizes.Num() - 1);
		return FMath::Max(QuadSizes[ClampedIndex], 1.0f);
	}

	void GPUTessellationAddCollisionRingCoordinate(TArray<float>& Coordinates, float Value, float MinValue, float MaxValue)
	{
		Coordinates.Add(FMath::Clamp(Value, MinValue, MaxValue));
	}

	void GPUTessellationAddCollisionRingRange(TArray<float>& Coordinates, float StartValue, float EndValue, float Step, float MinValue, float MaxValue)
	{
		if (FMath::IsNearlyEqual(StartValue, EndValue, KINDA_SMALL_NUMBER))
		{
			GPUTessellationAddCollisionRingCoordinate(Coordinates, EndValue, MinValue, MaxValue);
			return;
		}

		const float Direction = EndValue > StartValue ? 1.0f : -1.0f;
		const float SafeStep = FMath::Max(FMath::Abs(Step), KINDA_SMALL_NUMBER) * Direction;
		float CurrentValue = StartValue;
		while ((Direction > 0.0f && CurrentValue + SafeStep < EndValue) ||
			(Direction < 0.0f && CurrentValue + SafeStep > EndValue))
		{
			CurrentValue += SafeStep;
			GPUTessellationAddCollisionRingCoordinate(Coordinates, CurrentValue, MinValue, MaxValue);
		}

		GPUTessellationAddCollisionRingCoordinate(Coordinates, EndValue, MinValue, MaxValue);
	}

	void GPUTessellationBuildCollisionRingAxisCoordinates(
		float MinValue,
		float MaxValue,
		float CenterValue,
		float AxisWorldScale,
		const TArray<float>& RingDistances,
		const TArray<float>& RingQuadSizes,
		int32 MaxSamplesPerAxis,
		TArray<float>& OutCoordinates)
	{
		OutCoordinates.Reset();
		if (MaxValue <= MinValue)
		{
			return;
		}

		TArray<float> SortedDistances;
		SortedDistances.Reserve(RingDistances.Num());
		for (float Distance : RingDistances)
		{
			if (Distance > KINDA_SMALL_NUMBER)
			{
				SortedDistances.Add(Distance);
			}
		}
		SortedDistances.Sort();

		const float SafeAxisScale = FMath::Max(FMath::Abs(AxisWorldScale), UE_SMALL_NUMBER);
		const float ClampedCenter = FMath::Clamp(CenterValue, MinValue, MaxValue);
		const int32 SafeMaxSamples = FMath::Clamp(MaxSamplesPerAxis, 8, 2049);

		GPUTessellationAddCollisionRingCoordinate(OutCoordinates, MinValue, MinValue, MaxValue);
		GPUTessellationAddCollisionRingCoordinate(OutCoordinates, ClampedCenter, MinValue, MaxValue);
		GPUTessellationAddCollisionRingCoordinate(OutCoordinates, MaxValue, MinValue, MaxValue);

		float PositiveStart = ClampedCenter;
		float NegativeStart = ClampedCenter;
		for (int32 BandIndex = 0; BandIndex <= SortedDistances.Num(); ++BandIndex)
		{
			const float StepLocal = FMath::Max(GPUTessellationGetCollisionRingQuadSize(RingQuadSizes, BandIndex) / SafeAxisScale, KINDA_SMALL_NUMBER);
			const float PositiveEnd = BandIndex < SortedDistances.Num()
				? FMath::Min(MaxValue, ClampedCenter + SortedDistances[BandIndex] / SafeAxisScale)
				: MaxValue;
			const float NegativeEnd = BandIndex < SortedDistances.Num()
				? FMath::Max(MinValue, ClampedCenter - SortedDistances[BandIndex] / SafeAxisScale)
				: MinValue;

			if (PositiveEnd > PositiveStart + KINDA_SMALL_NUMBER)
			{
				GPUTessellationAddCollisionRingRange(OutCoordinates, PositiveStart, PositiveEnd, StepLocal, MinValue, MaxValue);
				PositiveStart = PositiveEnd;
			}

			if (NegativeEnd < NegativeStart - KINDA_SMALL_NUMBER)
			{
				GPUTessellationAddCollisionRingRange(OutCoordinates, NegativeStart, NegativeEnd, StepLocal, MinValue, MaxValue);
				NegativeStart = NegativeEnd;
			}
		}

		OutCoordinates.Sort();
		TArray<float> UniqueCoordinates;
		UniqueCoordinates.Reserve(OutCoordinates.Num());
		for (float Coordinate : OutCoordinates)
		{
			if (UniqueCoordinates.Num() == 0 || !FMath::IsNearlyEqual(UniqueCoordinates.Last(), Coordinate, KINDA_SMALL_NUMBER))
			{
				UniqueCoordinates.Add(Coordinate);
			}
		}

		if (UniqueCoordinates.Num() > SafeMaxSamples)
		{
			TArray<float> DecimatedCoordinates;
			DecimatedCoordinates.Reserve(SafeMaxSamples + 1);
			for (int32 SampleIndex = 0; SampleIndex < SafeMaxSamples; ++SampleIndex)
			{
				const float SourceAlpha = (float)SampleIndex / (float)FMath::Max(SafeMaxSamples - 1, 1);
				const int32 SourceIndex = FMath::Clamp(FMath::RoundToInt(SourceAlpha * (float)(UniqueCoordinates.Num() - 1)), 0, UniqueCoordinates.Num() - 1);
				DecimatedCoordinates.Add(UniqueCoordinates[SourceIndex]);
			}
			DecimatedCoordinates.Add(ClampedCenter);
			DecimatedCoordinates.Sort();

			UniqueCoordinates.Reset(DecimatedCoordinates.Num());
			for (float Coordinate : DecimatedCoordinates)
			{
				if (UniqueCoordinates.Num() == 0 || !FMath::IsNearlyEqual(UniqueCoordinates.Last(), Coordinate, KINDA_SMALL_NUMBER))
				{
					UniqueCoordinates.Add(Coordinate);
				}
			}
		}

		OutCoordinates = MoveTemp(UniqueCoordinates);
	}

	void GPUTessellationConvertCollisionRingCoordinatesToSourceIndices(
		const TArray<float>& Coordinates,
		float MinValue,
		float MaxValue,
		int32 SourceResolution,
		TArray<int32>& OutIndices)
	{
		OutIndices.Reset();
		if (SourceResolution < 2 || MaxValue <= MinValue)
		{
			return;
		}

		OutIndices.Reserve(Coordinates.Num() + 2);
		OutIndices.Add(0);
		OutIndices.Add(SourceResolution - 1);
		for (float Coordinate : Coordinates)
		{
			const float Alpha = FMath::Clamp((Coordinate - MinValue) / (MaxValue - MinValue), 0.0f, 1.0f);
			OutIndices.Add(FMath::Clamp(FMath::RoundToInt(Alpha * (float)(SourceResolution - 1)), 0, SourceResolution - 1));
		}

		OutIndices.Sort();
		TArray<int32> UniqueIndices;
		UniqueIndices.Reserve(OutIndices.Num());
		for (int32 Index : OutIndices)
		{
			if (UniqueIndices.Num() == 0 || UniqueIndices.Last() != Index)
			{
				UniqueIndices.Add(Index);
			}
		}

		OutIndices = MoveTemp(UniqueIndices);
	}
}

UGPUTessellationComponent::UGPUTessellationComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, CurrentLODLevel(16.0f)
	, LastAppliedTessFactor(16)
	, LastCameraPosition(FVector::ZeroVector)
	, CurrentResolution(32, 32)
	, LastLogTime(0.0)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	
#if WITH_EDITOR
	// Enable ticking in editor so LOD works in viewport
	bTickInEditor = true;
#endif
	
	// Set default bounds
	Bounds = FBoxSphereBounds(FBox(FVector(-500, -500, -100), FVector(500, 500, 100)));
	
	// Enable shadow casting
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = true;
	bAffectDistanceFieldLighting = true;

	// Let direct component/actor line traces and optional coarse Chaos collision work out of
	// the box. Trace-only mode still has no physics body unless coarse mesh mode is enabled.
	SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	SetCollisionResponseToAllChannels(ECR_Block);

	WaterInteractionObjectTypes.Add(UEngineTypes::ConvertToObjectType(ECC_PhysicsBody));
}

void UGPUTessellationComponent::OnRegister()
{
	Super::OnRegister();

	// Update bounds before scene proxy creation
	UpdateBounds();

	// FIX (CRITICAL #2): Reset per-instance LOD init so re-registered components warm up cleanly.
	bLODInitialized = false;

	// Force the sampled textures to be fully resident before the first proxy build so the
	// compute pipeline doesn't bake a near-flat displacement from an unstreamed placeholder.
	GPUTessellationForceTextureResident(DisplacementTexture);
	GPUTessellationForceTextureResident(GetVectorDisplacementTexture());
	GPUTessellationForceTextureResident(SubtractTexture);
	GPUTessellationForceTextureResident(NormalMapTexture);

	// Reset the streaming-completion tracker so TickComponent will rebuild the proxy as
	// soon as a real RHI shows up (covers the case where WaitForStreaming was a no-op,
	// e.g. cooked builds, async-loaded assets, or render targets).
	LastObservedDisplacementTextureRHI = nullptr;
	LastObservedVectorDisplacementTextureRHI = nullptr;
	LastObservedSubtractTextureRHI = nullptr;
	LastObservedNormalMapTextureRHI = nullptr;

	// Initial mesh generation
	if (bAutoUpdate)
	{
		UpdateTessellatedMesh();
	}

	MarkCollisionMeshDirty();
	UpdateCoarseCollisionMesh(true);
}

void UGPUTessellationComponent::OnUnregister()
{
	for (UBodySetup* PendingBodySetup : AsyncCollisionBodySetupQueue)
	{
		if (PendingBodySetup)
		{
			PendingBodySetup->AbortPhysicsMeshAsyncCreation();
		}
	}
	AsyncCollisionBodySetupQueue.Empty();

	Super::OnUnregister();
}

void UGPUTessellationComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	
	if (!bAutoUpdate)
	{
		UpdateCoarseCollisionMesh(false);
		UpdateWaterInteraction();
		DrawCollisionMeshDebug();
		DrawWaterInteractionDebug();
		return;
	}
	
	// Update LOD based on selected mode
	switch (TessellationSettings.LODMode)
	{
		case EGPUTessellationLODMode::DistanceBased:
		{
			// FIX (CRITICAL #2): Per-instance init flag instead of process-wide static.
			if (!bLODInitialized)
			{
				// Initialize from MaxTessellationFactor (LOD range max)
				CurrentLODLevel = (float)TessellationSettings.MaxTessellationFactor;
				LastAppliedTessFactor = ApplyGeometrySubdivisionMultiplier(TessellationSettings.MaxTessellationFactor);
				bLODInitialized = true;

				if (bEnableDebugLogging)
				{
					UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: LOD Initialized - Max Factor: %d, Min Factor: %d"),
						TessellationSettings.MaxTessellationFactor, TessellationSettings.MinTessellationFactor);
				}
			}
			UpdateDistanceBasedLOD(DeltaTime);
			break;
		}
		
		case EGPUTessellationLODMode::DistanceBasedDiscrete:
		{
			UpdateDiscreteLOD(DeltaTime);
			break;
		}
		
		case EGPUTessellationLODMode::DistanceBasedPatches:
		{
			UpdatePatchBasedLOD(DeltaTime);
			break;
		}

		case EGPUTessellationLODMode::DistanceBasedQuadtree:
		{
			UpdateQuadtreeLOD(DeltaTime);
			break;
		}
			
		case EGPUTessellationLODMode::DensityTexture:
			UpdateDensityBasedLOD(DeltaTime);
			break;
			
		case EGPUTessellationLODMode::Disabled:
		default:
			// No LOD - use TessellationFactor directly via CalculateGridResolution()
			break;
	}
	
	if (ShouldUpdateRenderTargetDrivenResources())
	{
		MarkCollisionMeshDirty();
		MarkWaterSurfaceReadbackDirty();
		MarkRenderStateDirty();
	}

	// Texture-streaming completion detection.
	// The proxy bakes displacement into the position buffer in a one-shot compute dispatch
	// at proxy-build time. If the texture's FTextureResource RHI wasn't ready yet (still
	// streaming, async-loading, or a placeholder), the bake samples a 2x2 stub and the
	// plane comes out flat / blurred. Once the real RHI shows up, the resource pointer
	// changes -- we detect that here and rebuild the proxy so the bake re-runs against
	// the fully-resident texture. Fixes the "open texture asset to magically repair the
	// plane" symptom.
	{
		void* CurrentDisplacementRHI = GPUTessellationGetTextureRHIIdentity(DisplacementTexture);
		void* CurrentVectorDisplacementRHI = GPUTessellationGetTextureRHIIdentity(GetVectorDisplacementTexture());
		void* CurrentSubtractRHI     = GPUTessellationGetTextureRHIIdentity(SubtractTexture);
		void* CurrentNormalMapRHI    = GPUTessellationGetTextureRHIIdentity(NormalMapTexture);

		const bool bDisplacementChanged = DisplacementTexture && CurrentDisplacementRHI && CurrentDisplacementRHI != LastObservedDisplacementTextureRHI;
		const bool bVectorDisplacementChanged = GetVectorDisplacementTexture() && CurrentVectorDisplacementRHI && CurrentVectorDisplacementRHI != LastObservedVectorDisplacementTextureRHI;
		const bool bSubtractChanged     = SubtractTexture     && CurrentSubtractRHI     && CurrentSubtractRHI     != LastObservedSubtractTextureRHI;
		const bool bNormalMapChanged    = NormalMapTexture    && CurrentNormalMapRHI    && CurrentNormalMapRHI    != LastObservedNormalMapTextureRHI;

		if (bDisplacementChanged || bVectorDisplacementChanged || bSubtractChanged || bNormalMapChanged)
		{
			LastObservedDisplacementTextureRHI = CurrentDisplacementRHI;
			LastObservedVectorDisplacementTextureRHI = CurrentVectorDisplacementRHI;
			LastObservedSubtractTextureRHI     = CurrentSubtractRHI;
			LastObservedNormalMapTextureRHI    = CurrentNormalMapRHI;

			MarkCollisionMeshDirty();
			MarkWaterSurfaceReadbackDirty();
			MarkRenderStateDirty();

			if (bEnableDebugLogging)
			{
				UE_LOG(LogTemp, Log, TEXT("GPUTessellation: Texture RHI became resident (Displacement:%d Vector:%d Subtract:%d Normal:%d) -- rebuilding proxy."),
					bDisplacementChanged ? 1 : 0, bVectorDisplacementChanged ? 1 : 0, bSubtractChanged ? 1 : 0, bNormalMapChanged ? 1 : 0);
			}
		}
	}

	UpdateCoarseCollisionMesh(false);
	UpdateWaterInteraction();
	DrawCollisionMeshDebug();
	DrawWaterInteractionDebug();
}

FPrimitiveSceneProxy* UGPUTessellationComponent::CreateSceneProxy()
{
	if (TessellationSettings.TessellationFactor > 0.0f)
	{
		return new FGPUTessellationSceneProxy(this);
	}
	return nullptr;
}

FBoxSphereBounds UGPUTessellationComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Calculate bounds based on plane size and displacement.
	// FIX: The compute shader (GPUVertexGeneration.usf) generates the plane on the XY plane
	// with Z as the displacement axis (UE5 Z-up convention). Previously the bounds were built
	// as an XZ slab (Y treated as displacement), producing a paper-thin box perpendicular to
	// the actual mesh. Frustum culling rejected the proxy as soon as the camera approached
	// the real plane edge along Y, causing the surface to disappear.
	const FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
	float HalfSizeX = EffectiveSettings.PlaneSizeX * 0.5f;
	float HalfSizeY = EffectiveSettings.PlaneSizeY * 0.5f;
	float MaxDisplacement = EffectiveSettings.bUseVectorDisplacement && !EffectiveSettings.bAddScalarHeightDisplacementToVector
		? 0.0f
		: EffectiveSettings.DisplacementIntensity + FMath::Abs(EffectiveSettings.DisplacementOffset);
	if (EffectiveSettings.bUseVectorDisplacement)
	{
		const FVector VectorScale = EffectiveSettings.VectorDisplacementScale * EffectiveSettings.VectorDisplacementIntensity;
		const FVector VectorBias = EffectiveSettings.VectorDisplacementBias * EffectiveSettings.VectorDisplacementIntensity;
		const FVector VectorExtent(
			FMath::Abs(VectorScale.X) + FMath::Abs(VectorBias.X) + FMath::Max(0.0, EffectiveSettings.VectorDisplacementBoundsPadding.X),
			FMath::Abs(VectorScale.Y) + FMath::Abs(VectorBias.Y) + FMath::Max(0.0, EffectiveSettings.VectorDisplacementBoundsPadding.Y),
			FMath::Abs(VectorScale.Z) + FMath::Abs(VectorBias.Z) + FMath::Max(0.0, EffectiveSettings.VectorDisplacementBoundsPadding.Z));
		HalfSizeX += VectorExtent.X;
		HalfSizeY += VectorExtent.Y;
		MaxDisplacement += VectorExtent.Z;
	}
	// Add a small safety pad so vertices that sit exactly on the boundary (and any subpixel
	// jitter in the displacement amplitude) cannot push geometry outside the proxy bounds.
	const float BoundsPadding = 1.0f;
	MaxDisplacement += BoundsPadding;
	
	// Check for zero or near-zero scale which would make bounds invalid
	FVector Scale3D = LocalToWorld.GetScale3D();
	const float MinScale = 0.001f;
	if (FMath::IsNearlyZero(Scale3D.X, MinScale) || 
		FMath::IsNearlyZero(Scale3D.Y, MinScale) || 
		FMath::IsNearlyZero(Scale3D.Z, MinScale))
	{
		// This is an error condition - always log as Warning
		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: CalcBounds - ZERO OR NEAR-ZERO SCALE DETECTED: %s - Using identity scale"), 
				*Scale3D.ToString());
		}
		// Use a transform with identity scale
		FTransform FixedTransform = LocalToWorld;
		FixedTransform.SetScale3D(FVector::OneVector);
		
		FBox LocalBox(
			FVector(-HalfSizeX, -HalfSizeY, -MaxDisplacement),
			FVector(HalfSizeX, HalfSizeY, MaxDisplacement)
		);
		
		return FBoxSphereBounds(LocalBox).TransformBy(FixedTransform);
	}
	
	FBox LocalBox(
		FVector(-HalfSizeX, -HalfSizeY, -MaxDisplacement),
		FVector(HalfSizeX, HalfSizeY, MaxDisplacement)
	);
	
	FBoxSphereBounds Result = FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
	
	// Throttled logging (max once every 2 seconds)
	if (bEnableDebugLogging)
	{
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastLogTime >= 2.0)
		{
			LastLogTime = CurrentTime;
			UE_LOG(LogTemp, Log, TEXT("GPUTessellation: CalcBounds - PlaneSizeX:%.1f PlaneSizeZ:%.1f MaxDisp:%.1f Scale:%s Result:%s"), 
				EffectiveSettings.PlaneSizeX, EffectiveSettings.PlaneSizeY, MaxDisplacement, 
				*Scale3D.ToString(), *Result.ToString());
		}
	}
	
	return Result;
}

void UGPUTessellationComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (Material)
	{
		OutMaterials.AddUnique(Material);
	}
}

int32 UGPUTessellationComponent::GetNumMaterials() const
{
	return Material ? 1 : 0;
}

UMaterialInterface* UGPUTessellationComponent::GetMaterial(int32 ElementIndex) const
{
	return (ElementIndex == 0) ? Material : nullptr;
}

bool UGPUTessellationComponent::LineTraceComponent(FHitResult& OutHit, const FVector Start, const FVector End, const FCollisionQueryParams& Params)
{
	return LineTraceComponent(OutHit, Start, End, ECC_Visibility, Params, FCollisionResponseParams::DefaultResponseParam, FCollisionObjectQueryParams::DefaultObjectQueryParam);
}

bool UGPUTessellationComponent::LineTraceComponent(FHitResult& OutHit, const FVector Start, const FVector End, ECollisionChannel TraceChannel, const FCollisionQueryParams& Params, const FCollisionResponseParams& ResponseParams, const FCollisionObjectQueryParams& ObjectParams)
{
	if (CollisionMode != EGPUTessellationCollisionMode::Disabled && bEnableHeightFieldLineTrace && IsQueryCollisionEnabled() && GetCollisionResponseToChannel(TraceChannel) == ECollisionResponse::ECR_Block)
	{
		if (LineTraceHeightField(Start, End, OutHit, HeightFieldLineTraceSteps))
		{
			return true;
		}
	}

	return Super::LineTraceComponent(OutHit, Start, End, TraceChannel, Params, ResponseParams, ObjectParams);
}

UBodySetup* UGPUTessellationComponent::GetBodySetup()
{
	if (!IsPhysicsCollisionMeshEnabled())
	{
		return nullptr;
	}

	CreateCollisionBodySetup();
	return CollisionBodySetup;
}

bool UGPUTessellationComponent::GetTriMeshSizeEstimates(FTriMeshCollisionDataEstimates& OutTriMeshEstimates, bool bInUseAllTriData) const
{
	OutTriMeshEstimates.VerticeCount += CollisionMeshVertices.Num();
	return IsPhysicsCollisionMeshEnabled();
}

bool UGPUTessellationComponent::GetPhysicsTriMeshData(FTriMeshCollisionData* CollisionData, bool InUseAllTriData)
{
	if (!CollisionData || !ContainsPhysicsTriMeshData(InUseAllTriData))
	{
		return false;
	}

	const int32 VertexBase = CollisionData->Vertices.Num();
	CollisionData->Vertices.Reserve(VertexBase + CollisionMeshVertices.Num());
	for (const FVector& Vertex : CollisionMeshVertices)
	{
		CollisionData->Vertices.Add((FVector3f)Vertex);
	}

	const int32 TriangleCount = CollisionMeshIndices.Num() / 3;
	CollisionData->Indices.Reserve(CollisionData->Indices.Num() + TriangleCount);
	CollisionData->MaterialIndices.Reserve(CollisionData->MaterialIndices.Num() + TriangleCount);
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		FTriIndices Triangle;
		Triangle.v0 = CollisionMeshIndices[TriangleIndex * 3 + 0] + VertexBase;
		Triangle.v1 = CollisionMeshIndices[TriangleIndex * 3 + 1] + VertexBase;
		Triangle.v2 = CollisionMeshIndices[TriangleIndex * 3 + 2] + VertexBase;
		CollisionData->Indices.Add(Triangle);
		CollisionData->MaterialIndices.Add(0);
	}

	const bool bUseRobustVertexPerfectCook = IsVertexPerfectCollisionMeshEnabled();
	CollisionData->bFlipNormals = false;
	CollisionData->bDeformableMesh = !bUseRobustVertexPerfectCook;
	CollisionData->bFastCook = !bUseRobustVertexPerfectCook;
	CollisionData->bDisableActiveEdgePrecompute = false;
	return true;
}

bool UGPUTessellationComponent::ContainsPhysicsTriMeshData(bool InUseAllTriData) const
{
	return IsPhysicsCollisionMeshEnabled() && CollisionMeshVertices.Num() >= 3 && CollisionMeshIndices.Num() >= 3;
}

#if WITH_EDITOR
void UGPUTessellationComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	// Update mesh when properties change
	if (PropertyChangedEvent.Property)
	{
		LastObservedRenderTargetRenderTime = -TNumericLimits<double>::Max();
		MarkCollisionMeshDirty();
		MarkWaterSurfaceReadbackDirty();
		UpdateCoarseCollisionMesh(true);
		MarkRenderStateDirty();
	}
}
#endif

void UGPUTessellationComponent::UpdateTessellatedMesh()
{
	MarkRenderStateDirty();
}

void UGPUTessellationComponent::SetDisplacementTexture(UTexture* InTexture)
{
	DisplacementTexture = InTexture;
	GPUTessellationForceTextureResident(DisplacementTexture);
	LastObservedRenderTargetRenderTime = -TNumericLimits<double>::Max();
	LastObservedDisplacementTextureRHI = nullptr;
	MarkCollisionMeshDirty();
	MarkWaterSurfaceReadbackDirty();
	UpdateCoarseCollisionMesh(true);
	UpdateTessellatedMesh();
}

void UGPUTessellationComponent::SetSubtractTexture(UTexture* InTexture)
{
	SubtractTexture = InTexture;
	GPUTessellationForceTextureResident(SubtractTexture);
	LastObservedRenderTargetRenderTime = -TNumericLimits<double>::Max();
	LastObservedSubtractTextureRHI = nullptr;
	MarkCollisionMeshDirty();
	MarkWaterSurfaceReadbackDirty();
	UpdateCoarseCollisionMesh(true);
	UpdateTessellatedMesh();
}

void UGPUTessellationComponent::SetNormalMapTexture(UTexture* InTexture)
{
	NormalMapTexture = InTexture;
	GPUTessellationForceTextureResident(NormalMapTexture);
	LastObservedRenderTargetRenderTime = -TNumericLimits<double>::Max();
	LastObservedNormalMapTextureRHI = nullptr;
	MarkWaterSurfaceReadbackDirty();
	UpdateTessellatedMesh();
}

void UGPUTessellationComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial)
{
	if (ElementIndex == 0)
	{
		Material = InMaterial;
		MarkRenderStateDirty();
	}
}

void UGPUTessellationComponent::UpdateSettings(const FGPUTessellationSettings& NewSettings)
{
	TessellationSettings = NewSettings;
	MarkCollisionMeshDirty();
	MarkWaterSurfaceReadbackDirty();
	UpdateCoarseCollisionMesh(true);
	UpdateTessellatedMesh();
}

#if WITH_EDITOR
void UGPUTessellationComponent::BakeCurrentTessellationToStaticMesh()
{
	BakeCurrentTessellationToStaticMeshAsset();
}

UStaticMesh* UGPUTessellationComponent::BakeCurrentTessellationToStaticMeshAsset()
{
	const UObject* SourceObject = GetOwner() ? static_cast<const UObject*>(GetOwner()) : static_cast<const UObject*>(this);
	if (bBakeNormalMapTexture && bBakeNormalMapOnly)
	{
		const FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
		FGPUTessellationNormalMapBakeOptions NormalMapOptions;
		NormalMapOptions.AssetDirectory = BakeAssetDirectory;
		NormalMapOptions.AssetName = BakeNormalMapAssetName;
		NormalMapOptions.PlaneSizeX = EffectiveSettings.PlaneSizeX;
		NormalMapOptions.PlaneSizeY = EffectiveSettings.PlaneSizeY;
		NormalMapOptions.DisplacementIntensity = EffectiveSettings.DisplacementIntensity;
		NormalMapOptions.TexelStep = BakeNormalMapTexelStep;
		NormalMapOptions.Strength = BakeNormalMapStrength;
		NormalMapOptions.bAutoSaveAsset = bBakeMeshAutoSaveAsset;
		BakeGPUTessellationHeightNormalMapToTexture(SourceObject, DisplacementTexture, SubtractTexture, NormalMapOptions);
		return nullptr;
	}

	FGPUTessellatedMeshData MeshData;
	if (!BuildBakeMeshData(MeshData))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Failed to build bake mesh data for %s."), *GetName());
		return nullptr;
	}

	TArray<FGPUTessellationStaticMeshBakeSection> Sections;
	FGPUTessellationStaticMeshBakeSection& Section = Sections.AddDefaulted_GetRef();
	Section.FirstIndex = 0;
	Section.NumTriangles = MeshData.Indices.Num() / 3;
	Section.MaterialIndex = 0;

	TArray<UMaterialInterface*> Materials;
	Materials.Add(Material);

	FGPUTessellationStaticMeshBakeOptions Options;
	Options.AssetDirectory = BakeAssetDirectory;
	Options.AssetName = BakeAssetName;
	Options.bAllowCPUAccess = bBakeMeshAllowCPUAccess;
	Options.bUseComplexAsSimpleCollision = bBakeMeshUseComplexCollision;
	Options.bAutoSaveAsset = bBakeMeshAutoSaveAsset;

	UStaticMesh* BakedMesh = BakeGPUTessellationMeshDataToStaticMesh(SourceObject, MeshData, Sections, Materials, Options);

	if (BakedMesh && bBakeNormalMapTexture)
	{
		const FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
		FGPUTessellationNormalMapBakeOptions NormalMapOptions;
		NormalMapOptions.AssetDirectory = BakeAssetDirectory;
		NormalMapOptions.AssetName = BakeNormalMapAssetName.IsEmpty()
			? FString::Printf(TEXT("%s_N"), *BakedMesh->GetName())
			: BakeNormalMapAssetName;
		NormalMapOptions.PlaneSizeX = EffectiveSettings.PlaneSizeX;
		NormalMapOptions.PlaneSizeY = EffectiveSettings.PlaneSizeY;
		NormalMapOptions.DisplacementIntensity = EffectiveSettings.DisplacementIntensity;
		NormalMapOptions.TexelStep = BakeNormalMapTexelStep;
		NormalMapOptions.Strength = BakeNormalMapStrength;
		NormalMapOptions.bAutoSaveAsset = bBakeMeshAutoSaveAsset;
		BakeGPUTessellationHeightNormalMapToTexture(SourceObject, DisplacementTexture, SubtractTexture, NormalMapOptions);
	}

	return BakedMesh;
}
#endif

FGPUTessellationSettings UGPUTessellationComponent::GetEffectiveTessellationSettings() const
{
	FGPUTessellationSettings EffectiveSettings = TessellationSettings;
	if (!bEnableProceduralOceanSettings)
	{
		EffectiveSettings.OceanSettings = FGPUOceanSettings();
	}
	if (EffectiveSettings.LODMode == EGPUTessellationLODMode::Disabled)
	{
		EffectiveSettings.TessellationFactor = ApplyGeometrySubdivisionMultiplier(EffectiveSettings.TessellationFactor);
	}
	return EffectiveSettings;
}

FIntPoint UGPUTessellationComponent::GetTessellationResolution() const
{
	return CalculateGridResolution();
}

int32 UGPUTessellationComponent::ApplyGeometrySubdivisionMultiplier(int32 TessellationFactor) const
{
	int32 EffectiveFactor = FMath::Clamp(TessellationFactor, 1, 1024);
	if (TessellationSettings.bSubdivideHardEdges)
	{
		const int32 Multiplier = FMath::Clamp(TessellationSettings.SubdivisionMultiplier, 2, 8);
		EffectiveFactor = FMath::Clamp(EffectiveFactor * Multiplier, 1, 1024);
	}
	return EffectiveFactor;
}

int32 UGPUTessellationComponent::GetVertexCount() const
{
	FIntPoint Res = CalculateGridResolution();
	return Res.X * Res.Y;
}

int32 UGPUTessellationComponent::GetTriangleCount() const
{
	FIntPoint Res = CalculateGridResolution();
	return (Res.X - 1) * (Res.Y - 1) * 2;
}

bool UGPUTessellationComponent::ProjectWorldToTessellationUV(const FVector& WorldPosition, FVector2D& OutUV, FVector& OutLocalPosition) const
{
	const float PlaneSizeX = TessellationSettings.PlaneSizeX;
	const float PlaneSizeY = TessellationSettings.PlaneSizeY;
	if (PlaneSizeX <= KINDA_SMALL_NUMBER || PlaneSizeY <= KINDA_SMALL_NUMBER)
	{
		OutUV = FVector2D::ZeroVector;
		OutLocalPosition = FVector::ZeroVector;
		return false;
	}

	OutLocalPosition = GetComponentTransform().InverseTransformPosition(WorldPosition);
	OutUV = FVector2D(
		(OutLocalPosition.X / PlaneSizeX) + 0.5,
		(OutLocalPosition.Y / PlaneSizeY) + 0.5);

	return OutUV.X >= 0.0 && OutUV.X <= 1.0 && OutUV.Y >= 0.0 && OutUV.Y <= 1.0;
}

bool UGPUTessellationComponent::SampleHeightAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const
{
	FVector2D SurfaceUV;
	FVector LocalPosition;
	if (!ProjectWorldToTessellationUV(WorldPosition, SurfaceUV, LocalPosition))
	{
		OutWorldHeight = 0.0f;
		OutWorldPosition = FVector::ZeroVector;
		OutWorldNormal = FVector::UpVector;
		return false;
	}

	float LocalHeight = 0.0f;
	const FVector2D LocalXY(LocalPosition.X, LocalPosition.Y);
	if (!EvaluateLocalSurfaceHeight(LocalXY, LocalHeight))
	{
		OutWorldHeight = 0.0f;
		OutWorldPosition = FVector::ZeroVector;
		OutWorldNormal = FVector::UpVector;
		return false;
	}

	const FVector LocalSurfacePosition(LocalPosition.X, LocalPosition.Y, LocalHeight);
	OutWorldPosition = GetComponentTransform().TransformPosition(LocalSurfacePosition);
	OutWorldHeight = (float)OutWorldPosition.Z;

	const FVector LocalNormal = CalculateLocalSurfaceNormal(LocalXY);
	const FMatrix NormalToWorld = GetComponentTransform().ToInverseMatrixWithScale().GetTransposed();
	OutWorldNormal = FVector(NormalToWorld.TransformVector(LocalNormal)).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	return true;
}

bool UGPUTessellationComponent::SampleWaterSurfaceAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const
{
	if (SampleHeightAtWorldPosition(WorldPosition, OutWorldHeight, OutWorldPosition, OutWorldNormal))
	{
		return true;
	}

	if (SampleCachedWaterSurfaceHeightAtWorldPosition(WorldPosition, OutWorldHeight, OutWorldPosition, OutWorldNormal))
	{
		return true;
	}

	if (bWaterUseCachedCollisionMeshSurface && SampleCachedCollisionMeshHeightAtWorldPosition(WorldPosition, OutWorldHeight, OutWorldPosition, OutWorldNormal))
	{
		return true;
	}

	if (bWaterFallbackToPlaneSurface)
	{
		FVector2D SurfaceUV;
		FVector LocalPosition;
		if (ProjectWorldToTessellationUV(WorldPosition, SurfaceUV, LocalPosition))
		{
			const FVector LocalSurfacePosition(LocalPosition.X, LocalPosition.Y, TessellationSettings.DisplacementOffset);
			OutWorldPosition = GetComponentTransform().TransformPosition(LocalSurfacePosition);
			OutWorldHeight = (float)OutWorldPosition.Z;

			const FMatrix NormalToWorld = GetComponentTransform().ToInverseMatrixWithScale().GetTransposed();
			OutWorldNormal = FVector(NormalToWorld.TransformVector(FVector::UpVector)).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			return true;
		}
	}

	OutWorldHeight = 0.0f;
	OutWorldPosition = FVector::ZeroVector;
	OutWorldNormal = FVector::UpVector;
	return false;
}

bool UGPUTessellationComponent::LineTraceHeightField(const FVector& TraceStart, const FVector& TraceEnd, FHitResult& OutHit, int32 NumSteps) const
{
	OutHit.Init(TraceStart, TraceEnd);

	const FVector WorldDelta = TraceEnd - TraceStart;
	if (WorldDelta.IsNearlyZero())
	{
		return false;
	}

	const FTransform ComponentTransform = GetComponentTransform();
	const int32 StepCount = FMath::Clamp(NumSteps, 1, 512);
	const auto SampleSignedDistance = [this, &ComponentTransform, TraceStart, WorldDelta](float Alpha, float& OutSignedDistance, FVector& OutLocalPosition) -> bool
	{
		const FVector WorldPosition = TraceStart + (WorldDelta * Alpha);
		OutLocalPosition = ComponentTransform.InverseTransformPosition(WorldPosition);
		const FVector2D LocalXY(OutLocalPosition.X, OutLocalPosition.Y);
		if (!IsLocalXYInsidePlane(LocalXY))
		{
			return false;
		}

		float LocalHeight = 0.0f;
		if (!EvaluateLocalSurfaceHeight(LocalXY, LocalHeight))
		{
			return false;
		}

		OutSignedDistance = (float)(OutLocalPosition.Z - LocalHeight);
		return true;
	};

	bool bHasPreviousSample = false;
	bool bFoundHit = false;
	float PreviousAlpha = 0.0f;
	float PreviousSignedDistance = 0.0f;
	FVector PreviousLocalPosition = FVector::ZeroVector;
	float HitAlpha = 0.0f;
	FVector HitLocalPosition = FVector::ZeroVector;

	for (int32 StepIndex = 0; StepIndex <= StepCount; ++StepIndex)
	{
		const float CurrentAlpha = (float)StepIndex / (float)StepCount;
		float CurrentSignedDistance = 0.0f;
		FVector CurrentLocalPosition = FVector::ZeroVector;
		if (!SampleSignedDistance(CurrentAlpha, CurrentSignedDistance, CurrentLocalPosition))
		{
			bHasPreviousSample = false;
			continue;
		}

		if (FMath::IsNearlyZero(CurrentSignedDistance, 0.1f))
		{
			HitAlpha = CurrentAlpha;
			HitLocalPosition = CurrentLocalPosition;
			bFoundHit = true;
			break;
		}

		if (bHasPreviousSample && ((PreviousSignedDistance < 0.0f) != (CurrentSignedDistance < 0.0f)))
		{
			float LowAlpha = PreviousAlpha;
			float HighAlpha = CurrentAlpha;
			float LowSignedDistance = PreviousSignedDistance;
			FVector MidLocalPosition = CurrentLocalPosition;

			for (int32 RefineIndex = 0; RefineIndex < 8; ++RefineIndex)
			{
				const float MidAlpha = (LowAlpha + HighAlpha) * 0.5f;
				float MidSignedDistance = 0.0f;
				if (!SampleSignedDistance(MidAlpha, MidSignedDistance, MidLocalPosition))
				{
					break;
				}

				if ((LowSignedDistance < 0.0f) == (MidSignedDistance < 0.0f))
				{
					LowAlpha = MidAlpha;
					LowSignedDistance = MidSignedDistance;
				}
				else
				{
					HighAlpha = MidAlpha;
				}
			}

			HitAlpha = (LowAlpha + HighAlpha) * 0.5f;
			HitLocalPosition = MidLocalPosition;
			bFoundHit = true;
			break;
		}

		bHasPreviousSample = true;
		PreviousAlpha = CurrentAlpha;
		PreviousSignedDistance = CurrentSignedDistance;
		PreviousLocalPosition = CurrentLocalPosition;
	}

	if (!bFoundHit)
	{
		return false;
	}

	const FVector2D HitLocalXY(HitLocalPosition.X, HitLocalPosition.Y);
	float HitLocalHeight = 0.0f;
	if (!EvaluateLocalSurfaceHeight(HitLocalXY, HitLocalHeight))
	{
		return false;
	}

	const FVector HitWorldPosition = ComponentTransform.TransformPosition(FVector(HitLocalPosition.X, HitLocalPosition.Y, HitLocalHeight));
	const FVector LocalNormal = CalculateLocalSurfaceNormal(HitLocalXY);
	const FMatrix NormalToWorld = ComponentTransform.ToInverseMatrixWithScale().GetTransposed();
	const FVector HitWorldNormal = FVector(NormalToWorld.TransformVector(LocalNormal)).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);

	OutHit = FHitResult(GetOwner(), const_cast<UGPUTessellationComponent*>(this), HitWorldPosition, HitWorldNormal);
	OutHit.TraceStart = TraceStart;
	OutHit.TraceEnd = TraceEnd;
	OutHit.Time = HitAlpha;
	OutHit.Distance = (HitWorldPosition - TraceStart).Size();
	OutHit.Location = HitWorldPosition;
	OutHit.ImpactPoint = HitWorldPosition;
	OutHit.Normal = HitWorldNormal;
	OutHit.ImpactNormal = HitWorldNormal;
	OutHit.bBlockingHit = true;
	return true;
}

void UGPUTessellationComponent::RebuildCollisionMesh()
{
	MarkCollisionMeshDirty();
	UpdateCoarseCollisionMesh(true);
}

void UGPUTessellationComponent::MarkCollisionMeshDirty()
{
	bCollisionMeshDirty = true;
	bCollisionLODRingSourceDirty = true;
	LastVertexPerfectCollisionVisualLODSignature = 0;
}

bool UGPUTessellationComponent::IsCoarseCollisionMeshEnabled() const
{
	return CollisionMode == EGPUTessellationCollisionMode::CoarseHeightFieldMesh;
}

bool UGPUTessellationComponent::IsCollisionLODRingsMeshEnabled() const
{
	return CollisionMode == EGPUTessellationCollisionMode::CollisionLODRingsMesh;
}

bool UGPUTessellationComponent::IsVertexPerfectCollisionMeshEnabled() const
{
	return CollisionMode == EGPUTessellationCollisionMode::VertexPerfectMesh;
}

bool UGPUTessellationComponent::IsPhysicsCollisionMeshEnabled() const
{
	return IsCoarseCollisionMeshEnabled() || IsCollisionLODRingsMeshEnabled() || IsVertexPerfectCollisionMeshEnabled();
}

bool UGPUTessellationComponent::IsCollisionSurfaceAnimated() const
{
	const FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
	if (!IsPhysicsCollisionMeshEnabled())
	{
		return false;
	}

	if (EffectiveSettings.OceanSettings.WaveMode == EGPUOceanWaveMode::FFT)
	{
		return true;
	}

	return EffectiveSettings.OceanSettings.WaveMode == EGPUOceanWaveMode::Gerstner ||
		EffectiveSettings.OceanSettings.WaveMode == EGPUOceanWaveMode::PerlinFBM;
}

bool UGPUTessellationComponent::BuildCoarseCollisionMeshData()
{
	CollisionMeshVertices.Reset();
	CollisionMeshIndices.Reset();

	if (!IsCoarseCollisionMeshEnabled())
	{
		return false;
	}

	const float PlaneSizeX = TessellationSettings.PlaneSizeX;
	const float PlaneSizeY = TessellationSettings.PlaneSizeY;
	if (PlaneSizeX <= KINDA_SMALL_NUMBER || PlaneSizeY <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const int32 QuadCount = FMath::Clamp(CollisionResolution, 2, 256);
	const int32 CoarseReadbackTessellationFactor = FMath::Clamp(FMath::DivideAndRoundUp(QuadCount, 4), 1, 256);

	float TestHeight = 0.0f;
	if (!EvaluateLocalSurfaceHeight(FVector2D::ZeroVector, TestHeight))
	{
		return BuildGPUReadbackCollisionMeshData(CoarseReadbackTessellationFactor);
	}

	const int32 VertexCountPerSide = QuadCount + 1;
	CollisionMeshVertices.Reserve(VertexCountPerSide * VertexCountPerSide);
	CollisionMeshIndices.Reserve(QuadCount * QuadCount * 6);

	const float HalfSizeX = PlaneSizeX * 0.5f;
	const float HalfSizeY = PlaneSizeY * 0.5f;
	for (int32 YIndex = 0; YIndex < VertexCountPerSide; ++YIndex)
	{
		const float V = (float)YIndex / (float)QuadCount;
		const float LocalY = FMath::Lerp(-HalfSizeY, HalfSizeY, V);
		for (int32 XIndex = 0; XIndex < VertexCountPerSide; ++XIndex)
		{
			const float U = (float)XIndex / (float)QuadCount;
			const float LocalX = FMath::Lerp(-HalfSizeX, HalfSizeX, U);
			float LocalHeight = 0.0f;
			if (!EvaluateLocalSurfaceHeight(FVector2D(LocalX, LocalY), LocalHeight))
			{
				return BuildGPUReadbackCollisionMeshData(CoarseReadbackTessellationFactor);
			}

			CollisionMeshVertices.Add(FVector(LocalX, LocalY, LocalHeight));
		}
	}

	for (int32 YIndex = 0; YIndex < QuadCount; ++YIndex)
	{
		for (int32 XIndex = 0; XIndex < QuadCount; ++XIndex)
		{
			const int32 V00 = YIndex * VertexCountPerSide + XIndex;
			const int32 V10 = V00 + 1;
			const int32 V01 = V00 + VertexCountPerSide;
			const int32 V11 = V01 + 1;

			CollisionMeshIndices.Add(V00);
			CollisionMeshIndices.Add(V10);
			CollisionMeshIndices.Add(V11);
			CollisionMeshIndices.Add(V00);
			CollisionMeshIndices.Add(V11);
			CollisionMeshIndices.Add(V01);
		}
	}

	return true;
}

bool UGPUTessellationComponent::GetCollisionLODRingCameraPosition(FVector& OutCameraPosition) const
{
	const UWorld* World = GetWorld();
	if (World)
	{
		if (APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			FVector ViewLocation;
			FRotator ViewRotation;
			PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
			OutCameraPosition = ViewLocation;
			return true;
		}
	}

#if WITH_EDITOR
	const bool bCanUseEditorViewport = World && World->WorldType == EWorldType::Editor && GEditor && !GEditor->PlayWorld;
	if (bCanUseEditorViewport && GEditor)
	{
		FViewport* Viewport = GEditor->GetActiveViewport();
		if (Viewport)
		{
			FViewportClient* ViewportClientBase = Viewport->GetClient();
			FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(ViewportClientBase);
			if (ViewportClient)
			{
				OutCameraPosition = ViewportClient->GetViewLocation();
				return true;
			}
		}
	}
#endif

	OutCameraPosition = GetComponentLocation();
	return true;
}

bool UGPUTessellationComponent::UpdateCollisionLODRingCameraState(bool bForceUpdate)
{
	if (!IsCollisionLODRingsMeshEnabled())
	{
		bCollisionLODRingCameraInitialized = false;
		return false;
	}

	FVector CameraPosition = FVector::ZeroVector;
	if (!GetCollisionLODRingCameraPosition(CameraPosition))
	{
		return false;
	}

	const float MovementDistance = bCollisionLODRingCameraInitialized
		? FVector::Dist(CameraPosition, LastCollisionLODRingCameraPosition)
		: TNumericLimits<float>::Max();
	const float UpdateDistance = FMath::Max(0.0f, CollisionLODRingUpdateDistance);
	if (bForceUpdate || !bCollisionLODRingCameraInitialized || MovementDistance >= UpdateDistance)
	{
		LastCollisionLODRingCameraPosition = CameraPosition;
		LastCameraPosition = CameraPosition;
		bCollisionLODRingCameraInitialized = true;
		return true;
	}

	return false;
}

bool UGPUTessellationComponent::BuildCollisionLODRingSourceCache()
{
	if (!bCollisionLODRingSourceDirty && CollisionLODRingSourceVertices.Num() >= 3 &&
		CollisionLODRingSourceResolutionX >= 2 && CollisionLODRingSourceResolutionY >= 2)
	{
		return true;
	}

	CollisionLODRingSourceVertices.Reset();
	CollisionLODRingSourceResolutionX = 0;
	CollisionLODRingSourceResolutionY = 0;

	if (!IsCollisionLODRingsMeshEnabled())
	{
		return false;
	}

	FGPUTessellationSettings CollisionSettings = GetEffectiveTessellationSettings();
	CollisionSettings.LODMode = EGPUTessellationLODMode::Disabled;
	CollisionSettings.TessellationFactor = FMath::Clamp(CollisionLODRingReadbackTessellationFactor, 1, 512);
	CollisionSettings.UVOffset = FVector2f(0.0f, 0.0f);
	CollisionSettings.UVScale = FVector2f(1.0f, 1.0f);

	FGPUTessellatedMeshData ReadbackMeshData;
	FGPUTessellationMeshBuilder MeshBuilder;
	MeshBuilder.GenerateMeshSync(
		CollisionSettings,
		GetComponentTransform().ToMatrixWithScale(),
		LastCameraPosition,
		DisplacementTexture,
		SubtractTexture,
		ReadbackMeshData,
		FIntVector4(1, 1, 1, 1),
		2048,
		GetVectorDisplacementTexture());

	if (!ReadbackMeshData.IsValid() || ReadbackMeshData.ResolutionX < 2 || ReadbackMeshData.ResolutionY < 2)
	{
		return false;
	}

	CollisionLODRingSourceVertices.Reserve(ReadbackMeshData.Vertices.Num());
	for (const FVector3f& Vertex : ReadbackMeshData.Vertices)
	{
		CollisionLODRingSourceVertices.Add(FVector(Vertex));
	}

	CollisionLODRingSourceResolutionX = ReadbackMeshData.ResolutionX;
	CollisionLODRingSourceResolutionY = ReadbackMeshData.ResolutionY;
	bCollisionLODRingSourceDirty = false;
	return CollisionLODRingSourceVertices.Num() >= 3;
}

bool UGPUTessellationComponent::BuildCollisionLODRingsMeshDataFromSourceCache()
{
	CollisionMeshVertices.Reset();
	CollisionMeshIndices.Reset();

	if (!BuildCollisionLODRingSourceCache())
	{
		return false;
	}

	const float PlaneSizeX = TessellationSettings.PlaneSizeX;
	const float PlaneSizeY = TessellationSettings.PlaneSizeY;
	if (PlaneSizeX <= KINDA_SMALL_NUMBER || PlaneSizeY <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	if (!bCollisionLODRingCameraInitialized)
	{
		UpdateCollisionLODRingCameraState(true);
	}

	const FTransform ComponentTransform = GetComponentTransform();
	const FVector LocalCameraPosition = ComponentTransform.InverseTransformPosition(LastCollisionLODRingCameraPosition);
	const FVector Scale3D = ComponentTransform.GetScale3D();
	const float HalfSizeX = PlaneSizeX * 0.5f;
	const float HalfSizeY = PlaneSizeY * 0.5f;

	TArray<float> XCoordinates;
	TArray<float> YCoordinates;
	GPUTessellationBuildCollisionRingAxisCoordinates(
		-HalfSizeX,
		HalfSizeX,
		(float)LocalCameraPosition.X,
		(float)Scale3D.X,
		CollisionLODRingDistances,
		CollisionLODRingQuadSizes,
		CollisionLODRingMaxSamplesPerAxis,
		XCoordinates);
	GPUTessellationBuildCollisionRingAxisCoordinates(
		-HalfSizeY,
		HalfSizeY,
		(float)LocalCameraPosition.Y,
		(float)Scale3D.Y,
		CollisionLODRingDistances,
		CollisionLODRingQuadSizes,
		CollisionLODRingMaxSamplesPerAxis,
		YCoordinates);

	if (XCoordinates.Num() < 2 || YCoordinates.Num() < 2)
	{
		return false;
	}

	TArray<int32> XSourceIndices;
	TArray<int32> YSourceIndices;
	GPUTessellationConvertCollisionRingCoordinatesToSourceIndices(
		XCoordinates,
		-HalfSizeX,
		HalfSizeX,
		CollisionLODRingSourceResolutionX,
		XSourceIndices);
	GPUTessellationConvertCollisionRingCoordinatesToSourceIndices(
		YCoordinates,
		-HalfSizeY,
		HalfSizeY,
		CollisionLODRingSourceResolutionY,
		YSourceIndices);

	if (XSourceIndices.Num() < 2 || YSourceIndices.Num() < 2)
	{
		return false;
	}

	CollisionMeshVertices.Reserve(XSourceIndices.Num() * YSourceIndices.Num());
	CollisionMeshIndices.Reserve((XSourceIndices.Num() - 1) * (YSourceIndices.Num() - 1) * 6);

	for (int32 SourceY : YSourceIndices)
	{
		for (int32 SourceX : XSourceIndices)
		{
			const int32 SourceIndex = SourceY * CollisionLODRingSourceResolutionX + SourceX;
			if (!CollisionLODRingSourceVertices.IsValidIndex(SourceIndex))
			{
				CollisionMeshVertices.Reset();
				CollisionMeshIndices.Reset();
				return false;
			}

			CollisionMeshVertices.Add(CollisionLODRingSourceVertices[SourceIndex]);
		}
	}

	const int32 VertexCountX = XSourceIndices.Num();
	for (int32 YIndex = 0; YIndex < YSourceIndices.Num() - 1; ++YIndex)
	{
		for (int32 XIndex = 0; XIndex < XSourceIndices.Num() - 1; ++XIndex)
		{
			const int32 Vertex00 = YIndex * VertexCountX + XIndex;
			const int32 Vertex10 = Vertex00 + 1;
			const int32 Vertex01 = Vertex00 + VertexCountX;
			const int32 Vertex11 = Vertex01 + 1;

			CollisionMeshIndices.Add(Vertex00);
			CollisionMeshIndices.Add(Vertex10);
			CollisionMeshIndices.Add(Vertex11);
			CollisionMeshIndices.Add(Vertex00);
			CollisionMeshIndices.Add(Vertex11);
			CollisionMeshIndices.Add(Vertex01);
		}
	}

	return CollisionMeshVertices.Num() >= 3 && CollisionMeshIndices.Num() >= 3;
}

bool UGPUTessellationComponent::BuildCollisionLODRingsMeshData()
{
	CollisionMeshVertices.Reset();
	CollisionMeshIndices.Reset();

	if (!IsCollisionLODRingsMeshEnabled())
	{
		return false;
	}

	return BuildCollisionLODRingsMeshDataFromSourceCache();
}

bool UGPUTessellationComponent::BuildGPUReadbackCollisionMeshData(int32 RequestedTessellationFactor)
{
	CollisionMeshVertices.Reset();
	CollisionMeshIndices.Reset();

	if (!IsPhysicsCollisionMeshEnabled())
	{
		return false;
	}

	const int32 EffectiveTessellationFactor = GetEffectiveVertexPerfectCollisionTessellationFactor(RequestedTessellationFactor);
	FGPUTessellationSettings CollisionSettings = GetEffectiveTessellationSettings();
	CollisionSettings.LODMode = EGPUTessellationLODMode::Disabled;
	CollisionSettings.TessellationFactor = EffectiveTessellationFactor;
	CollisionSettings.UVOffset = FVector2f(0.0f, 0.0f);
	CollisionSettings.UVScale = FVector2f(1.0f, 1.0f);

	return AppendGPUReadbackCollisionMeshData(
		CollisionSettings,
		LastCameraPosition,
		FIntVector4(1, 1, 1, 1),
		GetVertexPerfectCollisionReadbackMaxResolution(EffectiveTessellationFactor));
}

int32 UGPUTessellationComponent::GetEffectiveVertexPerfectCollisionTessellationFactor(int32 BaseTessellationFactor) const
{
	return FMath::Clamp(BaseTessellationFactor, 1, 1024);
}

int32 UGPUTessellationComponent::GetVertexPerfectCollisionReadbackMaxResolution(int32 TessellationFactor) const
{
	const int32 ClampedFactor = FMath::Clamp(TessellationFactor, 1, 1024);
	const int32 DesiredResolution = ClampedFactor * 4 + 1;
	return FMath::Clamp(DesiredResolution, 2048, 4097);
}

bool UGPUTessellationComponent::IsPatchBasedLODMode(EGPUTessellationLODMode LODMode) const
{
	return LODMode == EGPUTessellationLODMode::DistanceBasedPatches ||
		LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree;
}

int32 UGPUTessellationComponent::CalculateFullPatchMeshTessellationFactor(const FGPUTessellationSettings& EffectiveSettings, int32 FullMeshCapValue) const
{
	if (!IsPatchBasedLODMode(EffectiveSettings.LODMode))
	{
		return GetEffectiveVertexPerfectCollisionTessellationFactor(EffectiveSettings.TessellationFactor);
	}

	int32 HighestPatchTessellation = 16;
	int64 PatchAxisMultiplier = 1;
	if (EffectiveSettings.LODMode == EGPUTessellationLODMode::DistanceBasedPatches)
	{
		HighestPatchTessellation = GPUTessellationHighestPatchLevelTessellation(EffectiveSettings.PatchLevels, 16);
		const int32 PatchCountX = FMath::Clamp(EffectiveSettings.PatchCountX, 1, 32);
		const int32 PatchCountY = FMath::Clamp(EffectiveSettings.PatchCountY, 1, 32);
		PatchAxisMultiplier = FMath::Max(PatchCountX, PatchCountY);
	}
	else
	{
		HighestPatchTessellation = GPUTessellationHighestPatchLevelTessellation(EffectiveSettings.QuadtreeLevels, 16);
		const int32 RootTileCountX = FMath::Clamp(EffectiveSettings.QuadtreeRootTileCountX, 1, 8);
		const int32 RootTileCountY = FMath::Clamp(EffectiveSettings.QuadtreeRootTileCountY, 1, 8);
		const int32 MaxDepth = FMath::Clamp(EffectiveSettings.QuadtreeMaxDepth, 0, 8);
		const int64 LeafScale = 1LL << MaxDepth;
		PatchAxisMultiplier = FMath::Max((int64)RootTileCountX * LeafScale, (int64)RootTileCountY * LeafScale);
	}

	const int32 EffectivePatchTessellation = ApplyGeometrySubdivisionMultiplier(HighestPatchTessellation);
	const int32 FullMeshCap = FMath::Clamp(FullMeshCapValue, 1, 1024);
	const int64 DesiredFullMeshFactor = (int64)EffectivePatchTessellation * FMath::Max(PatchAxisMultiplier, (int64)1);
	const int32 FullMeshFactor = (int32)FMath::Clamp(DesiredFullMeshFactor, (int64)1, (int64)FullMeshCap);
	return FullMeshFactor;
}

int32 UGPUTessellationComponent::CalculateFullPatchMeshCollisionTessellationFactor(const FGPUTessellationSettings& EffectiveSettings) const
{
	return CalculateFullPatchMeshTessellationFactor(EffectiveSettings, VertexPerfectCollisionFullPatchMeshTessellationCap);
}

bool UGPUTessellationComponent::BuildFullPatchMeshVertexPerfectCollisionMeshData(const FGPUTessellationSettings& EffectiveSettings)
{
	if (!IsPatchBasedLODMode(EffectiveSettings.LODMode))
	{
		return false;
	}

	const int32 FullMeshTessellationFactor = CalculateFullPatchMeshCollisionTessellationFactor(EffectiveSettings);
	if (bEnableDebugLogging)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("GPUTessellation: Baking full patch collision as one mesh with tessellation factor %d."),
			FullMeshTessellationFactor);
	}

	return BuildGPUReadbackCollisionMeshData(FullMeshTessellationFactor);
}

bool UGPUTessellationComponent::AppendGPUReadbackCollisionMeshData(
	const FGPUTessellationSettings& CollisionSettings,
	const FVector& CameraPosition,
	const FIntVector4& EdgeCollapseFactors,
	int32 MaxResolutionOverride)
{
	FGPUTessellatedMeshData ReadbackMeshData;
	FGPUTessellationMeshBuilder MeshBuilder;
	MeshBuilder.GenerateMeshSync(
		CollisionSettings,
		GetComponentTransform().ToMatrixWithScale(),
		CameraPosition,
		DisplacementTexture,
		SubtractTexture,
		ReadbackMeshData,
		EdgeCollapseFactors,
		MaxResolutionOverride,
		GetVectorDisplacementTexture());

	if (!ReadbackMeshData.IsValid())
	{
		return false;
	}

	if ((int64)CollisionMeshVertices.Num() + (int64)ReadbackMeshData.Vertices.Num() > (int64)TNumericLimits<int32>::Max())
	{
		return false;
	}

	const int32 VertexBase = CollisionMeshVertices.Num();
	TArray<FVector> NewVertices;
	NewVertices.Reserve(ReadbackMeshData.Vertices.Num());
	for (const FVector3f& Vertex : ReadbackMeshData.Vertices)
	{
		NewVertices.Add(FVector(Vertex));
	}

	TArray<int32> NewIndices;
	NewIndices.Reserve(ReadbackMeshData.Indices.Num());
	for (uint32 Index : ReadbackMeshData.Indices)
	{
		if (Index > (uint32)TNumericLimits<int32>::Max() || VertexBase > TNumericLimits<int32>::Max() - (int32)Index)
		{
			return false;
		}
		NewIndices.Add(VertexBase + (int32)Index);
	}

	CollisionMeshVertices.Reserve(CollisionMeshVertices.Num() + NewVertices.Num());
	CollisionMeshIndices.Reserve(CollisionMeshIndices.Num() + NewIndices.Num());
	CollisionMeshVertices.Append(NewVertices);
	CollisionMeshIndices.Append(NewIndices);

	return ReadbackMeshData.Vertices.Num() >= 3 && ReadbackMeshData.Indices.Num() >= 3;
}

#if WITH_EDITOR
bool UGPUTessellationComponent::AppendGPUReadbackBakeMeshData(
	FGPUTessellatedMeshData& InOutMeshData,
	const FGPUTessellationSettings& BakeSettings,
	const FVector& CameraPosition,
	const FIntVector4& EdgeCollapseFactors,
	int32 MaxResolutionOverride) const
{
	FGPUTessellatedMeshData ReadbackMeshData;
	FGPUTessellationMeshBuilder MeshBuilder;
	MeshBuilder.GenerateMeshSync(
		BakeSettings,
		GetComponentTransform().ToMatrixWithScale(),
		CameraPosition,
		DisplacementTexture,
		SubtractTexture,
		ReadbackMeshData,
		EdgeCollapseFactors,
		MaxResolutionOverride,
		GetVectorDisplacementTexture());

	if (!ReadbackMeshData.IsValid())
	{
		return false;
	}

	if ((int64)InOutMeshData.Vertices.Num() + (int64)ReadbackMeshData.Vertices.Num() > (int64)TNumericLimits<int32>::Max())
	{
		return false;
	}

	const int32 VertexBase = InOutMeshData.Vertices.Num();
	InOutMeshData.Vertices.Reserve(InOutMeshData.Vertices.Num() + ReadbackMeshData.Vertices.Num());
	InOutMeshData.Normals.Reserve(InOutMeshData.Normals.Num() + ReadbackMeshData.Normals.Num());
	InOutMeshData.UVs.Reserve(InOutMeshData.UVs.Num() + ReadbackMeshData.UVs.Num());
	InOutMeshData.Indices.Reserve(InOutMeshData.Indices.Num() + ReadbackMeshData.Indices.Num());

	InOutMeshData.Vertices.Append(ReadbackMeshData.Vertices);
	InOutMeshData.Normals.Append(ReadbackMeshData.Normals);
	InOutMeshData.UVs.Append(ReadbackMeshData.UVs);

	for (uint32 Index : ReadbackMeshData.Indices)
	{
		if (Index > (uint32)TNumericLimits<int32>::Max() || VertexBase > TNumericLimits<int32>::Max() - (int32)Index)
		{
			return false;
		}
		InOutMeshData.Indices.Add((uint32)(VertexBase + (int32)Index));
	}

	if (InOutMeshData.ResolutionX == 0 && InOutMeshData.ResolutionY == 0)
	{
		InOutMeshData.ResolutionX = ReadbackMeshData.ResolutionX;
		InOutMeshData.ResolutionY = ReadbackMeshData.ResolutionY;
	}

	return InOutMeshData.Vertices.Num() >= 3 && InOutMeshData.Indices.Num() >= 3;
}

bool UGPUTessellationComponent::BuildBakeMeshData(FGPUTessellatedMeshData& OutMeshData) const
{
	OutMeshData.Reset();

	FVector CameraPosition = LastCameraPosition;
	if (!GetCollisionLODRingCameraPosition(CameraPosition))
	{
		CameraPosition = GetComponentLocation();
	}

	FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
	const EGPUTessellationLODMode LODMode = EffectiveSettings.LODMode;

	if (bBakeMeshFullPatchMesh && IsPatchBasedLODMode(LODMode))
	{
		const int32 FullMeshTessellationFactor = CalculateFullPatchMeshTessellationFactor(EffectiveSettings, BakeFullPatchMeshTessellationCap);
		FGPUTessellationSettings BakeSettings = EffectiveSettings;
		BakeSettings.LODMode = EGPUTessellationLODMode::Disabled;
		BakeSettings.TessellationFactor = FullMeshTessellationFactor;
		BakeSettings.UVOffset = FVector2f::ZeroVector;
		BakeSettings.UVScale = FVector2f(1.0f, 1.0f);

		return AppendGPUReadbackBakeMeshData(
			OutMeshData,
			BakeSettings,
			CameraPosition,
			FIntVector4(1, 1, 1, 1),
			GetVertexPerfectCollisionReadbackMaxResolution(FullMeshTessellationFactor));
	}

	if (bBakeMeshUseCurrentVisualLOD && (LODMode == EGPUTessellationLODMode::DistanceBasedPatches || LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree))
	{
		TArray<FGPUTessellationPatchInfo> PatchState;
		FGPUTessellationMeshBuilder MeshBuilder;
		const FMatrix LocalToWorld = GetComponentTransform().ToMatrixWithScale();
		if (LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree)
		{
			MeshBuilder.CalculateQuadtreePatchState(
				EffectiveSettings,
				LocalToWorld,
				CameraPosition,
				nullptr,
				PatchState);
		}
		else
		{
			MeshBuilder.CalculatePatchState(
				EffectiveSettings,
				LocalToWorld,
				CameraPosition,
				nullptr,
				EffectiveSettings.PatchCountX,
				EffectiveSettings.PatchCountY,
				PatchState);
		}

		if (PatchState.Num() == 0)
		{
			return false;
		}

		for (const FGPUTessellationPatchInfo& Patch : PatchState)
		{
			const int32 EffectivePatchTessellationFactor = FMath::Clamp(Patch.TessellationLevel, 1, 1024);
			FGPUTessellationSettings PatchSettings = EffectiveSettings;
			PatchSettings.LODMode = EGPUTessellationLODMode::Disabled;
			PatchSettings.TessellationFactor = EffectivePatchTessellationFactor;
			PatchSettings.UVOffset = Patch.PatchOffset;
			PatchSettings.UVScale = Patch.PatchSize;

			if (!AppendGPUReadbackBakeMeshData(
				OutMeshData,
				PatchSettings,
				CameraPosition,
				Patch.EdgeCollapseFactors,
				GetVertexPerfectCollisionReadbackMaxResolution(EffectivePatchTessellationFactor)))
			{
				OutMeshData.Reset();
				return false;
			}
		}

		return OutMeshData.IsValid();
	}

	int32 BakeTessellationFactor = EffectiveSettings.TessellationFactor;
	if (bBakeMeshUseCurrentVisualLOD && LODMode != EGPUTessellationLODMode::Disabled)
	{
		BakeTessellationFactor = LastAppliedTessFactor > 0 ? LastAppliedTessFactor : EffectiveSettings.TessellationFactor;
	}
	else if (LODMode != EGPUTessellationLODMode::Disabled)
	{
		BakeTessellationFactor = ApplyGeometrySubdivisionMultiplier(TessellationSettings.TessellationFactor);
	}

	FGPUTessellationSettings BakeSettings = EffectiveSettings;
	BakeSettings.LODMode = EGPUTessellationLODMode::Disabled;
	BakeSettings.TessellationFactor = FMath::Clamp(BakeTessellationFactor, 1, 1024);
	BakeSettings.UVOffset = FVector2f::ZeroVector;
	BakeSettings.UVScale = FVector2f(1.0f, 1.0f);

	return AppendGPUReadbackBakeMeshData(
		OutMeshData,
		BakeSettings,
		CameraPosition,
		FIntVector4(1, 1, 1, 1),
		GetVertexPerfectCollisionReadbackMaxResolution(BakeSettings.TessellationFactor));
}
#endif

bool UGPUTessellationComponent::BuildVisualLODMatchedVertexPerfectCollisionMeshData()
{
	CollisionMeshVertices.Reset();
	CollisionMeshIndices.Reset();

	if (!IsVertexPerfectCollisionMeshEnabled())
	{
		return false;
	}

	FVector CameraPosition = LastCameraPosition;
	if (!GetCollisionLODRingCameraPosition(CameraPosition))
	{
		CameraPosition = GetComponentLocation();
	}

	FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
	const EGPUTessellationLODMode LODMode = EffectiveSettings.LODMode;
	if (LODMode == EGPUTessellationLODMode::DistanceBasedPatches || LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree)
	{
		TArray<FGPUTessellationPatchInfo> PatchState;
		FGPUTessellationMeshBuilder MeshBuilder;
		const FMatrix LocalToWorld = GetComponentTransform().ToMatrixWithScale();
		if (LODMode == EGPUTessellationLODMode::DistanceBasedQuadtree)
		{
			MeshBuilder.CalculateQuadtreePatchState(
				EffectiveSettings,
				LocalToWorld,
				CameraPosition,
				nullptr,
				PatchState);
		}
		else
		{
			MeshBuilder.CalculatePatchState(
				EffectiveSettings,
				LocalToWorld,
				CameraPosition,
				nullptr,
				EffectiveSettings.PatchCountX,
				EffectiveSettings.PatchCountY,
				PatchState);
		}

		if (PatchState.Num() == 0)
		{
			return false;
		}

		for (const FGPUTessellationPatchInfo& Patch : PatchState)
		{
			const int32 EffectivePatchTessellationFactor = GetEffectiveVertexPerfectCollisionTessellationFactor(Patch.TessellationLevel);
			FGPUTessellationSettings PatchSettings = EffectiveSettings;
			PatchSettings.LODMode = EGPUTessellationLODMode::Disabled;
			PatchSettings.TessellationFactor = EffectivePatchTessellationFactor;
			PatchSettings.UVOffset = Patch.PatchOffset;
			PatchSettings.UVScale = Patch.PatchSize;

			if (!AppendGPUReadbackCollisionMeshData(
				PatchSettings,
				CameraPosition,
				Patch.EdgeCollapseFactors,
				GetVertexPerfectCollisionReadbackMaxResolution(EffectivePatchTessellationFactor)))
			{
				CollisionMeshVertices.Reset();
				CollisionMeshIndices.Reset();
				return false;
			}
		}

		return CollisionMeshVertices.Num() >= 3 && CollisionMeshIndices.Num() >= 3;
	}

	int32 VisualTessellationFactor = EffectiveSettings.TessellationFactor;
	if (LODMode != EGPUTessellationLODMode::Disabled)
	{
		VisualTessellationFactor = LastAppliedTessFactor > 0 ? LastAppliedTessFactor : EffectiveSettings.TessellationFactor;
	}

	return BuildGPUReadbackCollisionMeshData(FMath::Clamp(VisualTessellationFactor, 1, 1024));
}

uint32 UGPUTessellationComponent::CalculateVertexPerfectCollisionVisualLODSignature() const
{
	if (!IsVertexPerfectCollisionMeshEnabled() || !bVertexPerfectCollisionMatchVisualLOD)
	{
		return 0;
	}

	const FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
	uint32 Signature = 2166136261u;
	Signature = HashCombine(Signature, GetTypeHash((uint8)EffectiveSettings.LODMode));
	const bool bUseFullPatchMeshCollision =
		bVertexPerfectCollisionBakeFullPatchMesh &&
		IsPatchBasedLODMode(EffectiveSettings.LODMode);
	Signature = HashCombine(Signature, GetTypeHash(bUseFullPatchMeshCollision ? 1 : 0));

	switch (EffectiveSettings.LODMode)
	{
		case EGPUTessellationLODMode::DistanceBasedPatches:
			if (bUseFullPatchMeshCollision)
			{
				Signature = HashCombine(Signature, GetTypeHash(CalculateFullPatchMeshCollisionTessellationFactor(EffectiveSettings)));
			}
			else
			{
				Signature = HashCombine(Signature, GetTypeHash(EffectiveSettings.PatchCountX));
				Signature = HashCombine(Signature, GetTypeHash(EffectiveSettings.PatchCountY));
				Signature = HashCombine(Signature, LastPatchTopologySignature);
			}
			break;

		case EGPUTessellationLODMode::DistanceBasedQuadtree:
			if (bUseFullPatchMeshCollision)
			{
				Signature = HashCombine(Signature, GetTypeHash(CalculateFullPatchMeshCollisionTessellationFactor(EffectiveSettings)));
			}
			else
			{
				Signature = HashCombine(Signature, GetTypeHash(EffectiveSettings.QuadtreeRootTileCountX));
				Signature = HashCombine(Signature, GetTypeHash(EffectiveSettings.QuadtreeRootTileCountY));
				Signature = HashCombine(Signature, GetTypeHash(EffectiveSettings.QuadtreeMaxDepth));
				Signature = HashCombine(Signature, GetTypeHash(EffectiveSettings.QuadtreeMaxVisibleLeaves));
				Signature = HashCombine(Signature, LastPatchTopologySignature);
			}
			break;

		case EGPUTessellationLODMode::Disabled:
			Signature = HashCombine(Signature, GetTypeHash(GetEffectiveVertexPerfectCollisionTessellationFactor(EffectiveSettings.TessellationFactor)));
			break;

		case EGPUTessellationLODMode::DistanceBased:
		case EGPUTessellationLODMode::DistanceBasedDiscrete:
		case EGPUTessellationLODMode::DensityTexture:
		default:
			Signature = HashCombine(Signature, GetTypeHash(GetEffectiveVertexPerfectCollisionTessellationFactor(LastAppliedTessFactor)));
			break;
	}

	Signature = HashCombine(Signature, GetTypeHash(EffectiveSettings.bSubdivideHardEdges ? 1 : 0));
	Signature = HashCombine(Signature, GetTypeHash(FMath::Clamp(EffectiveSettings.SubdivisionMultiplier, 2, 8)));
	return Signature;
}

bool UGPUTessellationComponent::BuildVertexPerfectCollisionMeshData()
{
	bLastVertexPerfectCollisionBuildUsedMatchedVisualLOD = false;

	if (!IsVertexPerfectCollisionMeshEnabled())
	{
		CollisionMeshVertices.Reset();
		CollisionMeshIndices.Reset();
		return false;
	}

	const FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
	if (bVertexPerfectCollisionBakeFullPatchMesh && IsPatchBasedLODMode(EffectiveSettings.LODMode))
	{
		if (BuildFullPatchMeshVertexPerfectCollisionMeshData(EffectiveSettings))
		{
			bLastVertexPerfectCollisionBuildUsedMatchedVisualLOD = bVertexPerfectCollisionMatchVisualLOD;
			return true;
		}

		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Full patch collision bake failed; falling back to matched patch readback or manual VertexPerfectCollisionTessellationFactor."));
		}
	}

	if (bVertexPerfectCollisionMatchVisualLOD)
	{
		if (BuildVisualLODMatchedVertexPerfectCollisionMeshData())
		{
			bLastVertexPerfectCollisionBuildUsedMatchedVisualLOD = true;
			return true;
		}

		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Match Visual LOD collision readback failed; falling back to VertexPerfectCollisionTessellationFactor."));
		}
	}

	return BuildGPUReadbackCollisionMeshData(FMath::Clamp(VertexPerfectCollisionTessellationFactor, 1, 1024));
}

bool UGPUTessellationComponent::BuildCollisionMeshData()
{
	bool bBuiltCollisionMesh = false;
	if (IsVertexPerfectCollisionMeshEnabled())
	{
		bBuiltCollisionMesh = BuildVertexPerfectCollisionMeshData();
	}
	else if (IsCollisionLODRingsMeshEnabled())
	{
		bBuiltCollisionMesh = BuildCollisionLODRingsMeshData();
	}
	else
	{
		bBuiltCollisionMesh = BuildCoarseCollisionMeshData();
	}

	if (bBuiltCollisionMesh)
	{
		FinalizeCollisionMeshData();
	}

	return CollisionMeshVertices.Num() >= 3 && CollisionMeshIndices.Num() >= 3;
}

void UGPUTessellationComponent::NormalizeCollisionMeshTriangles()
{
	TArray<int32> NormalizedIndices;
	NormalizedIndices.Reserve(CollisionMeshIndices.Num());

	const int32 TriangleCount = CollisionMeshIndices.Num() / 3;
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		int32 Index0 = CollisionMeshIndices[TriangleIndex * 3 + 0];
		int32 Index1 = CollisionMeshIndices[TriangleIndex * 3 + 1];
		int32 Index2 = CollisionMeshIndices[TriangleIndex * 3 + 2];

		if (!CollisionMeshVertices.IsValidIndex(Index0) || !CollisionMeshVertices.IsValidIndex(Index1) || !CollisionMeshVertices.IsValidIndex(Index2))
		{
			continue;
		}

		const FVector& Vertex0 = CollisionMeshVertices[Index0];
		const FVector& Vertex1 = CollisionMeshVertices[Index1];
		const FVector& Vertex2 = CollisionMeshVertices[Index2];
		if (Vertex0.ContainsNaN() || Vertex1.ContainsNaN() || Vertex2.ContainsNaN())
		{
			continue;
		}

		const FVector TriangleNormal = FVector::CrossProduct(Vertex1 - Vertex0, Vertex2 - Vertex0);
		if (TriangleNormal.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}

		if (TriangleNormal.Z < 0.0)
		{
			Swap(Index1, Index2);
		}

		NormalizedIndices.Add(Index0);
		NormalizedIndices.Add(Index1);
		NormalizedIndices.Add(Index2);
	}

	CollisionMeshIndices = MoveTemp(NormalizedIndices);
}

void UGPUTessellationComponent::ApplyCollisionMeshThickness()
{
	const float Thickness = FMath::Max(0.0f, CollisionThickness);
	if (Thickness <= KINDA_SMALL_NUMBER || CollisionMeshVertices.Num() < 3 || CollisionMeshIndices.Num() < 3)
	{
		return;
	}

	const int32 TopVertexCount = CollisionMeshVertices.Num();
	const int32 TopIndexCount = CollisionMeshIndices.Num();
	CollisionMeshVertices.Reserve(TopVertexCount * 2);
	CollisionMeshIndices.Reserve(TopIndexCount * 2);

	for (int32 VertexIndex = 0; VertexIndex < TopVertexCount; ++VertexIndex)
	{
		CollisionMeshVertices.Add(CollisionMeshVertices[VertexIndex] - FVector(0.0, 0.0, Thickness));
	}

	for (int32 Index = 0; Index + 2 < TopIndexCount; Index += 3)
	{
		const int32 Index0 = CollisionMeshIndices[Index + 0] + TopVertexCount;
		const int32 Index1 = CollisionMeshIndices[Index + 1] + TopVertexCount;
		const int32 Index2 = CollisionMeshIndices[Index + 2] + TopVertexCount;

		CollisionMeshIndices.Add(Index0);
		CollisionMeshIndices.Add(Index2);
		CollisionMeshIndices.Add(Index1);
	}
}

void UGPUTessellationComponent::FinalizeCollisionMeshData()
{
	NormalizeCollisionMeshTriangles();
	ApplyCollisionMeshThickness();
}

bool UGPUTessellationComponent::HasRenderTargetInputs() const
{
	return (DisplacementTexture && DisplacementTexture->IsA<UTextureRenderTarget>()) ||
		(GetVectorDisplacementTexture() && GetVectorDisplacementTexture()->IsA<UTextureRenderTarget>()) ||
		(SubtractTexture && SubtractTexture->IsA<UTextureRenderTarget>()) ||
		(NormalMapTexture && NormalMapTexture->IsA<UTextureRenderTarget>());
}

double UGPUTessellationComponent::GetRenderTargetInputsLastRenderTime() const
{
	double LastRenderTime = -TNumericLimits<double>::Max();
	LastRenderTime = FMath::Max(LastRenderTime, GetGPUTessellationRenderTargetLastRenderTime(DisplacementTexture));
	LastRenderTime = FMath::Max(LastRenderTime, GetGPUTessellationRenderTargetLastRenderTime(GetVectorDisplacementTexture()));
	LastRenderTime = FMath::Max(LastRenderTime, GetGPUTessellationRenderTargetLastRenderTime(SubtractTexture));
	LastRenderTime = FMath::Max(LastRenderTime, GetGPUTessellationRenderTargetLastRenderTime(NormalMapTexture));
	return LastRenderTime;
}

bool UGPUTessellationComponent::ShouldUpdateRenderTargetDrivenResources()
{
	if (!bAutoUpdateRenderTargets || !HasRenderTargetInputs())
	{
		return false;
	}

	double CurrentRenderTargetRenderTime = -TNumericLimits<double>::Max();
	if (bAutoDetectRenderTargetChanges)
	{
		CurrentRenderTargetRenderTime = GetRenderTargetInputsLastRenderTime();
		if (CurrentRenderTargetRenderTime <= LastObservedRenderTargetRenderTime + UE_SMALL_NUMBER)
		{
			return false;
		}
	}

	if (RenderTargetUpdateFPS > 0)
	{
		const double CurrentTime = FPlatformTime::Seconds();
		const double MinTimeBetweenUpdates = 1.0 / static_cast<double>(RenderTargetUpdateFPS);
		if (CurrentTime - LastRenderTargetUpdateTime < MinTimeBetweenUpdates)
		{
			return false;
		}
		LastRenderTargetUpdateTime = CurrentTime;
	}

	if (bAutoDetectRenderTargetChanges)
	{
		LastObservedRenderTargetRenderTime = CurrentRenderTargetRenderTime;
	}

	return true;
}

void UGPUTessellationComponent::MarkWaterSurfaceReadbackDirty()
{
	bWaterSurfaceReadbackDirty = true;
}

bool UGPUTessellationComponent::ShouldUseWaterSurfaceReadbackCache() const
{
	if (!bEnableWaterInteraction || !bWaterUseGPUSurfaceReadback)
	{
		return false;
	}

	const FGPUTessellationSettings EffectiveSettings = GetEffectiveTessellationSettings();
	return EffectiveSettings.OceanSettings.WaveMode == EGPUOceanWaveMode::FFT ||
		DisplacementTexture != nullptr ||
		GetVectorDisplacementTexture() != nullptr ||
		SubtractTexture != nullptr;
}

void UGPUTessellationComponent::UpdateWaterSurfaceReadbackCache()
{
	if (!ShouldUseWaterSurfaceReadbackCache())
	{
		WaterSurfaceCacheVertices.Reset();
		WaterSurfaceCacheIndices.Reset();
		LastWaterSurfaceReadbackTime = -1.0;
		bWaterSurfaceReadbackDirty = true;
		return;
	}

	const double CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : FPlatformTime::Seconds();
	const float UpdateRate = FMath::Max(0.0f, WaterSurfaceReadbackUpdateRate);
	const bool bDueByRate = UpdateRate <= 0.0f || LastWaterSurfaceReadbackTime < 0.0 || CurrentTime - LastWaterSurfaceReadbackTime >= 1.0 / (double)UpdateRate;
	if (!bWaterSurfaceReadbackDirty && !bDueByRate)
	{
		return;
	}

	LastWaterSurfaceReadbackTime = CurrentTime;
	if (BuildWaterSurfaceReadbackCache())
	{
		bWaterSurfaceReadbackDirty = false;
	}
}

bool UGPUTessellationComponent::BuildWaterSurfaceReadbackCache()
{
	FGPUTessellationSettings ReadbackSettings = GetEffectiveTessellationSettings();
	ReadbackSettings.TessellationFactor = FMath::Clamp(WaterSurfaceReadbackTessellationFactor, 1, 256);
	ReadbackSettings.UVOffset = FVector2f(0.0f, 0.0f);
	ReadbackSettings.UVScale = FVector2f(1.0f, 1.0f);

	FGPUTessellatedMeshData ReadbackMeshData;
	FGPUTessellationMeshBuilder MeshBuilder;
	MeshBuilder.GenerateMeshSync(
		ReadbackSettings,
		GetComponentTransform().ToMatrixWithScale(),
		LastCameraPosition,
		DisplacementTexture,
		SubtractTexture,
		ReadbackMeshData,
		FIntVector4(1, 1, 1, 1),
		2048,
		GetVectorDisplacementTexture());

	if (!ReadbackMeshData.IsValid())
	{
		return false;
	}

	TArray<FVector> NewVertices;
	NewVertices.Reserve(ReadbackMeshData.Vertices.Num());
	for (const FVector3f& Vertex : ReadbackMeshData.Vertices)
	{
		NewVertices.Add(FVector(Vertex));
	}

	TArray<int32> NewIndices;
	NewIndices.Reserve(ReadbackMeshData.Indices.Num());
	for (uint32 Index : ReadbackMeshData.Indices)
	{
		if (Index > (uint32)TNumericLimits<int32>::Max())
		{
			return false;
		}
		NewIndices.Add((int32)Index);
	}

	WaterSurfaceCacheVertices = MoveTemp(NewVertices);
	WaterSurfaceCacheIndices = MoveTemp(NewIndices);
	return WaterSurfaceCacheVertices.Num() >= 3 && WaterSurfaceCacheIndices.Num() >= 3;
}

bool UGPUTessellationComponent::SampleCachedMeshHeightAtWorldPosition(const TArray<FVector>& MeshVertices, const TArray<int32>& MeshIndices, const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const
{
	if (MeshVertices.Num() < 3 || MeshIndices.Num() < 3)
	{
		return false;
	}

	FVector2D SurfaceUV;
	FVector LocalPosition;
	if (!ProjectWorldToTessellationUV(WorldPosition, SurfaceUV, LocalPosition))
	{
		return false;
	}

	const FVector2D LocalXY(LocalPosition.X, LocalPosition.Y);
	const int32 TriangleCount = MeshIndices.Num() / 3;
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		const int32 Index0 = MeshIndices[TriangleIndex * 3 + 0];
		const int32 Index1 = MeshIndices[TriangleIndex * 3 + 1];
		const int32 Index2 = MeshIndices[TriangleIndex * 3 + 2];
		if (!MeshVertices.IsValidIndex(Index0) || !MeshVertices.IsValidIndex(Index1) || !MeshVertices.IsValidIndex(Index2))
		{
			continue;
		}

		const FVector& Vertex0 = MeshVertices[Index0];
		const FVector& Vertex1 = MeshVertices[Index1];
		const FVector& Vertex2 = MeshVertices[Index2];
		const FVector LocalNormalUnnormalized = FVector::CrossProduct(Vertex1 - Vertex0, Vertex2 - Vertex0);
		if (LocalNormalUnnormalized.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}

		FVector Barycentric;
		if (!GPUTessellationComputeBarycentric2D(
			LocalXY,
			FVector2D(Vertex0.X, Vertex0.Y),
			FVector2D(Vertex1.X, Vertex1.Y),
			FVector2D(Vertex2.X, Vertex2.Y),
			Barycentric))
		{
			continue;
		}

		const double LocalHeight = (Vertex0.Z * Barycentric.X) + (Vertex1.Z * Barycentric.Y) + (Vertex2.Z * Barycentric.Z);
		OutWorldPosition = GetComponentTransform().TransformPosition(FVector(LocalPosition.X, LocalPosition.Y, LocalHeight));
		OutWorldHeight = (float)OutWorldPosition.Z;

		FVector LocalNormal = LocalNormalUnnormalized.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		if (FVector::DotProduct(LocalNormal, FVector::UpVector) < 0.0)
		{
			LocalNormal *= -1.0;
		}
		const FMatrix NormalToWorld = GetComponentTransform().ToInverseMatrixWithScale().GetTransposed();
		OutWorldNormal = FVector(NormalToWorld.TransformVector(LocalNormal)).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		return true;
	}

	return false;
}

bool UGPUTessellationComponent::SampleCachedWaterSurfaceHeightAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const
{
	return SampleCachedMeshHeightAtWorldPosition(WaterSurfaceCacheVertices, WaterSurfaceCacheIndices, WorldPosition, OutWorldHeight, OutWorldPosition, OutWorldNormal);
}

bool UGPUTessellationComponent::SampleCachedCollisionMeshHeightAtWorldPosition(const FVector& WorldPosition, float& OutWorldHeight, FVector& OutWorldPosition, FVector& OutWorldNormal) const
{
	return SampleCachedMeshHeightAtWorldPosition(CollisionMeshVertices, CollisionMeshIndices, WorldPosition, OutWorldHeight, OutWorldPosition, OutWorldNormal);
}

bool UGPUTessellationComponent::GetWaterInteractionBox(FVector& OutCenter, FVector& OutExtent, FQuat& OutRotation) const
{
	const float PlaneSizeX = TessellationSettings.PlaneSizeX;
	const float PlaneSizeY = TessellationSettings.PlaneSizeY;
	const float Depth = FMath::Max(0.0f, WaterInteractionDepth);
	const float HeightAboveSurface = FMath::Max(0.0f, WaterInteractionHeightAboveSurface);
	const float TotalHeight = Depth + HeightAboveSurface;
	if (PlaneSizeX <= KINDA_SMALL_NUMBER || PlaneSizeY <= KINDA_SMALL_NUMBER || TotalHeight <= KINDA_SMALL_NUMBER)
	{
		OutCenter = FVector::ZeroVector;
		OutExtent = FVector::ZeroVector;
		OutRotation = FQuat::Identity;
		return false;
	}

	const FTransform ComponentTransform = GetComponentTransform();
	const FVector Scale = ComponentTransform.GetScale3D();
	const FVector AbsScale(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
	const FVector LocalCenter(0.0, 0.0, (HeightAboveSurface - Depth) * 0.5f);
	OutCenter = ComponentTransform.TransformPosition(LocalCenter);
	OutExtent = FVector(
		PlaneSizeX * 0.5f * FMath::Max(AbsScale.X, UE_SMALL_NUMBER),
		PlaneSizeY * 0.5f * FMath::Max(AbsScale.Y, UE_SMALL_NUMBER),
		TotalHeight * 0.5f * FMath::Max(AbsScale.Z, UE_SMALL_NUMBER));
	OutRotation = ComponentTransform.GetRotation();
	return true;
}

void UGPUTessellationComponent::UpdateWaterInteraction()
{
	if (!bEnableWaterInteraction || !bWaterApplyBuoyancyToPhysicsBodies)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	FVector VolumeCenter;
	FVector VolumeExtent;
	FQuat VolumeRotation;
	if (!GetWaterInteractionBox(VolumeCenter, VolumeExtent, VolumeRotation))
	{
		return;
	}

	FCollisionObjectQueryParams ObjectQueryParams(WaterInteractionObjectTypes);
	if (!ObjectQueryParams.IsValid())
	{
		ObjectQueryParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GPUTessellationWaterInteraction), false);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.AddIgnoredComponent(this);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, VolumeCenter, VolumeRotation, ObjectQueryParams, FCollisionShape::MakeBox(VolumeExtent), QueryParams);

	TSet<UPrimitiveComponent*> ProcessedComponents;
	TArray<UPrimitiveComponent*> ComponentsToApplyBuoyancy;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		UPrimitiveComponent* PrimitiveComponent = Overlap.GetComponent();
		if (!PrimitiveComponent || PrimitiveComponent == this || !PrimitiveComponent->IsSimulatingPhysics() || ProcessedComponents.Contains(PrimitiveComponent))
		{
			continue;
		}

		ProcessedComponents.Add(PrimitiveComponent);
		ComponentsToApplyBuoyancy.Add(PrimitiveComponent);
	}

	if (ComponentsToApplyBuoyancy.Num() == 0)
	{
		return;
	}

	UpdateWaterSurfaceReadbackCache();
	for (UPrimitiveComponent* PrimitiveComponent : ComponentsToApplyBuoyancy)
	{
		ApplyWaterBuoyancyToComponent(PrimitiveComponent);
	}
}

void UGPUTessellationComponent::ApplyWaterBuoyancyToComponent(UPrimitiveComponent* PrimitiveComponent) const
{
	if (!PrimitiveComponent || !PrimitiveComponent->IsSimulatingPhysics())
	{
		return;
	}

	const ECollisionEnabled::Type CollisionEnabled = PrimitiveComponent->GetCollisionEnabled();
	if (CollisionEnabled != ECollisionEnabled::QueryAndPhysics && CollisionEnabled != ECollisionEnabled::PhysicsOnly)
	{
		return;
	}

	TArray<FVector> SamplePoints;
	TArray<float> SampleRadii;
	BuildWaterBuoyancySamplePoints(PrimitiveComponent, SamplePoints, SampleRadii);
	if (SamplePoints.Num() == 0 || SamplePoints.Num() != SampleRadii.Num())
	{
		return;
	}

	UWorld* World = GetWorld();
	const float GravityMagnitude = FMath::Abs(World ? World->GetGravityZ() : -980.0f);
	const float BodyMass = FMath::Max(PrimitiveComponent->GetMass(), 1.0f);
	const float ForceShare = 1.0f / (float)SamplePoints.Num();
	const FVector WorldUp = FVector::UpVector;

	float SubmersionSum = 0.0f;
	int32 SubmergedSampleCount = 0;
	for (int32 SampleIndex = 0; SampleIndex < SamplePoints.Num(); ++SampleIndex)
	{
		const FVector& SamplePoint = SamplePoints[SampleIndex];
		const float SampleRadius = SampleRadii[SampleIndex];
		if (SampleRadius <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		float WaterHeight = 0.0f;
		FVector WaterSurfacePosition = FVector::ZeroVector;
		FVector WaterSurfaceNormal = FVector::UpVector;
		if (!SampleWaterSurfaceAtWorldPosition(SamplePoint, WaterHeight, WaterSurfacePosition, WaterSurfaceNormal))
		{
			continue;
		}

		const float PontoonBottomZ = (float)SamplePoint.Z - SampleRadius;
		const float ImmersionDepth = WaterHeight - PontoonBottomZ;
		if (ImmersionDepth <= 0.0f)
		{
			continue;
		}

		const float SubmersionAlpha = FMath::Clamp(ImmersionDepth / FMath::Max(SampleRadius * 2.0f, 1.0f), 0.0f, 1.0f);
		const FVector VelocityAtPoint = PrimitiveComponent->GetPhysicsLinearVelocityAtPoint(SamplePoint);
		const float VerticalVelocity = (float)FVector::DotProduct(VelocityAtPoint, WorldUp);
		const float BuoyantMagnitude = BodyMass * GravityMagnitude * FMath::Max(WaterBuoyancyStrength, 0.0f) * SubmersionAlpha * ForceShare;
		const float DampingMagnitude = BodyMass * FMath::Max(WaterBuoyancyDamping, 0.0f) * VerticalVelocity * SubmersionAlpha * ForceShare;

		FVector SafeWaterNormal = WaterSurfaceNormal.GetSafeNormal(UE_SMALL_NUMBER, WorldUp);
		if (FVector::DotProduct(SafeWaterNormal, WorldUp) < 0.0)
		{
			SafeWaterNormal *= -1.0;
		}

		const FVector PlanarVelocityAtPoint = VelocityAtPoint - (WorldUp * VerticalVelocity);
		const FVector PointDragForce = -PlanarVelocityAtPoint * BodyMass * FMath::Max(WaterPointDrag, 0.0f) * SubmersionAlpha * ForceShare;
		const FVector SurfaceSlopeForce = FVector(SafeWaterNormal.X, SafeWaterNormal.Y, 0.0) * BuoyantMagnitude * FMath::Max(WaterSurfaceNormalForce, 0.0f);
		FVector Force = (WorldUp * (BuoyantMagnitude - DampingMagnitude)) + PointDragForce + SurfaceSlopeForce;

		if (WaterMaxBuoyantForce > KINDA_SMALL_NUMBER)
		{
			Force = Force.GetClampedToMaxSize(WaterMaxBuoyantForce * ForceShare);
		}

		PrimitiveComponent->AddForceAtLocation(Force, SamplePoint);
		SubmersionSum += SubmersionAlpha;
		++SubmergedSampleCount;

		if (bShowWaterInteractionDebug)
		{
			const float Lifetime = 0.0f;
			const uint8 DepthPriority = 0;
			const float Thickness = FMath::Max(0.0f, WaterInteractionDebugLineThickness);
			DrawDebugSphere(GetWorld(), SamplePoint, SampleRadius, 12, FColor::Blue, false, Lifetime, DepthPriority, Thickness);
			DrawDebugLine(GetWorld(), SamplePoint, WaterSurfacePosition, FColor::Cyan, false, Lifetime, DepthPriority, Thickness);
			DrawDebugLine(GetWorld(), SamplePoint, SamplePoint + Force * 0.0005f, FColor::Green, false, Lifetime, DepthPriority, Thickness);
		}
	}

	if (SubmergedSampleCount <= 0)
	{
		return;
	}

	const float AverageSubmersion = SubmersionSum / (float)SamplePoints.Num();
	if (WaterLinearDrag > 0.0f)
	{
		const FVector LinearVelocity = PrimitiveComponent->GetPhysicsLinearVelocity();
		PrimitiveComponent->AddForce(-LinearVelocity * BodyMass * WaterLinearDrag * AverageSubmersion, NAME_None, false);
	}

	if (WaterAngularDrag > 0.0f)
	{
		const FVector AngularVelocity = PrimitiveComponent->GetPhysicsAngularVelocityInDegrees();
		PrimitiveComponent->AddTorqueInDegrees(-AngularVelocity * WaterAngularDrag * AverageSubmersion, NAME_None, true);
	}
}

void UGPUTessellationComponent::BuildWaterBuoyancySamplePoints(const UPrimitiveComponent* PrimitiveComponent, TArray<FVector>& OutSamplePoints, TArray<float>& OutSampleRadii) const
{
	OutSamplePoints.Reset();
	OutSampleRadii.Reset();
	if (!PrimitiveComponent)
	{
		return;
	}

	const FTransform ComponentTransform = PrimitiveComponent->GetComponentTransform();
	const FVector AbsScale(
		FMath::Max(FMath::Abs(ComponentTransform.GetScale3D().X), UE_SMALL_NUMBER),
		FMath::Max(FMath::Abs(ComponentTransform.GetScale3D().Y), UE_SMALL_NUMBER),
		FMath::Max(FMath::Abs(ComponentTransform.GetScale3D().Z), UE_SMALL_NUMBER));
	const FBoxSphereBounds LocalBounds = PrimitiveComponent->GetLocalBounds();
	const FVector LocalCenter = LocalBounds.Origin;
	const FVector LocalExtent = LocalBounds.BoxExtent;
	const FVector WorldExtent(LocalExtent.X * AbsScale.X, LocalExtent.Y * AbsScale.Y, LocalExtent.Z * AbsScale.Z);
	const float MinExtent = FMath::Min3((float)WorldExtent.X, (float)WorldExtent.Y, (float)WorldExtent.Z);
	const float DefaultRadius = WaterBuoyancySampleRadius > KINDA_SMALL_NUMBER
		? WaterBuoyancySampleRadius
		: FMath::Clamp(MinExtent * 0.45f, 10.0f, 500.0f);
	if (DefaultRadius <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const auto AddWorldSample = [&OutSamplePoints, &OutSampleRadii](const FVector& WorldPoint, float Radius)
	{
		if (Radius > KINDA_SMALL_NUMBER)
		{
			OutSamplePoints.Add(WorldPoint);
			OutSampleRadii.Add(Radius);
		}
	};

	if (WaterBuoyancySampleMode == EGPUTessellationWaterBuoyancySampleMode::CustomPontoons && WaterCustomPontoons.Num() > 0)
	{
		for (const FGPUTessellationWaterPontoon& Pontoon : WaterCustomPontoons)
		{
			if (!Pontoon.bEnabled)
			{
				continue;
			}

			AddWorldSample(ComponentTransform.TransformPosition(Pontoon.RelativeLocation), FMath::Max(Pontoon.Radius, 1.0f));
		}
		return;
	}

	const float LocalRadiusX = DefaultRadius / (float)AbsScale.X;
	const float LocalRadiusY = DefaultRadius / (float)AbsScale.Y;
	const float LocalRadiusZ = DefaultRadius / (float)AbsScale.Z;
	const float SampleZ = (float)(LocalCenter.Z - LocalExtent.Z) + LocalRadiusZ;
	const float OffsetX = FMath::Max((float)LocalExtent.X - LocalRadiusX, 0.0f) * 0.75f;
	const float OffsetY = FMath::Max((float)LocalExtent.Y - LocalRadiusY, 0.0f) * 0.75f;
	const auto AddLocalPoint = [&AddWorldSample, &ComponentTransform, &LocalCenter, SampleZ, DefaultRadius](float OffsetXValue, float OffsetYValue)
	{
		const FVector LocalPoint(LocalCenter.X + OffsetXValue, LocalCenter.Y + OffsetYValue, SampleZ);
		AddWorldSample(ComponentTransform.TransformPosition(LocalPoint), DefaultRadius);
	};

	AddLocalPoint(0.0f, 0.0f);
	if (WaterBuoyancySampleMode == EGPUTessellationWaterBuoyancySampleMode::Center)
	{
		return;
	}

	AddLocalPoint(OffsetX, 0.0f);
	AddLocalPoint(-OffsetX, 0.0f);
	AddLocalPoint(0.0f, OffsetY);
	AddLocalPoint(0.0f, -OffsetY);

	if (WaterBuoyancySampleMode == EGPUTessellationWaterBuoyancySampleMode::Bounds9Point)
	{
		AddLocalPoint(OffsetX, OffsetY);
		AddLocalPoint(OffsetX, -OffsetY);
		AddLocalPoint(-OffsetX, OffsetY);
		AddLocalPoint(-OffsetX, -OffsetY);
	}
}

void UGPUTessellationComponent::DrawWaterInteractionDebug() const
{
	if (!bEnableWaterInteraction || !bShowWaterInteractionDebug)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FVector VolumeCenter;
	FVector VolumeExtent;
	FQuat VolumeRotation;
	if (!GetWaterInteractionBox(VolumeCenter, VolumeExtent, VolumeRotation))
	{
		return;
	}

	DrawDebugBox(World, VolumeCenter, VolumeExtent, VolumeRotation, WaterInteractionDebugColor, false, 0.0f, 0, FMath::Max(0.0f, WaterInteractionDebugLineThickness));
}

void UGPUTessellationComponent::UpdateCoarseCollisionMesh(bool bForceUpdate)
{
	if (!IsPhysicsCollisionMeshEnabled())
	{
		CollisionMeshVertices.Reset();
		CollisionMeshIndices.Reset();
		CollisionLODRingSourceVertices.Reset();
		CollisionLODRingSourceResolutionX = 0;
		CollisionLODRingSourceResolutionY = 0;
		bCollisionLODRingSourceDirty = true;
		for (UBodySetup* PendingBodySetup : AsyncCollisionBodySetupQueue)
		{
			if (PendingBodySetup)
			{
				PendingBodySetup->AbortPhysicsMeshAsyncCreation();
			}
		}
		AsyncCollisionBodySetupQueue.Empty();
		if (CollisionBodySetup)
		{
			CollisionBodySetup->AbortPhysicsMeshAsyncCreation();
			CollisionBodySetup->ClearPhysicsMeshes();
			CollisionBodySetup->InvalidatePhysicsData();
			RecreatePhysicsState();
		}
		bHasCookedCollisionBody = false;
		bCollisionMeshDirty = false;
		LastVertexPerfectCollisionVisualLODSignature = 0;
		PendingVertexPerfectCollisionVisualLODSignature = 0;
		bPendingVertexPerfectCollisionVisualLODSignatureValid = false;
		PendingVertexPerfectCollisionLODRecookPosition = FVector::ZeroVector;
		bPendingVertexPerfectCollisionLODRecookPositionValid = false;
		bLastVertexPerfectCollisionBuildUsedMatchedVisualLOD = false;
		LastVertexPerfectCollisionLODRecookPosition = FVector::ZeroVector;
		bVertexPerfectCollisionLODRecookPositionInitialized = false;
		return;
	}

	if (GetCollisionEnabled() == ECollisionEnabled::NoCollision || GetCollisionEnabled() == ECollisionEnabled::QueryOnly)
	{
		SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	if (GetCollisionResponseToChannel(ECC_PhysicsBody) != ECollisionResponse::ECR_Block)
	{
		SetCollisionResponseToChannel(ECC_PhysicsBody, ECollisionResponse::ECR_Block);
	}

	UWorld* World = GetWorld();
	const double CurrentTime = World ? World->GetTimeSeconds() : FPlatformTime::Seconds();
	const bool bRateLimitedAnimation = IsCollisionSurfaceAnimated() && CollisionUpdateRate > 0.0f &&
		(LastCollisionUpdateTime < 0.0 || CurrentTime - LastCollisionUpdateTime >= 1.0 / (double)CollisionUpdateRate);
	const bool bCollisionRateAllowsLODRecook = CollisionUpdateRate > 0.0f &&
		(LastCollisionUpdateTime < 0.0 || CurrentTime - LastCollisionUpdateTime >= 1.0 / (double)CollisionUpdateRate);
	const bool bCollisionLODRingCameraMoved = UpdateCollisionLODRingCameraState(bForceUpdate);
	const uint32 CurrentVertexPerfectVisualLODSignature = CalculateVertexPerfectCollisionVisualLODSignature();
	FVector CurrentVertexPerfectCollisionLODPosition = FVector::ZeroVector;
	const bool bHasVertexPerfectCollisionLODPosition = GetCollisionLODRingCameraPosition(CurrentVertexPerfectCollisionLODPosition);
	const bool bAllowMatchedLODRecookInThisWorld =
		bAutoRecookVertexPerfectCollisionForLODChanges &&
		((World && World->IsGameWorld()) || bAutoRecookVertexPerfectCollisionInEditor);
	const float MatchedLODRecookDistance = FMath::Max(0.0f, VertexPerfectCollisionMatchedLODRecookDistance);
	const bool bMatchedLODRecookPositionMovedEnough =
		!bVertexPerfectCollisionLODRecookPositionInitialized ||
		!bHasVertexPerfectCollisionLODPosition ||
		FVector::Dist(CurrentVertexPerfectCollisionLODPosition, LastVertexPerfectCollisionLODRecookPosition) >= MatchedLODRecookDistance;
	const bool bVertexPerfectVisualLODChanged =
		IsVertexPerfectCollisionMeshEnabled() &&
		bVertexPerfectCollisionMatchVisualLOD &&
		bAllowMatchedLODRecookInThisWorld &&
		bCollisionRateAllowsLODRecook &&
		bMatchedLODRecookPositionMovedEnough &&
		CurrentVertexPerfectVisualLODSignature != LastVertexPerfectCollisionVisualLODSignature;
	const bool bNeedsActiveCollisionBody = !bHasCookedCollisionBody && AsyncCollisionBodySetupQueue.Num() == 0;
	if (IsCollisionLODRingsMeshEnabled() && (bForceUpdate || bCollisionMeshDirty || bRateLimitedAnimation))
	{
		bCollisionLODRingSourceDirty = true;
	}
	if (!bForceUpdate && !bCollisionMeshDirty && !bRateLimitedAnimation && !bCollisionLODRingCameraMoved && !bVertexPerfectVisualLODChanged && !bNeedsActiveCollisionBody)
	{
		return;
	}

	const bool bOnlyAutoMatchedLODUpdate =
		!bForceUpdate &&
		!bCollisionMeshDirty &&
		!bRateLimitedAnimation &&
		!bNeedsActiveCollisionBody &&
		!bCollisionLODRingCameraMoved &&
		bVertexPerfectVisualLODChanged;
	if (bOnlyAutoMatchedLODUpdate && AsyncCollisionBodySetupQueue.Num() > 0)
	{
		return;
	}

	for (UBodySetup* PendingBodySetup : AsyncCollisionBodySetupQueue)
	{
		if (PendingBodySetup)
		{
			PendingBodySetup->AbortPhysicsMeshAsyncCreation();
		}
	}

	bLastVertexPerfectCollisionBuildUsedMatchedVisualLOD = false;
	const bool bBuiltCollisionMesh = BuildCollisionMeshData();
	bCollisionMeshDirty = false;
	LastCollisionUpdateTime = CurrentTime;
	bPendingVertexPerfectCollisionVisualLODSignatureValid = false;
	bPendingVertexPerfectCollisionLODRecookPositionValid = false;
	if (bBuiltCollisionMesh && bLastVertexPerfectCollisionBuildUsedMatchedVisualLOD)
	{
		PendingVertexPerfectCollisionVisualLODSignature = CalculateVertexPerfectCollisionVisualLODSignature();
		bPendingVertexPerfectCollisionVisualLODSignatureValid = true;
		if (bHasVertexPerfectCollisionLODPosition)
		{
			PendingVertexPerfectCollisionLODRecookPosition = CurrentVertexPerfectCollisionLODPosition;
			bPendingVertexPerfectCollisionLODRecookPositionValid = true;
		}
	}

	if (!bBuiltCollisionMesh || !ContainsPhysicsTriMeshData(false))
	{
		if (CollisionBodySetup)
		{
			CollisionBodySetup->ClearPhysicsMeshes();
			CollisionBodySetup->InvalidatePhysicsData();
			RecreatePhysicsState();
		}
		bHasCookedCollisionBody = false;
		bPendingVertexPerfectCollisionVisualLODSignatureValid = false;
		bPendingVertexPerfectCollisionLODRecookPositionValid = false;
		LastVertexPerfectCollisionVisualLODSignature = 0;
		bVertexPerfectCollisionLODRecookPositionInitialized = false;
		return;
	}

	const bool bForceSynchronousInitialCook =
		bUseSynchronousInitialCollisionCook &&
		!bHasCookedCollisionBody &&
		AsyncCollisionBodySetupQueue.Num() == 0;
	const bool bUseAsyncCook = World && World->IsGameWorld() && bUseAsyncCollisionCooking && !bForceSynchronousInitialCook;
	UBodySetup* BodySetupToCook = nullptr;

	if (bUseAsyncCook)
	{
		AsyncCollisionBodySetupQueue.Add(CreateCollisionBodySetupHelper());
		BodySetupToCook = AsyncCollisionBodySetupQueue.Last();
	}
	else
	{
		AsyncCollisionBodySetupQueue.Empty();
		CreateCollisionBodySetup();
		BodySetupToCook = CollisionBodySetup;
	}

	if (!BodySetupToCook)
	{
		bPendingVertexPerfectCollisionVisualLODSignatureValid = false;
		bPendingVertexPerfectCollisionLODRecookPositionValid = false;
		return;
	}

	BodySetupToCook->AggGeom.EmptyElements();
	BodySetupToCook->CollisionTraceFlag = CTF_UseComplexAsSimple;

	if (bUseAsyncCook)
	{
		BodySetupToCook->CreatePhysicsMeshesAsync(FOnAsyncPhysicsCookFinished::CreateUObject(this, &UGPUTessellationComponent::FinishCollisionAsyncCook, BodySetupToCook));
	}
	else
	{
		BodySetupToCook->BodySetupGuid = FGuid::NewGuid();
		BodySetupToCook->bHasCookedCollisionData = true;
		BodySetupToCook->InvalidatePhysicsData();
		BodySetupToCook->CreatePhysicsMeshes();
		RecreatePhysicsState();
		bHasCookedCollisionBody = true;
		CommitPendingVertexPerfectCollisionLODState();
	}
}

UBodySetup* UGPUTessellationComponent::CreateCollisionBodySetupHelper()
{
	UBodySetup* NewBodySetup = NewObject<UBodySetup>(this, NAME_None, (IsTemplate() ? RF_Public | RF_ArchetypeObject : RF_NoFlags));
	NewBodySetup->BodySetupGuid = FGuid::NewGuid();
	NewBodySetup->bGenerateMirroredCollision = false;
	NewBodySetup->bDoubleSidedGeometry = true;
	NewBodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	return NewBodySetup;
}

void UGPUTessellationComponent::CreateCollisionBodySetup()
{
	if (CollisionBodySetup == nullptr)
	{
		CollisionBodySetup = CreateCollisionBodySetupHelper();
	}
}

void UGPUTessellationComponent::FinishCollisionAsyncCook(bool bSuccess, UBodySetup* FinishedBodySetup)
{
	if (!FinishedBodySetup)
	{
		return;
	}

	int32 FoundIndex = INDEX_NONE;
	if (!AsyncCollisionBodySetupQueue.Find(FinishedBodySetup, FoundIndex))
	{
		return;
	}

	if (bSuccess)
	{
		CollisionBodySetup = FinishedBodySetup;
		RecreatePhysicsState();
		bHasCookedCollisionBody = true;
		CommitPendingVertexPerfectCollisionLODState();

		TArray<TObjectPtr<UBodySetup>> NewQueue;
		NewQueue.Reserve(AsyncCollisionBodySetupQueue.Num());
		for (int32 QueueIndex = FoundIndex + 1; QueueIndex < AsyncCollisionBodySetupQueue.Num(); ++QueueIndex)
		{
			NewQueue.Add(AsyncCollisionBodySetupQueue[QueueIndex]);
		}
		AsyncCollisionBodySetupQueue = NewQueue;
	}
	else
	{
		AsyncCollisionBodySetupQueue.RemoveAt(FoundIndex);
		bPendingVertexPerfectCollisionVisualLODSignatureValid = false;
		bPendingVertexPerfectCollisionLODRecookPositionValid = false;
	}
}

void UGPUTessellationComponent::CommitPendingVertexPerfectCollisionLODState()
{
	if (bPendingVertexPerfectCollisionVisualLODSignatureValid)
	{
		LastVertexPerfectCollisionVisualLODSignature = PendingVertexPerfectCollisionVisualLODSignature;
		PendingVertexPerfectCollisionVisualLODSignature = 0;
		bPendingVertexPerfectCollisionVisualLODSignatureValid = false;
	}

	if (bPendingVertexPerfectCollisionLODRecookPositionValid)
	{
		LastVertexPerfectCollisionLODRecookPosition = PendingVertexPerfectCollisionLODRecookPosition;
		bVertexPerfectCollisionLODRecookPositionInitialized = true;
		PendingVertexPerfectCollisionLODRecookPosition = FVector::ZeroVector;
		bPendingVertexPerfectCollisionLODRecookPositionValid = false;
	}
}

void UGPUTessellationComponent::DrawCollisionMeshDebug() const
{
	if (!bShowCollisionMeshDebug)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float Lifetime = 0.0f;
	const uint8 DepthPriority = 0;
	const float Thickness = FMath::Max(0.0f, CollisionMeshDebugLineThickness);
	const FTransform& ComponentTransform = GetComponentTransform();

	if (!ContainsPhysicsTriMeshData(false))
	{
		const FBoxSphereBounds WorldBounds = CalcBounds(ComponentTransform);
		DrawDebugBox(World, WorldBounds.Origin, WorldBounds.BoxExtent, FColor::Red, false, Lifetime, DepthPriority, Thickness);
		DrawDebugString(World, WorldBounds.Origin + FVector(0.0, 0.0, WorldBounds.BoxExtent.Z + 50.0), TEXT("No coarse collision mesh"), nullptr, FColor::Red, Lifetime, false, 1.0f);
		return;
	}

	const int32 TriangleStride = FMath::Clamp(CollisionMeshDebugTriangleStride, 1, 64);
	const int32 TriangleCount = CollisionMeshIndices.Num() / 3;
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; TriangleIndex += TriangleStride)
	{
		const int32 Index0 = CollisionMeshIndices[TriangleIndex * 3 + 0];
		const int32 Index1 = CollisionMeshIndices[TriangleIndex * 3 + 1];
		const int32 Index2 = CollisionMeshIndices[TriangleIndex * 3 + 2];
		if (!CollisionMeshVertices.IsValidIndex(Index0) || !CollisionMeshVertices.IsValidIndex(Index1) || !CollisionMeshVertices.IsValidIndex(Index2))
		{
			continue;
		}

		const FVector World0 = ComponentTransform.TransformPosition(CollisionMeshVertices[Index0]);
		const FVector World1 = ComponentTransform.TransformPosition(CollisionMeshVertices[Index1]);
		const FVector World2 = ComponentTransform.TransformPosition(CollisionMeshVertices[Index2]);
		DrawDebugLine(World, World0, World1, CollisionMeshDebugColor, false, Lifetime, DepthPriority, Thickness);
		DrawDebugLine(World, World1, World2, CollisionMeshDebugColor, false, Lifetime, DepthPriority, Thickness);
		DrawDebugLine(World, World2, World0, CollisionMeshDebugColor, false, Lifetime, DepthPriority, Thickness);
	}
}

void UGPUTessellationComponent::MarkRenderStateDirty()
{
	Super::MarkRenderStateDirty();
}

FIntPoint UGPUTessellationComponent::CalculateGridResolution() const
{
	// Calculate resolution based on tessellation factor
	// When LOD is enabled, use the calculated LOD factor; otherwise use user's TessellationFactor
	int32 EffectiveTessellationFactor = (TessellationSettings.LODMode != EGPUTessellationLODMode::Disabled) 
		? LastAppliedTessFactor 
		: ApplyGeometrySubdivisionMultiplier(TessellationSettings.TessellationFactor);
	
	int32 Resolution = EffectiveTessellationFactor * 4;
	// Mirror the cap used by FGPUTessellationMeshBuilder::CalculateResolution (single-mesh path).
	// Visual subdivision can push the effective factor beyond the base 512 UI cap.
	Resolution = FMath::Clamp(Resolution, 4, 4096);
	
	// Make it a multiple of 8 for better compute shader performance
	Resolution = FMath::DivideAndRoundUp(Resolution, 8) * 8;
	
	CurrentResolution = FIntPoint(Resolution, Resolution);
	return CurrentResolution;
}

void UGPUTessellationComponent::UpdateDistanceBasedLOD(float DeltaTime)
{
	// Get camera position - works in both editor and game mode
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	
	FVector CameraPos = FVector::ZeroVector;
	bool bFoundCamera = false;
	
	// Try to get camera from player controller (game mode)
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		FVector ViewLocation;
		FRotator ViewRotation;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
		CameraPos = ViewLocation;
		bFoundCamera = true;
	}
#if WITH_EDITOR
	// In editor, use editor viewport camera
	else if (GEditor && GEditor->GetActiveViewport())
	{
		FViewport* Viewport = GEditor->GetActiveViewport();
		FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
		if (ViewportClient)
		{
			CameraPos = ViewportClient->GetViewLocation();
			bFoundCamera = true;
		}
	}
#endif
	
	if (!bFoundCamera)
	{
		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation LOD: NO CAMERA FOUND!"));
		}
		return;
	}
	
	// Calculate distance (to pivot or bounds based on setting)
	FVector ComponentPos;
	float Distance = CalculateDistanceToCamera(CameraPos, ComponentPos);
	
	// Account for component scale - larger objects should use LOD at proportionally larger distances
	// Use the maximum scale component to represent overall size
	FVector Scale3D = GetComponentScale();
	float MaxScale = FMath::Max3(FMath::Abs(Scale3D.X), FMath::Abs(Scale3D.Y), FMath::Abs(Scale3D.Z));
	
	// Scale LOD distances by the component scale
	// This makes LOD distances work consistently regardless of actor scale
	// Example: Actor at scale 100 will use LOD distances 100x larger
	float ScaledMinDistance = TessellationSettings.MinTessellationDistance * MaxScale;
	float ScaledMaxDistance = TessellationSettings.MaxTessellationDistance * MaxScale;
	
	// Store camera position for tracking changes
	float CameraMovement = FVector::Dist(CameraPos, LastCameraPosition);
	LastCameraPosition = CameraPos;
	
	// Calculate target LOD factor based on distance (using scaled distances)
	int32 TargetTessFactor = CalculateLODFactorScaled(Distance, ScaledMinDistance, ScaledMaxDistance);
	
	// Debug logging (throttled) - show LOD calculation every 2 seconds
	if (bEnableDebugLogging)
	{
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastLogTime >= 2.0)
		{
			LastLogTime = CurrentTime;
			
			// Calculate which zone we're in (using scaled distances)
			FString DistanceZone;
			if (Distance <= ScaledMinDistance)
			{
				DistanceZone = TEXT("NEAR (Max Tessellation)");
			}
			else if (Distance >= ScaledMaxDistance)
			{
				DistanceZone = TEXT("FAR (Min Tessellation)");
			}
			else
			{
				float DistanceRange = ScaledMaxDistance - ScaledMinDistance;
				float DistanceInRange = Distance - ScaledMinDistance;
				float Percentage = (DistanceInRange / DistanceRange) * 100.0f;
				DistanceZone = FString::Printf(TEXT("TRANSITION (%.1f%% through range)"), Percentage);
			}
			
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation LOD Status:"));
			UE_LOG(LogTemp, Warning, TEXT("  Camera: %s (moved %.1f since last frame)"), *CameraPos.ToString(), CameraMovement);
			UE_LOG(LogTemp, Warning, TEXT("  Component: %s, Scale: %.2f (max component)"), *ComponentPos.ToString(), MaxScale);
			UE_LOG(LogTemp, Warning, TEXT("  Distance: %.1f units (%.1f meters) - %s"), Distance, Distance / 100.0f, *DistanceZone);
			UE_LOG(LogTemp, Warning, TEXT("  Distance Range (scaled): %.1f to %.1f (base: %.1f to %.1f, scale: %.2fx)"), 
				ScaledMinDistance, ScaledMaxDistance,
				TessellationSettings.MinTessellationDistance, TessellationSettings.MaxTessellationDistance,
				MaxScale);
			UE_LOG(LogTemp, Warning, TEXT("  Target LOD: %d, Current: %.1f, Applied: %d"), TargetTessFactor, CurrentLODLevel, LastAppliedTessFactor);
			UE_LOG(LogTemp, Warning, TEXT("  Factor Range: %d (max) to %d (min)"), TessellationSettings.MaxTessellationFactor, TessellationSettings.MinTessellationFactor);
			UE_LOG(LogTemp, Warning, TEXT("  User TessellationFactor: %d (NOT modified by LOD)"), TessellationSettings.TessellationFactor);
			UE_LOG(LogTemp, Warning, TEXT("  Mode: %s, DeltaTime: %.4f"), 
				World->WorldType == EWorldType::Editor ? TEXT("Editor") : TEXT("Game"), DeltaTime);
		}
	}
	
	// Smooth interpolation for transitions
	if (TessellationSettings.LODTransitionSpeed > 0.0f)
	{
		CurrentLODLevel = FMath::FInterpTo(
			CurrentLODLevel,
			(float)TargetTessFactor,
			DeltaTime,
			TessellationSettings.LODTransitionSpeed
		);
	}
	else
	{
		CurrentLODLevel = (float)TargetTessFactor;
	}
	
	int32 NewTessFactor = ApplyGeometrySubdivisionMultiplier(FMath::RoundToInt(CurrentLODLevel));
	
	// Apply hysteresis to prevent oscillation
	if (FMath::Abs(NewTessFactor - LastAppliedTessFactor) > TessellationSettings.LODHysteresis)
	{
		// LOD changed significantly - apply it (store for grid resolution calculation)
		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: LOD TRANSITION"));
			UE_LOG(LogTemp, Warning, TEXT("  Change: %d -> %d (diff: %d, hysteresis: %d)"),
				LastAppliedTessFactor, NewTessFactor, FMath::Abs(NewTessFactor - LastAppliedTessFactor), TessellationSettings.LODHysteresis);
			UE_LOG(LogTemp, Warning, TEXT("  Distance: %.1f units (%.1f meters)"), Distance, Distance / 100.0f);
			UE_LOG(LogTemp, Warning, TEXT("  Camera: %s"), *CameraPos.ToString());
			UE_LOG(LogTemp, Warning, TEXT("  Component: %s"), *ComponentPos.ToString());
			UE_LOG(LogTemp, Warning, TEXT("  TessellationFactor preserved: %d"), TessellationSettings.TessellationFactor);
			UE_LOG(LogTemp, Warning, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
		}
		
		// CRITICAL: Store LOD factor separately - DO NOT modify user's TessellationFactor!
		LastAppliedTessFactor = NewTessFactor;
		MarkRenderStateDirty();
	}
}

void UGPUTessellationComponent::UpdateDensityBasedLOD(float DeltaTime)
{
	// Density texture LOD would require sampling the texture on CPU
	// For now, fall back to distance-based LOD
	// Full implementation would require reading density at component location
	UpdateDistanceBasedLOD(DeltaTime);
}

float UGPUTessellationComponent::CalculateDistanceToCamera(const FVector& CameraPos, FVector& OutComponentPos) const
{
	OutComponentPos = GetComponentLocation();
	
	if (!TessellationSettings.bUseDistanceToBounds)
	{
		// Simple: distance to pivot point
		return FVector::Dist(OutComponentPos, CameraPos);
	}
	
	// Calculate distance to closest point on plane bounds.
	// FIX: Plane lives on local XY with Z as the displacement axis (matches the compute shader
	// in GPUVertexGeneration.usf and CalcBounds). Previous code clamped XZ and forced Y=0,
	// which collapsed the plane into a line along Y and produced wrong LOD distances when
	// the camera moved outside the patch — the closest point would jump to the origin
	// instead of tracking the real plane edge.
	FTransform ComponentTransform = GetComponentTransform();
	FVector LocalCameraPos = ComponentTransform.InverseTransformPosition(CameraPos);
	
	// Plane size (local space)
	float HalfSizeX = TessellationSettings.PlaneSizeX * 0.5f;
	float HalfSizeY = TessellationSettings.PlaneSizeY * 0.5f;
	
	// Clamp camera position to plane bounds (in local XY space)
	float ClampedX = FMath::Clamp(LocalCameraPos.X, -HalfSizeX, HalfSizeX);
	float ClampedY = FMath::Clamp(LocalCameraPos.Y, -HalfSizeY, HalfSizeY);
	
	// Z is the displacement axis. Use the average displaced height so LOD distance reflects
	// the surface, not the unmodified plane.
	float ClampedZ = TessellationSettings.DisplacementOffset + (TessellationSettings.DisplacementIntensity * 0.5f);
	
	FVector ClosestPointLocal(ClampedX, ClampedY, ClampedZ);
	FVector ClosestPointWorld = ComponentTransform.TransformPosition(ClosestPointLocal);
	
	// Return distance to closest point on plane
	return FVector::Dist(ClosestPointWorld, CameraPos);
}

void UGPUTessellationComponent::UpdateDiscreteLOD(float DeltaTime)
{
	// Get camera position
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	
	FVector CameraPos = FVector::ZeroVector;
	bool bFoundCamera = false;
	
	// Try to get camera from player controller (game mode)
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		FVector ViewLocation;
		FRotator ViewRotation;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
		CameraPos = ViewLocation;
		bFoundCamera = true;
	}
#if WITH_EDITOR
	// In editor, use editor viewport camera
	else if (GEditor && GEditor->GetActiveViewport())
	{
		FViewport* Viewport = GEditor->GetActiveViewport();
		FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
		if (ViewportClient)
		{
			CameraPos = ViewportClient->GetViewLocation();
			bFoundCamera = true;
		}
	}
#endif
	
	if (!bFoundCamera)
	{
		return;
	}
	
	// Calculate distance (to pivot or bounds)
	FVector ComponentPos;
	float Distance = CalculateDistanceToCamera(CameraPos, ComponentPos);
	
	// Account for component scale
	FVector Scale3D = GetComponentScale();
	float MaxScale = FMath::Max3(FMath::Abs(Scale3D.X), FMath::Abs(Scale3D.Y), FMath::Abs(Scale3D.Z));
	float ScaledDistance = Distance / MaxScale;
	
	// Determine which discrete level to use based on distance thresholds
	int32 TargetTessFactor = 4; // Default to lowest
	
	if (TessellationSettings.DiscreteLODLevels.Num() > 0)
	{
		// Start with highest quality (first level) and work down
		TargetTessFactor = StaticCast<int32>(TessellationSettings.DiscreteLODLevels[0]);
		
		// Check each distance threshold
		for (int32 i = 0; i < TessellationSettings.DiscreteLODDistances.Num() && i < TessellationSettings.DiscreteLODLevels.Num(); ++i)
		{
			if (ScaledDistance > TessellationSettings.DiscreteLODDistances[i])
			{
				// Beyond this threshold, use next lower level if available
				if (i + 1 < TessellationSettings.DiscreteLODLevels.Num())
				{
					TargetTessFactor = StaticCast<int32>(TessellationSettings.DiscreteLODLevels[i + 1]);
				}
			}
			else
			{
				// Within threshold, use current level
				TargetTessFactor = StaticCast<int32>(TessellationSettings.DiscreteLODLevels[i]);
				break;
			}
		}
	}
	
	// Convert enum to actual tessellation factor
	switch (StaticCast<EGPUTessellationPatchLevel>(TargetTessFactor))
	{
		case EGPUTessellationPatchLevel::Patch_4:   TargetTessFactor = 4; break;
		case EGPUTessellationPatchLevel::Patch_8:   TargetTessFactor = 8; break;
		case EGPUTessellationPatchLevel::Patch_16:  TargetTessFactor = 16; break;
		case EGPUTessellationPatchLevel::Patch_32:  TargetTessFactor = 32; break;
		case EGPUTessellationPatchLevel::Patch_64:  TargetTessFactor = 64; break;
		case EGPUTessellationPatchLevel::Patch_128: TargetTessFactor = 128; break;
		default: TargetTessFactor = 16; break;
	}
	TargetTessFactor = ApplyGeometrySubdivisionMultiplier(TargetTessFactor);
	
	// Apply hysteresis to prevent oscillation
	int32 Difference = FMath::Abs(TargetTessFactor - LastAppliedTessFactor);
	if (Difference >= TessellationSettings.LODHysteresis)
	{
		LastAppliedTessFactor = TargetTessFactor;
		CurrentLODLevel = static_cast<float>(TargetTessFactor);
		MarkRenderStateDirty();
		
		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Discrete LOD: Distance=%.1f (scaled=%.1f), Level=%d"), 
				Distance, ScaledDistance, TargetTessFactor);
		}
	}

}

void UGPUTessellationComponent::UpdatePatchBasedLOD(float DeltaTime)
{
	// Spatial patch system generates patches with per-patch LOD based on camera distance
	// Track camera position and send updates to scene proxy when camera moves significantly
	
	// Get camera position (same logic as other LOD modes)
	FVector CameraPos = FVector::ZeroVector;
	bool bFoundCamera = false;
	
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector ViewLocation;
			FRotator ViewRotation;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			CameraPos = ViewLocation;
			bFoundCamera = true;
		}
	}
	
#if WITH_EDITOR
	// In editor, use editor viewport camera
	if (!bFoundCamera && GEditor && GEditor->GetActiveViewport())
	{
		FViewport* Viewport = GEditor->GetActiveViewport();
		FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
		if (ViewportClient)
		{
			CameraPos = ViewportClient->GetViewLocation();
			bFoundCamera = true;
		}
	}
#endif
	
	if (!bFoundCamera)
	{
		if (bEnableDebugLogging)
		{
			static double LastWarningTime = 0.0;
			double CurrentTime = FPlatformTime::Seconds();
			if (CurrentTime - LastWarningTime >= 5.0)
			{
				LastWarningTime = CurrentTime;
				UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Patch LOD: NO CAMERA FOUND!"));
			}
		}
		return;
	}
	
	// Check if camera moved significantly (threshold to avoid constant updates)
	float CameraMovement = FVector::Dist(CameraPos, LastCameraPosition);
	float UpdateThreshold = 100.0f; // Update if camera moved more than 100 units (1 meter)
	
	// Spatial patch/quadtree distances are evaluated in world space. Use a small fraction of
	// the placed plane size for the update cadence so huge scaled instances do not rebuild on
	// nearly every editor-camera tick, while normal-sized planes keep the 100 cm baseline.
	const FVector Scale3D = GetComponentScale();
	const float PlacedPlaneSize = FMath::Max(
		TessellationSettings.PlaneSizeX * FMath::Abs(Scale3D.X),
		TessellationSettings.PlaneSizeY * FMath::Abs(Scale3D.Y));
	float ScaledThreshold = FMath::Clamp(FMath::Max(UpdateThreshold, PlacedPlaneSize * 0.005f), UpdateThreshold, 10000.0f);
	
	const bool bPatchStructureChanged = LastPatchCountX != TessellationSettings.PatchCountX ||
		LastPatchCountY != TessellationSettings.PatchCountY;
	const bool bCameraMovedEnough = CameraMovement > ScaledThreshold;

	if (bPatchStructureChanged || bCameraMovedEnough)
	{
		TArray<FGPUTessellationPatchInfo> DesiredPatchState;
		FGPUTessellationMeshBuilder MeshBuilder;
		MeshBuilder.CalculatePatchState(
			GetEffectiveTessellationSettings(),
			GetComponentTransform().ToMatrixWithScale(),
			CameraPos,
			nullptr,
			TessellationSettings.PatchCountX,
			TessellationSettings.PatchCountY,
			DesiredPatchState);

		uint32 PatchTopologySignature = 2166136261u;
		for (const FGPUTessellationPatchInfo& Patch : DesiredPatchState)
		{
			PatchTopologySignature = HashCombine(PatchTopologySignature, GetTypeHash(Patch.TessellationLevel));
			PatchTopologySignature = HashCombine(PatchTopologySignature, GetTypeHash(Patch.EdgeCollapseFactors.X));
			PatchTopologySignature = HashCombine(PatchTopologySignature, GetTypeHash(Patch.EdgeCollapseFactors.Y));
			PatchTopologySignature = HashCombine(PatchTopologySignature, GetTypeHash(Patch.EdgeCollapseFactors.Z));
			PatchTopologySignature = HashCombine(PatchTopologySignature, GetTypeHash(Patch.EdgeCollapseFactors.W));
		}

		const bool bPatchTopologyChanged = PatchTopologySignature != LastPatchTopologySignature;
		const bool bShouldSendPatchUpdate = bPatchStructureChanged ||
			TessellationSettings.bUsePersistentPatchBuffers ||
			bPatchTopologyChanged;

		if (!bShouldSendPatchUpdate)
		{
			LastCameraPosition = CameraPos;
			if (bEnableDebugLogging)
			{
				UE_LOG(LogTemp, Verbose, TEXT("GPUTessellation Patch LOD: Camera moved %.1f units, but patch topology is unchanged; skipping geometry rebuild."), CameraMovement);
			}
			return;
		}

		LastPatchCountX = TessellationSettings.PatchCountX;
		LastPatchCountY = TessellationSettings.PatchCountY;
		LastPatchTopologySignature = PatchTopologySignature;
		LastCameraPosition = CameraPos;
		
		// Send camera position to the scene proxy. The proxy either regenerates patches or,
		// when experimental persistent patch buffers are enabled, updates patch state only.
		SendRenderDynamicData_Concurrent();
		
		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Patch LOD: Camera moved %.1f units (threshold %.1f) - %s with camera at: %s"),
				CameraMovement,
				ScaledThreshold,
				TessellationSettings.bUsePersistentPatchBuffers ? TEXT("Updating persistent patch state") : TEXT("Regenerating patches"),
				*CameraPos.ToString());
		}
	}
}

void UGPUTessellationComponent::UpdateQuadtreeLOD(float DeltaTime)
{
	FVector CameraPos = FVector::ZeroVector;
	bool bFoundCamera = false;

	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector ViewLocation;
			FRotator ViewRotation;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			CameraPos = ViewLocation;
			bFoundCamera = true;
		}
	}

#if WITH_EDITOR
	if (!bFoundCamera && GEditor && GEditor->GetActiveViewport())
	{
		FViewport* Viewport = GEditor->GetActiveViewport();
		FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
		if (ViewportClient)
		{
			CameraPos = ViewportClient->GetViewLocation();
			bFoundCamera = true;
		}
	}
#endif

	if (!bFoundCamera)
	{
		return;
	}

	const float CameraMovement = FVector::Dist(CameraPos, LastCameraPosition);
	const float UpdateThreshold = 100.0f;
	// Spatial patch/quadtree distances are evaluated in world space. Use a small fraction of
	// the placed plane size for the update cadence so huge scaled instances do not rebuild on
	// nearly every editor-camera tick, while normal-sized planes keep the 100 cm baseline.
	const FVector Scale3D = GetComponentScale();
	const float PlacedPlaneSize = FMath::Max(
		TessellationSettings.PlaneSizeX * FMath::Abs(Scale3D.X),
		TessellationSettings.PlaneSizeY * FMath::Abs(Scale3D.Y));
	const float ScaledThreshold = FMath::Clamp(FMath::Max(UpdateThreshold, PlacedPlaneSize * 0.005f), UpdateThreshold, 10000.0f);
	const bool bCameraMovedEnough = CameraMovement > ScaledThreshold;
	const bool bNeedsInitialState = LastPatchTopologySignature == 0;

	if (!bNeedsInitialState && !bCameraMovedEnough)
	{
		return;
	}

	TArray<FGPUTessellationPatchInfo> DesiredLeafState;
	FGPUTessellationMeshBuilder MeshBuilder;
	MeshBuilder.CalculateQuadtreePatchState(
		GetEffectiveTessellationSettings(),
		GetComponentTransform().ToMatrixWithScale(),
		CameraPos,
		nullptr,
		DesiredLeafState);

	uint32 QuadtreeTopologySignature = 2166136261u;
	QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(DesiredLeafState.Num()));
	for (const FGPUTessellationPatchInfo& Leaf : DesiredLeafState)
	{
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(Leaf.TessellationLevel));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(Leaf.QuadtreeDepth));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(Leaf.QuadtreeNodeIndex));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(FMath::RoundToInt(Leaf.PatchOffset.X * 65535.0f)));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(FMath::RoundToInt(Leaf.PatchOffset.Y * 65535.0f)));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(FMath::RoundToInt(Leaf.PatchSize.X * 65535.0f)));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(FMath::RoundToInt(Leaf.PatchSize.Y * 65535.0f)));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(Leaf.EdgeCollapseFactors.X));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(Leaf.EdgeCollapseFactors.Y));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(Leaf.EdgeCollapseFactors.Z));
		QuadtreeTopologySignature = HashCombine(QuadtreeTopologySignature, GetTypeHash(Leaf.EdgeCollapseFactors.W));
	}

	if (!bNeedsInitialState && QuadtreeTopologySignature == LastPatchTopologySignature)
	{
		LastCameraPosition = CameraPos;
		if (bEnableDebugLogging)
		{
			UE_LOG(LogTemp, Verbose, TEXT("GPUTessellation Quadtree LOD: Camera moved %.1f units, but leaf topology is unchanged; skipping geometry rebuild."), CameraMovement);
		}
		return;
	}

	LastPatchCountX = DesiredLeafState.Num();
	LastPatchCountY = 1;
	LastPatchTopologySignature = QuadtreeTopologySignature;
	LastCameraPosition = CameraPos;

	SendRenderDynamicData_Concurrent();

	if (bEnableDebugLogging)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Quadtree LOD: Camera moved %.1f units (threshold %.1f) - regenerating %d leaves with camera at: %s"),
			CameraMovement,
			ScaledThreshold,
			DesiredLeafState.Num(),
			*CameraPos.ToString());
	}
}

void UGPUTessellationComponent::SendRenderDynamicData_Concurrent()
{
	// Send camera position to scene proxy for patch regeneration or persistent state update.
	// This follows the Unreal pattern used by other dynamic mesh components
	
	if (SceneProxy)
	{
		// Create dynamic data with current camera position
		FGPUTessellationDynamicData* DynamicData = new FGPUTessellationDynamicData();
		DynamicData->CameraPosition = LastCameraPosition;
		DynamicData->LocalToWorld = GetComponentTransform().ToMatrixWithScale();
		
		// Send to scene proxy on render thread
		FGPUTessellationSceneProxy* TessSceneProxy = static_cast<FGPUTessellationSceneProxy*>(SceneProxy);
		ENQUEUE_RENDER_COMMAND(SendGPUTessellationDynamicData)(
			[TessSceneProxy, DynamicData](FRHICommandListImmediate& RHICmdList)
			{
				TessSceneProxy->UpdateDynamicData_RenderThread(DynamicData);
			});
	}
}

int32 UGPUTessellationComponent::CalculateLODFactorScaled(float Distance, float ScaledMinDistance, float ScaledMaxDistance) const
{
	// Distance-based falloff with min and max distance ranges
	// Distance < MinDistance: Use MaxTessellationFactor (high detail when close)
	// Distance between Min and Max: Lerp from MaxTessellationFactor to MinTessellationFactor
	// Distance > MaxDistance: Use MinTessellationFactor (low detail when far)
	
	// Uses scaled distances passed in (already adjusted for component scale)
	float MinDist = ScaledMinDistance;
	float MaxDist = ScaledMaxDistance;
	
	// Ensure min < max
	if (MinDist >= MaxDist)
	{
		MaxDist = MinDist + 1000.0f; // Failsafe
	}
	
	// Calculate interpolation factor
	float t;
	if (Distance <= MinDist)
	{
		t = 0.0f; // Full max tessellation
	}
	else if (Distance >= MaxDist)
	{
		t = 1.0f; // Full min tessellation
	}
	else
	{
		// Interpolate between min and max distance
		t = (Distance - MinDist) / (MaxDist - MinDist);
		
		// Apply smooth curve (ease-in-out) for more natural transitions
		t = t * t * (3.0f - 2.0f * t);  // Smoothstep
	}
	
	float LerpedFactor = FMath::Lerp(
		(float)TessellationSettings.MaxTessellationFactor,  // Close distance
		(float)TessellationSettings.MinTessellationFactor,  // Far distance
		t
	);
	
	return FMath::Clamp(FMath::RoundToInt(LerpedFactor), 1, 256);
}

int32 UGPUTessellationComponent::CalculateLODFactor(float Distance) const
{
	// Legacy method - use CalculateLODFactorScaled instead for scale-aware LOD
	// Distance-based falloff with min and max distance ranges
	// Distance < MinDistance: Use MaxTessellationFactor (high detail when close)
	// Distance between Min and Max: Lerp from MaxTessellationFactor to MinTessellationFactor
	// Distance > MaxDistance: Use MinTessellationFactor (low detail when far)
	
	float MinDist = TessellationSettings.MinTessellationDistance;
	float MaxDist = TessellationSettings.MaxTessellationDistance;
	
	// Ensure min < max
	if (MinDist >= MaxDist)
	{
		MaxDist = MinDist + 1000.0f; // Failsafe
	}
	
	// Calculate interpolation factor
	float t;
	if (Distance <= MinDist)
	{
		t = 0.0f; // Full max tessellation
	}
	else if (Distance >= MaxDist)
	{
		t = 1.0f; // Full min tessellation
	}
	else
	{
		// Interpolate between min and max distance
		t = (Distance - MinDist) / (MaxDist - MinDist);
		
		// Apply smooth curve (ease-in-out) for more natural transitions
		t = t * t * (3.0f - 2.0f * t);  // Smoothstep
	}
	
	float LerpedFactor = FMath::Lerp(
		(float)TessellationSettings.MaxTessellationFactor,  // Close distance
		(float)TessellationSettings.MinTessellationFactor,  // Far distance
		t
	);
	
	return FMath::Clamp(FMath::RoundToInt(LerpedFactor), 1, 256);
}

bool UGPUTessellationComponent::IsLocalXYInsidePlane(const FVector2D& LocalXY) const
{
	const float HalfSizeX = TessellationSettings.PlaneSizeX * 0.5f;
	const float HalfSizeY = TessellationSettings.PlaneSizeY * 0.5f;
	return LocalXY.X >= -HalfSizeX && LocalXY.X <= HalfSizeX && LocalXY.Y >= -HalfSizeY && LocalXY.Y <= HalfSizeY;
}

bool UGPUTessellationComponent::EvaluateLocalSurfaceHeight(const FVector2D& LocalXY, float& OutLocalHeight) const
{
	if (!IsLocalXYInsidePlane(LocalXY))
	{
		OutLocalHeight = 0.0f;
		return false;
	}

	const FGPUTessellationSettings Settings = GetEffectiveTessellationSettings();
	const FGPUOceanSettings& Ocean = Settings.OceanSettings;
	const FVector2D SurfaceUV(
		(LocalXY.X / FMath::Max(Settings.PlaneSizeX, 1.0f)) + 0.5,
		(LocalXY.Y / FMath::Max(Settings.PlaneSizeY, 1.0f)) + 0.5);

	switch (Ocean.WaveMode)
	{
		case EGPUOceanWaveMode::Gerstner:
		{
			float Height = 0.0f;
			const int32 WaveCount = FMath::Min(Ocean.GerstnerWaves.Num(), GPU_OCEAN_MAX_GERSTNER_WAVES);
			for (int32 WaveIndex = 0; WaveIndex < WaveCount; ++WaveIndex)
			{
				const FGPUOceanGerstnerWave& Wave = Ocean.GerstnerWaves[WaveIndex];
				FVector2D Direction = Wave.Direction;
				const double DirectionLength = Direction.Size();
				if (DirectionLength > KINDA_SMALL_NUMBER)
				{
					Direction /= DirectionLength;
				}
				else
				{
					Direction = FVector2D(1.0, 0.0);
				}

				const float Wavelength = FMath::Max(Wave.Wavelength, 1.0f);
				const float AngularWaveNumber = (2.0f * PI) / Wavelength;
				const float Phase = AngularWaveNumber * ((float)FVector2D::DotProduct(Direction, LocalXY) - Wave.Speed * Ocean.Time) + Wave.PhaseOffset;
				const float SineValue = FMath::Sin(Phase);
				const float Steepness = FMath::Clamp(Wave.Steepness, 0.0f, 1.0f);
				const float Sharpened = Steepness > 0.001f
					? (SineValue >= 0.0f ? 1.0f : -1.0f) * FMath::Pow(FMath::Abs(SineValue), 1.0f + Steepness * 2.0f)
					: SineValue;
				Height += Wave.Amplitude * Sharpened;
			}
			OutLocalHeight = Height + Settings.DisplacementOffset;
			return true;
		}

		case EGPUOceanWaveMode::PerlinFBM:
		{
			FVector2D Flow = Ocean.PerlinFlowDirection * Ocean.PerlinFlowSpeed;
			FVector2D SampleXY = LocalXY - (Flow * Ocean.Time * 0.25f);
			float Frequency = Ocean.PerlinFrequency;
			float Amplitude = 1.0f;
			float Sum = 0.0f;
			float Normalization = 0.0f;
			const int32 Octaves = FMath::Clamp(Ocean.PerlinOctaves, 1, 8);

			for (int32 OctaveIndex = 0; OctaveIndex < Octaves; ++OctaveIndex)
			{
				const float ZAxis = Ocean.Time * (0.6f + 0.35f * (float)OctaveIndex) + (float)OctaveIndex * 17.13f;
				const FVector NoisePosition(SampleXY.X * Frequency, SampleXY.Y * Frequency, ZAxis * Frequency);
				Sum += Amplitude * (float)GPUTessellationPerlinNoise3D(NoisePosition);
				Normalization += Amplitude;
				Amplitude *= Ocean.PerlinPersistence;
				Frequency *= Ocean.PerlinLacunarity;
			}

			OutLocalHeight = (Sum / FMath::Max(Normalization, 0.0001f)) * Settings.DisplacementIntensity + Settings.DisplacementOffset;
			return true;
		}

		case EGPUOceanWaveMode::FFT:
			OutLocalHeight = 0.0f;
			return false;

		case EGPUOceanWaveMode::Disabled:
		default:
			break;
	}

	if (Settings.bUseSineWaveDisplacement)
	{
		const float Height = FMath::Sin((float)SurfaceUV.X * 10.0f) * FMath::Sin((float)SurfaceUV.Y * 10.0f) * 0.5f + 0.5f;
		OutLocalHeight = Height * Settings.DisplacementIntensity + Settings.DisplacementOffset;
		return true;
	}

	if (DisplacementTexture == nullptr && SubtractTexture == nullptr)
	{
		OutLocalHeight = Settings.DisplacementIntensity + Settings.DisplacementOffset;
		return true;
	}

	OutLocalHeight = 0.0f;
	return false;
}

FVector UGPUTessellationComponent::CalculateLocalSurfaceNormal(const FVector2D& LocalXY) const
{
	const float SampleStep = FMath::Max(1.0f, FMath::Max(TessellationSettings.PlaneSizeX, TessellationSettings.PlaneSizeY) / 1024.0f);

	float HeightLeft = 0.0f;
	float HeightRight = 0.0f;
	float HeightDown = 0.0f;
	float HeightUp = 0.0f;
	const bool bHasLeft = EvaluateLocalSurfaceHeight(LocalXY + FVector2D(-SampleStep, 0.0f), HeightLeft);
	const bool bHasRight = EvaluateLocalSurfaceHeight(LocalXY + FVector2D(SampleStep, 0.0f), HeightRight);
	const bool bHasDown = EvaluateLocalSurfaceHeight(LocalXY + FVector2D(0.0f, -SampleStep), HeightDown);
	const bool bHasUp = EvaluateLocalSurfaceHeight(LocalXY + FVector2D(0.0f, SampleStep), HeightUp);

	float CenterHeight = 0.0f;
	EvaluateLocalSurfaceHeight(LocalXY, CenterHeight);
	if (!bHasLeft) { HeightLeft = CenterHeight; }
	if (!bHasRight) { HeightRight = CenterHeight; }
	if (!bHasDown) { HeightDown = CenterHeight; }
	if (!bHasUp) { HeightUp = CenterHeight; }

	const FVector TangentX(2.0f * SampleStep, 0.0f, HeightRight - HeightLeft);
	const FVector TangentY(0.0f, 2.0f * SampleStep, HeightUp - HeightDown);
	return FVector::CrossProduct(TangentX, TangentY).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
}
