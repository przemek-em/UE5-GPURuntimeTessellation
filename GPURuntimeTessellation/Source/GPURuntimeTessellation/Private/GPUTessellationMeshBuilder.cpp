// Copyright Epic Games, Inc. All Rights Reserved.

#include "GPUTessellationMeshBuilder.h"
#include "GPUTessellationComputeShaders.h"
#include "GPUTessellationComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "SystemTextures.h"

FGPUTessellationMeshBuilder::FGPUTessellationMeshBuilder()
{
}

FGPUTessellationMeshBuilder::~FGPUTessellationMeshBuilder()
{
}

FIntPoint FGPUTessellationMeshBuilder::CalculateResolution(float TessellationFactor) const
{
	// Convert tessellation factor to "segment" count so adjacent LODs share divisors.
	// Each factor step contributes 4 segments (matching historical density), then we add 1 vertex
	// to close the grid so seams can collapse cleanly between high/low detail edges.
	const int32 ThreadGroupSize = 8; // must match THREADGROUP_SIZE_X/Y in shaders
	const int32 MaxResolution = 1024;
	const int32 MaxSegments = MaxResolution - 1;

	int32 DesiredSegments = FMath::Max(1, FMath::RoundToInt(TessellationFactor) * 4);
	DesiredSegments = FMath::Clamp(DesiredSegments, ThreadGroupSize, MaxSegments);

	// Pad the segment count (not the vertex count) to the threadgroup size so compute dispatches stay aligned.
	int32 Segments = FMath::DivideAndRoundUp(DesiredSegments, ThreadGroupSize) * ThreadGroupSize;
	Segments = FMath::Clamp(Segments, ThreadGroupSize, MaxSegments);

	// Add the extra vertex necessary to close the grid, ensuring adjacent patch edges now line up exactly.
	const int32 Resolution = FMath::Min(Segments + 1, MaxResolution);

	return FIntPoint(Resolution, Resolution);
}

