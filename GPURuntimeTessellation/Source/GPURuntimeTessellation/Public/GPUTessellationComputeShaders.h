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
		// Linear 1D dispatch (matches displacement/normal passes). Eliminates a 2D-dispatch
		// driver/compiler edge case where the corner thread (last x, last y) failed to write
		// any of its 3 UAVs (position/normal/UV), leaving the corner vertex slot at the
		// cleared (0,0,0) value. With 1D dispatch every slot 0..VertexCount-1 gets one thread.
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
		// Bump on shader edits to force DDC re-hash / recompile.
		OutEnvironment.SetDefine(TEXT("GPU_TESS_SHADER_REV"), 10);
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
		SHADER_PARAMETER(FMatrix44f, WorldToLocal)

		// Vector displacement path. Populated by UGPUVectorDisplacementComponent.
		SHADER_PARAMETER(uint32, bUseVectorDisplacement)
		SHADER_PARAMETER(uint32, VectorDisplacementSpace)
		SHADER_PARAMETER(uint32, VectorDisplacementDecodeMode)
		SHADER_PARAMETER(uint32, bUseVectorDisplacementAlphaAsStrength)
		SHADER_PARAMETER(uint32, bAddScalarHeightDisplacementToVector)
		SHADER_PARAMETER(FVector3f, VectorDisplacementScale)
		SHADER_PARAMETER(FVector3f, VectorDisplacementBias)
		SHADER_PARAMETER(float, VectorDisplacementIntensity)

		// ----- Procedural ocean -----
		// 0=Disabled, 1=Gerstner, 2=FFT/Tessendorf, 3=PerlinFBM
		SHADER_PARAMETER(uint32, OceanWaveMode)
		SHADER_PARAMETER(uint32, OceanGerstnerWaveCount) // [0..GPU_OCEAN_MAX_GERSTNER_WAVES]
		SHADER_PARAMETER(float,  OceanTime)
		SHADER_PARAMETER(float,  OceanPlaneSizeX)
		SHADER_PARAMETER(float,  OceanPlaneSizeY)
		// Per-wave packed data: PackA = (DirX, DirY, Wavelength, Speed)
		//                       PackB = (Amplitude, Steepness, PhaseOffset, _padding)
		SHADER_PARAMETER_ARRAY(FVector4f, OceanGerstnerPackA, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, OceanGerstnerPackB, [8])
		// Perlin / fBm
		SHADER_PARAMETER(float,  OceanPerlinFrequency)
		SHADER_PARAMETER(uint32, OceanPerlinOctaves)
		SHADER_PARAMETER(float,  OceanPerlinPersistence)
		SHADER_PARAMETER(float,  OceanPerlinLacunarity)
		SHADER_PARAMETER(FVector2f, OceanPerlinFlow)

		// FFT displacement map (active when OceanWaveMode == 2). XY = horizontal chop, Z = height.
		SHADER_PARAMETER(float, OceanFFTTileSize)
		SHADER_PARAMETER(float, OceanFFTWindSpeed)
		SHADER_PARAMETER(FVector2f, OceanFFTWindDir)
		SHADER_PARAMETER(uint32, OceanFFTMotionMode)
		SHADER_PARAMETER(float, OceanFFTSwayIntensity)
		SHADER_PARAMETER(float, OceanFFTSwayRate)
		SHADER_PARAMETER(float, OceanFFTSwayDrift)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, OceanFFTDisplacementMap)
		SHADER_PARAMETER_SAMPLER(SamplerState, OceanFFTDisplacementMapSampler)

		// UV remapping for patch rendering (allows each patch to sample correct portion of texture)
		SHADER_PARAMETER(FVector2f, UVOffset)
		SHADER_PARAMETER(FVector2f, UVScale)
		
		// Texture resources
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, DisplacementTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, VectorDisplacementTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, RVTMaskTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, RVTMaskSampler)
		
		// Input/Output buffers (positions are read & written through the UAV to
		// avoid binding the same VertexBuffer as both SRV and UAV in one pass).
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
		OutEnvironment.SetDefine(TEXT("GPU_TESS_SHADER_REV"), 8);
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
		SHADER_PARAMETER(uint32, NormalCalculationMethod) // 0=Disabled, 1=FiniteDiff, 2=GeometryBased, 3=Hybrid, 4=FromNormalMap, 5=GeometryHeightTextureBlend, 6=FromHeightTexture
		SHADER_PARAMETER(float, NormalSmoothingFactor)
		SHADER_PARAMETER(float, NormalIntensity)
		SHADER_PARAMETER(float, HeightTextureNormalDetailStrength)
		SHADER_PARAMETER(float, HeightTextureNormalTexelStep)
		SHADER_PARAMETER(uint32, bInvertNormals)
		SHADER_PARAMETER(uint32, VertexCount)
		SHADER_PARAMETER(uint32, ResolutionX)
		SHADER_PARAMETER(uint32, ResolutionY)
		SHADER_PARAMETER(FVector2f, NormalUVStep)
		SHADER_PARAMETER(float, PlaneSizeX)
		SHADER_PARAMETER(float, PlaneSizeY)
		// Mirrors GPUDisplacement: when true, normals must derive from the same procedural sine
		// the displacement pass uses, otherwise the sine plane appears flat-shaded.
		SHADER_PARAMETER(uint32, bUseSineWaveDisplacement)

		// Mirrors GPUDisplacement: same procedural ocean parameters so finite-difference / hybrid
		// normal sampling agrees with the displaced positions.
		SHADER_PARAMETER(uint32, OceanWaveMode)
		SHADER_PARAMETER(uint32, OceanGerstnerWaveCount)
		SHADER_PARAMETER(float,  OceanTime)
		SHADER_PARAMETER_ARRAY(FVector4f, OceanGerstnerPackA, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, OceanGerstnerPackB, [8])
		SHADER_PARAMETER(float,  OceanPerlinFrequency)
		SHADER_PARAMETER(uint32, OceanPerlinOctaves)
		SHADER_PARAMETER(float,  OceanPerlinPersistence)
		SHADER_PARAMETER(float,  OceanPerlinLacunarity)
		SHADER_PARAMETER(FVector2f, OceanPerlinFlow)

		// Mirrors GPUDisplacement: FFT displacement map for normal sampling agreement.
		SHADER_PARAMETER(float, OceanFFTTileSize)
		SHADER_PARAMETER(float, OceanFFTWindSpeed)
		SHADER_PARAMETER(FVector2f, OceanFFTWindDir)
		SHADER_PARAMETER(uint32, OceanFFTMotionMode)
		SHADER_PARAMETER(float, OceanFFTSwayIntensity)
		SHADER_PARAMETER(float, OceanFFTSwayRate)
		SHADER_PARAMETER(float, OceanFFTSwayDrift)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, OceanFFTDisplacementMap)
		SHADER_PARAMETER_SAMPLER(SamplerState, OceanFFTDisplacementMapSampler)
		
		// Displacement texture for gradient-based normals
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, DisplacementTexture)
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
		OutEnvironment.SetDefine(TEXT("GPU_TESS_SHADER_REV"), 7);
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
		// Linear 1D dispatch (matches vertex/displacement/normal passes). 2D dispatch caused
		// the corner thread to skip its 6 index writes, leaving degenerate triangles all
		// referencing vertex 0 - visible as the bottom-left corner vertex collapsing.
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
		OutEnvironment.SetDefine(TEXT("GPU_TESS_SHADER_REV"), 11);
	}
};

