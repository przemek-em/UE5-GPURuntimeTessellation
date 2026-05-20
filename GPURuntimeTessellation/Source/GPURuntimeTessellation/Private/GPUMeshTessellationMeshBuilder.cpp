// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUMeshTessellationMeshBuilder.h"

#include "GPUTessellationComputeShaders.h"
#include "RHIGPUReadback.h"
#include "RenderGraphUtils.h"
#include "SystemTextures.h"
#include "TextureResource.h"

FRDGTextureRef FGPUMeshTessellationMeshBuilder::CreateRDGTextureFromUTexture(FRDGBuilder& GraphBuilder, UTexture* Texture, const TCHAR* Name)
{
	if (!Texture || !Texture->GetResource())
	{
		return GetDefaultBlackTexture(GraphBuilder);
	}

	FTextureResource* TextureResource = Texture->GetResource();
	FRHITexture* RHITexture = TextureResource ? TextureResource->TextureRHI : nullptr;
	return RHITexture ? GraphBuilder.RegisterExternalTexture(CreateRenderTarget(RHITexture, Name)) : GetDefaultBlackTexture(GraphBuilder);
}

FRDGTextureRef FGPUMeshTessellationMeshBuilder::GetDefaultBlackTexture(FRDGBuilder& GraphBuilder)
{
	return GSystemTextures.GetBlackDummy(GraphBuilder);
}

void FGPUMeshTessellationMeshBuilder::ExecuteTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUMeshTessellationBuildData& SourceData,
	UTexture* DisplacementTexture,
	FGPUTessellationBuffers& OutBuffers)
{
	ExecuteTessellationPipelineInternal(GraphBuilder, SourceData, DisplacementTexture, &OutBuffers, nullptr);
}

void FGPUMeshTessellationMeshBuilder::ExecuteTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUMeshTessellationBuildData& SourceData,
	UTexture* DisplacementTexture,
	FGPUTessellatedMeshData& OutMeshData)
{
	ExecuteTessellationPipelineInternal(GraphBuilder, SourceData, DisplacementTexture, nullptr, &OutMeshData);
}

