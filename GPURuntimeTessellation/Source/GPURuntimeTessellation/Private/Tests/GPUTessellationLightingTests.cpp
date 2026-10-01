// Licensed under the MIT License. See LICENSE file in the project root.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "GPUTessellationActor.h"
#include "GPUTiledTessellationActor.h"
#include "GPUTiledTessellationComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Editor.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "Materials/Material.h"
#include "ShaderCompiler.h"
#include "GPUTessellationRendering.h"
#include "RenderUtils.h"
#include "GPUTessellationMeshBuilder.h"
#include "RHIGPUReadback.h"
#include "Misc/ScopedCVar.h"

namespace
{
UTexture2D* MakeLightingHeightmap(bool bSlope)
{
	UTexture2D* Texture = UTexture2D::CreateTransient(8, 8, PF_R32_FLOAT);
	Texture->SRGB = false;
	Texture->NeverStream = true;
	float* Samples = static_cast<float*>(Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
	for (int32 Y = 0; Y < 8; ++Y)
	{
		for (int32 X = 0; X < 8; ++X)
		{
			Samples[Y * 8 + X] = bSlope ? 0.25f + 0.25f * (X + 0.5f) / 8 + 0.125f * (Y + 0.5f) / 8 : 0;
		}
	}
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}

struct FLightingCapture
{
	TStrongObjectPtr<UTextureRenderTarget2D> Target;
	USceneCaptureComponent2D* Capture;
	explicit FLightingCapture(AActor* Actor, ESceneCaptureSource Source)
		: Target(NewObject<UTextureRenderTarget2D>())
		, Capture(NewObject<USceneCaptureComponent2D>(Actor))
	{
		Target->InitCustomFormat(32, 32, PF_FloatRGBA, true);
		Target->UpdateResourceImmediate(true);
		Actor->AddInstanceComponent(Capture);
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->TextureTarget = Target.Get();
		Capture->CaptureSource = Source;
		Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		Capture->ShowFlags.SetAtmosphere(false);
		Capture->ShowFlags.SetFog(false);
		Capture->ShowFlags.SetSkyLighting(false);
		Capture->ShowFlags.SetGlobalIllumination(false);
		Capture->ShowFlags.SetReflectionEnvironment(false);
		Capture->ShowFlags.SetScreenSpaceReflections(false);
		Capture->ShowFlags.SetAmbientOcclusion(false);
		Capture->ShowFlags.SetTemporalAA(false);
		Capture->ShowFlags.SetAntiAliasing(false);
		Capture->ShowFlags.SetPostProcessing(false);
		Capture->ShowFlags.SetDecals(false);
		Capture->RegisterComponent();
	}
	FVector ReadCenter()
	{
		Capture->CaptureScene();
		FlushRenderingCommands();
		TArray<FFloat16Color> Pixels;
		if (!Target->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels) || Pixels.Num() != 32 * 32)
		{
			return FVector(1.e9);
		}
		const FFloat16Color& Pixel = Pixels[16 * 32 + 16];
		return FVector(Pixel.R.GetFloat(), Pixel.G.GetFloat(), Pixel.B.GetFloat());
	}
	~FLightingCapture() { Capture->DestroyComponent(); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUTessellationNormalTransformTest, "GPURuntimeTessellation.Renderer.NormalTransforms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUTessellationNormalTransformTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) { AddWarning(TEXT("Requires a real SM5+ RHI.")); return true; }
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world"), World)) { return false; }
	TStrongObjectPtr<UTexture2D> Heightmap(MakeLightingHeightmap(true));
	AGPUTessellationActor* Actor = World->SpawnActor<AGPUTessellationActor>();
	Actor->SetFlags(RF_Transient);
	UGPUTessellationComponent* Component = Actor->GetTessellationComponent();
	Component->CollisionMode = EGPUTessellationCollisionMode::Disabled;
	Component->SetDisplacementTexture(Heightmap.Get());
	// The engine's default grid material adds its own normal detail; isolate the VF normal.
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage()));
	Material->SetShadingModel(MSM_DefaultLit);
	Material->GetEditorOnlyData()->BaseColor.UseConstant = true;
	Material->GetEditorOnlyData()->BaseColor.Constant = FColor::White;
	Material->GetEditorOnlyData()->Roughness.UseConstant = true;
	Material->GetEditorOnlyData()->Roughness.Constant = 1;
	Material->GetEditorOnlyData()->Specular.UseConstant = true;
	Material->GetEditorOnlyData()->Specular.Constant = 0;
	Material->PostEditChange();
	// UE 5.8 PostEditChange requests metadata only; compile the rendering shaders explicitly.
	Material->ForceRecompileForRendering();
	Component->SetMaterial(0, Material.Get());
	if (GShaderCompilingManager) { GShaderCompilingManager->FinishAllCompilation(); }
	FGPUTessellationSettings Settings;
	Settings.PlaneSizeX = Settings.PlaneSizeY = 100;
	Settings.TessellationFactor = 4;
	Settings.DisplacementIntensity = 100;
	Settings.bUseSineWaveDisplacement = false;
	const FVector Scale(2, 1, 0.25);
	Actor->SetActorScale3D(Scale);
	{
		FLightingCapture Readback(Actor, SCS_Normal);
		Readback.Capture->ShowOnlyComponent(Component);
		Readback.Capture->SetWorldLocationAndRotation(FVector(0, 0, 100), FRotator(-90, 0, 0));
		Readback.Capture->FOVAngle = 30;
		Component->UpdateSettings(Settings);
		Readback.Capture->CaptureSource = SCS_BaseColor;
		const FVector BaseColor = Readback.ReadCenter();
		AddInfo(FString::Printf(TEXT("Plain material base color: %s"), *BaseColor.ToString()));
		TestTrue(TEXT("Capture uses the plain test material"), BaseColor.Equals(FVector(1), 0.01));
		Readback.Capture->CaptureSource = SCS_Normal;
		const FVector Expected = FVector(-0.25 / Scale.X, -0.125 / Scale.Y, 1.0 / Scale.Z).GetSafeNormal();
		for (EGPUTessellationNormalMethod Mode : { EGPUTessellationNormalMethod::FiniteDifference,
			EGPUTessellationNormalMethod::GeometryBased, EGPUTessellationNormalMethod::FromHeightTexture })
		{
			Settings.NormalCalculationMethod = Mode;
			Component->UpdateSettings(Settings);
			const FVector Actual = Readback.ReadCenter().GetSafeNormal();
			TestTrue(FString::Printf(TEXT("Mode %d scaled normal: expected %s, actual %s"), int32(Mode), *Expected.ToString(), *Actual.ToString()), Actual.Equals(Expected, 0.015));
		}
	}
	Actor->Destroy();
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUHeightTextureShadowNormalTest, "GPURuntimeTessellation.Renderer.HeightTextureShadowNormals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUHeightTextureShadowNormalTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) { AddWarning(TEXT("Requires a real SM5+ RHI.")); return true; }
	TStrongObjectPtr<UTexture2D> Heightmap(MakeLightingHeightmap(true));
	FlushRenderingCommands();
	TArray<FVector3f> Normals;
	ENQUEUE_RENDER_COMMAND(TestGPUHeightShadowNormals)([Texture = Heightmap.Get(), &Normals](FRHICommandListImmediate& RHICmdList)
	{
		FGPUTessellationSettings Settings;
		Settings.PlaneSizeX = Settings.PlaneSizeY = 100;
		Settings.DisplacementIntensity = 100;
		Settings.bUseSineWaveDisplacement = false;
		Settings.TessellationFactor = 4;
		Settings.NormalCalculationMethod = EGPUTessellationNormalMethod::FromHeightTexture;
		Settings.HeightTextureNormalDetailStrength = 1.1f;
		Settings.HeightTextureNormalTexelStep = 0.5f;
		Settings.bUsePersistentPatchBuffers = true;
		Settings.bEnableQuadtreeCulling = false;
		Settings.QuadtreeMaxDepth = 1;
		Settings.QuadtreeMaxVisibleLeaves = 4;
		Settings.QuadtreeLevels = { EGPUTessellationPatchLevel::Patch_8 };
		Settings.QuadtreeDistances = { 800000 };
		FGPUTessellationMeshBuilder Builder;
		auto ReadNormal = [&](FGPUTessellationBuffers& Buffers)
		{
			if (!Buffers.IsValid()) { return; }
			FRHIGPUBufferReadback Readback(TEXT("GPUHeightTexture.ShadowNormal"));
			RHICmdList.Transition(FRHITransitionInfo(Buffers.NormalBuffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
			const uint32 Bytes = Buffers.VertexCount * sizeof(FVector3f);
			Readback.EnqueueCopy(RHICmdList, Buffers.NormalBuffer, Bytes);
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			const auto* Values = static_cast<const FVector3f*>(Readback.Lock(Bytes));
			Normals.Add(Values[(Buffers.ResolutionY / 2) * Buffers.ResolutionX + Buffers.ResolutionX / 2]);
			Readback.Unlock();
		};
		FGPUTessellationBuffers Single;
		{
			FRDGBuilder Graph(RHICmdList);
			Builder.ExecuteTessellationPipeline(Graph, Settings, FMatrix::Identity, FVector::ZeroVector, Texture, nullptr, nullptr, Single);
			Graph.Execute();
		}
		ReadNormal(Single);
		Single.Reset();
		FGPUTessellationPatchBuffers Patches;
		{
			FRDGBuilder Graph(RHICmdList);
			Builder.ExecutePatchTessellationPipeline(Graph, Settings, FMatrix::Identity, FVector::ZeroVector, nullptr, 2, 2, Texture, nullptr, nullptr, Patches);
			Graph.Execute();
		}
		for (auto& Patch : Patches.PatchBuffers) { ReadNormal(Patch); }
		Patches.Reset();
		Settings.LODMode = EGPUTessellationLODMode::DistanceBasedQuadtree;
		{
			FRDGBuilder Graph(RHICmdList);
			Builder.ExecuteQuadtreeTessellationPipeline(Graph, Settings, FScaleMatrix(FVector(4096)), FVector::ZeroVector, nullptr, Texture, nullptr, nullptr, Patches);
			Graph.Execute();
		}
		for (auto& Patch : Patches.PatchBuffers) { ReadNormal(Patch); }
		Patches.Reset();
	});
	FlushRenderingCommands();
	TestEqual(TEXT("Read single, four spatial patches and four quadtree leaves"), Normals.Num(), 9);
	const FVector3f Expected = FVector3f(-0.25f, -0.125f, 1).GetSafeNormal();
	for (int32 Index = 0; Index < Normals.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("Shadow/WPO normal %d follows geometry: %s"), Index, *Normals[Index].ToString()), Normals[Index].Equals(Expected, 0.001f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPULargeQuadtreeShadowTest, "GPURuntimeTessellation.Renderer.LargeQuadtreeShadowCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPULargeQuadtreeShadowTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) { AddWarning(TEXT("Requires a real SM5+ RHI.")); return true; }
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world"), World)) { return false; }
	FScopedCVar<int32> ConventionalShadows(TEXT("r.Shadow.Virtual.Enable"), 0);
	TStrongObjectPtr<UTexture2D> Heightmap(MakeLightingHeightmap(false));
	constexpr double Scale = 4096;
	AGPUTiledTessellationActor* Ground = World->SpawnActor<AGPUTiledTessellationActor>();
	Ground->SetFlags(RF_Transient);
	UGPUTiledTessellationComponent* Receiver = Ground->GetTessellationComponent();
	FGPUTiledHeightmap Tile;
	Tile.Heightmap = Heightmap.Get();
	Receiver->Tiles.Add(Tile);
	Receiver->TileSize = FVector2D(1000, 1000);
	Receiver->QuadsPerTile = 4;
	Ground->SetActorScale3D(FVector(Scale));
	Ground->SetActorLocation(FVector(-500 * Scale, -500 * Scale, 0));
	Receiver->RebuildTiles();
	AGPUTessellationActor* Ridge = World->SpawnActor<AGPUTessellationActor>();
	Ridge->SetFlags(RF_Transient);
	UGPUTessellationComponent* Caster = Ridge->GetTessellationComponent();
	Caster->CollisionMode = EGPUTessellationCollisionMode::Disabled;
	Caster->SetDisplacementTexture(Heightmap.Get());
	FGPUTessellationSettings Settings;
	Settings.PlaneSizeX = Settings.PlaneSizeY = 1000;
	Settings.DisplacementIntensity = 200;
	Settings.bUseSineWaveDisplacement = false;
	Settings.LODMode = EGPUTessellationLODMode::DistanceBasedQuadtree;
	Settings.bUsePersistentPatchBuffers = true;
	Settings.bEnableQuadtreeCulling = false;
	Settings.NormalCalculationMethod = EGPUTessellationNormalMethod::FromHeightTexture;
	Settings.HeightTextureNormalDetailStrength = 1.1f;
	Settings.HeightTextureNormalTexelStep = 0.5f;
	// Keep the user's world dimensions and render path with inexpensive flat geometry.
	Settings.QuadtreeMaxDepth = 2;
	Settings.QuadtreeMaxVisibleLeaves = 16;
	Settings.QuadtreeLevels = { EGPUTessellationPatchLevel::Patch_8, EGPUTessellationPatchLevel::Patch_4 };
	Settings.QuadtreeDistances = { 800000, 2000000, 4000000 };
	Ridge->SetActorScale3D(FVector(Scale));
	Ridge->SetActorLocation(FVector(-800 * Scale, 0, 80 * Scale));
	Caster->UpdateSettings(Settings);
	AActor* LightActor = World->SpawnActor<AActor>();
	LightActor->SetFlags(RF_Transient);
	UDirectionalLightComponent* Light = NewObject<UDirectionalLightComponent>(LightActor);
	LightActor->AddInstanceComponent(Light);
	LightActor->SetRootComponent(Light);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetIntensity(50); // Keep the low-sun lit control well above readback noise.
	// A low sun behind a distant ridge: 3.28km elevation, 32.77km upstream.
	Light->SetWorldRotation(FRotator(-FMath::RadiansToDegrees(FMath::Atan(0.1)), 0, 0));
	Light->SetDynamicShadowDistanceMovableLight(40000);
	Light->DynamicShadowCascades = 4;
	Light->RegisterComponent();
	{
		FLightingCapture Readback(Ground, SCS_SceneColorHDR);
		Readback.Capture->ShowOnlyComponent(Receiver);
		Readback.Capture->ShowOnlyComponent(Caster);
		// Receiver depth is 8.19km, inside the user's 20km shadow distance.
		Readback.Capture->SetWorldLocationAndRotation(FVector(0, 0, 200 * Scale), FRotator(-90, 0, 0));
		Readback.Capture->FOVAngle = 20;
		Caster->SetCastShadow(false);
		const FVector Lit = Readback.ReadCenter();
		Caster->SetCastShadow(true);
		const FVector ShortRange = Readback.ReadCenter();
		Light->DynamicShadowCascades = 1;
		Light->SetDynamicShadowDistanceMovableLight(2000000);
		Light->MarkRenderStateDirty();
		const FVector SingleCascade = Readback.ReadCenter();
		Light->DynamicShadowCascades = 4;
		Light->MarkRenderStateDirty();
		const FVector Expanded = Readback.ReadCenter();
		Light->SetDynamicShadowDistanceMovableLight(40000);
		Light->FarShadowCascadeCount = 2;
		Light->FarShadowDistance = 2000000;
		Light->MarkRenderStateDirty();
		Caster->bCastFarShadow = false;
		Caster->UpdateSettings(Settings);
		const FVector FarDisabled = Readback.ReadCenter();
		Caster->bCastFarShadow = true;
		Caster->UpdateSettings(Settings);
		const FVector FarEnabled = Readback.ReadCenter();
		AddInfo(FString::Printf(TEXT("4096-scale conventional RGB: lit %s, 400m coverage %s, 20km/one cascade %s, 20km/four %s, far flag off %s, on %s"),
			*Lit.ToString(), *ShortRange.ToString(), *SingleCascade.ToString(), *Expanded.ToString(), *FarDisabled.ToString(), *FarEnabled.ToString()));
		TestTrue(TEXT("Receiver is lit outside the near cascade range"), Lit.GetMax() > 0.01 && ShortRange.Equals(Lit, 0.01));
		TestTrue(TEXT("Expanded conventional coverage shadows the large quadtree caster"), Expanded.GetMax() < Lit.GetMax() * 0.15);
		TestTrue(TEXT("One cascade still casts a broad shadow with sufficient distance"), SingleCascade.GetMax() < Lit.GetMax() * 0.15);
		TestTrue(TEXT("Far cascades require the component's Far Shadow flag"), FarDisabled.Equals(Lit, 0.01) && FarEnabled.GetMax() < Lit.GetMax() * 0.15);
	}
	LightActor->Destroy();
	Ridge->Destroy();
	Ground->Destroy();
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUTessellationShadowCullTest, "GPURuntimeTessellation.Renderer.ShadowCullVolumes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUTessellationShadowCullTest::RunTest(const FString& Parameters)
{
	FSceneViewInitOptions Options;
	Options.SetViewRectangle(FIntRect(0, 0, 32, 32));
	Options.ViewRotationMatrix = FMatrix::Identity;
	Options.ProjectionMatrix = FMatrix::Identity;
	FSceneView View(Options);
	View.ViewFrustum.Planes = { FPlane(1, 0, 0, 10) }; // Camera excludes the caster at x=100.
	View.ViewFrustum.Init();
	const FVector Center(100, 0, 0), Extent(1);
	TestFalse(TEXT("Main view culls an offscreen caster"), IsGPUTessellationBoxVisible(View, Center, Extent));
	FConvexVolume ShadowFrustum;
	ShadowFrustum.Planes = { FPlane(1, 0, 0, 10), FPlane(-1, 0, 0, 10) };
	ShadowFrustum.Init();
	View.SetDynamicMeshElementsShadowCullFrustum(&ShadowFrustum);
	View.SetPreShadowTranslation(FVector(-100, 0, 0));
	TestTrue(TEXT("Translated light volume includes the offscreen caster"), IsGPUTessellationBoxVisible(View, Center, Extent));
	TestFalse(TEXT("Translated light volume still excludes out-of-volume geometry"), IsGPUTessellationBoxVisible(View, FVector(200, 0, 0), Extent));
	ShadowFrustum.Planes.Empty();
	ShadowFrustum.Init();
	TestTrue(TEXT("An unconstrained shadow volume keeps casters"), IsGPUTessellationBoxVisible(View, Center, Extent));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGPUTessellationOffscreenShadowTest, "GPURuntimeTessellation.Renderer.OffscreenPatchShadow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGPUTessellationOffscreenShadowTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) { AddWarning(TEXT("Requires a real SM5+ RHI.")); return true; }
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world"), World)) { return false; }
	TStrongObjectPtr<UTexture2D> Heightmap(MakeLightingHeightmap(false));
	AGPUTiledTessellationActor* Ground = World->SpawnActor<AGPUTiledTessellationActor>();
	Ground->SetFlags(RF_Transient);
	UGPUTiledTessellationComponent* Receiver = Ground->GetTessellationComponent();
	FGPUTiledHeightmap Tile;
	Tile.Heightmap = Heightmap.Get();
	Receiver->Tiles.Add(Tile);
	Receiver->TileSize = FVector2D(100, 100);
	Receiver->QuadsPerTile = 4;
	Ground->SetActorLocation(FVector(-50, -50, 0));
	Receiver->RebuildTiles();
	AGPUTessellationActor* Ridge = World->SpawnActor<AGPUTessellationActor>();
	Ridge->SetFlags(RF_Transient);
	UGPUTessellationComponent* Caster = Ridge->GetTessellationComponent();
	Caster->CollisionMode = EGPUTessellationCollisionMode::Disabled;
	Caster->SetDisplacementTexture(Heightmap.Get());
	FGPUTessellationSettings Settings;
	Settings.PlaneSizeX = Settings.PlaneSizeY = 60;
	Settings.TessellationFactor = 2;
	Settings.DisplacementIntensity = 0; // Tight caster bounds, so the far-distance test exercises culling.
	Settings.bUseSineWaveDisplacement = false;
	Settings.LODMode = EGPUTessellationLODMode::DistanceBasedPatches;
	Settings.PatchCountX = Settings.PatchCountY = 2;
	Settings.bEnablePatchCulling = true;
	Caster->UpdateSettings(Settings);
	Ridge->SetActorLocation(FVector(-100, 0, 100));
	AActor* LightActor = World->SpawnActor<AActor>();
	LightActor->SetFlags(RF_Transient);
	UDirectionalLightComponent* Light = NewObject<UDirectionalLightComponent>(LightActor);
	LightActor->AddInstanceComponent(Light);
	LightActor->SetRootComponent(Light);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetIntensity(5);
	Light->SetWorldRotation(FRotator(-45, 0, 0));
	Light->SetDynamicShadowDistanceMovableLight(2000);
	Light->RegisterComponent();
	bool bVirtualShadowMaps = false;
	ENQUEUE_RENDER_COMMAND(GPUTessellationCheckShadowMethod)([&bVirtualShadowMaps](FRHICommandListImmediate&)
	{
		bVirtualShadowMaps = UseNonNaniteVirtualShadowMaps(GMaxRHIShaderPlatform, GMaxRHIFeatureLevel);
	});
	FlushRenderingCommands();
	AddInfo(bVirtualShadowMaps ? TEXT("Shadow path: non-Nanite Virtual Shadow Maps") : TEXT("Shadow path: conventional shadow maps"));
	{
		FLightingCapture Readback(Ground, SCS_SceneColorHDR);
		Readback.Capture->ShowOnlyComponent(Receiver);
		Readback.Capture->ShowOnlyComponent(Caster);
		Readback.Capture->SetWorldLocationAndRotation(FVector(0, 0, 200), FRotator(-90, 0, 0));
		Readback.Capture->FOVAngle = 20;
		Caster->SetCastShadow(false);
		const FVector Lit = Readback.ReadCenter();
		Caster->SetCastShadow(true);
		const FVector Shadowed = Readback.ReadCenter();
		Settings.bEnablePatchCulling = false;
		Caster->UpdateSettings(Settings);
		const FVector Unculled = Readback.ReadCenter();
		AddInfo(FString::Printf(TEXT("Direct-light RGB: lit %s, shadowed %s, culling disabled %s"), *Lit.ToString(), *Shadowed.ToString(), *Unculled.ToString()));
		TestTrue(TEXT("Receiver has measurable direct light with shadow casting disabled"), Lit.GetMax() > 0.01 && Lit.GetMax() < 1.e6);
		TestTrue(TEXT("Offscreen patch blocks direct light"), Shadowed.GetMax() < Lit.GetMax() * 0.15);
		TestTrue(TEXT("Enabling patch culling preserves the shadow"), Shadowed.Equals(Unculled, FMath::Max(0.001, Lit.GetMax() * 0.02)));
		Caster->SetCastShadow(false);
		const FVector ShadowDisabled = Readback.ReadCenter();
		TestTrue(TEXT("Disabling Cast Shadow also works with patch culling disabled"), ShadowDisabled.Equals(Lit, FMath::Max(0.001, Lit.GetMax() * 0.02)));
		Caster->SetCastShadow(true);
		if (bVirtualShadowMaps)
		{
			Light->SetDynamicShadowDistanceMovableLight(10);
			Caster->bCastFarShadow = false;
			Caster->UpdateSettings(Settings);
			const FVector FarCulled = Readback.ReadCenter();
			Caster->bCastFarShadow = true;
			Caster->UpdateSettings(Settings);
			const FVector FarShadow = Readback.ReadCenter();
			AddInfo(FString::Printf(TEXT("Distant VSM RGB: Far Shadow off %s, on %s"), *FarCulled.ToString(), *FarShadow.ToString()));
			TestTrue(TEXT("Far Shadow opt-in restores the distant terrain shadow"), FarShadow.GetMax() < Lit.GetMax() * 0.15 && FarCulled.GetMax() > Lit.GetMax() * 0.8);
		}
	}
	LightActor->Destroy();
	Ridge->Destroy();
	Ground->Destroy();
	FlushRenderingCommands();
	return true;
}
#endif
