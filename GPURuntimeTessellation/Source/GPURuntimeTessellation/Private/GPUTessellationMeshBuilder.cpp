// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUTessellationMeshBuilder.h"
#include "GPUTessellationComputeShaders.h"
#include "GPUTessellationComponent.h"
#include "GPUOceanFFTShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "SystemTextures.h"

FGPUTessellationMeshBuilder::FGPUTessellationMeshBuilder()
{
}

namespace
{
	bool ShouldDispatchGPUTessellationVertexNormalCalculation(const FGPUTessellationSettings& Settings, UTexture* DisplacementTexture)
	{
		if (Settings.NormalCalculationMethod == EGPUTessellationNormalMethod::Disabled)
		{
			return false;
		}

		// Pure height texture normal mode is handled in the vertex factory pixel shader when a
		// component height texture/render target is available. Keep the mesh normal buffer flat so
		// low-LOD vertex-normal interpolation cannot show through the generated height normal.
		if (Settings.NormalCalculationMethod == EGPUTessellationNormalMethod::FromHeightTexture && DisplacementTexture != nullptr)
		{
			return false;
		}

		return true;
	}

	int32 ApplyGPUTessellationSubdivisionMultiplier(int32 TessellationFactor, const FGPUTessellationSettings& Settings)
	{
		int32 EffectiveFactor = FMath::Clamp(TessellationFactor, 1, 1024);
		if (Settings.bSubdivideHardEdges)
		{
			const int32 Multiplier = FMath::Clamp(Settings.SubdivisionMultiplier, 2, 8);
			EffectiveFactor = FMath::Clamp(EffectiveFactor * Multiplier, 1, 1024);
		}
		return EffectiveFactor;
	}

	int32 GetPatchResolutionCap(const FGPUTessellationSettings& Settings)
	{
		return Settings.bSubdivideHardEdges ? 4097 : 1024;
	}

	bool HasActiveGPUTessellationVectorDisplacement(const FGPUTessellationSettings& Settings, UTexture* VectorDisplacementTexture)
	{
		return Settings.bUseVectorDisplacement && VectorDisplacementTexture != nullptr;
	}

	FGPUTessellationSettings GetGPUTessellationNormalSettings(const FGPUTessellationSettings& Settings, UTexture* VectorDisplacementTexture)
	{
		FGPUTessellationSettings NormalSettings = Settings;
		if (HasActiveGPUTessellationVectorDisplacement(Settings, VectorDisplacementTexture) &&
			NormalSettings.NormalCalculationMethod != EGPUTessellationNormalMethod::Disabled &&
			NormalSettings.NormalCalculationMethod != EGPUTessellationNormalMethod::FromNormalMap)
		{
			NormalSettings.NormalCalculationMethod = EGPUTessellationNormalMethod::GeometryBased;
		}
		return NormalSettings;
	}

	FVector GetGPUTessellationVectorDisplacementMaxAbsOffset(const FGPUTessellationSettings& Settings)
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

	float GetGPUTessellationScalarDisplacementBoundsRange(const FGPUTessellationSettings& Settings)
	{
		if (Settings.bUseVectorDisplacement && !Settings.bAddScalarHeightDisplacementToVector)
		{
			return 0.0f;
		}

		const float MaxDisplacementUp = Settings.DisplacementIntensity + FMath::Max(0.0f, Settings.DisplacementOffset);
		const float MaxDisplacementDown = FMath::Abs(FMath::Min(0.0f, Settings.DisplacementOffset));
		return MaxDisplacementUp + MaxDisplacementDown;
	}
}

FGPUTessellationMeshBuilder::~FGPUTessellationMeshBuilder()
{
}

namespace GPUTessellationOceanInternal
{
	// Pack the user-facing FGPUOceanGerstnerWave array into the two float4 arrays the shaders
	// consume. Used by both the displacement and normal CS dispatches so the two passes always
	// agree on the active waveform (otherwise normals lag behind geometry).
	template <typename TArrayA, typename TArrayB>
	static void PackGerstnerWaves(const FGPUOceanSettings& Ocean, TArrayA& OutPackA, TArrayB& OutPackB, uint32& OutCount)
	{
		const int32 Cap = GPU_OCEAN_MAX_GERSTNER_WAVES;
		const int32 Used = FMath::Min(Ocean.GerstnerWaves.Num(), Cap);
		for (int32 i = 0; i < Cap; ++i)
		{
			if (i < Used)
			{
				const FGPUOceanGerstnerWave& W = Ocean.GerstnerWaves[i];
				FVector2D Dir = W.Direction;
				const double Len = Dir.Size();
				if (Len > KINDA_SMALL_NUMBER) { Dir /= Len; } else { Dir = FVector2D(1.0, 0.0); }
				OutPackA[i] = FVector4f((float)Dir.X, (float)Dir.Y, FMath::Max(W.Wavelength, 1.0f), W.Speed);
				OutPackB[i] = FVector4f(W.Amplitude, FMath::Clamp(W.Steepness, 0.0f, 1.0f), W.PhaseOffset, 0.0f);
			}
			else
			{
				OutPackA[i] = FVector4f(1, 0, 1, 0);
				OutPackB[i] = FVector4f(0, 0, 0, 0);
			}
		}
		OutCount = (uint32)Used;
	}
}


FIntPoint FGPUTessellationMeshBuilder::CalculateResolution(float TessellationFactor) const
{
	// Single-mesh default cap: 4097 verts per edge when visual subdivision raises the effective
	// tessellation factor above the base UI cap. This is intentionally expensive and opt-in.
	return CalculateResolutionWithCap(TessellationFactor, 4097);
}

FIntPoint FGPUTessellationMeshBuilder::CalculateResolutionWithCap(float TessellationFactor, int32 MaxResolutionOverride) const
{
	// Convert tessellation factor to "segment" count so adjacent LODs share divisors.
	// Each factor step contributes 4 segments (matching historical density), then we add 1 vertex
	// to close the grid so seams can collapse cleanly between high/low detail edges.
	const int32 ThreadGroupSize = 8; // must match THREADGROUP_SIZE_X/Y in shaders
	const int32 MaxResolution = FMath::Max(ThreadGroupSize + 1, MaxResolutionOverride);
	const int32 MaxSegments = MaxResolution - 1;

	const int32 RoundedFactor = FMath::Max(1, FMath::RoundToInt(TessellationFactor));
	int32 DesiredSegments = RoundedFactor * 4;

	// Warn-once when the requested factor is silently clamped so users do not chase a UI value
	// that has no visible effect (the old behavior — cap was ~254 with MaxResolution=1024).
	static int32 LastWarnedFactor = 0;
	const int32 EffectiveFactorCap = MaxSegments / 4;
	if (RoundedFactor > EffectiveFactorCap && RoundedFactor != LastWarnedFactor)
	{
		LastWarnedFactor = RoundedFactor;
		UE_LOG(LogTemp, Warning,
			TEXT("GPUTessellation: TessellationFactor=%d exceeds effective cap (%d for MaxResolution=%d). Clamping."),
			RoundedFactor, EffectiveFactorCap, MaxResolution);
	}

	DesiredSegments = FMath::Clamp(DesiredSegments, ThreadGroupSize, MaxSegments);

	// Pad the segment count (not the vertex count) to the threadgroup size so compute dispatches stay aligned.
	int32 Segments = FMath::DivideAndRoundUp(DesiredSegments, ThreadGroupSize) * ThreadGroupSize;
	Segments = FMath::Clamp(Segments, ThreadGroupSize, MaxSegments);

	// Add the extra vertex necessary to close the grid, ensuring adjacent patch edges now line up exactly.
	const int32 Resolution = FMath::Min(Segments + 1, MaxResolution);

	return FIntPoint(Resolution, Resolution);
}

