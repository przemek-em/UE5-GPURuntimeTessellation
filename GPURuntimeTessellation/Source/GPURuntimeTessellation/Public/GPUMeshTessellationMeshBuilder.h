// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "GPUTessellationMeshBuilder.h"
#include "GPUMeshTessellationComponent.h"

class UTexture;

/** Executes the arbitrary static-mesh tessellation compute path. */
class GPURUNTIMETESSELLATION_API FGPUMeshTessellationMeshBuilder
{
public:
	void ExecuteTessellationPipeline(
		FRDGBuilder& GraphBuilder,
		const FGPUMeshTessellationBuildData& SourceData,
		UTexture* DisplacementTexture,
		FGPUTessellationBuffers& OutBuffers);

	void ExecuteTessellationPipeline(
		FRDGBuilder& GraphBuilder,
		const FGPUMeshTessellationBuildData& SourceData,
		UTexture* DisplacementTexture,
		FGPUTessellatedMeshData& OutMeshData);

private:
	void ExecuteTessellationPipelineInternal(
		FRDGBuilder& GraphBuilder,
		const FGPUMeshTessellationBuildData& SourceData,
		UTexture* DisplacementTexture,
		FGPUTessellationBuffers* OutBuffers,
		FGPUTessellatedMeshData* OutMeshData);

	void ExtractMeshData(
		FRDGBuilder& GraphBuilder,
		int32 VertexCount,
		int32 IndexCount,
		FRDGBufferRef PositionBuffer,
		FRDGBufferRef NormalBuffer,
		FRDGBufferRef UVBuffer,
		FRDGBufferRef IndexBuffer,
		FGPUTessellatedMeshData& OutMeshData);

	FRDGTextureRef CreateRDGTextureFromUTexture(FRDGBuilder& GraphBuilder, UTexture* Texture, const TCHAR* Name);
	FRDGTextureRef GetDefaultBlackTexture(FRDGBuilder& GraphBuilder);
};
