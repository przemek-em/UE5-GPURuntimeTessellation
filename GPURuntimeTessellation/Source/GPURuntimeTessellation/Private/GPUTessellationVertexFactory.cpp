// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUTessellationVertexFactory.h"
#include "RenderResource.h"
#include "VertexFactory.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "Materials/Material.h"
#include "ShaderParameterUtils.h"
#include "MeshMaterialShader.h"
#include "MeshDrawShaderBindings.h"
#include "GlobalRenderResources.h"
#include "RHIStaticStates.h"
#include "SceneInterface.h"

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
		TangentBufferParameter.Bind(ParameterMap, TEXT("TangentBuffer"));
		bHasTangentBufferParameter.Bind(ParameterMap, TEXT("bHasTangentBuffer"));
		HeightNormalTextureParameter.Bind(ParameterMap, TEXT("HeightNormalTexture"));
		HeightNormalTextureSamplerParameter.Bind(ParameterMap, TEXT("HeightNormalTextureSampler"));
		HeightNormalSubtractTextureParameter.Bind(ParameterMap, TEXT("HeightNormalSubtractTexture"));
		HeightNormalSubtractSamplerParameter.Bind(ParameterMap, TEXT("HeightNormalSubtractSampler"));
		bUseHeightTexturePixelNormalsParameter.Bind(ParameterMap, TEXT("bUseHeightTexturePixelNormals"));
		bHasHeightNormalSubtractTextureParameter.Bind(ParameterMap, TEXT("bHasHeightNormalSubtractTexture"));
		HeightNormalDisplacementIntensityParameter.Bind(ParameterMap, TEXT("HeightNormalDisplacementIntensity"));
		HeightNormalStrengthParameter.Bind(ParameterMap, TEXT("HeightNormalStrength"));
		HeightNormalTexelStepParameter.Bind(ParameterMap, TEXT("HeightNormalTexelStep"));
		HeightNormalPlaneSizeXParameter.Bind(ParameterMap, TEXT("HeightNormalPlaneSizeX"));
		HeightNormalPlaneSizeYParameter.Bind(ParameterMap, TEXT("HeightNormalPlaneSizeY"));
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

		if (TangentBufferParameter.IsBound())
		{
			ShaderBindings.Add(TangentBufferParameter, GPUVertexFactory->TangentSRV.IsValid() ? GPUVertexFactory->TangentSRV.GetReference() : GBlackFloat4VertexBufferWithSRV->ShaderResourceViewRHI.GetReference());
		}
		if (bHasTangentBufferParameter.IsBound())
		{
			ShaderBindings.Add(bHasTangentBufferParameter, GPUVertexFactory->TangentSRV.IsValid() ? 1u : 0u);
		}

		if (bUseHeightTexturePixelNormalsParameter.IsBound())
		{
			ShaderBindings.Add(bUseHeightTexturePixelNormalsParameter, GPUVertexFactory->bUseHeightTexturePixelNormals ? 1u : 0u);
		}
		if (bHasHeightNormalSubtractTextureParameter.IsBound())
		{
			ShaderBindings.Add(bHasHeightNormalSubtractTextureParameter, GPUVertexFactory->bHasHeightNormalSubtractTexture ? 1u : 0u);
		}
		if (HeightNormalDisplacementIntensityParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalDisplacementIntensityParameter, GPUVertexFactory->HeightNormalDisplacementIntensity);
		}
		if (HeightNormalStrengthParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalStrengthParameter, GPUVertexFactory->HeightNormalStrength);
		}
		if (HeightNormalTexelStepParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalTexelStepParameter, GPUVertexFactory->HeightNormalTexelStep);
		}
		if (HeightNormalPlaneSizeXParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalPlaneSizeXParameter, GPUVertexFactory->HeightNormalPlaneSizeX);
		}
		if (HeightNormalPlaneSizeYParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalPlaneSizeYParameter, GPUVertexFactory->HeightNormalPlaneSizeY);
		}

		FRHITexture* HeightTextureRHI = GPUVertexFactory->HeightNormalTextureRHI.IsValid() ? GPUVertexFactory->HeightNormalTextureRHI.GetReference() : GBlackTexture->TextureRHI.GetReference();
		FRHITexture* SubtractTextureRHI = GPUVertexFactory->HeightNormalSubtractTextureRHI.IsValid() ? GPUVertexFactory->HeightNormalSubtractTextureRHI.GetReference() : GBlackTexture->TextureRHI.GetReference();
		if (HeightNormalTextureParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalTextureParameter, HeightTextureRHI);
		}
		if (HeightNormalTextureSamplerParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalTextureSamplerParameter, TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		}
		if (HeightNormalSubtractTextureParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalSubtractTextureParameter, SubtractTextureRHI);
		}
		if (HeightNormalSubtractSamplerParameter.IsBound())
		{
			ShaderBindings.Add(HeightNormalSubtractSamplerParameter, TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		}
	}