void FGPUTessellationMeshBuilder::ExecuteTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellatedMeshData& OutMeshData,
	const FIntVector4& EdgeCollapseFactors,
	int32 MaxResolutionOverride,
	UTexture* VectorDisplacementTexture)
{
	// Calculate resolution
	FIntPoint Resolution = CalculateResolutionWithCap(Settings.TessellationFactor, MaxResolutionOverride);
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Create RDG buffers
	FRDGBufferRef VertexBuffer = nullptr;
	FRDGBufferRef NormalBuffer = nullptr;
	FRDGBufferRef UVBuffer = nullptr;
	FRDGBufferRef IndexBuffer = nullptr;

	// Optional FFT ocean pre-pass: build the per-frame displacement map that the displacement and
	// normal compute shaders sample. Mirrors the patch pipeline branch in
	// ExecutePatchTessellationPipeline. Without this the single-mesh path silently produces a
	// flat plane in FFT mode because CachedOceanFFTDisplacementMap stays null.
	CachedOceanFFTDisplacementMap = nullptr;
	if (Settings.OceanSettings.WaveMode == EGPUOceanWaveMode::FFT)
	{
		CachedOceanFFTDisplacementMap = GPUOceanFFT::ExecutePipeline(
			GraphBuilder,
			(uint32)Settings.OceanSettings.FFTSpectrumSeed,
			Settings.OceanSettings.Time,
			Settings.OceanSettings.FFTTileSize,
			Settings.OceanSettings.WindSpeed,
			FVector2f((float)Settings.OceanSettings.Wind.X, (float)Settings.OceanSettings.Wind.Y),
			Settings.OceanSettings.FFTAmplitude,
			Settings.OceanSettings.FFTChoppiness,
			(uint32)Settings.OceanSettings.FFTMotionMode,
			Settings.OceanSettings.FFTSwayIntensity,
			Settings.OceanSettings.FFTSwayRate,
			Settings.OceanSettings.FFTSwayDrift);
	}

	// Step 1: Generate vertices
	// Single-mesh generation: no per-patch offset
	DispatchVertexGeneration(GraphBuilder, Settings, Resolution, LocalToWorld, FVector::ZeroVector, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 2: Apply displacement
	DispatchDisplacement(GraphBuilder, Settings, Resolution, LocalToWorld, DisplacementTexture, SubtractTexture, VectorDisplacementTexture, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 3: Calculate normals (if enabled)
	const FGPUTessellationSettings NormalSettings = GetGPUTessellationNormalSettings(Settings, VectorDisplacementTexture);
	if (ShouldDispatchGPUTessellationVertexNormalCalculation(NormalSettings, DisplacementTexture))
	{
		DispatchNormalCalculation(GraphBuilder, NormalSettings, Resolution, DisplacementTexture, SubtractTexture, NormalMapTexture, VertexBuffer, NormalBuffer, UVBuffer);
	}

	// Step 4: Generate indices. Single-mesh callers use no edge collapse; patch collision
	// readback can pass the same collapse factors as the visual patch renderer.
	DispatchIndexGeneration(GraphBuilder, Resolution, EdgeCollapseFactors, IndexBuffer);

	// Step 5: Extract mesh data to CPU
	ExtractMeshData(GraphBuilder, Resolution, VertexBuffer, NormalBuffer, UVBuffer, IndexBuffer, OutMeshData);
}

void FGPUTessellationMeshBuilder::GenerateMeshSync(
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	UTexture* DisplacementTexture,
	UTexture* RVTMaskTexture,
	FGPUTessellatedMeshData& OutMeshData,
	const FIntVector4& EdgeCollapseFactors,
	int32 MaxResolutionOverride,
	UTexture* VectorDisplacementTexture)
{
	ENQUEUE_RENDER_COMMAND(GenerateTessellatedMesh)(
		[this, Settings, LocalToWorld, CameraPosition, DisplacementTexture, RVTMaskTexture, VectorDisplacementTexture, &OutMeshData, EdgeCollapseFactors, MaxResolutionOverride](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			
			ExecuteTessellationPipeline(GraphBuilder, Settings, LocalToWorld, CameraPosition, DisplacementTexture, RVTMaskTexture, nullptr, OutMeshData, EdgeCollapseFactors, MaxResolutionOverride, VectorDisplacementTexture);
			
			GraphBuilder.Execute();
		});
	
	FlushRenderingCommands();
}

void FGPUTessellationMeshBuilder::DispatchVertexGeneration(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	FIntPoint Resolution,
	const FMatrix& LocalToWorld,
	const FVector& PatchLocalOffset,
	FRDGBufferRef& OutVertexBuffer,
	FRDGBufferRef& OutNormalBuffer,
	FRDGBufferRef& OutUVBuffer)
{
	int32 VertexCount = Resolution.X * Resolution.Y;

	// Create output buffers
	OutVertexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount),
		TEXT("GPUTessellation.VertexBuffer"));

	OutNormalBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount),
		TEXT("GPUTessellation.NormalBuffer"));

	OutUVBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), VertexCount),
		TEXT("GPUTessellation.UVBuffer"));

	// CRITICAL: zero the output buffers BEFORE vertex generation runs. RDG buffers come from
	// a transient pool whose memory is NOT guaranteed to be initialized; if vertex generation
	// fails to write any slot (e.g. an unaligned dispatch corner case), the renderer would
	// read whichever stale pool bytes were left there and produce a vertex placed at a random
	// position. After this clear, any unwritten slot becomes deterministically (0,0,0) -
	// which both makes the bug visible/diagnosable AND protects against pool-reuse hazards
	// during LOD-driven rebuilds (camera movement triggering back-to-back patch regenerations).
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutVertexBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutNormalBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutUVBuffer), 0);

	// Setup shader parameters
	FGPUVertexGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUVertexGenerationCS::FParameters>();
	PassParameters->ResolutionX = Resolution.X;
	PassParameters->ResolutionY = Resolution.Y;
	PassParameters->PlaneSizeX = Settings.PlaneSizeX;
	PassParameters->PlaneSizeY = Settings.PlaneSizeY;
	PassParameters->LocalToWorld = FMatrix44f(LocalToWorld);
	// PatchLocalOffset is added to generated local positions so that the primitive's LocalToWorld
	// (set via the primitive uniform buffer) correctly positions each patch in world space.
	PassParameters->PatchLocalOffset = FVector3f(PatchLocalOffset);
	// PatchUVOffset and PatchUVScale remap UVs for material continuity across patches
	// For single-mesh mode, use full UV range [0,1]
	PassParameters->PatchUVOffset = FVector2f(Settings.UVOffset.X, Settings.UVOffset.Y);
	PassParameters->PatchUVScale = FVector2f(Settings.UVScale.X, Settings.UVScale.Y);
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutVertexBuffer);
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(OutNormalBuffer);
	PassParameters->OutputUVs = GraphBuilder.CreateUAV(OutUVBuffer);

	// Dummy input buffers (not used in simple grid generation, but required by shader parameters)
	// Create them with ERDGBufferFlags::None so RDG knows they're optional
	FRDGBufferRef DummyVertexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 1),
		TEXT("GPUTessellation.DummyVertexBuffer"),
		ERDGBufferFlags::None);
	
	FRDGBufferRef DummyIndexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
		TEXT("GPUTessellation.DummyIndexBuffer"),
		ERDGBufferFlags::None);
	
	FRDGBufferRef DummyTessFactorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 1),
		TEXT("GPUTessellation.DummyTessFactorBuffer"),
		ERDGBufferFlags::None);

	// Clear dummy buffers to satisfy RDG validation
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyVertexBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyIndexBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyTessFactorBuffer), 0);

	PassParameters->InputVertices = GraphBuilder.CreateSRV(DummyVertexBuffer);
	PassParameters->InputIndices = GraphBuilder.CreateSRV(DummyIndexBuffer);
	PassParameters->TessellationFactors = GraphBuilder.CreateSRV(DummyTessFactorBuffer);

	// Get shader
	TShaderMapRef<FGPUVertexGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Linear 1D dispatch: one thread per vertex slot. Matches displacement/normal pattern.
	// 2D dispatch caused the corner thread to silently skip its 3 UAV writes on some drivers.
	FIntVector GroupCount(FMath::DivideAndRoundUp(VertexCount, 64), 1, 1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.GenerateVertices"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::DispatchDisplacement(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	FIntPoint Resolution,
	const FMatrix& LocalToWorld,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* VectorDisplacementTexture,
	FRDGBufferRef VertexBuffer,
	FRDGBufferRef NormalBuffer,
	FRDGBufferRef UVBuffer)
{
	int32 VertexCount = Resolution.X * Resolution.Y;

	// Get or create textures
	FRDGTextureRef DisplacementTextureRDG = DisplacementTexture ? 
		CreateRDGTextureFromUTexture(GraphBuilder, DisplacementTexture, TEXT("DisplacementTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	FRDGTextureRef SubtractTextureRDG = SubtractTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, SubtractTexture, TEXT("SubtractTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	const bool bHasVectorDisplacementTextureResource = VectorDisplacementTexture != nullptr && VectorDisplacementTexture->GetResource() != nullptr;
	FRDGTextureRef VectorDisplacementTextureRDG = bHasVectorDisplacementTextureResource ?
		CreateRDGTextureFromUTexture(GraphBuilder, VectorDisplacementTexture, TEXT("VectorDisplacementTexture")) :
		GetDefaultBlackTexture(GraphBuilder);

	// Setup shader parameters
	FGPUDisplacementCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUDisplacementCS::FParameters>();
	PassParameters->DisplacementIntensity = Settings.DisplacementIntensity;
	PassParameters->DisplacementOffset = Settings.DisplacementOffset;
	PassParameters->bUseSineWaveDisplacement = Settings.bUseSineWaveDisplacement ? 1 : 0;
	PassParameters->bHasRVTMask = (SubtractTexture != nullptr) ? 1 : 0;
	PassParameters->VertexCount = VertexCount;
	PassParameters->WorldToLocal = FMatrix44f(LocalToWorld.Inverse());
	PassParameters->bUseVectorDisplacement = (Settings.bUseVectorDisplacement && bHasVectorDisplacementTextureResource) ? 1 : 0;
	PassParameters->VectorDisplacementSpace = (uint32)Settings.VectorDisplacementSpace;
	PassParameters->VectorDisplacementDecodeMode = (uint32)Settings.VectorDisplacementDecodeMode;
	PassParameters->bUseVectorDisplacementAlphaAsStrength = Settings.bUseVectorDisplacementAlphaAsStrength ? 1 : 0;
	PassParameters->bAddScalarHeightDisplacementToVector = Settings.bAddScalarHeightDisplacementToVector ? 1 : 0;
	PassParameters->VectorDisplacementScale = FVector3f(
		(float)Settings.VectorDisplacementScale.X,
		(float)Settings.VectorDisplacementScale.Y,
		(float)Settings.VectorDisplacementScale.Z);
	PassParameters->VectorDisplacementBias = FVector3f(
		(float)Settings.VectorDisplacementBias.X,
		(float)Settings.VectorDisplacementBias.Y,
		(float)Settings.VectorDisplacementBias.Z);
	PassParameters->VectorDisplacementIntensity = Settings.VectorDisplacementIntensity;

	// Procedural ocean inputs. WaveMode == Disabled (0) makes the shader take the legacy
	// sine/texture path so existing content is unaffected.
	PassParameters->OceanWaveMode = (uint32)Settings.OceanSettings.WaveMode;
	PassParameters->OceanTime = Settings.OceanSettings.Time;
	PassParameters->OceanPlaneSizeX = Settings.PlaneSizeX;
	PassParameters->OceanPlaneSizeY = Settings.PlaneSizeY;
	GPUTessellationOceanInternal::PackGerstnerWaves(
		Settings.OceanSettings,
		PassParameters->OceanGerstnerPackA,
		PassParameters->OceanGerstnerPackB,
		PassParameters->OceanGerstnerWaveCount);
	PassParameters->OceanPerlinFrequency = Settings.OceanSettings.PerlinFrequency;
	PassParameters->OceanPerlinOctaves = (uint32)FMath::Clamp(Settings.OceanSettings.PerlinOctaves, 1, 8);
	PassParameters->OceanPerlinPersistence = Settings.OceanSettings.PerlinPersistence;
	PassParameters->OceanPerlinLacunarity = Settings.OceanSettings.PerlinLacunarity;
	PassParameters->OceanPerlinFlow = FVector2f((float)Settings.OceanSettings.PerlinFlowDirection.X * Settings.OceanSettings.PerlinFlowSpeed,
		                                          (float)Settings.OceanSettings.PerlinFlowDirection.Y * Settings.OceanSettings.PerlinFlowSpeed);

	// FFT displacement map: bind cached texture if available (mode==FFT), else black as a
	// placeholder SRV (the shader's mode!=2 branch never samples it).
	PassParameters->OceanFFTTileSize = Settings.OceanSettings.FFTTileSize;
	PassParameters->OceanFFTWindSpeed = Settings.OceanSettings.WindSpeed;
	PassParameters->OceanFFTWindDir = FVector2f((float)Settings.OceanSettings.Wind.X, (float)Settings.OceanSettings.Wind.Y);
	PassParameters->OceanFFTMotionMode = (uint32)Settings.OceanSettings.FFTMotionMode;
	PassParameters->OceanFFTSwayIntensity = Settings.OceanSettings.FFTSwayIntensity;
	PassParameters->OceanFFTSwayRate = Settings.OceanSettings.FFTSwayRate;
	PassParameters->OceanFFTSwayDrift = Settings.OceanSettings.FFTSwayDrift;
	{
		FRDGTextureRef FFTTex = CachedOceanFFTDisplacementMap ? CachedOceanFFTDisplacementMap : GetDefaultBlackTexture(GraphBuilder);
		PassParameters->OceanFFTDisplacementMap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(FFTTex));
		PassParameters->OceanFFTDisplacementMapSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Wrap, AM_Clamp>::GetRHI();
	}

	PassParameters->UVOffset = Settings.UVOffset; // For patch rendering
	PassParameters->UVScale = Settings.UVScale;   // For patch rendering
	PassParameters->DisplacementTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisplacementTextureRDG));
	PassParameters->VectorDisplacementTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VectorDisplacementTextureRDG));
	PassParameters->RVTMaskTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SubtractTextureRDG));
	PassParameters->RVTMaskSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	// Position buffer is bound as UAV only; the displacement shader reads its own
	// slot from OutputPositions before writing, which avoids aliasing the same
	// resource as both SRV and UAV in a single RDG pass.
	PassParameters->InputNormals = GraphBuilder.CreateSRV(NormalBuffer);
	PassParameters->InputUVs = GraphBuilder.CreateSRV(UVBuffer);
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(VertexBuffer);

	// Get shader
	TShaderMapRef<FGPUDisplacementCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch size
	FIntVector GroupCount(FMath::DivideAndRoundUp(VertexCount, 64), 1, 1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.ApplyDisplacement"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::DispatchNormalCalculation(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	FIntPoint Resolution,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FRDGBufferRef VertexBuffer,
	FRDGBufferRef NormalBuffer,
	FRDGBufferRef UVBuffer)
{
	int32 VertexCount = Resolution.X * Resolution.Y;

	// Get displacement texture
	FRDGTextureRef DisplacementTextureRDG = DisplacementTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, DisplacementTexture, TEXT("DisplacementTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	// Get subtract/mask texture (NEW - for correct normal calculation)
	FRDGTextureRef SubtractTextureRDG = SubtractTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, SubtractTexture, TEXT("SubtractTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	// Get normal map texture (optional - only used when NormalCalculationMethod == FromNormalMap)
	FRDGTextureRef NormalMapTextureRDG = NormalMapTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, NormalMapTexture, TEXT("NormalMapTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	// Dummy index buffer for normal calculation (not actually used in grid-based normals)
	FRDGBufferRef DummyIndexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
		TEXT("GPUTessellation.DummyIndexBuffer"),
		ERDGBufferFlags::None);
	
	// Clear dummy buffer to satisfy RDG validation
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyIndexBuffer), 0);

	// Setup shader parameters
	FGPUNormalCalculationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUNormalCalculationCS::FParameters>();
	PassParameters->NormalCalculationMethod = (uint32)Settings.NormalCalculationMethod;
	PassParameters->NormalSmoothingFactor = Settings.NormalSmoothingFactor;
	PassParameters->NormalIntensity = Settings.NormalIntensity;
	PassParameters->HeightTextureNormalDetailStrength = Settings.HeightTextureNormalDetailStrength;
	PassParameters->HeightTextureNormalTexelStep = Settings.HeightTextureNormalTexelStep;
	PassParameters->bInvertNormals = Settings.bInvertNormals ? 1 : 0;
	PassParameters->VertexCount = VertexCount;
	PassParameters->ResolutionX = Resolution.X;
	PassParameters->ResolutionY = Resolution.Y;
	PassParameters->NormalUVStep = FVector2f(
		FMath::Max(Settings.UVScale.X, 1.0e-6f) / static_cast<float>(FMath::Max(1, Resolution.X - 1)),
		FMath::Max(Settings.UVScale.Y, 1.0e-6f) / static_cast<float>(FMath::Max(1, Resolution.Y - 1)));
	PassParameters->PlaneSizeX = Settings.PlaneSizeX;
	PassParameters->PlaneSizeY = Settings.PlaneSizeY;
	// FIX: keep normal sampling in sync with the displacement pass when the procedural sine
	// wave path is active (otherwise FiniteDifference/Hybrid sample DisplacementTexture which
	// is unrelated to the actual displaced positions, and the sine plane renders flat).
	PassParameters->bUseSineWaveDisplacement = Settings.bUseSineWaveDisplacement ? 1 : 0;

	// Mirror the same procedural ocean inputs as the displacement pass so finite-difference
	// and hybrid normals track the same waveform - otherwise the ocean surface lights as flat.
	PassParameters->OceanWaveMode = (uint32)Settings.OceanSettings.WaveMode;
	PassParameters->OceanTime = Settings.OceanSettings.Time;
	GPUTessellationOceanInternal::PackGerstnerWaves(
		Settings.OceanSettings,
		PassParameters->OceanGerstnerPackA,
		PassParameters->OceanGerstnerPackB,
		PassParameters->OceanGerstnerWaveCount);
	PassParameters->OceanPerlinFrequency = Settings.OceanSettings.PerlinFrequency;
	PassParameters->OceanPerlinOctaves = (uint32)FMath::Clamp(Settings.OceanSettings.PerlinOctaves, 1, 8);
	PassParameters->OceanPerlinPersistence = Settings.OceanSettings.PerlinPersistence;
	PassParameters->OceanPerlinLacunarity = Settings.OceanSettings.PerlinLacunarity;
	PassParameters->OceanPerlinFlow = FVector2f((float)Settings.OceanSettings.PerlinFlowDirection.X * Settings.OceanSettings.PerlinFlowSpeed,
		                                          (float)Settings.OceanSettings.PerlinFlowDirection.Y * Settings.OceanSettings.PerlinFlowSpeed);

	// FFT displacement map (mirror of displacement pass so finite-difference normals agree).
	PassParameters->OceanFFTTileSize = Settings.OceanSettings.FFTTileSize;
	PassParameters->OceanFFTWindSpeed = Settings.OceanSettings.WindSpeed;
	PassParameters->OceanFFTWindDir = FVector2f((float)Settings.OceanSettings.Wind.X, (float)Settings.OceanSettings.Wind.Y);
	PassParameters->OceanFFTMotionMode = (uint32)Settings.OceanSettings.FFTMotionMode;
	PassParameters->OceanFFTSwayIntensity = Settings.OceanSettings.FFTSwayIntensity;
	PassParameters->OceanFFTSwayRate = Settings.OceanSettings.FFTSwayRate;
	PassParameters->OceanFFTSwayDrift = Settings.OceanSettings.FFTSwayDrift;
	{
		FRDGTextureRef FFTTex = CachedOceanFFTDisplacementMap ? CachedOceanFFTDisplacementMap : GetDefaultBlackTexture(GraphBuilder);
		PassParameters->OceanFFTDisplacementMap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(FFTTex));
		PassParameters->OceanFFTDisplacementMapSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Wrap, AM_Clamp>::GetRHI();
	}

	PassParameters->DisplacementTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisplacementTextureRDG));
	PassParameters->DisplacementIntensity = Settings.DisplacementIntensity;
	// Subtract/mask texture parameters (NEW - for correct normals with RVT)
	PassParameters->bHasSubtractTexture = (SubtractTexture != nullptr) ? 1 : 0;
	PassParameters->SubtractTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SubtractTextureRDG));
	PassParameters->SubtractSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	// Normal map texture parameters
	PassParameters->NormalMapTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalMapTextureRDG));
	PassParameters->NormalMapSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->InputPositions = GraphBuilder.CreateSRV(VertexBuffer);
	PassParameters->InputUVs = GraphBuilder.CreateSRV(UVBuffer);
	PassParameters->InputIndices = GraphBuilder.CreateSRV(DummyIndexBuffer);
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(NormalBuffer);

	// Get shader
	TShaderMapRef<FGPUNormalCalculationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch size
	FIntVector GroupCount(FMath::DivideAndRoundUp(VertexCount, 64), 1, 1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.CalculateNormals"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::DispatchIndexGeneration(
	FRDGBuilder& GraphBuilder,
	FIntPoint Resolution,
	const FIntVector4& EdgeCollapseFactors,
	FRDGBufferRef& OutIndexBuffer)
{
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Create output buffer as a typed buffer with IndexBuffer usage for proper binding
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), IndexCount);
		// Ensure we can write via UAV and bind as an index buffer for drawing
		Desc.Usage |= EBufferUsageFlags::UnorderedAccess;
		Desc.Usage |= EBufferUsageFlags::IndexBuffer;
		OutIndexBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("GPUTessellation.IndexBuffer"));
	}

	// Pre-clear: RDG transient pool memory is uninitialized. If the index gen shader ever fails
	// to write a slot, the leftover bytes would be interpreted as a vertex index and produce a
	// triangle pointing at a random vertex (massive visual artifact). Clearing to 0 makes any
	// missing write deterministic - degenerate triangles all referencing vertex 0.
	const int32 QuadCount = FMath::Max(0, (Resolution.X - 1) * (Resolution.Y - 1));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutIndexBuffer, PF_R32_UINT)), 0u);

	// Setup shader parameters
	FGPUIndexGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUIndexGenerationCS::FParameters>();
	PassParameters->ResolutionX = Resolution.X;
	PassParameters->ResolutionY = Resolution.Y;
	PassParameters->EdgeCollapseFactors = EdgeCollapseFactors;
	// Create a typed UAV (R32_UINT) to match RWBuffer<uint> in the shader
	PassParameters->OutputIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutIndexBuffer, PF_R32_UINT));

	// Get shader
	TShaderMapRef<FGPUIndexGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Linear 1D dispatch: one thread per quad. Matches vertex/displacement/normal pattern.
	// 2D dispatch caused the corner thread (last X, last Y) to silently skip its 6 index
	// writes on some drivers, leaving degenerate triangles referencing vertex 0.
	FIntVector GroupCount(FMath::DivideAndRoundUp(QuadCount, 64), 1, 1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.GenerateIndices"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::ExtractMeshData(
	FRDGBuilder& GraphBuilder,
	FIntPoint Resolution,
	FRDGBufferRef VertexBuffer,
	FRDGBufferRef NormalBuffer,
	FRDGBufferRef UVBuffer,
	FRDGBufferRef IndexBuffer,
	FGPUTessellatedMeshData& OutMeshData)
{
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Prepare output arrays
	OutMeshData.Reset();
	OutMeshData.Vertices.SetNumUninitialized(VertexCount);
	OutMeshData.Normals.SetNumUninitialized(VertexCount);
	OutMeshData.UVs.SetNumUninitialized(VertexCount);
	OutMeshData.Indices.SetNumUninitialized(IndexCount);
	OutMeshData.ResolutionX = Resolution.X;
	OutMeshData.ResolutionY = Resolution.Y;

	// Create staging buffers for CPU readback
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VertexReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("NormalReadback"));
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("UVReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("IndexReadback"));

	// Enqueue copy operations
	AddEnqueueCopyPass(GraphBuilder, VertexReadback, VertexBuffer, sizeof(FVector3f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, NormalReadback, NormalBuffer, sizeof(FVector3f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, UVReadback, UVBuffer, sizeof(FVector2f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, IndexReadback, IndexBuffer, sizeof(uint32) * IndexCount);

	// Add pass to extract data after GPU work completes
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("ExtractTessellationData"),
		ERDGPassFlags::None,
		[VertexReadback, NormalReadback, UVReadback, IndexReadback, &OutMeshData, VertexCount, IndexCount](FRHICommandListImmediate& RHICmdList)
		{
			// Wait for GPU to finish
			RHICmdList.BlockUntilGPUIdle();

			// Copy vertices
			if (const void* VertexData = VertexReadback->Lock(sizeof(FVector3f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.Vertices.GetData(), VertexData, sizeof(FVector3f) * VertexCount);
				VertexReadback->Unlock();
			}

			// Copy normals
			if (const void* NormalData = NormalReadback->Lock(sizeof(FVector3f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.Normals.GetData(), NormalData, sizeof(FVector3f) * VertexCount);
				NormalReadback->Unlock();
			}

			// Copy UVs
			if (const void* UVData = UVReadback->Lock(sizeof(FVector2f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.UVs.GetData(), UVData, sizeof(FVector2f) * VertexCount);
				UVReadback->Unlock();
			}

			// Copy indices
			if (const void* IndexData = IndexReadback->Lock(sizeof(uint32) * IndexCount))
			{
				FMemory::Memcpy(OutMeshData.Indices.GetData(), IndexData, sizeof(uint32) * IndexCount);
				IndexReadback->Unlock();
			}

			// Cleanup
			delete VertexReadback;
			delete NormalReadback;
			delete UVReadback;
			delete IndexReadback;
		});
}

FRDGTextureRef FGPUTessellationMeshBuilder::CreateRDGTextureFromUTexture(
	FRDGBuilder& GraphBuilder,
	UTexture* Texture,
	const TCHAR* Name)
{
	if (!Texture || !Texture->GetResource())
	{
		return GetDefaultWhiteTexture(GraphBuilder);
	}

	FTextureResource* TextureResource = Texture->GetResource();
	FRHITexture* RHITexture = TextureResource->TextureRHI;

	return GraphBuilder.RegisterExternalTexture(CreateRenderTarget(RHITexture, Name));
}

FRDGTextureRef FGPUTessellationMeshBuilder::GetDefaultWhiteTexture(FRDGBuilder& GraphBuilder)
{
	// Use system white texture
	return GSystemTextures.GetWhiteDummy(GraphBuilder);
}

FRDGTextureRef FGPUTessellationMeshBuilder::GetDefaultBlackTexture(FRDGBuilder& GraphBuilder)
{
	// Use system black texture for vector displacement fallbacks (zero offset).
	return GSystemTextures.GetBlackDummy(GraphBuilder);
}

void FGPUTessellationMeshBuilder::ExecuteTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellationBuffers& OutGPUBuffers,
	UTexture* VectorDisplacementTexture)
{
	// Calculate resolution
	FIntPoint Resolution = CalculateResolution(Settings.TessellationFactor);
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Create RDG buffers for compute shaders
	FRDGBufferRef VertexBuffer = nullptr;
	FRDGBufferRef NormalBuffer = nullptr;
	FRDGBufferRef UVBuffer = nullptr;
	FRDGBufferRef IndexBuffer = nullptr;

	// Optional FFT ocean pre-pass: build the per-frame displacement map that the displacement and
	// normal compute shaders sample. This is the GPU-buffer-output overload used by the
	// SceneProxy single-mesh path; without this branch CachedOceanFFTDisplacementMap stays null
	// and FFT mode silently produces a flat plane.
	CachedOceanFFTDisplacementMap = nullptr;
	if (Settings.OceanSettings.WaveMode == EGPUOceanWaveMode::FFT)
	{
		CachedOceanFFTDisplacementMap = GPUOceanFFT::ExecutePipeline(
			GraphBuilder,
			(uint32)Settings.OceanSettings.FFTSpectrumSeed,
			Settings.OceanSettings.Time,
			Settings.OceanSettings.FFTTileSize,
			Settings.OceanSettings.WindSpeed,
			FVector2f((float)Settings.OceanSettings.Wind.X, (float)Settings.OceanSettings.Wind.Y),
			Settings.OceanSettings.FFTAmplitude,
			Settings.OceanSettings.FFTChoppiness,
			(uint32)Settings.OceanSettings.FFTMotionMode,
			Settings.OceanSettings.FFTSwayIntensity,
			Settings.OceanSettings.FFTSwayRate,
			Settings.OceanSettings.FFTSwayDrift);
	}

	// Step 1: Generate vertices
	// Single-mesh generation: no per-patch offset
	DispatchVertexGeneration(GraphBuilder, Settings, Resolution, LocalToWorld, FVector::ZeroVector, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 2: Apply displacement
	DispatchDisplacement(GraphBuilder, Settings, Resolution, LocalToWorld, DisplacementTexture, SubtractTexture, VectorDisplacementTexture, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 3: Calculate normals (if enabled)
	const FGPUTessellationSettings NormalSettings = GetGPUTessellationNormalSettings(Settings, VectorDisplacementTexture);
	if (ShouldDispatchGPUTessellationVertexNormalCalculation(NormalSettings, DisplacementTexture))
	{
		DispatchNormalCalculation(GraphBuilder, NormalSettings, Resolution, DisplacementTexture, SubtractTexture, NormalMapTexture, VertexBuffer, NormalBuffer, UVBuffer);
	}

	// Step 4: Generate indices (no edge collapsing needed for single mesh)
	DispatchIndexGeneration(GraphBuilder, Resolution, FIntVector4(1, 1, 1, 1), IndexBuffer);

	// Step 5: Create persistent RHI buffers (no CPU readback!)
	// Extract buffers to persistent pooled buffers
	
	// Store metadata
	OutGPUBuffers.VertexCount = VertexCount;
	OutGPUBuffers.IndexCount = IndexCount;
	OutGPUBuffers.ResolutionX = Resolution.X;
	OutGPUBuffers.ResolutionY = Resolution.Y;
	
	// Convert to external pooled buffers
	TRefCountPtr<FRDGPooledBuffer> PooledPositionBuffer = GraphBuilder.ConvertToExternalBuffer(VertexBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledNormalBuffer = GraphBuilder.ConvertToExternalBuffer(NormalBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledUVBuffer = GraphBuilder.ConvertToExternalBuffer(UVBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledIndexBuffer = GraphBuilder.ConvertToExternalBuffer(IndexBuffer);

	// CRITICAL: store the pooled-buffer wrappers persistently so the RDG transient pool
	// cannot recycle our underlying RHI buffers. Without these refs, the pool considers
	// the slots free as soon as the lambda below finishes capturing-by-value lifetime
	// expires, and any subsequent RDG pass (Lumen, VSM, post-process, anything) is
	// allowed to reuse the same RHI buffer - overwriting our position/normal/UV/index
	// data while our SRVs still point at it. Symptom: mesh starts correct then border
	// vertices drift / disappear over a few frames as pool slots get reused.
	OutGPUBuffers.PooledPositionBuffer = PooledPositionBuffer;
	OutGPUBuffers.PooledNormalBuffer = PooledNormalBuffer;
	OutGPUBuffers.PooledUVBuffer = PooledUVBuffer;
	OutGPUBuffers.PooledIndexBuffer = PooledIndexBuffer;

	// After graph execution, extract the RHI buffers and create SRVs
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("CreateGPUBufferSRVs"),
		ERDGPassFlags::None,
		[&OutGPUBuffers, PooledPositionBuffer, PooledNormalBuffer, PooledUVBuffer, PooledIndexBuffer](FRHICommandList& RHICmdList)
		{
			// Extract RHI buffers from pooled buffers
			if (PooledPositionBuffer.IsValid())
			{
				OutGPUBuffers.PositionBuffer = PooledPositionBuffer->GetRHI();
				
				// Create structured buffer SRV for float3 position data
				OutGPUBuffers.PositionSRV = RHICmdList.CreateShaderResourceView(
					OutGPUBuffers.PositionBuffer,
					FRHIViewDesc::CreateBufferSRV()
						.SetType(FRHIViewDesc::EBufferType::Structured)
				);
			}
			
			if (PooledNormalBuffer.IsValid())
			{
				OutGPUBuffers.NormalBuffer = PooledNormalBuffer->GetRHI();
				
				// Create structured buffer SRV for float3 normal data
				OutGPUBuffers.NormalSRV = RHICmdList.CreateShaderResourceView(
					OutGPUBuffers.NormalBuffer,
					FRHIViewDesc::CreateBufferSRV()
						.SetType(FRHIViewDesc::EBufferType::Structured)
				);
			}
			
			if (PooledUVBuffer.IsValid())
			{
				OutGPUBuffers.UVBuffer = PooledUVBuffer->GetRHI();
				
				// Create structured buffer SRV for float2 UV data
				OutGPUBuffers.UVSRV = RHICmdList.CreateShaderResourceView(
					OutGPUBuffers.UVBuffer,
					FRHIViewDesc::CreateBufferSRV()
						.SetType(FRHIViewDesc::EBufferType::Structured)
				);
			}
			
			// Set up index buffer wrapper for mesh rendering
			if (PooledIndexBuffer.IsValid())
			{
				OutGPUBuffers.IndexBufferRHI = PooledIndexBuffer->GetRHI();
				// Set the base class IndexBufferRHI member for rendering
				OutGPUBuffers.IndexBuffer.IndexBufferRHI = OutGPUBuffers.IndexBufferRHI;
				
				// Initialize the index buffer as a render resource if not already initialized
				if (!OutGPUBuffers.IndexBuffer.IsInitialized())
				{
					OutGPUBuffers.IndexBuffer.InitResource(RHICmdList);
				}
			}
			
			// Debug logging removed - too verbose, enable in SceneProxy if needed
		});
	
	// Tessellation pipeline scheduled (verbose logging removed for performance)
}

// ============================================================================
// SPATIAL PATCH SYSTEM IMPLEMENTATION
// ============================================================================

void FGPUTessellationMeshBuilder::CalculatePatchState(
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	const FConvexVolume* ViewFrustum,
	int32 PatchCountX,
	int32 PatchCountY,
	TArray<FGPUTessellationPatchInfo>& OutPatchInfo) const
{
	CalculatePatchInfo(Settings, LocalToWorld, CameraPosition, ViewFrustum, PatchCountX, PatchCountY, OutPatchInfo);
	if (Settings.bEnablePatchEdgeStitching)
	{
		ComputePatchEdgeTransitions(PatchCountX, PatchCountY, OutPatchInfo);
	}
	else
	{
		for (FGPUTessellationPatchInfo& Patch : OutPatchInfo)
		{
			Patch.EdgeCollapseFactors = FIntVector4(1, 1, 1, 1);
		}
	}
}

void FGPUTessellationMeshBuilder::CalculateQuadtreePatchState(
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	const FConvexVolume* ViewFrustum,
	TArray<FGPUTessellationPatchInfo>& OutPatchInfo) const
{
	OutPatchInfo.Reset();

	const int32 RootTileCountX = FMath::Clamp(Settings.QuadtreeRootTileCountX, 1, 8);
	const int32 RootTileCountY = FMath::Clamp(Settings.QuadtreeRootTileCountY, 1, 8);
	const int32 MaxDepth = FMath::Clamp(Settings.QuadtreeMaxDepth, 0, 8);
	const int32 MaxLeaves = FMath::Clamp(Settings.QuadtreeMaxVisibleLeaves, 1, 2048);
	const float PlaneSizeX = Settings.PlaneSizeX;
	const float PlaneSizeY = Settings.PlaneSizeY;
	const float Epsilon = 1.0e-5f;

	auto BuildLeafInfo = [&](const FVector2f& PatchOffset, const FVector2f& PatchSize, int32 Depth) -> FGPUTessellationPatchInfo
	{
		FGPUTessellationPatchInfo Leaf;
		Leaf.PatchOffset = PatchOffset;
		Leaf.PatchSize = PatchSize;
		Leaf.PatchIndexX = OutPatchInfo.Num();
		Leaf.PatchIndexY = Depth;
		Leaf.QuadtreeDepth = Depth;
		Leaf.EdgeCollapseFactors = FIntVector4(1, 1, 1, 1);

		const float PatchLocalSizeX = PatchSize.X * PlaneSizeX;
		const float PatchLocalSizeY = PatchSize.Y * PlaneSizeY;
		const float LocalMinX = (PatchOffset.X - 0.5f) * PlaneSizeX;
		const float LocalMinY = (PatchOffset.Y - 0.5f) * PlaneSizeY;
		const float LocalCenterX = LocalMinX + (PatchLocalSizeX * 0.5f);
		const float LocalCenterY = LocalMinY + (PatchLocalSizeY * 0.5f);
		const float LocalCenterZ = Settings.bUseVectorDisplacement && !Settings.bAddScalarHeightDisplacementToVector
			? 0.0f
			: Settings.DisplacementOffset + (Settings.DisplacementIntensity * 0.5f);
		const FVector LocalCenter(LocalCenterX, LocalCenterY, LocalCenterZ);
		Leaf.WorldCenter = LocalToWorld.TransformPosition(LocalCenter);

		const FVector VectorDisplacementExtent = GetGPUTessellationVectorDisplacementMaxAbsOffset(Settings);
		const float TotalDisplacementRange = GetGPUTessellationScalarDisplacementBoundsRange(Settings);
		const FVector HalfExtent(
			PatchLocalSizeX * 0.5f + VectorDisplacementExtent.X,
			PatchLocalSizeY * 0.5f + VectorDisplacementExtent.Y,
			TotalDisplacementRange * 0.5f + VectorDisplacementExtent.Z);

		TArray<FVector> Corners;
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z)));
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, -HalfExtent.Y, +HalfExtent.Z)));
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, +HalfExtent.Y, -HalfExtent.Z)));
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, +HalfExtent.Y, +HalfExtent.Z)));
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z)));
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, -HalfExtent.Y, +HalfExtent.Z)));
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, +HalfExtent.Y, -HalfExtent.Z)));
		Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, +HalfExtent.Y, +HalfExtent.Z)));
		Leaf.WorldBounds = FBox(Corners);

		int32 LODIndex = 0;
		const float Distance = FVector::Dist(Leaf.WorldCenter, CameraPosition);
		Leaf.TessellationLevel = CalculateQuadtreeTessellationLevel(Distance, Settings, LODIndex);
		const FIntPoint LeafResolution = CalculateResolutionWithCap(static_cast<float>(Leaf.TessellationLevel), GetPatchResolutionCap(Settings));
		Leaf.ResolutionX = LeafResolution.X;
		Leaf.ResolutionY = LeafResolution.Y;
		Leaf.QuadtreeNodeIndex = static_cast<int32>(HashCombine(
			GetTypeHash(FMath::RoundToInt(PatchOffset.X * 65535.0f)),
			HashCombine(GetTypeHash(FMath::RoundToInt(PatchOffset.Y * 65535.0f)), GetTypeHash(Depth))) & 0x7fffffff);

		if (ViewFrustum != nullptr && Settings.bEnableQuadtreeCulling)
		{
			Leaf.bVisible = ViewFrustum->IntersectBox(Leaf.WorldCenter, Leaf.WorldBounds.GetExtent());
		}
		else
		{
			Leaf.bVisible = true;
		}

		return Leaf;
	};

	TFunction<void(const FVector2f&, const FVector2f&, int32)> BuildNode;
	BuildNode = [&](const FVector2f& PatchOffset, const FVector2f& PatchSize, int32 Depth)
	{
		if (OutPatchInfo.Num() >= MaxLeaves)
		{
			return;
		}

		FGPUTessellationPatchInfo CandidateLeaf = BuildLeafInfo(PatchOffset, PatchSize, Depth);
		int32 LODIndex = 0;
		CalculateQuadtreeTessellationLevel(FVector::Dist(CandidateLeaf.WorldCenter, CameraPosition), Settings, LODIndex);
		const int32 TargetDepth = FMath::Clamp(MaxDepth - LODIndex, 0, MaxDepth);
		const bool bCanSplit = Depth < TargetDepth && (OutPatchInfo.Num() + 4) <= MaxLeaves;

		if (!bCanSplit)
		{
			CandidateLeaf.PatchIndexX = OutPatchInfo.Num();
			CandidateLeaf.PatchIndexY = Depth;
			OutPatchInfo.Add(CandidateLeaf);
			return;
		}

		const FVector2f ChildSize(PatchSize.X * 0.5f, PatchSize.Y * 0.5f);
		BuildNode(PatchOffset, ChildSize, Depth + 1);
		BuildNode(FVector2f(PatchOffset.X + ChildSize.X, PatchOffset.Y), ChildSize, Depth + 1);
		BuildNode(FVector2f(PatchOffset.X, PatchOffset.Y + ChildSize.Y), ChildSize, Depth + 1);
		BuildNode(FVector2f(PatchOffset.X + ChildSize.X, PatchOffset.Y + ChildSize.Y), ChildSize, Depth + 1);
	};

	const FVector2f RootSize(1.0f / static_cast<float>(RootTileCountX), 1.0f / static_cast<float>(RootTileCountY));
	for (int32 RootY = 0; RootY < RootTileCountY; ++RootY)
	{
		for (int32 RootX = 0; RootX < RootTileCountX; ++RootX)
		{
			BuildNode(FVector2f(RootX * RootSize.X, RootY * RootSize.Y), RootSize, 0);
		}
	}

	auto IntervalOverlap = [](float A0, float A1, float B0, float B1) -> float
	{
		return FMath::Max(0.0f, FMath::Min(A1, B1) - FMath::Max(A0, B0));
	};

	auto IsNearlyTouching = [Epsilon](float A, float B) -> bool
	{
		return FMath::Abs(A - B) <= Epsilon;
	};

	auto LeavesShareEdge = [&](const FGPUTessellationPatchInfo& A, const FGPUTessellationPatchInfo& B) -> bool
	{
		const float AMinX = A.PatchOffset.X;
		const float AMaxX = A.PatchOffset.X + A.PatchSize.X;
		const float AMinY = A.PatchOffset.Y;
		const float AMaxY = A.PatchOffset.Y + A.PatchSize.Y;
		const float BMinX = B.PatchOffset.X;
		const float BMaxX = B.PatchOffset.X + B.PatchSize.X;
		const float BMinY = B.PatchOffset.Y;
		const float BMaxY = B.PatchOffset.Y + B.PatchSize.Y;

		const bool bTouchX = IsNearlyTouching(AMinX, BMaxX) || IsNearlyTouching(AMaxX, BMinX);
		const bool bTouchY = IsNearlyTouching(AMinY, BMaxY) || IsNearlyTouching(AMaxY, BMinY);
		return (bTouchX && IntervalOverlap(AMinY, AMaxY, BMinY, BMaxY) > Epsilon) ||
			(bTouchY && IntervalOverlap(AMinX, AMaxX, BMinX, BMaxX) > Epsilon);
	};

	auto ReindexLeaves = [&]()
	{
		for (int32 LeafIndex = 0; LeafIndex < OutPatchInfo.Num(); ++LeafIndex)
		{
			OutPatchInfo[LeafIndex].PatchIndexX = LeafIndex;
			OutPatchInfo[LeafIndex].PatchIndexY = OutPatchInfo[LeafIndex].QuadtreeDepth;
		}
	};

	auto SplitLeafForBalance = [&](int32 LeafIndex) -> bool
	{
		if (!OutPatchInfo.IsValidIndex(LeafIndex))
		{
			return false;
		}

		const FGPUTessellationPatchInfo Parent = OutPatchInfo[LeafIndex];
		if (Parent.QuadtreeDepth >= MaxDepth || OutPatchInfo.Num() + 3 > MaxLeaves)
		{
			return false;
		}

		OutPatchInfo.RemoveAt(LeafIndex, 1, EAllowShrinking::No);
		const FVector2f ChildSize(Parent.PatchSize.X * 0.5f, Parent.PatchSize.Y * 0.5f);
		const int32 ChildDepth = Parent.QuadtreeDepth + 1;
		OutPatchInfo.Add(BuildLeafInfo(Parent.PatchOffset, ChildSize, ChildDepth));
		OutPatchInfo.Add(BuildLeafInfo(FVector2f(Parent.PatchOffset.X + ChildSize.X, Parent.PatchOffset.Y), ChildSize, ChildDepth));
		OutPatchInfo.Add(BuildLeafInfo(FVector2f(Parent.PatchOffset.X, Parent.PatchOffset.Y + ChildSize.Y), ChildSize, ChildDepth));
		OutPatchInfo.Add(BuildLeafInfo(FVector2f(Parent.PatchOffset.X + ChildSize.X, Parent.PatchOffset.Y + ChildSize.Y), ChildSize, ChildDepth));
		return true;
	};

	if (Settings.bBalanceQuadtreeLeaves)
	{
		const int32 MaxBalanceIterations = MaxDepth * MaxLeaves;
		for (int32 Iteration = 0; Iteration < MaxBalanceIterations; ++Iteration)
		{
			bool bSplitAnyLeaf = false;
			for (int32 LeafIndex = 0; LeafIndex < OutPatchInfo.Num() && !bSplitAnyLeaf; ++LeafIndex)
			{
				for (int32 NeighborIndex = 0; NeighborIndex < OutPatchInfo.Num(); ++NeighborIndex)
				{
					if (LeafIndex == NeighborIndex)
					{
						continue;
					}

					const FGPUTessellationPatchInfo& Leaf = OutPatchInfo[LeafIndex];
					const FGPUTessellationPatchInfo& Neighbor = OutPatchInfo[NeighborIndex];
					if (Neighbor.QuadtreeDepth > Leaf.QuadtreeDepth + 1 && LeavesShareEdge(Leaf, Neighbor))
					{
						bSplitAnyLeaf = SplitLeafForBalance(LeafIndex);
						break;
					}
				}
			}

			if (!bSplitAnyLeaf)
			{
				break;
			}
		}
		ReindexLeaves();
	}

	if (Settings.bEnableQuadtreeEdgeStitching)
	{
		ComputeQuadtreeEdgeTransitions(OutPatchInfo, Settings.QuadtreeMaxEdgeCollapseFactor);
	}
	else
	{
		for (FGPUTessellationPatchInfo& Leaf : OutPatchInfo)
		{
			Leaf.EdgeCollapseFactors = FIntVector4(1, 1, 1, 1);
		}
	}
}

