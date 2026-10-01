// Licensed under the MIT License. See LICENSE file in the project root.
#include "GPUTiledTessellationShaders.h"
#include "GPUTessellationComputeShaders.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"

class FGPUTiledVertexGenerationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGPUTiledVertexGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUTiledVertexGenerationCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Resolution)
		SHADER_PARAMETER(FVector2f, TileSize)
		SHADER_PARAMETER(FVector2f, TileCoordinate)
		SHADER_PARAMETER(FVector2f, HeightmapSize)
		SHADER_PARAMETER(FIntVector4, HasNeighbors)
		SHADER_PARAMETER(float, HeightScale)
		SHADER_PARAMETER(float, HeightOffset)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Heightmap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WestHeightmap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EastHeightmap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SouthHeightmap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NorthHeightmap)
		SHADER_PARAMETER_SAMPLER(SamplerState, HeightSampler)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutputNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, OutputUVs)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGPUTiledVertexGenerationCS, "/Plugin/GPURuntimeTessellation/Private/GPUTiledTessellation.usf", "MainCS", SF_Compute);

void GenerateGPUTiledTessellationGeometry(
	FRHICommandListImmediate& RHICmdList, ERHIFeatureLevel::Type FeatureLevel,
	TConstArrayView<FGPUTiledTessellationInput> Tiles, const FVector2f& TileSize,
	int32 QuadsPerTile, float HeightScale, float HeightOffset,
	TArray<TUniquePtr<FGPUTessellationBuffers>>& OutBuffers)
{
	check(IsInRenderingThread());
	check(OutBuffers.IsEmpty());
	check(QuadsPerTile >= 1 && QuadsPerTile <= 1024);
	if (Tiles.IsEmpty()) { return; }
	FRDGBuilder GraphBuilder(RHICmdList);
	const uint32 Resolution = QuadsPerTile + 1;
	const uint32 VertexCount = Resolution * Resolution;
	const uint32 IndexCount = QuadsPerTile * QuadsPerTile * 6;
	TMap<FIntPoint, int32> CoordinateToIndex;
	TArray<FRDGTextureRef> Textures;
	for (int32 Index = 0; Index < Tiles.Num(); ++Index)
	{
		CoordinateToIndex.Add(Tiles[Index].Coordinate, Index);
		Textures.Add(Tiles[Index].Heightmap.IsValid()
			? GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Tiles[Index].Heightmap, TEXT("TiledHeightmap"))) : nullptr);
	}

	FRDGBufferDesc IndexDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), IndexCount);
	IndexDesc.Usage |= BUF_IndexBuffer | BUF_UnorderedAccess | BUF_ShaderResource;
	FRDGBufferRef Indices = GraphBuilder.CreateBuffer(IndexDesc, TEXT("GPUTiled.SharedIndices"));
	auto* IndexParams = GraphBuilder.AllocParameters<FGPUIndexGenerationCS::FParameters>();
	IndexParams->ResolutionX = Resolution;
	IndexParams->ResolutionY = Resolution;
	IndexParams->EdgeCollapseFactors = FIntVector4(1, 1, 1, 1);
	IndexParams->OutputIndices = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
	TShaderMapRef<FGPUIndexGenerationCS> IndexShader(GetGlobalShaderMap(FeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPUTiled.SharedTopology"), IndexShader,
		IndexParams, FComputeShaderUtils::GetGroupCount(QuadsPerTile * QuadsPerTile, 64));
	TRefCountPtr<FRDGPooledBuffer> PooledIndices;
	GraphBuilder.QueueBufferExtraction(Indices, &PooledIndices, ERHIAccess::VertexOrIndexBuffer);

	OutBuffers.Reserve(Tiles.Num());
	for (int32 Index = 0; Index < Tiles.Num(); ++Index)
	{
		FGPUTessellationBuffers& Buffers = *OutBuffers.Emplace_GetRef(MakeUnique<FGPUTessellationBuffers>());
		if (!Textures[Index]) { continue; }
		FRDGBufferRef Positions = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(12, VertexCount), TEXT("GPUTiled.Positions"));
		FRDGBufferRef Normals = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(12, VertexCount), TEXT("GPUTiled.Normals"));
		FRDGBufferRef UVs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(8, VertexCount), TEXT("GPUTiled.UVs"));
		auto* Params = GraphBuilder.AllocParameters<FGPUTiledVertexGenerationCS::FParameters>();
		Params->Resolution = Resolution;
		Params->TileSize = TileSize;
		Params->TileCoordinate = FVector2f(Tiles[Index].Coordinate.X, Tiles[Index].Coordinate.Y);
		const FIntPoint Size = Textures[Index]->Desc.Extent;
		Params->HeightmapSize = FVector2f(Size.X, Size.Y);
		Params->HeightScale = HeightScale;
		Params->HeightOffset = HeightOffset;
		Params->Heightmap = Textures[Index];
		const FIntPoint Coordinate = Tiles[Index].Coordinate;
		auto Neighbor = [&](FIntPoint Offset, int32& HasNeighbor) -> FRDGTextureRef
		{
			const int32* NeighborIndex = CoordinateToIndex.Find(Coordinate + Offset);
			HasNeighbor = NeighborIndex && Textures[*NeighborIndex] && Textures[*NeighborIndex]->Desc.Extent == Size;
			return HasNeighbor ? Textures[*NeighborIndex] : Textures[Index];
		};
		Params->WestHeightmap = Neighbor(FIntPoint(-1, 0), Params->HasNeighbors.X);
		Params->EastHeightmap = Neighbor(FIntPoint(1, 0), Params->HasNeighbors.Y);
		Params->SouthHeightmap = Neighbor(FIntPoint(0, -1), Params->HasNeighbors.Z);
		Params->NorthHeightmap = Neighbor(FIntPoint(0, 1), Params->HasNeighbors.W);
		Params->HeightSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params->OutputPositions = GraphBuilder.CreateUAV(Positions);
		Params->OutputNormals = GraphBuilder.CreateUAV(Normals);
		Params->OutputUVs = GraphBuilder.CreateUAV(UVs);
		TShaderMapRef<FGPUTiledVertexGenerationCS> Shader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPUTiled.GenerateTile(%d,%d)", Coordinate.X, Coordinate.Y),
			Shader, Params, FComputeShaderUtils::GetGroupCount(VertexCount, 64));
		GraphBuilder.QueueBufferExtraction(Positions, &Buffers.PooledPositionBuffer, ERHIAccess::SRVMask);
		GraphBuilder.QueueBufferExtraction(Normals, &Buffers.PooledNormalBuffer, ERHIAccess::SRVMask);
		GraphBuilder.QueueBufferExtraction(UVs, &Buffers.PooledUVBuffer, ERHIAccess::SRVMask);
		Buffers.VertexCount = VertexCount;
		Buffers.IndexCount = IndexCount;
		Buffers.ResolutionX = Buffers.ResolutionY = Resolution;
	}
	GraphBuilder.Execute();
	for (TUniquePtr<FGPUTessellationBuffers>& Buffers : OutBuffers)
	{
		if (!Buffers->PooledPositionBuffer) { continue; }
		Buffers->PositionBuffer = Buffers->PooledPositionBuffer->GetRHI();
		Buffers->NormalBuffer = Buffers->PooledNormalBuffer->GetRHI();
		Buffers->UVBuffer = Buffers->PooledUVBuffer->GetRHI();
		Buffers->PooledIndexBuffer = PooledIndices;
		Buffers->IndexBufferRHI = PooledIndices->GetRHI();
		Buffers->IndexBuffer.IndexBufferRHI = Buffers->IndexBufferRHI;
		Buffers->IndexBuffer.InitResource(RHICmdList);
		const auto StructuredView = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured);
		Buffers->PositionSRV = RHICmdList.CreateShaderResourceView(Buffers->PositionBuffer, StructuredView);
		Buffers->NormalSRV = RHICmdList.CreateShaderResourceView(Buffers->NormalBuffer, StructuredView);
		Buffers->UVSRV = RHICmdList.CreateShaderResourceView(Buffers->UVBuffer, StructuredView);
	}
}