private:
	LAYOUT_FIELD(FShaderResourceParameter, PositionBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, NormalBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, UVBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, TangentBufferParameter);
	LAYOUT_FIELD(FShaderParameter, bHasTangentBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, HeightNormalTextureParameter);
	LAYOUT_FIELD(FShaderResourceParameter, HeightNormalTextureSamplerParameter);
	LAYOUT_FIELD(FShaderResourceParameter, HeightNormalSubtractTextureParameter);
	LAYOUT_FIELD(FShaderResourceParameter, HeightNormalSubtractSamplerParameter);
	LAYOUT_FIELD(FShaderParameter, bUseHeightTexturePixelNormalsParameter);
	LAYOUT_FIELD(FShaderParameter, bHasHeightNormalSubtractTextureParameter);
	LAYOUT_FIELD(FShaderParameter, HeightNormalDisplacementIntensityParameter);
	LAYOUT_FIELD(FShaderParameter, HeightNormalStrengthParameter);
	LAYOUT_FIELD(FShaderParameter, HeightNormalTexelStepParameter);
	LAYOUT_FIELD(FShaderParameter, HeightNormalPlaneSizeXParameter);
	LAYOUT_FIELD(FShaderParameter, HeightNormalPlaneSizeYParameter);
};

IMPLEMENT_TYPE_LAYOUT(FGPUTessellationVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGPUTessellationVertexFactory, SF_Vertex, FGPUTessellationVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGPUTessellationVertexFactory, SF_Pixel, FGPUTessellationVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGPUTessellationGPUSceneVertexFactory, SF_Vertex, FGPUTessellationVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGPUTessellationGPUSceneVertexFactory, SF_Pixel, FGPUTessellationVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FGPUTessellationVertexFactory, "/Plugin/GPURuntimeTessellation/Private/GPUTessellationVertexFactory.ush", 
	EVertexFactoryFlags::UsedWithMaterials | 
	EVertexFactoryFlags::SupportsDynamicLighting |
	EVertexFactoryFlags::SupportsPositionOnly |
	// Depth/shadow passes still need manual fetch support because all vertex data
	// comes from SRVs rather than traditional vertex streams.
	EVertexFactoryFlags::SupportsManualVertexFetch);

IMPLEMENT_VERTEX_FACTORY_TYPE(FGPUTessellationGPUSceneVertexFactory, "/Plugin/GPURuntimeTessellation/Private/GPUTessellationVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials |
	EVertexFactoryFlags::SupportsDynamicLighting |
	EVertexFactoryFlags::SupportsPrimitiveIdStream |
	EVertexFactoryFlags::SupportsManualVertexFetch);

FGPUTessellationVertexFactory::FGPUTessellationVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
	: FVertexFactory(InFeatureLevel)
{
}

void FGPUTessellationVertexFactory::SetBuffers(FShaderResourceViewRHIRef InPositionSRV, FShaderResourceViewRHIRef InNormalSRV, FShaderResourceViewRHIRef InUVSRV, FShaderResourceViewRHIRef InTangentSRV)
{
	PositionSRV = InPositionSRV;
	NormalSRV = InNormalSRV;
	UVSRV = InUVSRV;
	TangentSRV = InTangentSRV;
}