void FGPUTessellationMeshBuilder::ExecutePatchTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	const FConvexVolume* ViewFrustum,
	int32 PatchCountX,
	int32 PatchCountY,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellationPatchBuffers& OutPatchBuffers,
	UTexture* VectorDisplacementTexture)
{
	check(PatchCountX > 0 && PatchCountY > 0);
	
	// Calculate patch information (LOD, bounds, culling)
	TArray<FGPUTessellationPatchInfo> PatchInfo;
	
	// Debug: Log the LocalToWorld matrix
	FVector Location = LocalToWorld.GetOrigin();
	FVector Scale = LocalToWorld.GetScaleVector();
	UE_LOG(LogTemp, Verbose, TEXT("ExecutePatchPipeline: LocalToWorld Location=%s Scale=%s"), 
		*Location.ToString(), *Scale.ToString());
	
	CalculatePatchState(Settings, LocalToWorld, CameraPosition, ViewFrustum, PatchCountX, PatchCountY, PatchInfo);

	// Optional FFT pre-pass shared by all patches in this dispatch (one displacement map per frame
	// per primitive, sampled with frac(WorldXY/TileSize) so tiling is seamless across patches).
	CachedOceanFFTDisplacementMap = nullptr;
	if (Settings.OceanSettings.WaveMode == EGPUOceanWaveMode::FFT)
	{
		CachedOceanFFTDisplacementMap = GPUOceanFFT::ExecutePipeline(
			GraphBuilder,
			(uint32)Settings.OceanSettings.FFTSpectrumSeed,
			Settings.OceanSettings.Time,
			Settings.OceanSettings.FFTTileSize,
			Settings.OceanSettings.WindSpeed,
			FVector2f((float)Settings.OceanSettings.Wind.X, (float)Settings.OceanSettings.Wind.Y),
			Settings.OceanSettings.FFTAmplitude,
			Settings.OceanSettings.FFTChoppiness,
			(uint32)Settings.OceanSettings.FFTMotionMode,
			Settings.OceanSettings.FFTSwayIntensity,
			Settings.OceanSettings.FFTSwayRate,
			Settings.OceanSettings.FFTSwayDrift);
	}

	// Resize patch buffer arrays
	int32 TotalPatches = PatchCountX * PatchCountY;
	OutPatchBuffers.SetPatchBufferCount(TotalPatches);
	OutPatchBuffers.PatchInfo = PatchInfo;
	OutPatchBuffers.PatchCountX = PatchCountX;
	OutPatchBuffers.PatchCountY = PatchCountY;
	
	// Debug: Log patch subdivision info
	UE_LOG(LogTemp, Verbose, TEXT("GPUTessellation: Generating %dx%d = %d patches"), 
		PatchCountX, PatchCountY, TotalPatches);
	
	// Generate each patch independently (pure GPU)
	int32 SkippedCulled = 0;
	int32 SkippedInvalidLOD = 0;
	int32 GeneratedSuccessfully = 0;
	
	for (int32 PatchIndex = 0; PatchIndex < TotalPatches; ++PatchIndex)
	{
		const FGPUTessellationPatchInfo& Patch = PatchInfo[PatchIndex];
		
		// Debug: Log first few patches
		if (PatchIndex < 4)
		{
			UE_LOG(LogTemp, Verbose, TEXT("  Patch[%d]: UV:(%.3f,%.3f) Size:(%.3f,%.3f) LOD:%d Visible:%d WorldCenter:%s"),
				PatchIndex,
				Patch.PatchOffset.X, Patch.PatchOffset.Y,
				Patch.PatchSize.X, Patch.PatchSize.Y,
				Patch.TessellationLevel, Patch.bVisible, *Patch.WorldCenter.ToString());
		}
		
		// Skip culled patches
		if (!Patch.bVisible)
		{
			OutPatchBuffers.PatchBuffers[PatchIndex].Reset();
			SkippedCulled++;
			continue;
		}
		
		// Validate tessellation level before generating
		if (Patch.TessellationLevel <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("  Patch[%d]: INVALID TessellationLevel=%d, skipping!"), 
				PatchIndex, Patch.TessellationLevel);
			OutPatchBuffers.PatchBuffers[PatchIndex].Reset();
			SkippedInvalidLOD++;
			continue;
		}
		
		// Generate this patch
		GenerateSinglePatch(
			GraphBuilder,
			Settings,
			LocalToWorld,
			Patch,
			DisplacementTexture,
			SubtractTexture,
			NormalMapTexture,
			OutPatchBuffers.PatchBuffers[PatchIndex],
			VectorDisplacementTexture
		);
		
		GeneratedSuccessfully++;
		
		// Note: Buffers won't be valid until after GraphBuilder.Execute() is called
		// Validation happens in the scene proxy when rendering
	}
	
	// Log summary
	UE_LOG(LogTemp, Verbose, TEXT("GPUTessellation: Patch Generation Summary - Total:%d Generated:%d SkippedCulled:%d SkippedInvalidLOD:%d"),
		TotalPatches, GeneratedSuccessfully, SkippedCulled, SkippedInvalidLOD);
}

