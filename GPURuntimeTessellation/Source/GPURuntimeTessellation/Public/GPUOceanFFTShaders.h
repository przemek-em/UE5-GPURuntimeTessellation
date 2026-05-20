// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/** Ocean FFT grid resolution. Must match OCEAN_FFT_N in GPUOceanFFT.usf. */
#define GPU_OCEAN_FFT_N 256

/**
 * Spectrum init: per-cell deterministic Gaussian + Phillips spectrum + dispersion phase rotation.
 * Re-dispatched every frame (the per-frame proxy rebuild path doesn't preserve textures, and
 * the deterministic seed makes the result frame-stable apart from Time).
 */
class FGPUOceanFFTInitSpectrumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUOceanFFTInitSpectrumCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUOceanFFTInitSpectrumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, FFTSpectrumSeed)
		SHADER_PARAMETER(float,  FFTTime)
		SHADER_PARAMETER(float,  FFTTileSize)
		SHADER_PARAMETER(float,  FFTWindSpeed)
		SHADER_PARAMETER(FVector2f, FFTWindDir)
		SHADER_PARAMETER(float,  FFTAmplitudeScale)
		SHADER_PARAMETER(float,  FFTChoppiness)
		SHADER_PARAMETER(uint32, FFTMotionMode)
		SHADER_PARAMETER(float,  FFTSwayIntensity)
		SHADER_PARAMETER(float,  FFTSwayRate)
		SHADER_PARAMETER(float,  FFTSwayDrift)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumHeightOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumDisplacementXOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumDisplacementYOut)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** 1D inverse FFT along X. One threadgroup per row, GPU_OCEAN_FFT_N threads. */
class FGPUOceanFFTRowCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUOceanFFTRowCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUOceanFFTRowCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumHeightInOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumDisplacementXInOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumDisplacementYInOut)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** 1D inverse FFT along Y. Final pass writes horizontal displacement plus height. */
class FGPUOceanFFTColCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUOceanFFTColCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUOceanFFTColCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumHeightInOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumDisplacementXInOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SpectrumDisplacementYInOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DisplacementMapOut)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

namespace GPUOceanFFT
{
	/**
	 * Dispatch the full Init -> FFTRow -> FFTCol pipeline and return the final RGBA32F
	 * displacement texture (XY = horizontal chop, Z = height, W = reserved).
	 */
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
		float SwayDrift);
}