void FGPUTessellationVertexFactory::SetHeightNormalParameters(
	bool bInUseHeightTexturePixelNormals,
	FTextureRHIRef InHeightNormalTextureRHI,
	FTextureRHIRef InHeightNormalSubtractTextureRHI,
	bool bInHasHeightNormalSubtractTexture,
	float InHeightNormalDisplacementIntensity,
	float InHeightNormalStrength,
	float InHeightNormalTexelStep,
	float InHeightNormalPlaneSizeX,
	float InHeightNormalPlaneSizeY)
{
	bUseHeightTexturePixelNormals = bInUseHeightTexturePixelNormals && InHeightNormalTextureRHI.IsValid();
	HeightNormalTextureRHI = InHeightNormalTextureRHI;
	HeightNormalSubtractTextureRHI = InHeightNormalSubtractTextureRHI;
	bHasHeightNormalSubtractTexture = bInHasHeightNormalSubtractTexture && InHeightNormalSubtractTextureRHI.IsValid();
	HeightNormalDisplacementIntensity = InHeightNormalDisplacementIntensity;
	HeightNormalStrength = InHeightNormalStrength;
	HeightNormalTexelStep = InHeightNormalTexelStep;
	HeightNormalPlaneSizeX = InHeightNormalPlaneSizeX;
	HeightNormalPlaneSizeY = InHeightNormalPlaneSizeY;
}

void FGPUTessellationVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// GPU tessellation vertex factory doesn't use traditional vertex streams.
	// Hardware indexed draws provide SV_VertexID; the shader uses it to fetch
	// position/normal/UV data from structured buffers. Transform data is supplied
	// through the mesh batch primitive uniform buffer.

	FVertexDeclarationElementList Elements;
	InitDeclaration(Elements);

	// FIX (CRITICAL #19): Register depth-only declarations so the depth-only /
	// virtual shadow map rasterization passes can bind a valid PSO. The actual data
	// is fetched via SRVs in the shader (MANUAL_VERTEX_FETCH = 1), so the same
	// element layout works for all stream variants.
	FVertexDeclarationElementList PositionOnlyElements;
	InitDeclaration(PositionOnlyElements, EVertexInputStreamType::PositionOnly);

	FVertexDeclarationElementList PositionAndNormalOnlyElements;
	InitDeclaration(PositionAndNormalOnlyElements, EVertexInputStreamType::PositionAndNormalOnly);
}

FGPUTessellationGPUSceneVertexFactory::FGPUTessellationGPUSceneVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
	: FGPUTessellationVertexFactory(InFeatureLevel)
{
}

void FGPUTessellationGPUSceneVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	FVertexDeclarationElementList Elements;
	AddPrimitiveIdStreamElement(EVertexInputStreamType::Default, Elements, 13, 0xFF);
	InitDeclaration(Elements);
}

void FGPUTessellationVertexFactory::ReleaseRHI()
{
	PositionSRV.SafeRelease();
	NormalSRV.SafeRelease();
	UVSRV.SafeRelease();
	TangentSRV.SafeRelease();
	HeightNormalTextureRHI.SafeRelease();
	HeightNormalSubtractTextureRHI.SafeRelease();
	
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
	const bool bSupportsGPUSceneStream = Parameters.VertexFactoryType->SupportsPrimitiveIdStream() && UseGPUScene(Parameters.Platform, GetMaxSupportedFeatureLevel(Parameters.Platform));
	OutEnvironment.SetDefine(TEXT("GPU_TESSELLATION_USE_GPU_SCENE_STREAM"), bSupportsGPUSceneStream ? 1 : 0);
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), bSupportsGPUSceneStream ? 1 : 0);
}

void FGPUTessellationVertexFactory::ValidateCompiledResult(const FVertexFactoryType* Type, EShaderPlatform Platform, const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors)
{
	// Validation logic if needed
}
