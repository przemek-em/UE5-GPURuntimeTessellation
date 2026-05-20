// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "GPUTessellationComponent.h"
#include "GPUOceanComponent.generated.h"

/**
 * Specialised tessellation component for procedural ocean surfaces.
 *
 * Inherits the full GPU tessellation pipeline (compute-shader vertex generation, displacement,
 * normals, LOD, patches, VSM hookup, etc.) and adds:
 *   - Per-tick time advance so wave animation is automatic.
 *   - Ocean-friendly defaults (Gerstner mode, larger plane, wind-aligned waves).
 *   - Wind direction / speed convenience controls that auto-populate a Gerstner spectrum
 *     when ocean settings have not been hand-edited.
 *
 * Usage: drop the component on any actor (or use as the root) and pick a WaveMode in the
 * Ocean settings. FFT mode runs a Tessendorf-style GPU IFFT pre-pass and feeds the generated
 * heightmap into the tessellation displacement and normal passes.
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent, DisplayName = "GPU Ocean"), hidecategories = (Object, LOD, Physics, Collision))
class GPURUNTIMETESSELLATION_API UGPUOceanComponent : public UGPUTessellationComponent
{
	GENERATED_BODY()

public:
	UGPUOceanComponent(const FObjectInitializer& ObjectInitializer);

	//~ Begin USceneComponent / UActorComponent interface
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;
	//~ End interface

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Wave generation model used by the displacement compute pass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean")
	EGPUOceanWaveMode WaveMode = EGPUOceanWaveMode::Gerstner;

	/** Wind direction in local XY. Used to auto-generate a Gerstner spectrum aligned with the wind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Wind")
	FVector2D WindDirection = FVector2D(1.0, 0.0);

	/** Wind speed (cm/s). FFT uses physically plausible storm-range wind by default (1200 cm/s = 12 m/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Wind", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "3000.0"))
	float WindSpeed = 1200.0f;

	/** Number of Gerstner waves to auto-generate (capped at GPU_OCEAN_MAX_GERSTNER_WAVES). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Wind", meta = (ClampMin = "1", ClampMax = "8"))
	int32 GeneratedGerstnerWaveCount = 6;

	/** Half-angle (degrees) the auto-generated wave directions can deviate from the wind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Wind", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float WaveDirectionalSpreadDegrees = 30.0f;

	/** Random seed for the auto-generated spectrum. Change to get different wave patterns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Wind")
	int32 SpectrumSeed = 1337;

	/** Time multiplier applied each tick. 0 freezes the surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean", meta = (ClampMin = "0.0"))
	float TimeScale = 1.0f;

	/** FFT physical tile period in cm. Larger values hide periodicity but need cascades for final AAA coverage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|FFT", meta = (ClampMin = "100.0", UIMin = "10000.0", UIMax = "500000.0", EditCondition = "WaveMode == EGPUOceanWaveMode::FFT", EditConditionHides))
	float FFTTileSize = 100000.0f;

	/** Master amplitude for the Phillips spectrum (taller waves at higher values). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|FFT", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "2.0", EditCondition = "WaveMode == EGPUOceanWaveMode::FFT", EditConditionHides))
	float FFTAmplitude = 0.35f;

	/** Horizontal displacement strength (0 = vertical-only, 1 = standard choppy Tessendorf crests). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|FFT", meta = (ClampMin = "0.0", ClampMax = "2.0", EditCondition = "WaveMode == EGPUOceanWaveMode::FFT", EditConditionHides))
	float FFTChoppiness = 1.0f;

	/** FFT motion styling. Current Tessendorf preserves the existing phase animation; Natural Sway adds rough-sea surge and retreat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|FFT", meta = (EditCondition = "WaveMode == EGPUOceanWaveMode::FFT", EditConditionHides))
	EGPUOceanFFTMotionMode FFTMotionMode = EGPUOceanFFTMotionMode::ClassicTessendorf;

	/** Strength of Natural Sway local wave-group domain warp and amplitude modulation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|FFT", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.5", EditCondition = "WaveMode == EGPUOceanWaveMode::FFT && FFTMotionMode == EGPUOceanFFTMotionMode::NaturalSway", EditConditionHides))
	float FFTSwayIntensity = 0.65f;

	/** Natural Sway cycle rate in cycles per second. Lower values feel like heavier swell sets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|FFT", meta = (ClampMin = "0.001", ClampMax = "0.5", UIMin = "0.01", UIMax = "0.12", EditCondition = "WaveMode == EGPUOceanWaveMode::FFT && FFTMotionMode == EGPUOceanFFTMotionMode::NaturalSway", EditConditionHides))
	float FFTSwayRate = 0.045f;

	/** Fraction of wind speed used to drift Natural Sway's local noise groups. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|FFT", meta = (ClampMin = "0.0", ClampMax = "0.2", UIMin = "0.0", UIMax = "0.08", EditCondition = "WaveMode == EGPUOceanWaveMode::FFT && FFTMotionMode == EGPUOceanFFTMotionMode::NaturalSway", EditConditionHides))
	float FFTSwayDrift = 0.03f;

	/** Regenerate the Gerstner spectrum from WindDirection / WindSpeed / SpectrumSeed. */
	UFUNCTION(BlueprintCallable, Category = "Ocean")
	void RegenerateGerstnerSpectrum();

protected:
	/** Copy the top-level ocean properties into TessellationSettings before render-state updates. */
	void SyncOceanSettingsFromProperties();

	/** Rebuild the Gerstner spectrum, optionally requesting a render update. */
	void RebuildGerstnerSpectrum(bool bRequestRenderUpdate);

	/** Apply ocean-friendly defaults (large plane, Gerstner mode, sane displacement intensity). */
	void ApplyOceanDefaults();
};