void FGPUMeshTessellationMeshBuilder::ExecuteTessellationPipelineInternal(
	FRDGBuilder& GraphBuilder,
	const FGPUMeshTessellationBuildData& SourceData,
	UTexture* DisplacementTexture,
	FGPUTessellationBuffers* OutBuffers,
	FGPUTessellatedMeshData* OutMeshData)
{
	if (!SourceData.IsValid())
	{
		if (OutBuffers)
		{
			OutBuffers->Reset();
		}
		if (OutMeshData)
		{
			OutMeshData->Reset();
		}
		return;
	}

	FRDGBufferRef SourceVertexBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUMeshTessellation.SourceVertices"),
		sizeof(FGPUMeshTessellationVertex),
		SourceData.Vertices.Num(),
		SourceData.Vertices.GetData(),
		SourceData.Vertices.Num() * sizeof(FGPUMeshTessellationVertex));

	FRDGBufferRef SourceIndexBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUMeshTessellation.SourceIndices"),
		sizeof(uint32),
		SourceData.Indices.Num(),
		SourceData.Indices.GetData(),
		SourceData.Indices.Num() * sizeof(uint32));

	const FGPUMeshTessellationSeamEdge DummySeamEdge;
	const bool bHasSeamEdges = SourceData.SeamEdges.Num() > 0;
	FRDGBufferRef SeamEdgeBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUMeshTessellation.SeamEdges"),
		sizeof(FGPUMeshTessellationSeamEdge),
		bHasSeamEdges ? SourceData.SeamEdges.Num() : 1,
		bHasSeamEdges ? SourceData.SeamEdges.GetData() : &DummySeamEdge,
		(bHasSeamEdges ? SourceData.SeamEdges.Num() : 1) * sizeof(FGPUMeshTessellationSeamEdge));

	FRDGBufferRef PositionBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), SourceData.OutputVertexCount),
		TEXT("GPUMeshTessellation.PositionBuffer"));
	FRDGBufferRef BaseNormalBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), SourceData.OutputVertexCount),
		TEXT("GPUMeshTessellation.BaseNormalBuffer"));
	FRDGBufferRef GeneratedNormalBuffer = SourceData.bGenerateNormals
		? GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), SourceData.OutputVertexCount),
			TEXT("GPUMeshTessellation.GeneratedNormalBuffer"))
		: nullptr;
	FRDGBufferRef UVBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), SourceData.OutputVertexCount),
		TEXT("GPUMeshTessellation.UVBuffer"));
	FRDGBufferRef TangentBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), SourceData.OutputVertexCount),
		TEXT("GPUMeshTessellation.TangentBuffer"));

	FRDGBufferRef IndexBuffer = nullptr;
	{
		FRDGBufferDesc IndexDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SourceData.OutputIndexCount);
		IndexDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
		IndexDesc.Usage |= EBufferUsageFlags::IndexBuffer;
		IndexBuffer = GraphBuilder.CreateBuffer(IndexDesc, TEXT("GPUMeshTessellation.IndexBuffer"));
	}

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(PositionBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BaseNormalBuffer), 0);
	if (GeneratedNormalBuffer)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GeneratedNormalBuffer), 0);
	}
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(UVBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TangentBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndexBuffer, PF_R32_UINT)), 0u);

	FRDGTextureRef DisplacementTextureRDG = CreateRDGTextureFromUTexture(GraphBuilder, DisplacementTexture, TEXT("GPUMeshTessellation.DisplacementTexture"));

	{
		FGPUMeshTessellationVertexGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUMeshTessellationVertexGenerationCS::FParameters>();
		PassParameters->SourceTriangleCount = SourceData.Indices.Num() / 3;
		PassParameters->TessellationFactor = SourceData.TessellationFactor;
		PassParameters->VerticesPerTriangle = SourceData.VerticesPerTriangle;
		PassParameters->OutputVertexCount = SourceData.OutputVertexCount;
		PassParameters->bEnableDisplacement = SourceData.bEnableDisplacement ? 1u : 0u;
		PassParameters->bHasDisplacementTexture = DisplacementTexture != nullptr ? 1u : 0u;
		PassParameters->DisplacementIntensity = SourceData.DisplacementIntensity;
		PassParameters->DisplacementOffset = SourceData.DisplacementOffset;
		PassParameters->InputVertices = GraphBuilder.CreateSRV(SourceVertexBuffer);
		PassParameters->InputIndices = GraphBuilder.CreateSRV(SourceIndexBuffer);
		PassParameters->DisplacementTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisplacementTextureRDG));
		PassParameters->DisplacementSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->OutputPositions = GraphBuilder.CreateUAV(PositionBuffer);
		PassParameters->OutputNormals = GraphBuilder.CreateUAV(BaseNormalBuffer);
		PassParameters->OutputUVs = GraphBuilder.CreateUAV(UVBuffer);
		PassParameters->OutputTangents = GraphBuilder.CreateUAV(TangentBuffer);

		TShaderMapRef<FGPUMeshTessellationVertexGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector GroupCount(FMath::DivideAndRoundUp(SourceData.OutputVertexCount, 64), 1, 1);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GPUMeshTessellation.GenerateVertices"),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
			});
	}

	if (GeneratedNormalBuffer)
	{
		FGPUMeshTessellationNormalGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUMeshTessellationNormalGenerationCS::FParameters>();
		PassParameters->SourceTriangleCount = SourceData.Indices.Num() / 3;
		PassParameters->TessellationFactor = SourceData.TessellationFactor;
		PassParameters->VerticesPerTriangle = SourceData.VerticesPerTriangle;
		PassParameters->OutputVertexCount = SourceData.OutputVertexCount;
		PassParameters->InputGeneratedPositions = GraphBuilder.CreateSRV(PositionBuffer);
		PassParameters->InputBaseNormals = GraphBuilder.CreateSRV(BaseNormalBuffer);
		PassParameters->OutputNormals = GraphBuilder.CreateUAV(GeneratedNormalBuffer);

		TShaderMapRef<FGPUMeshTessellationNormalGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector GroupCount(FMath::DivideAndRoundUp(SourceData.OutputVertexCount, 64), 1, 1);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GPUMeshTessellation.GenerateNormals"),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
			});
	}

	{
		FGPUMeshTessellationIndexGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUMeshTessellationIndexGenerationCS::FParameters>();
		PassParameters->SourceTriangleCount = SourceData.Indices.Num() / 3;
		PassParameters->TessellationFactor = SourceData.TessellationFactor;
		PassParameters->VerticesPerTriangle = SourceData.VerticesPerTriangle;
		PassParameters->IndicesPerTriangle = SourceData.IndicesPerTriangle;
		PassParameters->OutputIndexCount = SourceData.OutputIndexCount;
		PassParameters->SeamEdgeCount = static_cast<uint32>(SourceData.SeamEdges.Num());
		PassParameters->InputSeamEdges = GraphBuilder.CreateSRV(SeamEdgeBuffer);
		PassParameters->OutputIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndexBuffer, PF_R32_UINT));

		TShaderMapRef<FGPUMeshTessellationIndexGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const int32 OutputPrimitiveCount = SourceData.OutputIndexCount / 3;
		const FIntVector GroupCount(FMath::DivideAndRoundUp(OutputPrimitiveCount, 64), 1, 1);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GPUMeshTessellation.GenerateIndices"),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
			});
	}

	FRDGBufferRef OutputNormalBuffer = GeneratedNormalBuffer ? GeneratedNormalBuffer : BaseNormalBuffer;

	if (OutMeshData)
	{
		ExtractMeshData(
			GraphBuilder,
			SourceData.OutputVertexCount,
			SourceData.OutputIndexCount,
			PositionBuffer,
			OutputNormalBuffer,
			UVBuffer,
			IndexBuffer,
			*OutMeshData);
	}

	if (!OutBuffers)
	{
		return;
	}

	OutBuffers->VertexCount = SourceData.OutputVertexCount;
	OutBuffers->IndexCount = SourceData.OutputIndexCount;
	OutBuffers->ResolutionX = 0;
	OutBuffers->ResolutionY = 0;

	TRefCountPtr<FRDGPooledBuffer> PooledPositionBuffer = GraphBuilder.ConvertToExternalBuffer(PositionBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledNormalBuffer = GraphBuilder.ConvertToExternalBuffer(OutputNormalBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledUVBuffer = GraphBuilder.ConvertToExternalBuffer(UVBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledTangentBuffer = GraphBuilder.ConvertToExternalBuffer(TangentBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledIndexBuffer = GraphBuilder.ConvertToExternalBuffer(IndexBuffer);

	OutBuffers->PooledPositionBuffer = PooledPositionBuffer;
	OutBuffers->PooledNormalBuffer = PooledNormalBuffer;
	OutBuffers->PooledUVBuffer = PooledUVBuffer;
	OutBuffers->PooledTangentBuffer = PooledTangentBuffer;
	OutBuffers->PooledIndexBuffer = PooledIndexBuffer;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUMeshTessellation.CreateBufferSRVs"),
		ERDGPassFlags::None,
		[OutBuffers, PooledPositionBuffer, PooledNormalBuffer, PooledUVBuffer, PooledTangentBuffer, PooledIndexBuffer](FRHICommandList& RHICmdList)
		{
			OutBuffers->PositionBuffer = PooledPositionBuffer->GetRHI();
			OutBuffers->PositionSRV = RHICmdList.CreateShaderResourceView(
				OutBuffers->PositionBuffer,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));

			OutBuffers->NormalBuffer = PooledNormalBuffer->GetRHI();
			OutBuffers->NormalSRV = RHICmdList.CreateShaderResourceView(
				OutBuffers->NormalBuffer,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));

			OutBuffers->UVBuffer = PooledUVBuffer->GetRHI();
			OutBuffers->UVSRV = RHICmdList.CreateShaderResourceView(
				OutBuffers->UVBuffer,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));

			OutBuffers->TangentBuffer = PooledTangentBuffer->GetRHI();
			OutBuffers->TangentSRV = RHICmdList.CreateShaderResourceView(
				OutBuffers->TangentBuffer,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));

			OutBuffers->IndexBufferRHI = PooledIndexBuffer->GetRHI();
			OutBuffers->IndexBuffer.IndexBufferRHI = OutBuffers->IndexBufferRHI;
			if (!OutBuffers->IndexBuffer.IsInitialized())
			{
				OutBuffers->IndexBuffer.InitResource(RHICmdList);
			}
		});
}