void FGPUTessellationMeshBuilder::ExecuteQuadtreeTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	const FConvexVolume* ViewFrustum,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellationPatchBuffers& OutPatchBuffers,
	UTexture* VectorDisplacementTexture)
{
	TArray<FGPUTessellationPatchInfo> LeafInfo;
	CalculateQuadtreePatchState(Settings, LocalToWorld, CameraPosition, ViewFrustum, LeafInfo);

	CachedOceanFFTDisplacementMap = nullptr;
	if (Settings.OceanSettings.WaveMode == EGPUOceanWaveMode::FFT)
	{
		CachedOceanFFTDisplacementMap = GPUOceanFFT::ExecutePipeline(
			GraphBuilder,
			(uint32)Settings.OceanSettings.FFTSpectrumSeed,
			Settings.OceanSettings.Time,
			Settings.OceanSettings.FFTTileSize,
			Settings.OceanSettings.WindSpeed,
			FVector2f((float)Settings.OceanSettings.Wind.X, (float)Settings.OceanSettings.Wind.Y),
			Settings.OceanSettings.FFTAmplitude,
			Settings.OceanSettings.FFTChoppiness,
			(uint32)Settings.OceanSettings.FFTMotionMode,
			Settings.OceanSettings.FFTSwayIntensity,
			Settings.OceanSettings.FFTSwayRate,
			Settings.OceanSettings.FFTSwayDrift);
	}

	const int32 TotalLeaves = LeafInfo.Num();
	OutPatchBuffers.SetPatchBufferCount(TotalLeaves);
	OutPatchBuffers.PatchInfo = LeafInfo;
	OutPatchBuffers.PatchCountX = FMath::Max(1, TotalLeaves);
	OutPatchBuffers.PatchCountY = 1;

	int32 SkippedCulled = 0;
	int32 SkippedInvalidLOD = 0;
	int32 GeneratedSuccessfully = 0;

	for (int32 LeafIndex = 0; LeafIndex < TotalLeaves; ++LeafIndex)
	{
		const FGPUTessellationPatchInfo& Leaf = LeafInfo[LeafIndex];

		if (!Leaf.bVisible)
		{
			OutPatchBuffers.PatchBuffers[LeafIndex].Reset();
			SkippedCulled++;
			continue;
		}

		if (Leaf.TessellationLevel <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("  QuadtreeLeaf[%d]: INVALID TessellationLevel=%d, skipping!"), LeafIndex, Leaf.TessellationLevel);
			OutPatchBuffers.PatchBuffers[LeafIndex].Reset();
			SkippedInvalidLOD++;
			continue;
		}

		GenerateSinglePatch(
			GraphBuilder,
			Settings,
			LocalToWorld,
			Leaf,
			DisplacementTexture,
			SubtractTexture,
			NormalMapTexture,
			OutPatchBuffers.PatchBuffers[LeafIndex],
			VectorDisplacementTexture
		);

		GeneratedSuccessfully++;
	}

	UE_LOG(LogTemp, Verbose, TEXT("GPUTessellation: Quadtree Generation Summary - Leaves:%d Generated:%d SkippedCulled:%d SkippedInvalidLOD:%d"),
		TotalLeaves, GeneratedSuccessfully, SkippedCulled, SkippedInvalidLOD);
}

