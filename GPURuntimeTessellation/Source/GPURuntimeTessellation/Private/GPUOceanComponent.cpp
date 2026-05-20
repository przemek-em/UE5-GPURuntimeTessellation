// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUOceanComponent.h"
#include "Math/RandomStream.h"

UGPUOceanComponent::UGPUOceanComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Drive wave time per-tick.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
#if WITH_EDITOR
	bTickInEditor = true;
#endif

	// Animation requires the proxy to re-run the displacement compute pass every frame. The
	// base component only has a "rebuild whole render state" path for non-patch single-mesh
	// mode (UpdateTessellatedMesh -> MarkRenderStateDirty), so we enable bAutoUpdate so the
	// parent's TickComponent triggers it.
	//
	// PERF NOTE: this is HEAVY - it recreates the scene proxy, vertex buffers, and vertex
	// factory every frame. Acceptable for a single ocean surface but not for many of them.
	// TODO: add a lightweight "redispatch displacement+normals only" fast path through
	// FGPUTessellationDynamicData and switch the ocean component to use it. Until then this
	// is the only way to actually see waves move at runtime.
	bAutoUpdate = true;
	bEnableProceduralOceanSettings = true;
}

void UGPUOceanComponent::OnRegister()
{
	const bool bIsTemplateObject = IsTemplate();
	if (!bIsTemplateObject)
	{
		ApplyOceanDefaults();

		// Auto-generate the spectrum the first time the component is registered with no waves
		// configured - otherwise users see a flat plane until they understand the Wind controls.
		if (TessellationSettings.OceanSettings.GerstnerWaves.Num() == 0)
		{
			RebuildGerstnerSpectrum(false);
		}
	}
	else
	{
		SyncOceanSettingsFromProperties();
	}

	Super::OnRegister();
}

void UGPUOceanComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	SyncOceanSettingsFromProperties();

	// Advance ocean time. Wrap at 10000s so float precision stays decent indefinitely; wave
	// phases are derived modulo 2*PI in the shader so the wrap is invisible.
	if (WaveMode != EGPUOceanWaveMode::Disabled)
	{
		float& T = TessellationSettings.OceanSettings.Time;
		T += DeltaTime * FMath::Max(TimeScale, 0.0f);
		if (T > 10000.0f) { T -= 10000.0f; }
	}

	// Super::TickComponent runs the inherited LOD updates (when bAutoUpdate is on). It does NOT
	// rebuild the mesh on its own when LODMode is Disabled, so we explicitly request a rebuild
	// below for ocean animation.
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (WaveMode != EGPUOceanWaveMode::Disabled)
	{
		// Force a proxy rebuild this frame so the displacement compute pass re-runs with the
		// updated Time. See bAutoUpdate=true comment in the constructor for the perf caveat.
		UpdateTessellatedMesh();
	}
}

#if WITH_EDITOR
void UGPUOceanComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropName = PropertyChangedEvent.GetPropertyName();
	SyncOceanSettingsFromProperties();

	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Keep the internal settings struct in sync so shaders see the new mode immediately.
	if (PropName == GET_MEMBER_NAME_CHECKED(UGPUOceanComponent, WaveMode))
	{
		MarkRenderStateDirty();
	}

	const bool bWindEdited =
		PropName == GET_MEMBER_NAME_CHECKED(UGPUOceanComponent, WindDirection) ||
		PropName == GET_MEMBER_NAME_CHECKED(UGPUOceanComponent, WindSpeed) ||
		PropName == GET_MEMBER_NAME_CHECKED(UGPUOceanComponent, GeneratedGerstnerWaveCount) ||
		PropName == GET_MEMBER_NAME_CHECKED(UGPUOceanComponent, WaveDirectionalSpreadDegrees) ||
		PropName == GET_MEMBER_NAME_CHECKED(UGPUOceanComponent, SpectrumSeed);
	if (bWindEdited)
	{
		RegenerateGerstnerSpectrum();
	}
}
#endif

void UGPUOceanComponent::SyncOceanSettingsFromProperties()
{
	FGPUOceanSettings& Ocean = TessellationSettings.OceanSettings;
	Ocean.WaveMode = WaveMode;

	FVector2D SafeWind = WindDirection;
	const double WindLen = SafeWind.Size();
	if (WindLen > KINDA_SMALL_NUMBER)
	{
		SafeWind /= WindLen;
	}
	else
	{
		SafeWind = FVector2D(1.0, 0.0);
	}

	Ocean.Wind = SafeWind;
	Ocean.WindSpeed = WindSpeed;
	Ocean.FFTTileSize = FFTTileSize;
	Ocean.FFTAmplitude = FFTAmplitude;
	Ocean.FFTChoppiness = FFTChoppiness;
	Ocean.FFTMotionMode = FFTMotionMode;
	Ocean.FFTSwayIntensity = FFTSwayIntensity;
	Ocean.FFTSwayRate = FFTSwayRate;
	Ocean.FFTSwayDrift = FFTSwayDrift;
	Ocean.FFTSpectrumSeed = SpectrumSeed;
}