void FGPUTessellationMeshBuilder::ExecuteTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellatedMeshData& OutMeshData)
{
	// Calculate resolution
	FIntPoint Resolution = CalculateResolution(Settings.TessellationFactor);
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Create RDG buffers
	FRDGBufferRef VertexBuffer = nullptr;
	FRDGBufferRef NormalBuffer = nullptr;
	FRDGBufferRef UVBuffer = nullptr;
	FRDGBufferRef IndexBuffer = nullptr;

	// Step 1: Generate vertices
	// Single-mesh generation: no per-patch offset
	DispatchVertexGeneration(GraphBuilder, Settings, Resolution, LocalToWorld, FVector::ZeroVector, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 2: Apply displacement
	DispatchDisplacement(GraphBuilder, Settings, Resolution, DisplacementTexture, SubtractTexture, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 3: Calculate normals (if enabled)
	if (Settings.NormalCalculationMethod != EGPUTessellationNormalMethod::Disabled)
	{
		DispatchNormalCalculation(GraphBuilder, Settings, Resolution, DisplacementTexture, SubtractTexture, NormalMapTexture, VertexBuffer, NormalBuffer, UVBuffer);
	}

	// Step 4: Generate indices (no edge collapsing needed for single mesh)
	DispatchIndexGeneration(GraphBuilder, Resolution, FIntVector4(1, 1, 1, 1), IndexBuffer);

	// Step 5: Extract mesh data to CPU
	ExtractMeshData(GraphBuilder, Resolution, VertexBuffer, NormalBuffer, UVBuffer, IndexBuffer, OutMeshData);
}

void FGPUTessellationMeshBuilder::GenerateMeshSync(
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	UTexture* DisplacementTexture,
	UTexture* RVTMaskTexture,
	FGPUTessellatedMeshData& OutMeshData)
{
	ENQUEUE_RENDER_COMMAND(GenerateTessellatedMesh)(
		[this, Settings, LocalToWorld, CameraPosition, DisplacementTexture, RVTMaskTexture, &OutMeshData](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			
			ExecuteTessellationPipeline(GraphBuilder, Settings, LocalToWorld, CameraPosition, DisplacementTexture, RVTMaskTexture, nullptr, OutMeshData);
			
			GraphBuilder.Execute();
		});
	
	FlushRenderingCommands();
}

void FGPUTessellationMeshBuilder::DispatchVertexGeneration(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	FIntPoint Resolution,
	const FMatrix& LocalToWorld,
	const FVector& PatchLocalOffset,
	FRDGBufferRef& OutVertexBuffer,
	FRDGBufferRef& OutNormalBuffer,
	FRDGBufferRef& OutUVBuffer)
{
	int32 VertexCount = Resolution.X * Resolution.Y;

	// Create output buffers
	OutVertexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount),
		TEXT("GPUTessellation.VertexBuffer"));

	OutNormalBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount),
		TEXT("GPUTessellation.NormalBuffer"));

	OutUVBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), VertexCount),
		TEXT("GPUTessellation.UVBuffer"));

	// Setup shader parameters
	FGPUVertexGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUVertexGenerationCS::FParameters>();
	PassParameters->ResolutionX = Resolution.X;
	PassParameters->ResolutionY = Resolution.Y;
	PassParameters->PlaneSizeX = Settings.PlaneSizeX;
	PassParameters->PlaneSizeY = Settings.PlaneSizeY;
	PassParameters->LocalToWorld = FMatrix44f(LocalToWorld);
	// PatchLocalOffset is added to generated local positions so that the primitive's LocalToWorld
	// (set via the primitive uniform buffer) correctly positions each patch in world space.
	PassParameters->PatchLocalOffset = FVector3f(PatchLocalOffset);
	// PatchUVOffset and PatchUVScale remap UVs for material continuity across patches
	// For single-mesh mode, use full UV range [0,1]
	PassParameters->PatchUVOffset = FVector2f(Settings.UVOffset.X, Settings.UVOffset.Y);
	PassParameters->PatchUVScale = FVector2f(Settings.UVScale.X, Settings.UVScale.Y);
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutVertexBuffer);
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(OutNormalBuffer);
	PassParameters->OutputUVs = GraphBuilder.CreateUAV(OutUVBuffer);

	// Dummy input buffers (not used in simple grid generation, but required by shader parameters)
	// Create them with ERDGBufferFlags::None so RDG knows they're optional
	FRDGBufferRef DummyVertexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 1),
		TEXT("GPUTessellation.DummyVertexBuffer"),
		ERDGBufferFlags::None);
	
	FRDGBufferRef DummyIndexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
		TEXT("GPUTessellation.DummyIndexBuffer"),
		ERDGBufferFlags::None);
	
	FRDGBufferRef DummyTessFactorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 1),
		TEXT("GPUTessellation.DummyTessFactorBuffer"),
		ERDGBufferFlags::None);

	// Clear dummy buffers to satisfy RDG validation
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyVertexBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyIndexBuffer), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyTessFactorBuffer), 0);

	PassParameters->InputVertices = GraphBuilder.CreateSRV(DummyVertexBuffer);
	PassParameters->InputIndices = GraphBuilder.CreateSRV(DummyIndexBuffer);
	PassParameters->TessellationFactors = GraphBuilder.CreateSRV(DummyTessFactorBuffer);

	// Get shader
	TShaderMapRef<FGPUVertexGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch size
	FIntVector GroupCount(
		FMath::DivideAndRoundUp(Resolution.X, 8),
		FMath::DivideAndRoundUp(Resolution.Y, 8),
		1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.GenerateVertices"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::DispatchDisplacement(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	FIntPoint Resolution,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	FRDGBufferRef VertexBuffer,
	FRDGBufferRef NormalBuffer,
	FRDGBufferRef UVBuffer)
{
	int32 VertexCount = Resolution.X * Resolution.Y;

	// Get or create textures
	FRDGTextureRef DisplacementTextureRDG = DisplacementTexture ? 
		CreateRDGTextureFromUTexture(GraphBuilder, DisplacementTexture, TEXT("DisplacementTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	FRDGTextureRef SubtractTextureRDG = SubtractTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, SubtractTexture, TEXT("SubtractTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	// Setup shader parameters
	FGPUDisplacementCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUDisplacementCS::FParameters>();
	PassParameters->DisplacementIntensity = Settings.DisplacementIntensity;
	PassParameters->DisplacementOffset = Settings.DisplacementOffset;
	PassParameters->bUseSineWaveDisplacement = Settings.bUseSineWaveDisplacement ? 1 : 0;
	PassParameters->bHasRVTMask = (SubtractTexture != nullptr) ? 1 : 0;
	PassParameters->VertexCount = VertexCount;
	PassParameters->UVOffset = Settings.UVOffset; // For patch rendering
	PassParameters->UVScale = Settings.UVScale;   // For patch rendering
	PassParameters->DisplacementTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisplacementTextureRDG));
	// Use Bilinear sampling with Clamp addressing to avoid edge wrapping artifacts
	PassParameters->DisplacementSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RVTMaskTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SubtractTextureRDG));
	PassParameters->RVTMaskSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->InputPositions = GraphBuilder.CreateSRV(VertexBuffer);
	PassParameters->InputNormals = GraphBuilder.CreateSRV(NormalBuffer);
	PassParameters->InputUVs = GraphBuilder.CreateSRV(UVBuffer);
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(VertexBuffer);

	// Get shader
	TShaderMapRef<FGPUDisplacementCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch size
	FIntVector GroupCount(FMath::DivideAndRoundUp(VertexCount, 64), 1, 1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.ApplyDisplacement"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::DispatchNormalCalculation(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	FIntPoint Resolution,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FRDGBufferRef VertexBuffer,
	FRDGBufferRef NormalBuffer,
	FRDGBufferRef UVBuffer)
{
	int32 VertexCount = Resolution.X * Resolution.Y;

	// Get displacement texture
	FRDGTextureRef DisplacementTextureRDG = DisplacementTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, DisplacementTexture, TEXT("DisplacementTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	// Get subtract/mask texture (NEW - for correct normal calculation)
	FRDGTextureRef SubtractTextureRDG = SubtractTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, SubtractTexture, TEXT("SubtractTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	// Get normal map texture (optional - only used when NormalCalculationMethod == FromNormalMap)
	FRDGTextureRef NormalMapTextureRDG = NormalMapTexture ?
		CreateRDGTextureFromUTexture(GraphBuilder, NormalMapTexture, TEXT("NormalMapTexture")) :
		GetDefaultWhiteTexture(GraphBuilder);

	// Dummy index buffer for normal calculation (not actually used in grid-based normals)
	FRDGBufferRef DummyIndexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
		TEXT("GPUTessellation.DummyIndexBuffer"),
		ERDGBufferFlags::None);
	
	// Clear dummy buffer to satisfy RDG validation
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyIndexBuffer), 0);

	// Setup shader parameters
	FGPUNormalCalculationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUNormalCalculationCS::FParameters>();
	PassParameters->NormalCalculationMethod = (uint32)Settings.NormalCalculationMethod;
	PassParameters->NormalSmoothingFactor = Settings.NormalSmoothingFactor;
	PassParameters->bInvertNormals = Settings.bInvertNormals ? 1 : 0;
	PassParameters->VertexCount = VertexCount;
	PassParameters->ResolutionX = Resolution.X;
	PassParameters->ResolutionY = Resolution.Y;
	PassParameters->TexelSize = 1.0f / FMath::Max(Resolution.X, Resolution.Y);
	PassParameters->PlaneSizeX = Settings.PlaneSizeX;
	PassParameters->PlaneSizeY = Settings.PlaneSizeY;
	PassParameters->DisplacementTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisplacementTextureRDG));
	// Use Bilinear sampling with Clamp addressing to avoid edge wrapping artifacts
	PassParameters->DisplacementSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->DisplacementIntensity = Settings.DisplacementIntensity;
	// Subtract/mask texture parameters (NEW - for correct normals with RVT)
	PassParameters->bHasSubtractTexture = (SubtractTexture != nullptr) ? 1 : 0;
	PassParameters->SubtractTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SubtractTextureRDG));
	PassParameters->SubtractSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	// Normal map texture parameters
	PassParameters->NormalMapTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalMapTextureRDG));
	PassParameters->NormalMapSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->InputPositions = GraphBuilder.CreateSRV(VertexBuffer);
	PassParameters->InputUVs = GraphBuilder.CreateSRV(UVBuffer);
	PassParameters->InputIndices = GraphBuilder.CreateSRV(DummyIndexBuffer);
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(NormalBuffer);

	// Get shader
	TShaderMapRef<FGPUNormalCalculationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch size
	FIntVector GroupCount(FMath::DivideAndRoundUp(VertexCount, 64), 1, 1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.CalculateNormals"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::DispatchIndexGeneration(
	FRDGBuilder& GraphBuilder,
	FIntPoint Resolution,
	const FIntVector4& EdgeCollapseFactors,
	FRDGBufferRef& OutIndexBuffer)
{
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Create output buffer as a typed buffer with IndexBuffer usage for proper binding
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), IndexCount);
		// Ensure we can write via UAV and bind as an index buffer for drawing
		Desc.Usage |= EBufferUsageFlags::UnorderedAccess;
		Desc.Usage |= EBufferUsageFlags::IndexBuffer;
		OutIndexBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("GPUTessellation.IndexBuffer"));
	}

	// Setup shader parameters
	FGPUIndexGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGPUIndexGenerationCS::FParameters>();
	PassParameters->ResolutionX = Resolution.X;
	PassParameters->ResolutionY = Resolution.Y;
	PassParameters->EdgeCollapseFactors = EdgeCollapseFactors;
	// Create a typed UAV (R32_UINT) to match RWBuffer<uint> in the shader
	PassParameters->OutputIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutIndexBuffer, PF_R32_UINT));

	// Get shader
	TShaderMapRef<FGPUIndexGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch size
	FIntVector GroupCount(
		FMath::DivideAndRoundUp(Resolution.X - 1, 8),
		FMath::DivideAndRoundUp(Resolution.Y - 1, 8),
		1);

	// Add compute pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GPUTessellation.GenerateIndices"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
}

