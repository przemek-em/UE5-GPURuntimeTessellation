// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameters.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"

/**
 * Compute shader for calculating per-triangle tessellation factors
 * based on distance to camera and other LOD criteria
 */
class FGPUTessellationFactorCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUTessellationFactorCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUTessellationFactorCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Camera and LOD parameters
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(float, MaxTessellationDistance)
		SHADER_PARAMETER(float, MinTessellationFactor)
		SHADER_PARAMETER(float, MaxTessellationFactor)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(uint32, TriangleCount)
		
		// Input buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputIndices)
		
		// Output buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, OutputTessFactors)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
	}
};

/**
 * Compute shader for generating subdivided vertices from base mesh
 * Replaces Hull Shader functionality
 */
class FGPUVertexGenerationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUVertexGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUVertexGenerationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Tessellation parameters
		SHADER_PARAMETER(uint32, ResolutionX)
		SHADER_PARAMETER(uint32, ResolutionY)
		SHADER_PARAMETER(float, PlaneSizeX)
		SHADER_PARAMETER(float, PlaneSizeY)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		// Per-patch local offset (in primitive local space) - used to position patch geometry before primitive transform
		SHADER_PARAMETER(FVector3f, PatchLocalOffset)
		// Per-patch UV offset and scale (for material UV continuity across patches)
		SHADER_PARAMETER(FVector2f, PatchUVOffset)
		SHADER_PARAMETER(FVector2f, PatchUVScale)
		
		// Input buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, TessellationFactors)
		
		// Output buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, OutputUVs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
	}
};

/**
 * Compute shader for applying displacement mapping to generated vertices
 * Replaces Domain Shader functionality
 */
class FGPUDisplacementCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUDisplacementCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUDisplacementCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Displacement parameters
		SHADER_PARAMETER(float, DisplacementIntensity)
		SHADER_PARAMETER(float, DisplacementOffset)
		SHADER_PARAMETER(uint32, bUseSineWaveDisplacement)
		SHADER_PARAMETER(uint32, bHasRVTMask)
		SHADER_PARAMETER(uint32, VertexCount)
		
		// UV remapping for patch rendering (allows each patch to sample correct portion of texture)
		SHADER_PARAMETER(FVector2f, UVOffset)
		SHADER_PARAMETER(FVector2f, UVScale)
		
		// Texture resources
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, DisplacementTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, DisplacementSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, RVTMaskTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, RVTMaskSampler)
		
		// Input/Output buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, InputUVs)
		
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputPositions)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
	}
};

/**
 * Compute shader for calculating vertex normals from displaced geometry
 * Uses finite difference method on displacement or geometry-based calculation
 */
class FGPUNormalCalculationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUNormalCalculationCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUNormalCalculationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Normal calculation parameters
		SHADER_PARAMETER(uint32, NormalCalculationMethod) // 0=Disabled, 1=FiniteDiff, 2=GeometryBased, 3=Hybrid, 4=FromNormalMap
		SHADER_PARAMETER(float, NormalSmoothingFactor)
		SHADER_PARAMETER(uint32, bInvertNormals)
		SHADER_PARAMETER(uint32, VertexCount)
		SHADER_PARAMETER(uint32, ResolutionX)
		SHADER_PARAMETER(uint32, ResolutionY)
		SHADER_PARAMETER(float, TexelSize)
		SHADER_PARAMETER(float, PlaneSizeX)
		SHADER_PARAMETER(float, PlaneSizeY)
		
		// Displacement texture for gradient-based normals
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, DisplacementTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, DisplacementSampler)
		SHADER_PARAMETER(float, DisplacementIntensity)
		
		// Subtract/mask texture (for correct normal calculation with RVT)
		SHADER_PARAMETER(uint32, bHasSubtractTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SubtractTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SubtractSampler)
		
		// Normal map texture (RGB = world space normal)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalMapTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, NormalMapSampler)
		
		// Input buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, InputUVs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputIndices)
		
		// Output buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputNormals)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
	}
};

/**
 * Compute shader for generating triangle indices from subdivided grid
 */
class FGPUIndexGenerationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUIndexGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUIndexGenerationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Grid parameters
		SHADER_PARAMETER(uint32, ResolutionX)
		SHADER_PARAMETER(uint32, ResolutionY)
		SHADER_PARAMETER(FIntVector4, EdgeCollapseFactors)
		
		// Output buffer (typed UAV so it can become a real IndexBuffer later)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutputIndices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
	}
};