/** Generate tessellated vertices for an arbitrary static mesh triangle list. */
class FGPUMeshTessellationVertexGenerationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUMeshTessellationVertexGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUMeshTessellationVertexGenerationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SourceTriangleCount)
		SHADER_PARAMETER(uint32, TessellationFactor)
		SHADER_PARAMETER(uint32, VerticesPerTriangle)
		SHADER_PARAMETER(uint32, OutputVertexCount)
		SHADER_PARAMETER(uint32, bEnableDisplacement)
		SHADER_PARAMETER(uint32, bHasDisplacementTexture)
		SHADER_PARAMETER(float, DisplacementIntensity)
		SHADER_PARAMETER(float, DisplacementOffset)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUMeshTessellationVertex>, InputVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputIndices)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, DisplacementTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, DisplacementSampler)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, OutputUVs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutputTangents)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
		OutEnvironment.SetDefine(TEXT("GPU_MESH_TESS_SHADER_REV"), 3);
	}
};

/** Generate triangle indices for uniformly subdivided arbitrary static mesh triangles. */
class FGPUMeshTessellationIndexGenerationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUMeshTessellationIndexGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUMeshTessellationIndexGenerationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SourceTriangleCount)
		SHADER_PARAMETER(uint32, TessellationFactor)
		SHADER_PARAMETER(uint32, VerticesPerTriangle)
		SHADER_PARAMETER(uint32, IndicesPerTriangle)
		SHADER_PARAMETER(uint32, OutputIndexCount)
		SHADER_PARAMETER(uint32, SeamEdgeCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, InputSeamEdges)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutputIndices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
		OutEnvironment.SetDefine(TEXT("GPU_MESH_TESS_SHADER_REV"), 3);
	}
};

/** Recalculate arbitrary mesh normals from generated displaced triangle patches. */
class FGPUMeshTessellationNormalGenerationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUMeshTessellationNormalGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUMeshTessellationNormalGenerationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SourceTriangleCount)
		SHADER_PARAMETER(uint32, TessellationFactor)
		SHADER_PARAMETER(uint32, VerticesPerTriangle)
		SHADER_PARAMETER(uint32, OutputVertexCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputGeneratedPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputBaseNormals)
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
		OutEnvironment.SetDefine(TEXT("GPU_MESH_TESS_SHADER_REV"), 3);
	}
};