void FGPUTessellationMeshBuilder::GenerateSinglePatch(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FGPUTessellationPatchInfo& PatchInfo,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellationBuffers& OutPatchBuffers,
	UTexture* VectorDisplacementTexture)
{
	// Validate tessellation level first
	const int32 TessellationLevel = PatchInfo.TessellationLevel;
	if (TessellationLevel <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid TessellationLevel=%d (must be > 0)"), 
			TessellationLevel);
		OutPatchBuffers.Reset();
		return;
	}
	
	// Calculate resolution for this patch's tessellation level. Visual subdivision raises this
	// protected cap only when explicitly requested.
	FIntPoint Resolution = CalculateResolutionWithCap(static_cast<float>(TessellationLevel), GetPatchResolutionCap(Settings));
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;
	
	// Validation checks
	if (Resolution.X < 2 || Resolution.Y < 2)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid resolution %dx%d (must be at least 2x2)"), 
			Resolution.X, Resolution.Y);
		OutPatchBuffers.Reset();
		return;
	}
	
	if (VertexCount <= 0 || IndexCount <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid counts: Verts=%d Indices=%d"), 
			VertexCount, IndexCount);
		OutPatchBuffers.Reset();
		return;
	}
	
	// Validate UV offset and scale
	if (PatchInfo.PatchOffset.X < 0.0f || PatchInfo.PatchOffset.Y < 0.0f || 
	    PatchInfo.PatchOffset.X > 1.0f || PatchInfo.PatchOffset.Y > 1.0f ||
	    PatchInfo.PatchSize.X <= 0.0f || PatchInfo.PatchSize.Y <= 0.0f ||
	    PatchInfo.PatchSize.X > 1.0f || PatchInfo.PatchSize.Y > 1.0f)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid UV parameters: Offset=(%.3f,%.3f) Size=(%.3f,%.3f)"),
			PatchInfo.PatchOffset.X, PatchInfo.PatchOffset.Y, PatchInfo.PatchSize.X, PatchInfo.PatchSize.Y);
		OutPatchBuffers.Reset();
		return;
	}
	
	// Debug: Log resolution calculation for first few patches
	static int32 DebugResCount = 0;
	if (DebugResCount < 3)
	{
		DebugResCount++;
		UE_LOG(LogTemp, Verbose, TEXT("    GeneratePatch: TessLevel=%d -> Resolution=%dx%d (%d verts, %d indices)"),
			TessellationLevel, Resolution.X, Resolution.Y, VertexCount, IndexCount);
	}
	
	// Create RDG buffers for this patch
	FRDGBufferRef VertexBuffer = nullptr;
	FRDGBufferRef NormalBuffer = nullptr;
	FRDGBufferRef UVBuffer = nullptr;
	FRDGBufferRef IndexBuffer = nullptr;
	
	// COORDINATE SPACE EXPLANATION:
	// - "Local Space" = Component's local coordinate system (before LocalToWorld transform)
	// - "World Space" = Final world position after LocalToWorld transform is applied
	// - "UV Space" = Texture coordinate space [0,1] for materials and displacement
	//
	// The full plane (Settings.PlaneSizeX × PlaneSizeY) is defined in LOCAL space.
	// Each patch is a subdivision of this full plane, NOT a separate small plane.
	// Patches must all use the SAME plane size to ensure consistent displacement mapping.
	
	float FullPlaneSizeX = Settings.PlaneSizeX;  // Total plane size in local space (X axis)
	float FullPlaneSizeY = Settings.PlaneSizeY;  // Total plane size in local space (Y axis)
	
	// CRITICAL FIX: All patches must use the SAME plane size for consistent displacement!
	// The plane size determines the world-space scale of displacement.
	// Each patch is a "window" into the full plane, not a separate small plane.
	
	// Calculate this patch's size in LOCAL space (size of the "window")
	float PatchLocalSizeX = PatchInfo.PatchSize.X * FullPlaneSizeX;  // Renamed from "PatchWorldSizeX" for clarity
	float PatchLocalSizeY = PatchInfo.PatchSize.Y * FullPlaneSizeY;  // Renamed from "PatchWorldSizeY" for clarity
	
	// Calculate the patch's corner position in local space (plane is centered at origin)
	// The vertex shader generates from [-0.5, +0.5], so we need to offset from there
	float LocalMinX = (PatchInfo.PatchOffset.X - 0.5f) * FullPlaneSizeX;
	float LocalMinY = (PatchInfo.PatchOffset.Y - 0.5f) * FullPlaneSizeY;
	
	// Calculate patch center in LOCAL space
	float LocalCenterX = LocalMinX + (PatchLocalSizeX * 0.5f);
	float LocalCenterY = LocalMinY + (PatchLocalSizeY * 0.5f);
	
	FVector PatchTranslation(LocalCenterX, LocalCenterY, 0.0f);
	
	// Debug: Log patch transform for first few patches
	static int32 DebugPatchCount = 0;
	if (DebugPatchCount < 4)
	{
		DebugPatchCount++;
		FVector WorldCenter = LocalToWorld.TransformPosition(PatchTranslation);
		UE_LOG(LogTemp, Verbose, TEXT("  Patch Transform: PatchLocalSize=(%.1f,%.1f) LocalOffset=%s WorldCenter=%s"),
			PatchLocalSizeX, PatchLocalSizeY, *PatchTranslation.ToString(), *WorldCenter.ToString());
	}
	
	// CRITICAL: Create patch-specific settings BUT keep GLOBAL plane size!
	// This ensures displacement intensity is consistent across all patches
	FGPUTessellationSettings PatchSettings = Settings;
	// DO NOT CHANGE PlaneSizeX/Y - must stay global!
	// PatchSettings.PlaneSizeX = Settings.PlaneSizeX;  // Keep original (already set)
	// PatchSettings.PlaneSizeY = Settings.PlaneSizeY;  // Keep original (already set)
	
	// Set UV offset/scale for material UVs and displacement sampling
	// This tells the shader which portion of the texture to sample
	PatchSettings.UVOffset = PatchInfo.PatchOffset;
	PatchSettings.UVScale = PatchInfo.PatchSize;
	
	// Step 1: Generate vertices for this patch using FULL plane size
	// CRITICAL: Pass Settings (not PatchSettings) for PlaneSizeX/Y to ensure global scale
	// Pass PatchSettings only for UV remapping (UVOffset/UVScale)
	// No patch translation needed - vertices generated at correct absolute positions
	DispatchVertexGeneration(GraphBuilder, PatchSettings, Resolution, LocalToWorld, FVector::ZeroVector, VertexBuffer, NormalBuffer, UVBuffer);
	
	// Step 2: Apply displacement (samples texture at patch's UV range using same UV offset/scale)
	DispatchDisplacement(GraphBuilder, PatchSettings, Resolution, LocalToWorld, DisplacementTexture, SubtractTexture, VectorDisplacementTexture, VertexBuffer, NormalBuffer, UVBuffer);
	
	// Step 3: Calculate normals if enabled (also use patch settings for consistent plane size)
	const FGPUTessellationSettings NormalSettings = GetGPUTessellationNormalSettings(PatchSettings, VectorDisplacementTexture);
	if (ShouldDispatchGPUTessellationVertexNormalCalculation(NormalSettings, DisplacementTexture))
	{
		DispatchNormalCalculation(GraphBuilder, NormalSettings, Resolution, DisplacementTexture, SubtractTexture, NormalMapTexture, VertexBuffer, NormalBuffer, UVBuffer);
	}
	
	// Step 4: Generate indices with seam stitching info
	DispatchIndexGeneration(GraphBuilder, Resolution, PatchInfo.EdgeCollapseFactors, IndexBuffer);
	
	// Step 5: Convert to persistent RHI buffers (pure GPU, no CPU readback)
	OutPatchBuffers.VertexCount = VertexCount;
	OutPatchBuffers.IndexCount = IndexCount;
	OutPatchBuffers.ResolutionX = Resolution.X;
	OutPatchBuffers.ResolutionY = Resolution.Y;
	
	// Convert RDG buffers to external pooled buffers
	TRefCountPtr<FRDGPooledBuffer> PooledPositionBuffer = GraphBuilder.ConvertToExternalBuffer(VertexBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledNormalBuffer = GraphBuilder.ConvertToExternalBuffer(NormalBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledUVBuffer = GraphBuilder.ConvertToExternalBuffer(UVBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledIndexBuffer = GraphBuilder.ConvertToExternalBuffer(IndexBuffer);

	// CRITICAL: keep the pooled wrappers alive in the persistent struct so the RDG
	// transient pool cannot recycle our underlying RHI buffers (see ExecuteTessellationPipeline
	// for full explanation). Without this, patch border vertices drift over time.
	OutPatchBuffers.PooledPositionBuffer = PooledPositionBuffer;
	OutPatchBuffers.PooledNormalBuffer = PooledNormalBuffer;
	OutPatchBuffers.PooledUVBuffer = PooledUVBuffer;
	OutPatchBuffers.PooledIndexBuffer = PooledIndexBuffer;

	// Create SRVs in RDG pass (after graph execution)
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("CreatePatchBufferSRVs"),
		ERDGPassFlags::None,
		[&OutPatchBuffers, PooledPositionBuffer, PooledNormalBuffer, PooledUVBuffer, PooledIndexBuffer](FRHICommandList& RHICmdList)
		{
			// Extract RHI references
			OutPatchBuffers.PositionBuffer = PooledPositionBuffer->GetRHI();
			OutPatchBuffers.NormalBuffer = PooledNormalBuffer->GetRHI();
			OutPatchBuffers.UVBuffer = PooledUVBuffer->GetRHI();
			OutPatchBuffers.IndexBufferRHI = PooledIndexBuffer->GetRHI();
			
			// Create SRVs
			OutPatchBuffers.PositionSRV = RHICmdList.CreateShaderResourceView(
				OutPatchBuffers.PositionBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
			);
			
			OutPatchBuffers.NormalSRV = RHICmdList.CreateShaderResourceView(
				OutPatchBuffers.NormalBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
			);
			
			OutPatchBuffers.UVSRV = RHICmdList.CreateShaderResourceView(
				OutPatchBuffers.UVBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
			);
			
			// Setup index buffer wrapper
			OutPatchBuffers.IndexBuffer.IndexBufferRHI = OutPatchBuffers.IndexBufferRHI;
			
			// Initialize the index buffer as a render resource if not already initialized
			if (!OutPatchBuffers.IndexBuffer.IsInitialized())
			{
				OutPatchBuffers.IndexBuffer.InitResource(RHICmdList);
			}
		}
	);
}

void FGPUTessellationMeshBuilder::CalculatePatchInfo(
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	const FConvexVolume* ViewFrustum,
	int32 PatchCountX,
	int32 PatchCountY,
	TArray<FGPUTessellationPatchInfo>& OutPatchInfo) const
{
	int32 TotalPatches = PatchCountX * PatchCountY;
	OutPatchInfo.SetNum(TotalPatches);
	
	// Full plane size in LOCAL space (before transform)
	float PlaneSizeX = Settings.PlaneSizeX;
	float PlaneSizeY = Settings.PlaneSizeY;
	
	// Each patch's UV space coverage
	float PatchUVSizeX = 1.0f / static_cast<float>(PatchCountX);
	float PatchUVSizeY = 1.0f / static_cast<float>(PatchCountY);
	
	// Each patch's size in LOCAL space (renamed from "PatchWorldSize" for clarity)
	float PatchLocalSizeX = PlaneSizeX / static_cast<float>(PatchCountX);
	float PatchLocalSizeY = PlaneSizeY / static_cast<float>(PatchCountY);
	
	FVector PlaneOrigin = LocalToWorld.GetOrigin();
	
	for (int32 Y = 0; Y < PatchCountY; ++Y)
	{
		for (int32 X = 0; X < PatchCountX; ++X)
		{
			int32 PatchIndex = Y * PatchCountX + X;
			FGPUTessellationPatchInfo& Patch = OutPatchInfo[PatchIndex];
			
			// UV space offset and size
			Patch.PatchOffset = FVector2f(X * PatchUVSizeX, Y * PatchUVSizeY);
			Patch.PatchSize = FVector2f(PatchUVSizeX, PatchUVSizeY);
			Patch.PatchIndexX = X;
			Patch.PatchIndexY = Y;
			
			// Calculate world space center - MUST match GenerateSinglePatch calculation
			// The vertex shader generates from [-0.5, +0.5] on XY plane (X, Y, Z=0)
			float LocalMinX = (Patch.PatchOffset.X - 0.5f) * PlaneSizeX;
			float LocalMinY = (Patch.PatchOffset.Y - 0.5f) * PlaneSizeY;
			
			float LocalCenterX = LocalMinX + (PatchLocalSizeX * 0.5f);
			float LocalCenterY = LocalMinY + (PatchLocalSizeY * 0.5f);
			// CRITICAL: Account for displacement offset in the Z center calculation
			// The patch center should be at the average displacement height, not at Z=0
			float LocalCenterZ = Settings.bUseVectorDisplacement && !Settings.bAddScalarHeightDisplacementToVector
				? 0.0f
				: Settings.DisplacementOffset + (Settings.DisplacementIntensity * 0.5f);
			FVector LocalCenter(LocalCenterX, LocalCenterY, LocalCenterZ);
			Patch.WorldCenter = LocalToWorld.TransformPosition(LocalCenter);
			
			// Debug: Log first few patch calculations
			if (PatchIndex < 4)
			{
				UE_LOG(LogTemp, Verbose, TEXT("  CalcPatchInfo[%d]: LocalMin=(%.1f, %.1f) LocalCenter=(%.1f, %.1f) WorldCenter=%s"),
					PatchIndex, LocalMinX, LocalMinY, LocalCenterX, LocalCenterY, *Patch.WorldCenter.ToString());
			}
			
			// Calculate world space bounds - need to transform all 8 corners to handle rotation/scale
			// HalfExtent on XY plane with Z for displacement
			// CRITICAL: Displacement can be both positive AND negative (offset + intensity)
			// We need to account for the full range of possible displacement
			const FVector VectorDisplacementExtent = GetGPUTessellationVectorDisplacementMaxAbsOffset(Settings);
			const float TotalDisplacementRange = GetGPUTessellationScalarDisplacementBoundsRange(Settings);
			
			FVector HalfExtent(
				PatchLocalSizeX * 0.5f + VectorDisplacementExtent.X,
				PatchLocalSizeY * 0.5f + VectorDisplacementExtent.Y,
				TotalDisplacementRange * 0.5f + VectorDisplacementExtent.Z);
			
			// Build bounds by transforming corners
			// The bounds extend from the center position in all directions by HalfExtent
			TArray<FVector> Corners;
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, -HalfExtent.Y, +HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, +HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, +HalfExtent.Y, +HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, -HalfExtent.Y, +HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, +HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, +HalfExtent.Y, +HalfExtent.Z)));
			
			Patch.WorldBounds = FBox(Corners);
			
			// Calculate distance from CAMERA to PATCH center
			// CRITICAL: This must be distance between camera and THIS patch's center,
			// NOT distance from patch to plane origin!
			const float Distance = FVector::Dist(Patch.WorldCenter, CameraPosition);
			
			// Determine tessellation level based on distance
			Patch.TessellationLevel = CalculatePatchTessellationLevel(Distance, Settings);
			// Patch path keeps a protected cap. With many patches (e.g. 8x8 = 64) the total vertex
			// count can otherwise scale catastrophically; subdivision only raises this when requested.
			FIntPoint PatchResolution = CalculateResolutionWithCap(static_cast<float>(Patch.TessellationLevel), GetPatchResolutionCap(Settings));
			Patch.ResolutionX = PatchResolution.X;
			Patch.ResolutionY = PatchResolution.Y;
			
			// Debug: Log ALL patches if first one has issues, or first 8 patches
			// Also log camera and patch positions to verify distance calculation
			if (PatchIndex < 8 || (PatchIndex == 0 && Patch.TessellationLevel <= 0))
			{
				UE_LOG(LogTemp, Verbose, TEXT("    Patch[%d]: PatchCenter=%s CameraPos=%s WorldDistance=%.1f -> LOD:%d (Tess=%d)"), 
					PatchIndex, 
					*Patch.WorldCenter.ToString(), 
					*CameraPosition.ToString(),
					Distance,
					Patch.TessellationLevel, 
					Patch.TessellationLevel);
			}
			
			// CRITICAL ERROR CHECK: If we got an invalid tessellation level, something is very wrong
			if (Patch.TessellationLevel <= 0)
			{
				UE_LOG(LogTemp, Error, TEXT("    Patch[%d]: INVALID TessellationLevel=%d! Distance=%.1f CameraPos=%s PatchCenter=%s"), 
					PatchIndex, Patch.TessellationLevel, Distance, 
					*CameraPosition.ToString(), *Patch.WorldCenter.ToString());
			}
			
			// Frustum culling
			if (ViewFrustum != nullptr && Settings.bEnablePatchCulling)
			{
				Patch.bVisible = ViewFrustum->IntersectBox(Patch.WorldCenter, Patch.WorldBounds.GetExtent());
				if (!Patch.bVisible)
				{
					UE_LOG(LogTemp, Verbose, TEXT("    Patch[%d] CULLED by frustum: Center=%s Extent=%s"), 
						PatchIndex, *Patch.WorldCenter.ToString(), *Patch.WorldBounds.GetExtent().ToString());
				}
			}
			else
			{
				// Culling disabled or no frustum - always visible
				Patch.bVisible = true;
			}
		}
	}
}