void FGPUTessellationMeshBuilder::ExtractMeshData(
	FRDGBuilder& GraphBuilder,
	FIntPoint Resolution,
	FRDGBufferRef VertexBuffer,
	FRDGBufferRef NormalBuffer,
	FRDGBufferRef UVBuffer,
	FRDGBufferRef IndexBuffer,
	FGPUTessellatedMeshData& OutMeshData)
{
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Prepare output arrays
	OutMeshData.Reset();
	OutMeshData.Vertices.SetNumUninitialized(VertexCount);
	OutMeshData.Normals.SetNumUninitialized(VertexCount);
	OutMeshData.UVs.SetNumUninitialized(VertexCount);
	OutMeshData.Indices.SetNumUninitialized(IndexCount);
	OutMeshData.ResolutionX = Resolution.X;
	OutMeshData.ResolutionY = Resolution.Y;

	// Create staging buffers for CPU readback
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VertexReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("NormalReadback"));
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("UVReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("IndexReadback"));

	// Enqueue copy operations
	AddEnqueueCopyPass(GraphBuilder, VertexReadback, VertexBuffer, sizeof(FVector3f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, NormalReadback, NormalBuffer, sizeof(FVector3f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, UVReadback, UVBuffer, sizeof(FVector2f) * VertexCount);
	AddEnqueueCopyPass(GraphBuilder, IndexReadback, IndexBuffer, sizeof(uint32) * IndexCount);

	// Add pass to extract data after GPU work completes
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("ExtractTessellationData"),
		ERDGPassFlags::None,
		[VertexReadback, NormalReadback, UVReadback, IndexReadback, &OutMeshData, VertexCount, IndexCount](FRHICommandListImmediate& RHICmdList)
		{
			// Wait for GPU to finish
			RHICmdList.BlockUntilGPUIdle();

			// Copy vertices
			if (const void* VertexData = VertexReadback->Lock(sizeof(FVector3f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.Vertices.GetData(), VertexData, sizeof(FVector3f) * VertexCount);
				VertexReadback->Unlock();
			}

			// Copy normals
			if (const void* NormalData = NormalReadback->Lock(sizeof(FVector3f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.Normals.GetData(), NormalData, sizeof(FVector3f) * VertexCount);
				NormalReadback->Unlock();
			}

			// Copy UVs
			if (const void* UVData = UVReadback->Lock(sizeof(FVector2f) * VertexCount))
			{
				FMemory::Memcpy(OutMeshData.UVs.GetData(), UVData, sizeof(FVector2f) * VertexCount);
				UVReadback->Unlock();
			}

			// Copy indices
			if (const void* IndexData = IndexReadback->Lock(sizeof(uint32) * IndexCount))
			{
				FMemory::Memcpy(OutMeshData.Indices.GetData(), IndexData, sizeof(uint32) * IndexCount);
				IndexReadback->Unlock();
			}

			// Cleanup
			delete VertexReadback;
			delete NormalReadback;
			delete UVReadback;
			delete IndexReadback;
		});
}

FRDGTextureRef FGPUTessellationMeshBuilder::CreateRDGTextureFromUTexture(
	FRDGBuilder& GraphBuilder,
	UTexture* Texture,
	const TCHAR* Name)
{
	if (!Texture || !Texture->GetResource())
	{
		return GetDefaultWhiteTexture(GraphBuilder);
	}

	FTextureResource* TextureResource = Texture->GetResource();
	FRHITexture* RHITexture = TextureResource->TextureRHI;

	return GraphBuilder.RegisterExternalTexture(CreateRenderTarget(RHITexture, Name));
}

FRDGTextureRef FGPUTessellationMeshBuilder::GetDefaultWhiteTexture(FRDGBuilder& GraphBuilder)
{
	// Use system white texture
	return GSystemTextures.GetWhiteDummy(GraphBuilder);
}

void FGPUTessellationMeshBuilder::ExecuteTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellationBuffers& OutGPUBuffers)
{
	// Calculate resolution
	FIntPoint Resolution = CalculateResolution(Settings.TessellationFactor);
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;

	// Create RDG buffers for compute shaders
	FRDGBufferRef VertexBuffer = nullptr;
	FRDGBufferRef NormalBuffer = nullptr;
	FRDGBufferRef UVBuffer = nullptr;
	FRDGBufferRef IndexBuffer = nullptr;

	// Step 1: Generate vertices
	// Single-mesh generation: no per-patch offset
	DispatchVertexGeneration(GraphBuilder, Settings, Resolution, LocalToWorld, FVector::ZeroVector, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 2: Apply displacement
	DispatchDisplacement(GraphBuilder, Settings, Resolution, DisplacementTexture, SubtractTexture, VertexBuffer, NormalBuffer, UVBuffer);

	// Step 3: Calculate normals (if enabled)
	if (Settings.NormalCalculationMethod != EGPUTessellationNormalMethod::Disabled)
	{
		DispatchNormalCalculation(GraphBuilder, Settings, Resolution, DisplacementTexture, SubtractTexture, NormalMapTexture, VertexBuffer, NormalBuffer, UVBuffer);
	}

	// Step 4: Generate indices (no edge collapsing needed for single mesh)
	DispatchIndexGeneration(GraphBuilder, Resolution, FIntVector4(1, 1, 1, 1), IndexBuffer);

	// Step 5: Create persistent RHI buffers (no CPU readback!)
	// Extract buffers to persistent pooled buffers
	
	// Store metadata
	OutGPUBuffers.VertexCount = VertexCount;
	OutGPUBuffers.IndexCount = IndexCount;
	OutGPUBuffers.ResolutionX = Resolution.X;
	OutGPUBuffers.ResolutionY = Resolution.Y;
	
	// Convert to external pooled buffers
	TRefCountPtr<FRDGPooledBuffer> PooledPositionBuffer = GraphBuilder.ConvertToExternalBuffer(VertexBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledNormalBuffer = GraphBuilder.ConvertToExternalBuffer(NormalBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledUVBuffer = GraphBuilder.ConvertToExternalBuffer(UVBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledIndexBuffer = GraphBuilder.ConvertToExternalBuffer(IndexBuffer);
	
	// After graph execution, extract the RHI buffers and create SRVs
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("CreateGPUBufferSRVs"),
		ERDGPassFlags::None,
		[&OutGPUBuffers, PooledPositionBuffer, PooledNormalBuffer, PooledUVBuffer, PooledIndexBuffer](FRHICommandList& RHICmdList)
		{
			// Extract RHI buffers from pooled buffers
			if (PooledPositionBuffer.IsValid())
			{
				OutGPUBuffers.PositionBuffer = PooledPositionBuffer->GetRHI();
				
				// Create structured buffer SRV for float3 position data
				OutGPUBuffers.PositionSRV = RHICmdList.CreateShaderResourceView(
					OutGPUBuffers.PositionBuffer,
					FRHIViewDesc::CreateBufferSRV()
						.SetType(FRHIViewDesc::EBufferType::Structured)
				);
			}
			
			if (PooledNormalBuffer.IsValid())
			{
				OutGPUBuffers.NormalBuffer = PooledNormalBuffer->GetRHI();
				
				// Create structured buffer SRV for float3 normal data
				OutGPUBuffers.NormalSRV = RHICmdList.CreateShaderResourceView(
					OutGPUBuffers.NormalBuffer,
					FRHIViewDesc::CreateBufferSRV()
						.SetType(FRHIViewDesc::EBufferType::Structured)
				);
			}
			
			if (PooledUVBuffer.IsValid())
			{
				OutGPUBuffers.UVBuffer = PooledUVBuffer->GetRHI();
				
				// Create structured buffer SRV for float2 UV data
				OutGPUBuffers.UVSRV = RHICmdList.CreateShaderResourceView(
					OutGPUBuffers.UVBuffer,
					FRHIViewDesc::CreateBufferSRV()
						.SetType(FRHIViewDesc::EBufferType::Structured)
				);
			}
			
			// Set up index buffer wrapper for mesh rendering
			if (PooledIndexBuffer.IsValid())
			{
				OutGPUBuffers.IndexBufferRHI = PooledIndexBuffer->GetRHI();
				// Set the base class IndexBufferRHI member for rendering
				OutGPUBuffers.IndexBuffer.IndexBufferRHI = OutGPUBuffers.IndexBufferRHI;
				
				// Initialize the index buffer as a render resource if not already initialized
				if (!OutGPUBuffers.IndexBuffer.IsInitialized())
				{
					OutGPUBuffers.IndexBuffer.InitResource(RHICmdList);
				}
			}
			
			// Debug logging removed - too verbose, enable in SceneProxy if needed
		});
	
	// Tessellation pipeline scheduled (verbose logging removed for performance)
}

// ============================================================================
// SPATIAL PATCH SYSTEM IMPLEMENTATION
// ============================================================================

void FGPUTessellationMeshBuilder::ExecutePatchTessellationPipeline(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	const FConvexVolume* ViewFrustum,
	int32 PatchCountX,
	int32 PatchCountY,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellationPatchBuffers& OutPatchBuffers)
{
	check(PatchCountX > 0 && PatchCountY > 0);
	
	// Calculate patch information (LOD, bounds, culling)
	TArray<FGPUTessellationPatchInfo> PatchInfo;
	
	// Debug: Log the LocalToWorld matrix
	FVector Location = LocalToWorld.GetOrigin();
	FVector Scale = LocalToWorld.GetScaleVector();
	UE_LOG(LogTemp, Warning, TEXT("ExecutePatchPipeline: LocalToWorld Location=%s Scale=%s"), 
		*Location.ToString(), *Scale.ToString());
	
	CalculatePatchInfo(Settings, LocalToWorld, CameraPosition, ViewFrustum, PatchCountX, PatchCountY, PatchInfo);
	ComputePatchEdgeTransitions(PatchCountX, PatchCountY, PatchInfo);
	
	// Resize patch buffer arrays
	int32 TotalPatches = PatchCountX * PatchCountY;
	OutPatchBuffers.PatchBuffers.SetNum(TotalPatches);
	OutPatchBuffers.PatchInfo = PatchInfo;
	OutPatchBuffers.PatchCountX = PatchCountX;
	OutPatchBuffers.PatchCountY = PatchCountY;
	
	// Debug: Log patch subdivision info
	UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Generating %dx%d = %d patches"), 
		PatchCountX, PatchCountY, TotalPatches);
	
	// Generate each patch independently (pure GPU)
	int32 SkippedCulled = 0;
	int32 SkippedInvalidLOD = 0;
	int32 GeneratedSuccessfully = 0;
	
	for (int32 PatchIndex = 0; PatchIndex < TotalPatches; ++PatchIndex)
	{
		const FGPUTessellationPatchInfo& Patch = PatchInfo[PatchIndex];
		
		// Debug: Log first few patches
		if (PatchIndex < 4)
		{
			UE_LOG(LogTemp, Warning, TEXT("  Patch[%d]: UV:(%.3f,%.3f) Size:(%.3f,%.3f) LOD:%d Visible:%d WorldCenter:%s"),
				PatchIndex,
				Patch.PatchOffset.X, Patch.PatchOffset.Y,
				Patch.PatchSize.X, Patch.PatchSize.Y,
				Patch.TessellationLevel, Patch.bVisible, *Patch.WorldCenter.ToString());
		}
		
		// Skip culled patches
		if (!Patch.bVisible)
		{
			OutPatchBuffers.PatchBuffers[PatchIndex].Reset();
			SkippedCulled++;
			continue;
		}
		
		// Validate tessellation level before generating
		if (Patch.TessellationLevel <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("  Patch[%d]: INVALID TessellationLevel=%d, skipping!"), 
				PatchIndex, Patch.TessellationLevel);
			OutPatchBuffers.PatchBuffers[PatchIndex].Reset();
			SkippedInvalidLOD++;
			continue;
		}
		
		// Generate this patch
		GenerateSinglePatch(
			GraphBuilder,
			Settings,
			LocalToWorld,
			Patch,
			DisplacementTexture,
			SubtractTexture,
			NormalMapTexture,
			OutPatchBuffers.PatchBuffers[PatchIndex]
		);
		
		GeneratedSuccessfully++;
		
		// Note: Buffers won't be valid until after GraphBuilder.Execute() is called
		// Validation happens in the scene proxy when rendering
	}
	
	// Log summary
	UE_LOG(LogTemp, Warning, TEXT("GPUTessellation: Patch Generation Summary - Total:%d Generated:%d SkippedCulled:%d SkippedInvalidLOD:%d"),
		TotalPatches, GeneratedSuccessfully, SkippedCulled, SkippedInvalidLOD);
}

void FGPUTessellationMeshBuilder::GenerateSinglePatch(
	FRDGBuilder& GraphBuilder,
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FGPUTessellationPatchInfo& PatchInfo,
	UTexture* DisplacementTexture,
	UTexture* SubtractTexture,
	UTexture* NormalMapTexture,
	FGPUTessellationBuffers& OutPatchBuffers)
{
	// Validate tessellation level first
	const int32 TessellationLevel = PatchInfo.TessellationLevel;
	if (TessellationLevel <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid TessellationLevel=%d (must be > 0)"), 
			TessellationLevel);
		OutPatchBuffers.Reset();
		return;
	}
	
	// Calculate resolution for this patch's tessellation level
	FIntPoint Resolution = CalculateResolution(static_cast<float>(TessellationLevel));
	int32 VertexCount = Resolution.X * Resolution.Y;
	int32 IndexCount = (Resolution.X - 1) * (Resolution.Y - 1) * 6;
	
	// Validation checks
	if (Resolution.X < 2 || Resolution.Y < 2)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid resolution %dx%d (must be at least 2x2)"), 
			Resolution.X, Resolution.Y);
		OutPatchBuffers.Reset();
		return;
	}
	
	if (VertexCount <= 0 || IndexCount <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid counts: Verts=%d Indices=%d"), 
			VertexCount, IndexCount);
		OutPatchBuffers.Reset();
		return;
	}
	
	// Validate UV offset and scale
	if (PatchInfo.PatchOffset.X < 0.0f || PatchInfo.PatchOffset.Y < 0.0f || 
	    PatchInfo.PatchOffset.X > 1.0f || PatchInfo.PatchOffset.Y > 1.0f ||
	    PatchInfo.PatchSize.X <= 0.0f || PatchInfo.PatchSize.Y <= 0.0f ||
	    PatchInfo.PatchSize.X > 1.0f || PatchInfo.PatchSize.Y > 1.0f)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUTessellation: GenerateSinglePatch - Invalid UV parameters: Offset=(%.3f,%.3f) Size=(%.3f,%.3f)"),
			PatchInfo.PatchOffset.X, PatchInfo.PatchOffset.Y, PatchInfo.PatchSize.X, PatchInfo.PatchSize.Y);
		OutPatchBuffers.Reset();
		return;
	}
	
	// Debug: Log resolution calculation for first few patches
	static int32 DebugResCount = 0;
	if (DebugResCount < 3)
	{
		DebugResCount++;
		UE_LOG(LogTemp, Warning, TEXT("    GeneratePatch: TessLevel=%d -> Resolution=%dx%d (%d verts, %d indices)"),
			TessellationLevel, Resolution.X, Resolution.Y, VertexCount, IndexCount);
	}
	
	// Create RDG buffers for this patch
	FRDGBufferRef VertexBuffer = nullptr;
	FRDGBufferRef NormalBuffer = nullptr;
	FRDGBufferRef UVBuffer = nullptr;
	FRDGBufferRef IndexBuffer = nullptr;
	
	// COORDINATE SPACE EXPLANATION:
	// - "Local Space" = Component's local coordinate system (before LocalToWorld transform)
	// - "World Space" = Final world position after LocalToWorld transform is applied
	// - "UV Space" = Texture coordinate space [0,1] for materials and displacement
	//
	// The full plane (Settings.PlaneSizeX × PlaneSizeY) is defined in LOCAL space.
	// Each patch is a subdivision of this full plane, NOT a separate small plane.
	// Patches must all use the SAME plane size to ensure consistent displacement mapping.
	
	float FullPlaneSizeX = Settings.PlaneSizeX;  // Total plane size in local space (X axis)
	float FullPlaneSizeY = Settings.PlaneSizeY;  // Total plane size in local space (Y axis)
	
	// CRITICAL FIX: All patches must use the SAME plane size for consistent displacement!
	// The plane size determines the world-space scale of displacement.
	// Each patch is a "window" into the full plane, not a separate small plane.
	
	// Calculate this patch's size in LOCAL space (size of the "window")
	float PatchLocalSizeX = PatchInfo.PatchSize.X * FullPlaneSizeX;  // Renamed from "PatchWorldSizeX" for clarity
	float PatchLocalSizeY = PatchInfo.PatchSize.Y * FullPlaneSizeY;  // Renamed from "PatchWorldSizeY" for clarity
	
	// Calculate the patch's corner position in local space (plane is centered at origin)
	// The vertex shader generates from [-0.5, +0.5], so we need to offset from there
	float LocalMinX = (PatchInfo.PatchOffset.X - 0.5f) * FullPlaneSizeX;
	float LocalMinY = (PatchInfo.PatchOffset.Y - 0.5f) * FullPlaneSizeY;
	
	// Calculate patch center in LOCAL space
	float LocalCenterX = LocalMinX + (PatchLocalSizeX * 0.5f);
	float LocalCenterY = LocalMinY + (PatchLocalSizeY * 0.5f);
	
	FVector PatchTranslation(LocalCenterX, LocalCenterY, 0.0f);
	
	// Debug: Log patch transform for first few patches
	static int32 DebugPatchCount = 0;
	if (DebugPatchCount < 4)
	{
		DebugPatchCount++;
		FVector WorldCenter = LocalToWorld.TransformPosition(PatchTranslation);
		UE_LOG(LogTemp, Warning, TEXT("  Patch Transform: PatchLocalSize=(%.1f,%.1f) LocalOffset=%s WorldCenter=%s"),
			PatchLocalSizeX, PatchLocalSizeY, *PatchTranslation.ToString(), *WorldCenter.ToString());
	}
	
	// CRITICAL: Create patch-specific settings BUT keep GLOBAL plane size!
	// This ensures displacement intensity is consistent across all patches
	FGPUTessellationSettings PatchSettings = Settings;
	// DO NOT CHANGE PlaneSizeX/Y - must stay global!
	// PatchSettings.PlaneSizeX = Settings.PlaneSizeX;  // Keep original (already set)
	// PatchSettings.PlaneSizeY = Settings.PlaneSizeY;  // Keep original (already set)
	
	// Set UV offset/scale for material UVs and displacement sampling
	// This tells the shader which portion of the texture to sample
	PatchSettings.UVOffset = PatchInfo.PatchOffset;
	PatchSettings.UVScale = PatchInfo.PatchSize;
	
	// Step 1: Generate vertices for this patch using FULL plane size
	// CRITICAL: Pass Settings (not PatchSettings) for PlaneSizeX/Y to ensure global scale
	// Pass PatchSettings only for UV remapping (UVOffset/UVScale)
	// No patch translation needed - vertices generated at correct absolute positions
	DispatchVertexGeneration(GraphBuilder, PatchSettings, Resolution, LocalToWorld, FVector::ZeroVector, VertexBuffer, NormalBuffer, UVBuffer);
	
	// Step 2: Apply displacement (samples texture at patch's UV range using same UV offset/scale)
	DispatchDisplacement(GraphBuilder, PatchSettings, Resolution, DisplacementTexture, SubtractTexture, VertexBuffer, NormalBuffer, UVBuffer);
	
	// Step 3: Calculate normals if enabled (also use patch settings for consistent plane size)
	if (Settings.NormalCalculationMethod != EGPUTessellationNormalMethod::Disabled)
	{
		DispatchNormalCalculation(GraphBuilder, PatchSettings, Resolution, DisplacementTexture, SubtractTexture, NormalMapTexture, VertexBuffer, NormalBuffer, UVBuffer);
	}
	
	// Step 4: Generate indices with seam stitching info
	DispatchIndexGeneration(GraphBuilder, Resolution, PatchInfo.EdgeCollapseFactors, IndexBuffer);
	
	// Step 5: Convert to persistent RHI buffers (pure GPU, no CPU readback)
	OutPatchBuffers.VertexCount = VertexCount;
	OutPatchBuffers.IndexCount = IndexCount;
	OutPatchBuffers.ResolutionX = Resolution.X;
	OutPatchBuffers.ResolutionY = Resolution.Y;
	
	// Convert RDG buffers to external pooled buffers
	TRefCountPtr<FRDGPooledBuffer> PooledPositionBuffer = GraphBuilder.ConvertToExternalBuffer(VertexBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledNormalBuffer = GraphBuilder.ConvertToExternalBuffer(NormalBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledUVBuffer = GraphBuilder.ConvertToExternalBuffer(UVBuffer);
	TRefCountPtr<FRDGPooledBuffer> PooledIndexBuffer = GraphBuilder.ConvertToExternalBuffer(IndexBuffer);
	
	// Create SRVs in RDG pass (after graph execution)
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("CreatePatchBufferSRVs"),
		ERDGPassFlags::None,
		[&OutPatchBuffers, PooledPositionBuffer, PooledNormalBuffer, PooledUVBuffer, PooledIndexBuffer](FRHICommandList& RHICmdList)
		{
			// Extract RHI references
			OutPatchBuffers.PositionBuffer = PooledPositionBuffer->GetRHI();
			OutPatchBuffers.NormalBuffer = PooledNormalBuffer->GetRHI();
			OutPatchBuffers.UVBuffer = PooledUVBuffer->GetRHI();
			OutPatchBuffers.IndexBufferRHI = PooledIndexBuffer->GetRHI();
			
			// Create SRVs
			OutPatchBuffers.PositionSRV = RHICmdList.CreateShaderResourceView(
				OutPatchBuffers.PositionBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
			);
			
			OutPatchBuffers.NormalSRV = RHICmdList.CreateShaderResourceView(
				OutPatchBuffers.NormalBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
			);
			
			OutPatchBuffers.UVSRV = RHICmdList.CreateShaderResourceView(
				OutPatchBuffers.UVBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
			);
			
			// Setup index buffer wrapper
			OutPatchBuffers.IndexBuffer.IndexBufferRHI = OutPatchBuffers.IndexBufferRHI;
			
			// Initialize the index buffer as a render resource if not already initialized
			if (!OutPatchBuffers.IndexBuffer.IsInitialized())
			{
				OutPatchBuffers.IndexBuffer.InitResource(RHICmdList);
			}
		}
	);
}

