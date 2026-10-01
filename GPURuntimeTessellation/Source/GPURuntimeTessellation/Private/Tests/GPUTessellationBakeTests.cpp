// Licensed under the MIT License. See LICENSE file in the project root.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "GPUTessellationStaticMeshBaker.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "GPUTiledTessellationActor.h"
#include "GPUTiledTessellationComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Editor.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUTessellationBakeUVTest, "GPURuntimeTessellation.Renderer.BakedLightmapUVs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUTessellationBakeUVTest::RunTest(const FString& Parameters)
{
	FGPUTessellatedMeshData Data;
	Data.Vertices = { FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0), FVector3f(100, 100, 0) };
	Data.Normals.Init(FVector3f(0, 0, 1), 4);
	Data.UVs = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1), FVector2f(1, 1) };
	Data.Indices = { 0, 1, 2, 2, 1, 3 };
	FGPUTessellationStaticMeshBakeOptions Options;
	Options.AssetDirectory = TEXT("/Game/GPUTessellationAutomation");
	Options.AssetName = TEXT("LightmapUVTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Options.bUseComplexAsSimpleCollision = false;
	Options.bAutoSaveAsset = false;
	UStaticMesh* Mesh = BakeGPUTessellationMeshDataToStaticMesh(GetTransientPackage(), Data, {}, {}, Options);
	if (!TestNotNull(TEXT("Bake produced a static mesh"), Mesh)) { return false; }
	TestEqual(TEXT("Lightmap coordinate targets generated channel"), Mesh->GetLightMapCoordinateIndex(), 1);
	if (TestNotNull(TEXT("Bake built render data"), Mesh->GetRenderData()) && Mesh->GetRenderData()->LODResources.Num() > 0)
	{
		const FStaticMeshVertexBuffer& Vertices = Mesh->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer;
		if (TestTrue(TEXT("Rendered mesh includes generated UV1"), Vertices.GetNumTexCoords() >= 2))
		{
			FVector2f MinUV(1, 1), MaxUV(0, 0);
			for (uint32 Index = 0; Index < Vertices.GetNumVertices(); ++Index)
			{
				const FVector2f UV = Vertices.GetVertexUV(Index, 1);
				TestTrue(TEXT("Lightmap UVs are finite and fit the unit square"), !UV.ContainsNaN() && UV.X >= 0 && UV.X <= 1 && UV.Y >= 0 && UV.Y <= 1);
				MinUV.X = FMath::Min(MinUV.X, UV.X); MinUV.Y = FMath::Min(MinUV.Y, UV.Y);
				MaxUV.X = FMath::Max(MaxUV.X, UV.X); MaxUV.Y = FMath::Max(MaxUV.Y, UV.Y);
			}
			TestTrue(TEXT("Generated UV chart has area"), MaxUV.X - MinUV.X > 0.1f && MaxUV.Y - MinUV.Y > 0.1f);
		}
	}
	// This test creates an in-memory asset only; remove its editor registry/dirty state.
	FAssetRegistryModule::AssetDeleted(Mesh);
	Mesh->ClearFlags(RF_Standalone);
	Mesh->GetPackage()->SetDirtyFlag(false);
	Mesh->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUTiledDrawTest, "GPURuntimeTessellation.Renderer.TiledDepthCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUTiledDrawTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
	{
		AddWarning(TEXT("TiledDepthCapture requires a real SM5+ RHI; skipped."));
		return true;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world available"), World)) { return false; }
	TStrongObjectPtr<UTexture2D> Heightmap(UTexture2D::CreateTransient(2, 2, PF_R32_FLOAT));
	Heightmap->SRGB = false;
	Heightmap->NeverStream = true;
	float* Samples = static_cast<float*>(Heightmap->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
	for (int32 Index = 0; Index < 4; ++Index) { Samples[Index] = 0.5f; }
	Heightmap->GetPlatformData()->Mips[0].BulkData.Unlock();
	Heightmap->UpdateResource();
	AGPUTiledTessellationActor* Actor = World->SpawnActorDeferred<AGPUTiledTessellationActor>(AGPUTiledTessellationActor::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Terrain actor created"), Actor)) { return false; }
	Actor->SetFlags(RF_Transient);
	UGPUTiledTessellationComponent* Component = Actor->GetTessellationComponent();
	FGPUTiledHeightmap Tile;
	Tile.Heightmap = Heightmap.Get();
	Component->Tiles.Add(Tile);
	Component->TileSize = FVector2D(100, 100);
	Component->QuadsPerTile = 4;
	Component->HeightScale = 100;
	Actor->FinishSpawning(FTransform::Identity);
	Component->RebuildTiles();
	TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>());
	Target->InitCustomFormat(16, 16, PF_FloatRGBA, true);
	Target->UpdateResourceImmediate(true);
	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(Actor);
	Actor->AddInstanceComponent(Capture);
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->TextureTarget = Target.Get();
	Capture->CaptureSource = SCS_SceneDepth;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->ShowOnlyComponent(Component);
	Capture->SetWorldLocationAndRotation(FVector(50, 50, 200), FRotator(-90, 0, 0));
	Capture->RegisterComponent();
	FlushRenderingCommands();
	auto CaptureDepth = [&]() -> float
	{
		Capture->CaptureScene();
		FlushRenderingCommands();
		TArray<FFloat16Color> Pixels;
		const bool bRead = Target->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels);
		return bRead && Pixels.Num() == 256 ? Pixels[8 * 16 + 8].R.GetFloat() : -1.0f;
	};
	const float InitialDepth = CaptureDepth();
	TestTrue(FString::Printf(TEXT("Scene proxy draws displaced height into depth (expected 150, actual %.2f)"), InitialDepth), FMath::IsNearlyEqual(InitialDepth, 150.0f, 1.0f));
	Actor->SetActorLocation(FVector(0, 0, 25));
	FlushRenderingCommands();
	const float MovedDepth = CaptureDepth();
	TestTrue(FString::Printf(TEXT("Renderer follows updated transform (expected 125, actual %.2f)"), MovedDepth), FMath::IsNearlyEqual(MovedDepth, 125.0f, 1.0f));
	Capture->DestroyComponent();
	Actor->Destroy();
	FlushRenderingCommands();
	return true;
}
#endif
