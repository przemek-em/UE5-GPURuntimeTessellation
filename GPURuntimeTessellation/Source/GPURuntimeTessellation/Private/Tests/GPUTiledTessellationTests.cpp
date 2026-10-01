// Licensed under the MIT License. See LICENSE file in the project root.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "GPUTiledTessellationComponent.h"
#include "GPUTiledTessellationShaders.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"
#include "TextureResource.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUTiledValidationTest, "GPURuntimeTessellation.Tiled.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUTiledValidationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGPUTiledTessellationComponent> Component(NewObject<UGPUTiledTessellationComponent>());
	TStrongObjectPtr<UTexture2D> Heightmap(UTexture2D::CreateTransient(5, 3, PF_R32_FLOAT));
	Heightmap->SRGB = false;
	Heightmap->NeverStream = true;
	Component->QuadsPerTile = 4;
	Component->TileSize = FVector2D(100, 200);
	Component->HeightScale = -300;
	Component->HeightOffset = 100;
	FGPUTiledHeightmap Tile;
	Tile.Coordinate = FIntPoint(-1, 2);
	Tile.Heightmap = Heightmap.Get();
	Component->Tiles.Add(Tile);
	FString Issues;
	TestTrue(TEXT("Valid rectangular map and negative coordinate"), Component->ValidateTiles(Issues));
	const FBox Box = Component->CalcBounds(FTransform::Identity).GetBox();
	TestTrue(TEXT("Bounds include negative height scale and tile coordinate"),
		Box.IsInsideOrOn(FVector(-100, 400, -200)) && Box.IsInsideOrOn(FVector(0, 600, 100)));
	TestEqual(TEXT("Budget counts one shared index grid"), Component->GetEstimatedGeometryBytes(), int64(25 * 32 + 16 * 6 * 4));
	Component->Tiles.Add(Tile);
	TestFalse(TEXT("Reject duplicate tile coordinates"), Component->ValidateTiles(Issues));
	Component->Tiles[1].Coordinate.X = 0;
	TestTrue(TEXT("Accept adjacent tiles"), Component->ValidateTiles(Issues));
	TestEqual(TEXT("Two tiles share topology in estimate"), Component->GetEstimatedGeometryBytes(), int64(2 * 25 * 32 + 16 * 6 * 4));
	Heightmap->SRGB = true;
	TestFalse(TEXT("Reject gamma-encoded heightmaps"), Component->ValidateTiles(Issues));
	Heightmap->SRGB = false;
	Heightmap->NeverStream = false;
	TestFalse(TEXT("Reject streaming without a residency manager"), Component->ValidateTiles(Issues));
	Heightmap->NeverStream = true;
	Component->QuadsPerTile = 1024;
	Component->GeometryBudgetMB = 1;
	TestFalse(TEXT("Refuse over-budget geometry"), Component->ValidateTiles(Issues));
	Component->QuadsPerTile = 0;
	TestFalse(TEXT("Reject invalid topology"), Component->ValidateTiles(Issues));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUTiledGPUSeamsTest, "GPURuntimeTessellation.Tiled.GPUSeams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUTiledGPUSeamsTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
	{
		AddWarning(TEXT("GPUSeams requires a real SM5+ RHI; skipped."));
		return true;
	}
	TArray<TStrongObjectPtr<UTexture2D>> Textures;
	TArray<FTextureResource*> Resources;
	TArray<FIntPoint> Coordinates = { FIntPoint(-1, 0), FIntPoint(0, 0), FIntPoint(-1, 1), FIntPoint(0, 1) };
	for (FIntPoint Coordinate : Coordinates)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(5, 3, PF_R32_FLOAT);
		Textures.Emplace(Texture);
		Texture->SRGB = false;
		Texture->NeverStream = true;
		auto& Mip = Texture->GetPlatformData()->Mips[0];
		float* Samples = static_cast<float*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
		for (int32 Y = 0; Y < 3; ++Y)
		{
			for (int32 X = 0; X < 5; ++X)
			{
				Samples[Y * 5 + X] = 0.1f + 0.2f * (Coordinate.X + 1 + X / 4.0f) + 0.1f * (Coordinate.Y + Y / 2.0f);
			}
		}
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		Resources.Add(Texture->GetResource());
	}
	struct FReadbackData
	{
		TArray<TArray<FVector3f>> Positions;
		TArray<TArray<FVector3f>> Normals;
		TArray<TArray<FVector2f>> UVs;
		bool bSharedIndices = true;
	};
	FReadbackData Result;
	ENQUEUE_RENDER_COMMAND(TestGPUTiledSeams)([Resources, Coordinates, &Result](FRHICommandListImmediate& RHICmdList)
	{
		TArray<FGPUTiledTessellationInput> Inputs;
		for (int32 Index = 0; Index < Resources.Num(); ++Index)
		{
			FGPUTiledTessellationInput& Input = Inputs.AddDefaulted_GetRef();
			Input.Coordinate = Coordinates[Index];
			Input.Heightmap = Resources[Index]->TextureRHI;
		}
		TArray<TUniquePtr<FGPUTessellationBuffers>> Buffers;
		GenerateGPUTiledTessellationGeometry(RHICmdList, GMaxRHIFeatureLevel, Inputs, FVector2f(100, 200), 4, 1000, -100, Buffers);
		const FBufferRHIRef SharedIndices = Buffers[0]->IndexBufferRHI;
		for (const auto& TileBuffers : Buffers)
		{
			Result.bSharedIndices &= TileBuffers->IndexBufferRHI == SharedIndices;
			FRHIGPUBufferReadback Positions(TEXT("GPUTiledTest.Positions"));
			FRHIGPUBufferReadback Normals(TEXT("GPUTiledTest.Normals"));
			FRHIGPUBufferReadback UVs(TEXT("GPUTiledTest.UVs"));
			RHICmdList.Transition(FRHITransitionInfo(TileBuffers->PositionBuffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
			RHICmdList.Transition(FRHITransitionInfo(TileBuffers->NormalBuffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
			RHICmdList.Transition(FRHITransitionInfo(TileBuffers->UVBuffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
			Positions.EnqueueCopy(RHICmdList, TileBuffers->PositionBuffer, 25 * sizeof(FVector3f));
			Normals.EnqueueCopy(RHICmdList, TileBuffers->NormalBuffer, 25 * sizeof(FVector3f));
			UVs.EnqueueCopy(RHICmdList, TileBuffers->UVBuffer, 25 * sizeof(FVector2f));
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			auto& OutPositions = Result.Positions.AddDefaulted_GetRef();
			auto& OutNormals = Result.Normals.AddDefaulted_GetRef();
			auto& OutUVs = Result.UVs.AddDefaulted_GetRef();
			OutPositions.SetNumUninitialized(25);
			OutNormals.SetNumUninitialized(25);
			OutUVs.SetNumUninitialized(25);
			FMemory::Memcpy(OutPositions.GetData(), Positions.Lock(25 * sizeof(FVector3f)), 25 * sizeof(FVector3f));
			FMemory::Memcpy(OutNormals.GetData(), Normals.Lock(25 * sizeof(FVector3f)), 25 * sizeof(FVector3f));
			FMemory::Memcpy(OutUVs.GetData(), UVs.Lock(25 * sizeof(FVector2f)), 25 * sizeof(FVector2f));
			Positions.Unlock(); Normals.Unlock(); UVs.Unlock();
			TileBuffers->Reset();
		}
	});
	FlushRenderingCommands();
	TestEqual(TEXT("Generated all four tiles"), Result.Positions.Num(), 4);
	TestTrue(TEXT("Index buffer is shared"), Result.bSharedIndices);
	const FVector3f ExpectedNormal = FVector3f(-2.0f, -0.5f, 1.0f).GetSafeNormal();
	for (int32 TileIndex = 0; TileIndex < Result.Positions.Num(); ++TileIndex)
	{
		for (int32 Index = 0; Index < 25; ++Index)
		{
			const FVector3f Position = Result.Positions[TileIndex][Index];
			const float ExpectedHeight = 2 * (Position.X + 100) + 0.5f * Position.Y;
			TestTrue(TEXT("Height follows continuous plane, including closing corner"), FMath::IsNearlyEqual(Position.Z, ExpectedHeight, 0.01f));
			TestTrue(TEXT("Neighbor and outer-edge normals match the analytical plane"), Result.Normals[TileIndex][Index].Equals(ExpectedNormal, 0.001f));
		}
	}
	if (Result.Positions.Num() == 4)
	{
		for (int32 Row = 0; Row < 5; ++Row)
		{
			TestTrue(TEXT("East/west positions are coincident"), Result.Positions[0][Row * 5 + 4].Equals(Result.Positions[1][Row * 5], 0.001f));
			TestTrue(TEXT("East/west material UVs are continuous"), Result.UVs[0][Row * 5 + 4].Equals(Result.UVs[1][Row * 5], 0.001f));
			TestTrue(TEXT("North/south positions are coincident"), Result.Positions[0][20 + Row].Equals(Result.Positions[2][Row], 0.001f));
		}
	}
	return true;
}
#endif