void FGPUTessellationMeshBuilder::CalculatePatchInfo(
	const FGPUTessellationSettings& Settings,
	const FMatrix& LocalToWorld,
	const FVector& CameraPosition,
	const FConvexVolume* ViewFrustum,
	int32 PatchCountX,
	int32 PatchCountY,
	TArray<FGPUTessellationPatchInfo>& OutPatchInfo) const
{
	int32 TotalPatches = PatchCountX * PatchCountY;
	OutPatchInfo.SetNum(TotalPatches);
	
	// Full plane size in LOCAL space (before transform)
	float PlaneSizeX = Settings.PlaneSizeX;
	float PlaneSizeY = Settings.PlaneSizeY;
	
	// Each patch's UV space coverage
	float PatchUVSizeX = 1.0f / static_cast<float>(PatchCountX);
	float PatchUVSizeY = 1.0f / static_cast<float>(PatchCountY);
	
	// Each patch's size in LOCAL space (renamed from "PatchWorldSize" for clarity)
	float PatchLocalSizeX = PlaneSizeX / static_cast<float>(PatchCountX);
	float PatchLocalSizeY = PlaneSizeY / static_cast<float>(PatchCountY);
	
	FVector PlaneOrigin = LocalToWorld.GetOrigin();
	
	for (int32 Y = 0; Y < PatchCountY; ++Y)
	{
		for (int32 X = 0; X < PatchCountX; ++X)
		{
			int32 PatchIndex = Y * PatchCountX + X;
			FGPUTessellationPatchInfo& Patch = OutPatchInfo[PatchIndex];
			
			// UV space offset and size
			Patch.PatchOffset = FVector2f(X * PatchUVSizeX, Y * PatchUVSizeY);
			Patch.PatchSize = FVector2f(PatchUVSizeX, PatchUVSizeY);
			Patch.PatchIndexX = X;
			Patch.PatchIndexY = Y;
			
			// Calculate world space center - MUST match GenerateSinglePatch calculation
			// The vertex shader generates from [-0.5, +0.5] on XY plane (X, Y, Z=0)
			float LocalMinX = (Patch.PatchOffset.X - 0.5f) * PlaneSizeX;
			float LocalMinY = (Patch.PatchOffset.Y - 0.5f) * PlaneSizeY;
			
			float LocalCenterX = LocalMinX + (PatchLocalSizeX * 0.5f);
			float LocalCenterY = LocalMinY + (PatchLocalSizeY * 0.5f);
			// CRITICAL: Account for displacement offset in the Z center calculation
			// The patch center should be at the average displacement height, not at Z=0
			float LocalCenterZ = Settings.DisplacementOffset + (Settings.DisplacementIntensity * 0.5f);
			FVector LocalCenter(LocalCenterX, LocalCenterY, LocalCenterZ);
			Patch.WorldCenter = LocalToWorld.TransformPosition(LocalCenter);
			
			// Debug: Log first few patch calculations
			if (PatchIndex < 4)
			{
				UE_LOG(LogTemp, Warning, TEXT("  CalcPatchInfo[%d]: LocalMin=(%.1f, %.1f) LocalCenter=(%.1f, %.1f) WorldCenter=%s"),
					PatchIndex, LocalMinX, LocalMinY, LocalCenterX, LocalCenterY, *Patch.WorldCenter.ToString());
			}
			
			// Calculate world space bounds - need to transform all 8 corners to handle rotation/scale
			// HalfExtent on XY plane with Z for displacement
			// CRITICAL: Displacement can be both positive AND negative (offset + intensity)
			// We need to account for the full range of possible displacement
			float MaxDisplacementUp = Settings.DisplacementIntensity + FMath::Max(0.0f, Settings.DisplacementOffset);
			float MaxDisplacementDown = FMath::Abs(FMath::Min(0.0f, Settings.DisplacementOffset));
			float TotalDisplacementRange = MaxDisplacementUp + MaxDisplacementDown;
			
			FVector HalfExtent(PatchLocalSizeX * 0.5f, PatchLocalSizeY * 0.5f, TotalDisplacementRange * 0.5f);
			
			// Build bounds by transforming corners
			// The bounds extend from the center position in all directions by HalfExtent
			TArray<FVector> Corners;
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, -HalfExtent.Y, +HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, +HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(-HalfExtent.X, +HalfExtent.Y, +HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, -HalfExtent.Y, +HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, +HalfExtent.Y, -HalfExtent.Z)));
			Corners.Add(LocalToWorld.TransformPosition(LocalCenter + FVector(+HalfExtent.X, +HalfExtent.Y, +HalfExtent.Z)));
			
			Patch.WorldBounds = FBox(Corners);
			
			// Calculate distance from CAMERA to PATCH center
			// CRITICAL: This must be distance between camera and THIS patch's center,
			// NOT distance from patch to plane origin!
			float Distance = FVector::Dist(Patch.WorldCenter, CameraPosition);
			
			// Determine tessellation level based on distance
			Patch.TessellationLevel = CalculatePatchTessellationLevel(Distance, Settings);
			FIntPoint PatchResolution = CalculateResolution(static_cast<float>(Patch.TessellationLevel));
			Patch.ResolutionX = PatchResolution.X;
			Patch.ResolutionY = PatchResolution.Y;
			
			// Debug: Log ALL patches if first one has issues, or first 8 patches
			// Also log camera and patch positions to verify distance calculation
			if (PatchIndex < 8 || (PatchIndex == 0 && Patch.TessellationLevel <= 0))
			{
				UE_LOG(LogTemp, Warning, TEXT("    Patch[%d]: PatchCenter=%s CameraPos=%s Distance=%.1f -> LOD:%d (Tess=%d)"), 
					PatchIndex, 
					*Patch.WorldCenter.ToString(), 
					*CameraPosition.ToString(),
					Distance, 
					Patch.TessellationLevel, 
					Patch.TessellationLevel);
			}
			
			// CRITICAL ERROR CHECK: If we got an invalid tessellation level, something is very wrong
			if (Patch.TessellationLevel <= 0)
			{
				UE_LOG(LogTemp, Error, TEXT("    Patch[%d]: INVALID TessellationLevel=%d! Distance=%.1f CameraPos=%s PatchCenter=%s"), 
					PatchIndex, Patch.TessellationLevel, Distance, 
					*CameraPosition.ToString(), *Patch.WorldCenter.ToString());
			}
			
			// Frustum culling
			if (ViewFrustum != nullptr && Settings.bEnablePatchCulling)
			{
				Patch.bVisible = ViewFrustum->IntersectBox(Patch.WorldCenter, Patch.WorldBounds.GetExtent());
				if (!Patch.bVisible)
				{
					UE_LOG(LogTemp, Warning, TEXT("    Patch[%d] CULLED by frustum: Center=%s Extent=%s"), 
						PatchIndex, *Patch.WorldCenter.ToString(), *Patch.WorldBounds.GetExtent().ToString());
				}
			}
			else
			{
				// Culling disabled or no frustum - always visible
				Patch.bVisible = true;
			}
		}
	}
}