void FGPUTessellationMeshBuilder::ComputePatchEdgeTransitions(
	int32 PatchCountX,
	int32 PatchCountY,
	TArray<FGPUTessellationPatchInfo>& PatchInfo) const
{
	const int32 ExpectedCount = PatchCountX * PatchCountY;
	if (PatchCountX <= 0 || PatchCountY <= 0 || PatchInfo.Num() != ExpectedCount)
	{
		return;
	}

	const auto GetPatch = [&](int32 X, int32 Y) -> const FGPUTessellationPatchInfo*
	{
		if (X < 0 || X >= PatchCountX || Y < 0 || Y >= PatchCountY)
		{
			return nullptr;
		}
		return &PatchInfo[Y * PatchCountX + X];
	};

	for (int32 Y = 0; Y < PatchCountY; ++Y)
	{
		for (int32 X = 0; X < PatchCountX; ++X)
		{
			FGPUTessellationPatchInfo& Patch = PatchInfo[Y * PatchCountX + X];
			Patch.EdgeCollapseFactors = FIntVector4(1, 1, 1, 1);

			auto ComputeFactor = [&](const FGPUTessellationPatchInfo* Neighbor, bool bVerticalEdge) -> int32
			{
				if (!Neighbor)
				{
					return 1;
				}
				if (Patch.ResolutionX <= 0 || Patch.ResolutionY <= 0 || Neighbor->ResolutionX <= 0 || Neighbor->ResolutionY <= 0)
				{
					return 1;
				}
				if (Neighbor->TessellationLevel >= Patch.TessellationLevel)
				{
					return 1;
				}

				const int32 MySegments = bVerticalEdge ? FMath::Max(1, Patch.ResolutionY - 1) : FMath::Max(1, Patch.ResolutionX - 1);
				const int32 NeighborSegments = bVerticalEdge ? FMath::Max(1, Neighbor->ResolutionY - 1) : FMath::Max(1, Neighbor->ResolutionX - 1);
				if (NeighborSegments <= 0 || MySegments <= NeighborSegments)
				{
					return 1;
				}

				int32 Factor = FMath::Max(1, MySegments / NeighborSegments);
				Factor = FMath::Clamp(Factor, 1, 64);
				return Factor;
			};

			Patch.EdgeCollapseFactors.X = ComputeFactor(GetPatch(X - 1, Y), true);  // West edge (vertical segments)
			Patch.EdgeCollapseFactors.Y = ComputeFactor(GetPatch(X + 1, Y), true);  // East edge
			Patch.EdgeCollapseFactors.Z = ComputeFactor(GetPatch(X, Y - 1), false); // South edge (horizontal segments)
			Patch.EdgeCollapseFactors.W = ComputeFactor(GetPatch(X, Y + 1), false); // North edge
		}
	}
}