void FGPUMeshTessellationMeshBuilder::ExtractMeshData(
	FRDGBuilder& GraphBuilder,
	int32 VertexCount,
	int32 IndexCount,
	FRDGBufferRef PositionBuffer,
	FRDGBufferRef NormalBuffer,
	FRDGBufferRef UVBuffer,
	FRDGBufferRef IndexBuffer,
	FGPUTessellatedMeshData& OutMeshData)
{
	OutMeshData.Reset();
	OutMeshData.Vertices.SetNumUninitialized(VertexCount);
	OutMeshData.Normals.SetNumUninitialized(VertexCount);
	OutMeshData.UVs.SetNumUninitialized(VertexCount);
	OutMeshData.Indices.SetNumUninitialized(IndexCount);
	OutMeshData.ResolutionX = 0;
	OutMeshData.ResolutionY = 0;

	FRHIGPUBufferReadback* PositionReadback = new FRHIGPUBufferReadback(TEXT("GPUMeshTessellation.PositionReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("GPUMeshTessellation.NormalReadback"));
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("GPUMeshTessellation.UVReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("GPUMeshTessellation.IndexReadback"));

	AddEnqueueCopyPass(GraphBuilder, PositionReadback, PositionBuffer, sizeof(FVector3f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, NormalReadback, NormalBuffer, sizeof(FVector3f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, UVReadback, UVBuffer, sizeof(FVector2f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, IndexReadback, IndexBuffer, sizeof(uint32) * IndexCount);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUMeshTessellation.ExtractMeshData"),
		ERDGPassFlags::None,
		[PositionReadback, NormalReadback, UVReadback, IndexReadback, &OutMeshData, VertexCount, IndexCount](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.BlockUntilGPUIdle();

			if (const void* PositionData = PositionReadback->Lock(sizeof(FVector3f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.Vertices.GetData(), PositionData, sizeof(FVector3f) * VertexCount);
				PositionReadback->Unlock();
			}

			if (const void* NormalData = NormalReadback->Lock(sizeof(FVector3f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.Normals.GetData(), NormalData, sizeof(FVector3f) * VertexCount);
				NormalReadback->Unlock();
			}

			if (const void* UVData = UVReadback->Lock(sizeof(FVector2f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.UVs.GetData(), UVData, sizeof(FVector2f) * VertexCount);
				UVReadback->Unlock();
			}

			if (const void* IndexData = IndexReadback->Lock(sizeof(uint32) * IndexCount))
			{
				FMemory::Memcpy(OutMeshData.Indices.GetData(), IndexData, sizeof(uint32) * IndexCount);
				IndexReadback->Unlock();
			}

			delete PositionReadback;
			delete NormalReadback;
			delete UVReadback;
			delete IndexReadback;
		});
}