void FGPUTessellationMeshBuilder::ComputePatchEdgeTransitions(
	int32 PatchCountX,
	int32 PatchCountY,
	TArray<FGPUTessellationPatchInfo>& PatchInfo) const
{
	const int32 ExpectedCount = PatchCountX * PatchCountY;
	if (PatchCountX <= 0 || PatchCountY <= 0 || PatchInfo.Num() != ExpectedCount)
	{
		return;
	}

	const auto GetPatch = [&](int32 X, int32 Y) -> const FGPUTessellationPatchInfo*
	{
		if (X < 0 || X >= PatchCountX || Y < 0 || Y >= PatchCountY)
		{
			return nullptr;
		}
		return &PatchInfo[Y * PatchCountX + X];
	};

	for (int32 Y = 0; Y < PatchCountY; ++Y)
	{
		for (int32 X = 0; X < PatchCountX; ++X)
		{
			FGPUTessellationPatchInfo& Patch = PatchInfo[Y * PatchCountX + X];
			Patch.EdgeCollapseFactors = FIntVector4(1, 1, 1, 1);

			auto ComputeFactor = [&](const FGPUTessellationPatchInfo* Neighbor, bool bVerticalEdge) -> int32
			{
				if (!Neighbor)
				{
					return 1;
				}
				if (Patch.ResolutionX <= 0 || Patch.ResolutionY <= 0 || Neighbor->ResolutionX <= 0 || Neighbor->ResolutionY <= 0)
				{
					return 1;
				}
				if (Neighbor->TessellationLevel >= Patch.TessellationLevel)
				{
					return 1;
				}

				const int32 MySegments = bVerticalEdge ? FMath::Max(1, Patch.ResolutionY - 1) : FMath::Max(1, Patch.ResolutionX - 1);
				const int32 NeighborSegments = bVerticalEdge ? FMath::Max(1, Neighbor->ResolutionY - 1) : FMath::Max(1, Neighbor->ResolutionX - 1);
				if (NeighborSegments <= 0 || MySegments <= NeighborSegments)
				{
					return 1;
				}

				int32 Factor = FMath::Max(1, MySegments / NeighborSegments);
				Factor = FMath::Clamp(Factor, 1, 64);
				return Factor;
			};

			Patch.EdgeCollapseFactors.X = ComputeFactor(GetPatch(X - 1, Y), true);  // West edge (vertical segments)
			Patch.EdgeCollapseFactors.Y = ComputeFactor(GetPatch(X + 1, Y), true);  // East edge
			Patch.EdgeCollapseFactors.Z = ComputeFactor(GetPatch(X, Y - 1), false); // South edge (horizontal segments)
			Patch.EdgeCollapseFactors.W = ComputeFactor(GetPatch(X, Y + 1), false); // North edge
		}
	}
}