void FGPUTessellationMeshBuilder::ComputeQuadtreeEdgeTransitions(
	TArray<FGPUTessellationPatchInfo>& PatchInfo,
	int32 MaxCollapseFactor) const
{
	const float Epsilon = 1.0e-5f;
	const int32 ClampedMaxCollapseFactor = FMath::Clamp(MaxCollapseFactor, 1, 64);

	for (FGPUTessellationPatchInfo& Leaf : PatchInfo)
	{
		Leaf.EdgeCollapseFactors = FIntVector4(1, 1, 1, 1);
	}

	auto IntervalOverlap = [](float A0, float A1, float B0, float B1) -> float
	{
		return FMath::Max(0.0f, FMath::Min(A1, B1) - FMath::Max(A0, B0));
	};

	auto IsNearlyTouching = [Epsilon](float A, float B) -> bool
	{
		return FMath::Abs(A - B) <= Epsilon;
	};

	auto ComputeCollapseFactor = [Epsilon, ClampedMaxCollapseFactor](const FGPUTessellationPatchInfo& Leaf, const FGPUTessellationPatchInfo& Neighbor, bool bVerticalEdge, float OverlapLength) -> int32
	{
		if (FMath::Abs(Leaf.QuadtreeDepth - Neighbor.QuadtreeDepth) > 1)
		{
			return 1;
		}

		const float LeafEdgeLength = bVerticalEdge ? Leaf.PatchSize.Y : Leaf.PatchSize.X;
		const float NeighborEdgeLength = bVerticalEdge ? Neighbor.PatchSize.Y : Neighbor.PatchSize.X;
		if (LeafEdgeLength <= Epsilon || NeighborEdgeLength <= Epsilon || OverlapLength <= Epsilon)
		{
			return 1;
		}

		if (FMath::Abs(OverlapLength - LeafEdgeLength) > Epsilon)
		{
			return 1;
		}

		const int32 LeafSegments = FMath::Max(1, (bVerticalEdge ? Leaf.ResolutionY : Leaf.ResolutionX) - 1);
		const int32 NeighborSegments = FMath::Max(1, (bVerticalEdge ? Neighbor.ResolutionY : Neighbor.ResolutionX) - 1);
		const float NeighborSegmentsOnOverlapFloat = static_cast<float>(NeighborSegments) * (OverlapLength / NeighborEdgeLength);
		const int32 NeighborSegmentsOnOverlap = FMath::Max(1, FMath::RoundToInt(NeighborSegmentsOnOverlapFloat));

		if (LeafSegments <= NeighborSegmentsOnOverlap)
		{
			return 1;
		}

		if (LeafSegments % NeighborSegmentsOnOverlap != 0)
		{
			return 1;
		}

		return FMath::Clamp(LeafSegments / NeighborSegmentsOnOverlap, 1, ClampedMaxCollapseFactor);
	};

	for (int32 LeafIndex = 0; LeafIndex < PatchInfo.Num(); ++LeafIndex)
	{
		FGPUTessellationPatchInfo& Leaf = PatchInfo[LeafIndex];
		const float LeafMinX = Leaf.PatchOffset.X;
		const float LeafMaxX = Leaf.PatchOffset.X + Leaf.PatchSize.X;
		const float LeafMinY = Leaf.PatchOffset.Y;
		const float LeafMaxY = Leaf.PatchOffset.Y + Leaf.PatchSize.Y;

		for (int32 NeighborIndex = 0; NeighborIndex < PatchInfo.Num(); ++NeighborIndex)
		{
			if (NeighborIndex == LeafIndex)
			{
				continue;
			}

			const FGPUTessellationPatchInfo& Neighbor = PatchInfo[NeighborIndex];
			const float NeighborMinX = Neighbor.PatchOffset.X;
			const float NeighborMaxX = Neighbor.PatchOffset.X + Neighbor.PatchSize.X;
			const float NeighborMinY = Neighbor.PatchOffset.Y;
			const float NeighborMaxY = Neighbor.PatchOffset.Y + Neighbor.PatchSize.Y;

			if (Leaf.EdgeCollapseFactors.X == 1 && IsNearlyTouching(LeafMinX, NeighborMaxX))
			{
				const float OverlapY = IntervalOverlap(LeafMinY, LeafMaxY, NeighborMinY, NeighborMaxY);
				Leaf.EdgeCollapseFactors.X = ComputeCollapseFactor(Leaf, Neighbor, true, OverlapY);
			}

			if (Leaf.EdgeCollapseFactors.Y == 1 && IsNearlyTouching(LeafMaxX, NeighborMinX))
			{
				const float OverlapY = IntervalOverlap(LeafMinY, LeafMaxY, NeighborMinY, NeighborMaxY);
				Leaf.EdgeCollapseFactors.Y = ComputeCollapseFactor(Leaf, Neighbor, true, OverlapY);
			}

			if (Leaf.EdgeCollapseFactors.Z == 1 && IsNearlyTouching(LeafMinY, NeighborMaxY))
			{
				const float OverlapX = IntervalOverlap(LeafMinX, LeafMaxX, NeighborMinX, NeighborMaxX);
				Leaf.EdgeCollapseFactors.Z = ComputeCollapseFactor(Leaf, Neighbor, false, OverlapX);
			}

			if (Leaf.EdgeCollapseFactors.W == 1 && IsNearlyTouching(LeafMaxY, NeighborMinY))
			{
				const float OverlapX = IntervalOverlap(LeafMinX, LeafMaxX, NeighborMinX, NeighborMaxX);
				Leaf.EdgeCollapseFactors.W = ComputeCollapseFactor(Leaf, Neighbor, false, OverlapX);
			}
		}
	}
}

