// Copyright Epic Games, Inc. All Rights Reserved.

#include "GPUTessellationVertexFactory.h"
#include "RenderResource.h"
#include "VertexFactory.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "Materials/Material.h"
#include "ShaderParameterUtils.h"
#include "MeshMaterialShader.h"
#include "MeshDrawShaderBindings.h"

/**
 * Shader parameters for GPU Tessellation Vertex Factory
 */
class FGPUTessellationVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FGPUTessellationVertexFactoryShaderParameters, NonVirtual);
public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		PositionBufferParameter.Bind(ParameterMap, TEXT("PositionBuffer"));
		NormalBufferParameter.Bind(ParameterMap, TEXT("NormalBuffer"));
		UVBufferParameter.Bind(ParameterMap, TEXT("UVBuffer"));
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const
	{
		const FGPUTessellationVertexFactory* GPUVertexFactory = static_cast<const FGPUTessellationVertexFactory*>(VertexFactory);

		// Bind GPU buffer SRVs to shader parameters
		if (PositionBufferParameter.IsBound() && GPUVertexFactory->PositionSRV.IsValid())
		{
			ShaderBindings.Add(PositionBufferParameter, GPUVertexFactory->PositionSRV);
		}

		if (NormalBufferParameter.IsBound() && GPUVertexFactory->NormalSRV.IsValid())
		{
			ShaderBindings.Add(NormalBufferParameter, GPUVertexFactory->NormalSRV);
		}

		if (UVBufferParameter.IsBound() && GPUVertexFactory->UVSRV.IsValid())
		{
			ShaderBindings.Add(UVBufferParameter, GPUVertexFactory->UVSRV);
		}
	}

private:
	LAYOUT_FIELD(FShaderResourceParameter, PositionBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, NormalBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, UVBufferParameter);
};

IMPLEMENT_TYPE_LAYOUT(FGPUTessellationVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGPUTessellationVertexFactory, SF_Vertex, FGPUTessellationVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FGPUTessellationVertexFactory, "/Plugin/GPURuntimeTessellation/Private/GPUTessellationVertexFactory.ush", 
	EVertexFactoryFlags::UsedWithMaterials | 
	EVertexFactoryFlags::SupportsDynamicLighting |
	EVertexFactoryFlags::SupportsPositionOnly);

FGPUTessellationVertexFactory::FGPUTessellationVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
	: FVertexFactory(InFeatureLevel)
{
}

void FGPUTessellationVertexFactory::SetBuffers(FShaderResourceViewRHIRef InPositionSRV, FShaderResourceViewRHIRef InNormalSRV, FShaderResourceViewRHIRef InUVSRV)
{
	PositionSRV = InPositionSRV;
	NormalSRV = InNormalSRV;
	UVSRV = InUVSRV;
}

void FGPUTessellationVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// GPU tessellation vertex factory doesn't use traditional vertex streams
	// Instead, we fetch data from structured buffers in the vertex shader
	// We still need a minimal vertex declaration for the pipeline state
	
	FVertexDeclarationElementList Elements;
	Elements.Add(FVertexElement(0, 0, VET_Float3, 0, 0, false));
	
	InitDeclaration(Elements);
}

void FGPUTessellationVertexFactory::ReleaseRHI()
{
	PositionSRV.SafeRelease();
	NormalSRV.SafeRelease();
	UVSRV.SafeRelease();
	
	FVertexFactory::ReleaseRHI();
}

bool FGPUTessellationVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// Only compile for SM5+ platforms that support structured buffers in vertex shaders
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
		   (Parameters.MaterialParameters.bIsUsedWithStaticLighting ||
		    Parameters.MaterialParameters.bIsUsedWithSkeletalMesh ||
		    Parameters.MaterialParameters.bIsDefaultMaterial ||
		    Parameters.MaterialParameters.MaterialDomain == MD_Surface);
}

void FGPUTessellationVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	
	// Define shader compilation flags
	OutEnvironment.SetDefine(TEXT("GPU_TESSELLATION_VERTEX_FACTORY"), 1);
	OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), 0);
	OutEnvironment.SetDefine(TEXT("MANUAL_VERTEX_FETCH"), 1);
}

void FGPUTessellationVertexFactory::ValidateCompiledResult(const FVertexFactoryType* Type, EShaderPlatform Platform, const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors)
{
	// Validation logic if needed
}