int32 FGPUTessellationMeshBuilder::CalculatePatchTessellationLevel(
	float DistanceToCamera,
	const FGPUTessellationSettings& Settings) const
{
	// CRITICAL FIX: Use PatchLevels and PatchDistances, NOT DiscreteLODLevels!
	// DiscreteLODLevels is for DistanceBasedDiscrete mode (whole mesh LOD)
	// PatchLevels/PatchDistances are for DistanceBasedPatches mode (per-patch LOD)
	if (Settings.PatchLevels.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CalculatePatchTessellationLevel: No PatchLevels config, using default 16"));
		return 16; // Default
	}
	
	// Debug: Log LOD config (only once)
	static bool bLoggedConfig = false;
	if (!bLoggedConfig)
	{
		bLoggedConfig = true;
	UE_LOG(LogTemp, Warning, TEXT("Patch LOD Config: %d levels, %d distances"), 
		Settings.PatchLevels.Num(), Settings.PatchDistances.Num());
	for (int32 i = 0; i < FMath::Min(Settings.PatchLevels.Num(), Settings.PatchDistances.Num()); ++i)
	{
		const int32 LevelInt = static_cast<int32>(Settings.PatchLevels[i]);
		const int32 TessInt = ConvertPatchLevelToTessellation(Settings.PatchLevels[i]);
		UE_LOG(LogTemp, Warning, TEXT("  LOD[%d]: Distance <= %.1f uses Level %d (Tess=%d)"), 
			i, Settings.PatchDistances[i], LevelInt, TessInt);
	}
}	// CORRECT LOGIC: Find which distance bracket we fall into
	// PatchDistances should be ordered from smallest to largest
	// PatchLevels should be ordered from highest quality to lowest
	//
	// Example:
	//   PatchDistances = { 2000, 5000, 10000, 20000 }
	//   PatchLevels = { Patch_64, Patch_32, Patch_16, Patch_8 }
	//
	// If distance <= 2000: Use Patch_64 (highest quality)
	// If distance <= 5000: Use Patch_32
	// If distance <= 10000: Use Patch_16
	// If distance <= 20000: Use Patch_8
	// If distance > 20000: Use Patch_8 (lowest, stay there)
	
	int32 TargetLevel = static_cast<int32>(Settings.PatchLevels[0]); // Start with highest quality
	
	// If no distance thresholds, just use the first level
	if (Settings.PatchDistances.Num() == 0)
	{
		return ConvertPatchLevelToTessellation(Settings.PatchLevels[0]);
	}
	
	// Find the appropriate LOD level based on distance
	for (int32 i = 0; i < Settings.PatchDistances.Num(); ++i)
	{
		if (DistanceToCamera <= Settings.PatchDistances[i])
		{
			// We're within this distance threshold, use this level
			// Make sure we have a corresponding level
			if (i < Settings.PatchLevels.Num())
			{
				TargetLevel = static_cast<int32>(Settings.PatchLevels[i]);
			}
			break;
		}
	}
	
	// If we're beyond ALL distance thresholds, use the last (lowest quality) level
	if (DistanceToCamera > Settings.PatchDistances[Settings.PatchDistances.Num() - 1])
	{
		// Use the last level in the array (should be lowest quality)
		int32 LastIndex = FMath::Min(Settings.PatchDistances.Num(), Settings.PatchLevels.Num()) - 1;
		if (LastIndex >= 0 && LastIndex < Settings.PatchLevels.Num())
		{
			TargetLevel = static_cast<int32>(Settings.PatchLevels[LastIndex]);
		}
	}
	
	return ConvertPatchLevelToTessellation(static_cast<EGPUTessellationPatchLevel>(TargetLevel));
}

int32 FGPUTessellationMeshBuilder::ConvertPatchLevelToTessellation(EGPUTessellationPatchLevel Level) const
{
	// Convert enum to actual tessellation factor
	switch (Level)
	{
		case EGPUTessellationPatchLevel::Patch_4:   return 4;
		case EGPUTessellationPatchLevel::Patch_8:   return 8;
		case EGPUTessellationPatchLevel::Patch_16:  return 16;
		case EGPUTessellationPatchLevel::Patch_32:  return 32;
		case EGPUTessellationPatchLevel::Patch_64:  return 64;
		case EGPUTessellationPatchLevel::Patch_128: return 128;
		default: return 16;
	}
}
