// Copyright Epic Games, Inc. All Rights Reserved.

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
	void SetBuffers(FShaderResourceViewRHIRef InPositionSRV, FShaderResourceViewRHIRef InNormalSRV, FShaderResourceViewRHIRef InUVSRV);

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
};