int32 FGPUTessellationMeshBuilder::CalculatePatchTessellationLevel(
	float DistanceToCamera,
	const FGPUTessellationSettings& Settings) const
{
	// CRITICAL FIX: Use PatchLevels and PatchDistances, NOT DiscreteLODLevels!
	// DiscreteLODLevels is for DistanceBasedDiscrete mode (whole mesh LOD)
	// PatchLevels/PatchDistances are for DistanceBasedPatches mode (per-patch LOD)
	if (Settings.PatchLevels.Num() == 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("CalculatePatchTessellationLevel: No PatchLevels config, using default 16"));
		return ApplyGPUTessellationSubdivisionMultiplier(16, Settings); // Default
	}
	
	// Debug: Log LOD config (only once)
	static bool bLoggedConfig = false;
	if (!bLoggedConfig)
	{
		bLoggedConfig = true;
	UE_LOG(LogTemp, Verbose, TEXT("Patch LOD Config: %d levels, %d distances"), 
		Settings.PatchLevels.Num(), Settings.PatchDistances.Num());
	for (int32 i = 0; i < FMath::Min(Settings.PatchLevels.Num(), Settings.PatchDistances.Num()); ++i)
	{
		const int32 LevelInt = static_cast<int32>(Settings.PatchLevels[i]);
		const int32 TessInt = ConvertPatchLevelToTessellation(Settings.PatchLevels[i]);
		UE_LOG(LogTemp, Verbose, TEXT("  LOD[%d]: Distance <= %.1f uses Level %d (Tess=%d)"), 
			i, Settings.PatchDistances[i], LevelInt, TessInt);
	}
}	// CORRECT LOGIC: Find which distance bracket we fall into
	// PatchDistances should be ordered from smallest to largest
	// PatchLevels should be ordered from highest quality to lowest
	//
	// Example:
	//   PatchDistances = { 2000, 5000, 10000, 20000 }
	//   PatchLevels = { Patch_64, Patch_32, Patch_16, Patch_8 }
	//
	// If distance <= 2000: Use Patch_64 (highest quality)
	// If distance <= 5000: Use Patch_32
	// If distance <= 10000: Use Patch_16
	// If distance <= 20000: Use Patch_8
	// If distance > 20000: Use Patch_8 (lowest, stay there)
	
	int32 TargetLevel = static_cast<int32>(Settings.PatchLevels[0]); // Start with highest quality
	
	// If no distance thresholds, just use the first level
	if (Settings.PatchDistances.Num() == 0)
	{
		return ApplyGPUTessellationSubdivisionMultiplier(ConvertPatchLevelToTessellation(Settings.PatchLevels[0]), Settings);
	}
	
	// Find the appropriate LOD level based on distance
	for (int32 i = 0; i < Settings.PatchDistances.Num(); ++i)
	{
		if (DistanceToCamera <= Settings.PatchDistances[i])
		{
			// We're within this distance threshold, use this level
			// Make sure we have a corresponding level
			if (i < Settings.PatchLevels.Num())
			{
				TargetLevel = static_cast<int32>(Settings.PatchLevels[i]);
			}
			break;
		}
	}
	
	// If we're beyond ALL distance thresholds, use the last (lowest quality) level
	if (DistanceToCamera > Settings.PatchDistances[Settings.PatchDistances.Num() - 1])
	{
		// Use the last level in the array (should be lowest quality)
		int32 LastIndex = FMath::Min(Settings.PatchDistances.Num(), Settings.PatchLevels.Num()) - 1;
		if (LastIndex >= 0 && LastIndex < Settings.PatchLevels.Num())
		{
			TargetLevel = static_cast<int32>(Settings.PatchLevels[LastIndex]);
		}
	}
	
	return ApplyGPUTessellationSubdivisionMultiplier(ConvertPatchLevelToTessellation(static_cast<EGPUTessellationPatchLevel>(TargetLevel)), Settings);
}

int32 FGPUTessellationMeshBuilder::CalculateQuadtreeTessellationLevel(
	float DistanceToCamera,
	const FGPUTessellationSettings& Settings,
	int32& OutLODIndex) const
{
	OutLODIndex = 0;
	if (Settings.QuadtreeLevels.Num() == 0)
	{
		return ApplyGPUTessellationSubdivisionMultiplier(16, Settings);
	}

	if (Settings.QuadtreeDistances.Num() == 0)
	{
		return ApplyGPUTessellationSubdivisionMultiplier(ConvertPatchLevelToTessellation(Settings.QuadtreeLevels[0]), Settings);
	}

	const int32 LastLevelIndex = Settings.QuadtreeLevels.Num() - 1;
	OutLODIndex = LastLevelIndex;
	for (int32 DistanceIndex = 0; DistanceIndex < Settings.QuadtreeDistances.Num(); ++DistanceIndex)
	{
		if (DistanceToCamera <= Settings.QuadtreeDistances[DistanceIndex])
		{
			OutLODIndex = FMath::Clamp(DistanceIndex, 0, LastLevelIndex);
			break;
		}
	}

	return ApplyGPUTessellationSubdivisionMultiplier(ConvertPatchLevelToTessellation(Settings.QuadtreeLevels[OutLODIndex]), Settings);
}

int32 FGPUTessellationMeshBuilder::ConvertPatchLevelToTessellation(EGPUTessellationPatchLevel Level) const
{
	// Convert enum to actual tessellation factor
	switch (Level)
	{
		case EGPUTessellationPatchLevel::Patch_4:   return 4;
		case EGPUTessellationPatchLevel::Patch_8:   return 8;
		case EGPUTessellationPatchLevel::Patch_16:  return 16;
		case EGPUTessellationPatchLevel::Patch_32:  return 32;
		case EGPUTessellationPatchLevel::Patch_64:  return 64;
		case EGPUTessellationPatchLevel::Patch_128: return 128;
		default: return 16;
	}
}
