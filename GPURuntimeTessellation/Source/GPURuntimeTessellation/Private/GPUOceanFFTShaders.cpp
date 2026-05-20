// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUOceanFFTShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

IMPLEMENT_GLOBAL_SHADER(FGPUOceanFFTInitSpectrumCS, "/Plugin/GPURuntimeTessellation/Private/GPUOceanFFT.usf", "InitSpectrumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPUOceanFFTRowCS,          "/Plugin/GPURuntimeTessellation/Private/GPUOceanFFT.usf", "FFTRowCS",       SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPUOceanFFTColCS,          "/Plugin/GPURuntimeTessellation/Private/GPUOceanFFT.usf", "FFTColCS",       SF_Compute);

namespace GPUOceanFFT
{
	FRDGTextureRef ExecutePipeline(
		FRDGBuilder& GraphBuilder,
		uint32 Seed,
		float Time,
		float TileSize,
		float WindSpeed,
		FVector2f WindDir,
		float AmplitudeScale,
		float Choppiness,
		uint32 MotionMode,
		float SwayIntensity,
		float SwayRate,
		float SwayDrift)
	{
		const int32 N = GPU_OCEAN_FFT_N;

		// Spectra: complex (RG32F) NxN. Height plus horizontal displacement components are
		// transformed together so FFT mode produces a proper choppy Tessendorf surface instead
		// of only vertical height offsets.
		FRDGTextureDesc SpectrumDesc = FRDGTextureDesc::Create2D(
			FIntPoint(N, N), PF_G32R32F, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef SpectrumHeight = GraphBuilder.CreateTexture(SpectrumDesc, TEXT("GPUOceanFFT.SpectrumHeight"));
		FRDGTextureRef SpectrumDisplacementX = GraphBuilder.CreateTexture(SpectrumDesc, TEXT("GPUOceanFFT.SpectrumDisplacementX"));
		FRDGTextureRef SpectrumDisplacementY = GraphBuilder.CreateTexture(SpectrumDesc, TEXT("GPUOceanFFT.SpectrumDisplacementY"));

		// Final displacement map: RGBA32F NxN. XY are horizontal displacement in local space,
		// Z is vertical height, W is reserved for future foam/Jacobian work.
		FRDGTextureDesc DisplacementMapDesc = FRDGTextureDesc::Create2D(
			FIntPoint(N, N), PF_A32B32G32R32F, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef DisplacementMap = GraphBuilder.CreateTexture(DisplacementMapDesc, TEXT("GPUOceanFFT.DisplacementMap"));

		// PASS 1 - InitSpectrum
		{
			const float WindLenSq = WindDir.SizeSquared();
			const FVector2f SafeWindDir = WindLenSq > KINDA_SMALL_NUMBER
				? WindDir / FMath::Sqrt(WindLenSq)
				: FVector2f(1.0f, 0.0f);

			auto* P = GraphBuilder.AllocParameters<FGPUOceanFFTInitSpectrumCS::FParameters>();
			P->FFTSpectrumSeed   = Seed;
			P->FFTTime           = Time;
			P->FFTTileSize       = FMath::Max(TileSize, 1.0f);
			P->FFTWindSpeed      = FMath::Max(WindSpeed, 1.0f);
			P->FFTWindDir        = SafeWindDir;
			// FFTAmplitudeScale is a user-facing master multiplier. The shader internally:
			//   (a) computes Phillips in SI (meters/m/s),
			//   (b) converts back to cm at the end (x100), and
			//   (c) folds the 1/N^2 IFFT normalization here to keep the inner butterfly minimal.
			// So FFTAmplitudeScale=1.0 produces approximately 1m peak waves at default wind.
			P->FFTAmplitudeScale = AmplitudeScale;
			P->FFTChoppiness     = Choppiness;
			P->FFTMotionMode     = MotionMode;
			P->FFTSwayIntensity  = FMath::Max(SwayIntensity, 0.0f);
			P->FFTSwayRate       = FMath::Max(SwayRate, 0.001f);
			P->FFTSwayDrift      = FMath::Max(SwayDrift, 0.0f);
			P->SpectrumHeightOut = GraphBuilder.CreateUAV(SpectrumHeight);
			P->SpectrumDisplacementXOut = GraphBuilder.CreateUAV(SpectrumDisplacementX);
			P->SpectrumDisplacementYOut = GraphBuilder.CreateUAV(SpectrumDisplacementY);

			TShaderMapRef<FGPUOceanFFTInitSpectrumCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("GPUOceanFFT.InitSpectrum"),
				Shader, P, FIntVector(N / 8, N / 8, 1));
		}

		// PASS 2 - 1D Inverse FFT along X (rows)
		{
			auto* P = GraphBuilder.AllocParameters<FGPUOceanFFTRowCS::FParameters>();
			P->SpectrumHeightInOut = GraphBuilder.CreateUAV(SpectrumHeight);
			P->SpectrumDisplacementXInOut = GraphBuilder.CreateUAV(SpectrumDisplacementX);
			P->SpectrumDisplacementYInOut = GraphBuilder.CreateUAV(SpectrumDisplacementY);
			TShaderMapRef<FGPUOceanFFTRowCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("GPUOceanFFT.RowIFFT"),
				Shader, P, FIntVector(N, 1, 1)); // N groups, each with N threads internally
		}

		// PASS 3 - 1D Inverse FFT along Y (columns) -> writes Heightmap
		{
			auto* P = GraphBuilder.AllocParameters<FGPUOceanFFTColCS::FParameters>();
			P->SpectrumHeightInOut = GraphBuilder.CreateUAV(SpectrumHeight);
			P->SpectrumDisplacementXInOut = GraphBuilder.CreateUAV(SpectrumDisplacementX);
			P->SpectrumDisplacementYInOut = GraphBuilder.CreateUAV(SpectrumDisplacementY);
			P->DisplacementMapOut = GraphBuilder.CreateUAV(DisplacementMap);
			TShaderMapRef<FGPUOceanFFTColCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("GPUOceanFFT.ColIFFT"),
				Shader, P, FIntVector(N, 1, 1));
		}

		return DisplacementMap;
	}
}
