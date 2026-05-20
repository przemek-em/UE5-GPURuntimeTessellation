// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "VertexFactory.h"
#include "RenderResource.h"
#include "LocalVertexFactory.h"
#include "RHICommandList.h"

/**
 * Vertex Factory for GPU-tessellated geometry
 * Binds GPU buffer SRVs directly without CPU data
 */
class FGPUTessellationVertexFactory : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FGPUTessellationVertexFactory);

public:
	FGPUTessellationVertexFactory(ERHIFeatureLevel::Type InFeatureLevel);

	/**
	 * Set GPU buffer SRVs
	 */
	void SetBuffers(FShaderResourceViewRHIRef InPositionSRV, FShaderResourceViewRHIRef InNormalSRV, FShaderResourceViewRHIRef InUVSRV, FShaderResourceViewRHIRef InTangentSRV = FShaderResourceViewRHIRef());

	/** Set optional height texture parameters used to override pixel normals from the source height field. */
	void SetHeightNormalParameters(
		bool bInUseHeightTexturePixelNormals,
		FTextureRHIRef InHeightNormalTextureRHI,
		FTextureRHIRef InHeightNormalSubtractTextureRHI,
		bool bInHasHeightNormalSubtractTexture,
		float InHeightNormalDisplacementIntensity,
		float InHeightNormalStrength,
		float InHeightNormalTexelStep,
		float InHeightNormalPlaneSizeX,
		float InHeightNormalPlaneSizeY);

	/**
	 * Init RHI resources
	 */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;

	/**
	 * Release RHI resources
	 */
	virtual void ReleaseRHI() override;

	/**
	 * Should we cache vertex factory shader permutations
	 */
	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);

	/**
	 * Modify compile environment for this vertex factory
	 */
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	/**
	 * Validate compile-time settings
	 */
	static void ValidateCompiledResult(const FVertexFactoryType* Type, EShaderPlatform Platform, const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors);

	/** GPU buffer SRVs (public for shader parameter binding) */
	FShaderResourceViewRHIRef PositionSRV;
	FShaderResourceViewRHIRef NormalSRV;
	FShaderResourceViewRHIRef UVSRV;
	FShaderResourceViewRHIRef TangentSRV;

	FTextureRHIRef HeightNormalTextureRHI;
	FTextureRHIRef HeightNormalSubtractTextureRHI;
	bool bUseHeightTexturePixelNormals = false;
	bool bHasHeightNormalSubtractTexture = false;
	float HeightNormalDisplacementIntensity = 1.0f;
	float HeightNormalStrength = 1.0f;
	float HeightNormalTexelStep = 1.0f;
	float HeightNormalPlaneSizeX = 1000.0f;
	float HeightNormalPlaneSizeY = 1000.0f;
};

/** GPUScene-capable variant used only by shadow-depth views that require instance culling. */
class FGPUTessellationGPUSceneVertexFactory : public FGPUTessellationVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FGPUTessellationGPUSceneVertexFactory);

public:
	FGPUTessellationGPUSceneVertexFactory(ERHIFeatureLevel::Type InFeatureLevel);

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
};