void UGPUOceanComponent::ApplyOceanDefaults()
{
	// Only fill in defaults that genuinely need ocean-friendly tuning. We intentionally do NOT
	// overwrite values that the user has clearly hand-edited (we cannot reliably detect this,
	// so we only seed values when they still match the base component defaults).
	FGPUTessellationSettings& S = TessellationSettings;
	SyncOceanSettingsFromProperties();
	const bool bLooksLikeLegacyFFTDefaults =
		FMath::IsNearlyEqual(WindSpeed, 200.0f) &&
		FMath::IsNearlyEqual(FFTTileSize, 5000.0f) &&
		FMath::IsNearlyEqual(FFTAmplitude, 1.0f) &&
		FMath::IsNearlyEqual(FFTChoppiness, 0.5f);
	if (bLooksLikeLegacyFFTDefaults)
	{
		WindSpeed = 1200.0f;
		FFTTileSize = 100000.0f;
		FFTAmplitude = 0.35f;
		FFTChoppiness = 1.0f;
	}
	// A 1x1 m plane makes no sense for an ocean. If still at base default (1000) bump to 500m.
	if (FMath::IsNearlyEqual(S.PlaneSizeX, 1000.0f)) { S.PlaneSizeX = 50000.0f; }
	if (FMath::IsNearlyEqual(S.PlaneSizeY, 1000.0f)) { S.PlaneSizeY = 50000.0f; }
	// Ocean defaults need enough vertices to show the 256x256 FFT map without looking faceted.
	if (S.TessellationFactor == 16) { S.TessellationFactor = 64; }
	// Geometry-based contribution matters once FFT horizontal displacement is enabled.
	if (S.NormalSmoothingFactor <= 0.3f) { S.NormalSmoothingFactor = 0.65f; }
	SyncOceanSettingsFromProperties();
}

void UGPUOceanComponent::RegenerateGerstnerSpectrum()
{
	RebuildGerstnerSpectrum(true);
}

void UGPUOceanComponent::RebuildGerstnerSpectrum(bool bRequestRenderUpdate)
{
	const int32 Count = FMath::Clamp(GeneratedGerstnerWaveCount, 1, GPU_OCEAN_MAX_GERSTNER_WAVES);
	const float SpreadRad = FMath::DegreesToRadians(WaveDirectionalSpreadDegrees);

	FVector2D WindDir = WindDirection;
	const double WindLen = WindDir.Size();
	if (WindLen > KINDA_SMALL_NUMBER) { WindDir /= WindLen; }
	else { WindDir = FVector2D(1.0, 0.0); }
	const float WindAngle = FMath::Atan2((float)WindDir.Y, (float)WindDir.X);

	// Deterministic spectrum so editor previews match runtime; reseed via SpectrumSeed.
	FRandomStream Rng(SpectrumSeed);

	TArray<FGPUOceanGerstnerWave>& Out = TessellationSettings.OceanSettings.GerstnerWaves;
	Out.Reset(Count);

	// Build a small, plausible spectrum: a few long swell waves + several shorter chop waves.
	// Wavelength range scales with wind speed so a fast wind produces longer dominant waves.
	const float BaseWavelength = FMath::Lerp(150.0f, 600.0f, FMath::Clamp(WindSpeed / 800.0f, 0.0f, 1.0f));
	for (int32 i = 0; i < Count; ++i)
	{
		// Wavelengths span roughly [0.4x .. 2.0x] of base, biased toward longer waves at low i.
		const float WavelengthScale = FMath::Lerp(2.0f, 0.4f, (float)i / FMath::Max(1, Count - 1));
		const float Wavelength = BaseWavelength * WavelengthScale * Rng.FRandRange(0.85f, 1.15f);

		// Deep-water dispersion: phase speed = sqrt(g * Wavelength / 2pi). Use UE units (cm),
		// g = 980 cm/s^2. Slightly biased toward windspeed to keep crests moving with the wind.
		const float DispersionSpeed = FMath::Sqrt(980.0f * Wavelength / (2.0f * PI));
		const float Speed = FMath::Lerp(DispersionSpeed, WindSpeed, 0.25f);

		// Amplitude rolls off with wavelength (Pierson-Moskowitz-ish, very simplified).
		const float Amplitude = FMath::Clamp(WindSpeed * 0.05f * (Wavelength / BaseWavelength), 5.0f, 80.0f);

		const float Theta = WindAngle + Rng.FRandRange(-SpreadRad, SpreadRad);
		FGPUOceanGerstnerWave W;
		W.Direction = FVector2D(FMath::Cos(Theta), FMath::Sin(Theta));
		W.Wavelength = Wavelength;
		W.Speed = Speed;
		W.Amplitude = Amplitude;
		W.Steepness = FMath::Lerp(0.2f, 0.5f, Rng.FRand());
		W.PhaseOffset = Rng.FRandRange(0.0f, 2.0f * PI);
		Out.Add(W);
	}

	if (bRequestRenderUpdate)
	{
		MarkCollisionMeshDirty();
		MarkRenderStateDirty();
	}
}
