// Licensed under the MIT License. See LICENSE file in the project root.

/*
 * GPU Runtime Tessellation - Usage Examples
 * 
 * This file demonstrates how to use the GPURuntimeTessellation plugin
 * in both C++ and Blueprint contexts.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GPUTessellationComponent.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "GPUTessellationExamples.generated.h"

// ============================================================================
// EXAMPLE 1: Simple Tessellated Plane Actor
// ============================================================================

UCLASS(Blueprintable)
class GPURUNTIMETESSELLATION_API AGPUTessellatedPlaneActor : public AActor
{
	GENERATED_BODY()

public:
	AGPUTessellatedPlaneActor()
	{
		// Create tessellation component
		TessellationComponent = CreateDefaultSubobject<UGPUTessellationComponent>(TEXT("TessellationComponent"));
		RootComponent = TessellationComponent;

		// Configure basic settings
		TessellationComponent->TessellationSettings.TessellationFactor = 16;
		TessellationComponent->TessellationSettings.PlaneSizeX = 1000.0f;
		TessellationComponent->TessellationSettings.PlaneSizeY = 1000.0f;
		TessellationComponent->TessellationSettings.DisplacementIntensity = 100.0f;
		TessellationComponent->TessellationSettings.bUseSineWaveDisplacement = true;
		TessellationComponent->bAutoUpdate = true;
	}

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tessellation")
	UGPUTessellationComponent* TessellationComponent;
};

// ============================================================================
// EXAMPLE 2: Terrain Actor with Displacement Texture
// ============================================================================

UCLASS(Blueprintable)
class GPURUNTIMETESSELLATION_API AGPUTessellatedTerrain : public AActor
{
	GENERATED_BODY()

public:
	AGPUTessellatedTerrain()
	{
		TessellationComponent = CreateDefaultSubobject<UGPUTessellationComponent>(TEXT("TessellationComponent"));
		RootComponent = TessellationComponent;

		// Configure for terrain
		TessellationComponent->TessellationSettings.TessellationFactor = 32;
		TessellationComponent->TessellationSettings.PlaneSizeX = 10000.0f;  // 100m terrain
		TessellationComponent->TessellationSettings.PlaneSizeY = 10000.0f;
		TessellationComponent->TessellationSettings.DisplacementIntensity = 500.0f;  // 5m max height
		TessellationComponent->TessellationSettings.bUseSineWaveDisplacement = false;  // Use texture
		
		// Enable dynamic LOD
		TessellationComponent->TessellationSettings.LODMode = EGPUTessellationLODMode::DistanceBased;
		TessellationComponent->TessellationSettings.MaxTessellationDistance = 5000.0f;
		TessellationComponent->TessellationSettings.MinTessellationFactor = 4;
		TessellationComponent->TessellationSettings.LODTransitionSpeed = 2.0f;
		
		// Normal calculation
		TessellationComponent->TessellationSettings.NormalCalculationMethod = EGPUTessellationNormalMethod::FiniteDifference;
		TessellationComponent->bAutoUpdate = true;
	}

	virtual void BeginPlay() override
	{
		Super::BeginPlay();

		// Load displacement texture at runtime
		if (DisplacementTexturePath.IsValid())
		{
			UTexture2D* LoadedTexture = LoadObject<UTexture2D>(nullptr, *DisplacementTexturePath.ToString());
			if (LoadedTexture)
			{
				TessellationComponent->SetDisplacementTexture(LoadedTexture);
			}
		}

		// Load material
		if (TerrainMaterialPath.IsValid())
		{
			UMaterialInterface* LoadedMaterial = LoadObject<UMaterialInterface>(nullptr, *TerrainMaterialPath.ToString());
			if (LoadedMaterial)
			{
				TessellationComponent->SetMaterial(0, LoadedMaterial);
			}
		}
	}

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tessellation")
	UGPUTessellationComponent* TessellationComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tessellation")
	FSoftObjectPath DisplacementTexturePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tessellation")
	FSoftObjectPath TerrainMaterialPath;
};

// ============================================================================
// EXAMPLE 3: Dynamic Water Surface
// ============================================================================

UCLASS(Blueprintable)
class GPURUNTIMETESSELLATION_API AGPUWaterSurface : public AActor
{
	GENERATED_BODY()

public:
	AGPUWaterSurface()
	{
		TessellationComponent = CreateDefaultSubobject<UGPUTessellationComponent>(TEXT("TessellationComponent"));
		RootComponent = TessellationComponent;

		// Configure for water
		TessellationComponent->TessellationSettings.TessellationFactor = 24;
		TessellationComponent->TessellationSettings.PlaneSizeX = 5000.0f;
		TessellationComponent->TessellationSettings.PlaneSizeY = 5000.0f;
		TessellationComponent->TessellationSettings.DisplacementIntensity = 50.0f;  // Wave height
		TessellationComponent->TessellationSettings.bUseSineWaveDisplacement = true;  // Procedural waves
		
		// Water-specific settings
		TessellationComponent->TessellationSettings.LODMode = EGPUTessellationLODMode::DistanceBased;
		TessellationComponent->TessellationSettings.MaxTessellationDistance = 3000.0f;
		TessellationComponent->bAutoUpdate = true;
	}

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Water")
	UGPUTessellationComponent* TessellationComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	float WaveSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	float WaveAmplitude = 1.0f;

	virtual void Tick(float DeltaTime) override
	{
		Super::Tick(DeltaTime);

		// Animate wave intensity over time
		float Time = GetWorld()->GetTimeSeconds();
		float AnimatedIntensity = 50.0f + FMath::Sin(Time * WaveSpeed) * 25.0f * WaveAmplitude;
		
		TessellationComponent->TessellationSettings.DisplacementIntensity = AnimatedIntensity;
		TessellationComponent->UpdateTessellatedMesh();
	}
};

// ============================================================================
// EXAMPLE 4: Runtime Configuration Actor
// ============================================================================

UCLASS(Blueprintable, BlueprintType)
class GPURUNTIMETESSELLATION_API AGPUTessellationController : public AActor
{
	GENERATED_BODY()

public:
	AGPUTessellationController()
	{
		PrimaryActorTick.bCanEverTick = true;
	}

	// Blueprint callable function to update tessellation settings
	UFUNCTION(BlueprintCallable, Category = "Tessellation")
	void UpdateTessellationSettings(
		AActor* TargetActor,
		float TessellationFactor,
		float DisplacementIntensity,
		bool bEnableLOD)
	{
		if (!TargetActor) return;

		UGPUTessellationComponent* TessComp = TargetActor->FindComponentByClass<UGPUTessellationComponent>();
		if (TessComp)
		{
			FGPUTessellationSettings NewSettings = TessComp->TessellationSettings;
			NewSettings.TessellationFactor = FMath::RoundToInt(TessellationFactor);
			NewSettings.DisplacementIntensity = DisplacementIntensity;
			NewSettings.LODMode = bEnableLOD ? EGPUTessellationLODMode::DistanceBased : EGPUTessellationLODMode::Disabled;
			
			TessComp->UpdateSettings(NewSettings);
		}
	}

	// Blueprint callable function to set textures
	UFUNCTION(BlueprintCallable, Category = "Tessellation")
	void SetTessellationTextures(
		AActor* TargetActor,
		UTexture* DisplacementTexture,
		UTexture* SubtractTexture)
	{
		if (!TargetActor) return;

		UGPUTessellationComponent* TessComp = TargetActor->FindComponentByClass<UGPUTessellationComponent>();
		if (TessComp)
		{
			if (DisplacementTexture)
			{
				TessComp->SetDisplacementTexture(DisplacementTexture);
			}
			if (SubtractTexture)
			{
				TessComp->SetSubtractTexture(SubtractTexture);
			}
		}
	}

	// Blueprint pure function to get tessellation stats
	UFUNCTION(BlueprintPure, Category = "Tessellation")
	void GetTessellationStats(
		AActor* TargetActor,
		int32& OutVertexCount,
		int32& OutTriangleCount,
		FIntPoint& OutResolution)
	{
		OutVertexCount = 0;
		OutTriangleCount = 0;
		OutResolution = FIntPoint::ZeroValue;

		if (!TargetActor) return;

		UGPUTessellationComponent* TessComp = TargetActor->FindComponentByClass<UGPUTessellationComponent>();
		if (TessComp)
		{
			OutVertexCount = TessComp->GetVertexCount();
			OutTriangleCount = TessComp->GetTriangleCount();
			OutResolution = TessComp->GetTessellationResolution();
		}
	}
};

/*
 * ============================================================================
 * BLUEPRINT USAGE EXAMPLES
 * ============================================================================
 * 
 * 1. Add Component to Actor:
 *    - Select an actor in the level
 *    - Click "Add Component" in Details panel
 *    - Search for "GPU Tessellation Component"
 *    - Add component
 * 
 * 2. Configure Settings:
 *    - In Details panel, expand "GPU Tessellation"
 *    - Set Tessellation Factor (1-128)
 *    - Set Plane Size X/Y (in cm)
 *    - Set Displacement Intensity
 *    - Enable/disable "Use Sine Wave Displacement"
 *    - Assign Displacement Texture (if not using sine wave)
 *    - Assign Material
 * 
 * 3. Dynamic LOD:
 *    - Expand "Tessellation Settings"
 *    - Set "LOD Mode" to "Distance Based"
 *    - Set "Max Tessellation Distance"
 *    - Set "Min Tessellation Factor"
 *    - Set "LOD Transition Speed" (higher = faster transitions)
 *    - Set "LOD Hysteresis" (prevents oscillation)
 * 
 * 4. Blueprint Functions:
 *    - UpdateTessellatedMesh() - Force update
 *    - SetDisplacementTexture(Texture2D) - Change texture
 *    - SetMaterial(MaterialInterface) - Change material
 *    - GetVertexCount() - Get current vertex count
 *    - GetTriangleCount() - Get current triangle count
 * 
 * ============================================================================
 * C++ QUICK START
 * ============================================================================
 * 
 * // 1. Include header
 * #include "GPUTessellationComponent.h"
 * 
 * // 2. Create component
 * UGPUTessellationComponent* TessComp = 
 *     CreateDefaultSubobject<UGPUTessellationComponent>(TEXT("Tessellation"));
 * 
 * // 3. Configure
 * TessComp->TessellationSettings.TessellationFactor = 32;
 * TessComp->TessellationSettings.DisplacementIntensity = 100.0f;
 * TessComp->bAutoUpdate = true;
 * 
 * // 4. Set textures (optional)
 * TessComp->DisplacementTexture = LoadObject<UTexture2D>(...);
 * TessComp->Material = LoadObject<UMaterialInterface>(...);
 * 
 * // 5. Done! Component updates automatically.
 * 
 * ============================================================================
 * PERFORMANCE TIPS
 * ============================================================================
 * 
 * 1. Start with low tessellation factor (8-16) and increase as needed
 * 2. Use dynamic LOD to reduce distant tessellation
 * 3. Set reasonable LOD update frequency (5-10 Hz)
 * 4. Use finite difference normal calculation for best performance
 * 5. Consider plane size - larger planes need more tessellation
 * 
 * Grid Size Reference:
 * - Factor 4  = 16×16   = 256 vertices    (very low detail)
 * - Factor 8  = 32×32   = 1,024 vertices  (low detail)
 * - Factor 16 = 64×64   = 4,096 vertices  (medium detail)
 * - Factor 32 = 128×128 = 16,384 vertices (high detail)
 * - Factor 64 = 256×256 = 65,536 vertices (very high detail)
 * 
 * ============================================================================
 */
