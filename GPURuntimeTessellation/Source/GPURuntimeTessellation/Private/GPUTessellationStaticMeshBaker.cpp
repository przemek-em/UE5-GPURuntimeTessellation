// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUTessellationStaticMeshBaker.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Math/Float16.h"
#include "Math/Float16Color.h"
#include "MeshAttributes.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UnrealClient.h"

namespace
{
struct FGPUTessellationScalarTextureData
{
	int32 SizeX = 0;
	int32 SizeY = 0;
	TArray<float> Values;

	bool IsValid() const
	{
		return SizeX > 0 && SizeY > 0 && Values.Num() == SizeX * SizeY;
	}

	float Sample(float U, float V) const
	{
		if (!IsValid())
		{
			return 1.0f;
		}

		const float X = FMath::Clamp(U, 0.0f, 1.0f) * (float)SizeX - 0.5f;
		const float Y = FMath::Clamp(V, 0.0f, 1.0f) * (float)SizeY - 0.5f;
		const int32 X0 = FMath::Clamp(FMath::FloorToInt(X), 0, SizeX - 1);
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Y), 0, SizeY - 1);
		const int32 X1 = FMath::Clamp(X0 + 1, 0, SizeX - 1);
		const int32 Y1 = FMath::Clamp(Y0 + 1, 0, SizeY - 1);
		const float AlphaX = FMath::Clamp(X - (float)X0, 0.0f, 1.0f);
		const float AlphaY = FMath::Clamp(Y - (float)Y0, 0.0f, 1.0f);

		const float H00 = Values[Y0 * SizeX + X0];
		const float H10 = Values[Y0 * SizeX + X1];
		const float H01 = Values[Y1 * SizeX + X0];
		const float H11 = Values[Y1 * SizeX + X1];
		const float H0 = FMath::Lerp(H00, H10, AlphaX);
		const float H1 = FMath::Lerp(H01, H11, AlphaX);
		return FMath::Lerp(H0, H1, AlphaY);
	}
};

FString SanitizeBakeAssetName(const FString& RawName, const UObject* SourceObject)
{
	FString Name = RawName.TrimStartAndEnd();
	if (Name.IsEmpty())
	{
		Name = FString::Printf(TEXT("%s_BakedMesh"), SourceObject ? *SourceObject->GetName() : TEXT("GPUTessellation"));
	}

	for (TCHAR& Character : Name)
	{
		if (!FChar::IsAlnum(Character) && Character != TCHAR('_'))
		{
			Character = TCHAR('_');
		}
	}

	return Name.IsEmpty() ? TEXT("GPUTessellation_BakedMesh") : Name;
}

FString NormalizeBakePackageDirectory(const FString& RawDirectory)
{
	FString Directory = RawDirectory.TrimStartAndEnd();
	Directory.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (Directory.IsEmpty())
	{
		Directory = TEXT("/Game/GPUTessellationBakes");
	}

	while (Directory.EndsWith(TEXT("/")))
	{
		Directory.LeftChopInline(1);
	}

	if (!Directory.StartsWith(TEXT("/Game")))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Asset directory must be inside /Game. Got '%s'."), *Directory);
		return FString();
	}

	return Directory;
}

void MakeUniqueBakePackageName(const FString& Directory, const FString& BaseAssetName, FString& OutPackageName, FString& OutAssetName)
{
	OutAssetName = BaseAssetName;
	OutPackageName = FString::Printf(TEXT("%s/%s"), *Directory, *OutAssetName);

	int32 Suffix = 1;
	while (FindPackage(nullptr, *OutPackageName) || FPackageName::DoesPackageExist(OutPackageName))
	{
		OutAssetName = FString::Printf(TEXT("%s_%03d"), *BaseAssetName, Suffix++);
		OutPackageName = FString::Printf(TEXT("%s/%s"), *Directory, *OutAssetName);
	}
}

float GPUTessellationReadTextureSourceScalar(const uint8* SourceData, ETextureSourceFormat Format, int32 PixelIndex)
{
	switch (Format)
	{
		case TSF_G8:
			return (float)SourceData[PixelIndex] / 255.0f;

		case TSF_G16:
			return (float)reinterpret_cast<const uint16*>(SourceData)[PixelIndex] / 65535.0f;

		case TSF_R16F:
			return reinterpret_cast<const FFloat16*>(SourceData)[PixelIndex].GetFloat();

		case TSF_R32F:
			return reinterpret_cast<const float*>(SourceData)[PixelIndex];

		case TSF_BGRA8:
		case TSF_BGRE8:
			return (float)reinterpret_cast<const FColor*>(SourceData)[PixelIndex].R / 255.0f;

		case TSF_RGBA16:
			return (float)reinterpret_cast<const uint16*>(SourceData)[PixelIndex * 4 + 0] / 65535.0f;

		case TSF_RGBA16F:
			return reinterpret_cast<const FFloat16Color*>(SourceData)[PixelIndex].R.GetFloat();

		case TSF_RGBA32F:
			return reinterpret_cast<const FLinearColor*>(SourceData)[PixelIndex].R;

		default:
			return 1.0f;
	}
}

bool ReadTextureToScalarData(UTexture* Texture, FGPUTessellationScalarTextureData& OutData)
{
	OutData = FGPUTessellationScalarTextureData();
	if (!Texture)
	{
		return false;
	}

	if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
	{
		if (!Texture2D->Source.IsValid() || Texture2D->Source.GetSizeX() <= 0 || Texture2D->Source.GetSizeY() <= 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Texture '%s' has no readable editor source data."), *GetNameSafe(Texture2D));
			return false;
		}

		const int32 Width = (int32)Texture2D->Source.GetSizeX();
		const int32 Height = (int32)Texture2D->Source.GetSizeY();
		const ETextureSourceFormat Format = Texture2D->Source.GetFormat();
		const uint8* SourceData = Texture2D->Source.LockMipReadOnly(0);
		if (!SourceData)
		{
			return false;
		}

		OutData.SizeX = Width;
		OutData.SizeY = Height;
		OutData.Values.SetNumUninitialized(Width * Height);
		for (int32 PixelIndex = 0; PixelIndex < OutData.Values.Num(); ++PixelIndex)
		{
			OutData.Values[PixelIndex] = GPUTessellationReadTextureSourceScalar(SourceData, Format, PixelIndex);
		}

		Texture2D->Source.UnlockMip(0);
		return OutData.IsValid();
	}

	if (UTextureRenderTarget2D* RenderTarget = Cast<UTextureRenderTarget2D>(Texture))
	{
		FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTargetResource || RenderTarget->SizeX <= 0 || RenderTarget->SizeY <= 0)
		{
			return false;
		}

		TArray<FLinearColor> Pixels;
		const FIntRect SampleRect(0, 0, RenderTarget->SizeX, RenderTarget->SizeY);
		if (!RenderTargetResource->ReadLinearColorPixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax, CubeFace_MAX), SampleRect))
		{
			return false;
		}

		OutData.SizeX = RenderTarget->SizeX;
		OutData.SizeY = RenderTarget->SizeY;
		OutData.Values.SetNumUninitialized(Pixels.Num());
		for (int32 PixelIndex = 0; PixelIndex < Pixels.Num(); ++PixelIndex)
		{
			OutData.Values[PixelIndex] = Pixels[PixelIndex].R;
		}
		return OutData.IsValid();
	}

	UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Texture '%s' is not a Texture2D or TextureRenderTarget2D."), *GetNameSafe(Texture));
	return false;
}

float SampleBakedHeight(
	const FGPUTessellationScalarTextureData& HeightData,
	const FGPUTessellationScalarTextureData* SubtractData,
	float U,
	float V,
	float DisplacementIntensity)
{
	float Height = HeightData.Sample(U, V) * DisplacementIntensity;
	if (SubtractData && SubtractData->IsValid())
	{
		Height *= 1.0f - SubtractData->Sample(U, V);
	}
	return Height;
}

FVector3f ApplyBakedNormalStrength(const FVector3f& Normal, float Strength)
{
	const float SafeZ = FMath::Max(FMath::Abs(Normal.Z), 1.0e-4f);
	const FVector2f Slope(-Normal.X / SafeZ, -Normal.Y / SafeZ);
	const FVector2f ScaledSlope = Slope * FMath::Max(Strength, 0.0f);
	return FVector3f(-ScaledSlope.X, -ScaledSlope.Y, 1.0f).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::ZAxisVector);
}

FColor EncodeTangentSpaceNormal(const FVector3f& Normal)
{
	const FVector3f SafeNormal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::ZAxisVector);
	return FColor(
		(uint8)FMath::Clamp(FMath::RoundToInt((SafeNormal.X * 0.5f + 0.5f) * 255.0f), 0, 255),
		(uint8)FMath::Clamp(FMath::RoundToInt((SafeNormal.Y * 0.5f + 0.5f) * 255.0f), 0, 255),
		(uint8)FMath::Clamp(FMath::RoundToInt((SafeNormal.Z * 0.5f + 0.5f) * 255.0f), 0, 255),
		255);
}

FVector3f GetSafeBakeNormal(const FGPUTessellatedMeshData& MeshData, uint32 VertexIndex)
{
	if (MeshData.Normals.IsValidIndex((int32)VertexIndex))
	{
		const FVector3f Normal = MeshData.Normals[(int32)VertexIndex].GetSafeNormal(UE_SMALL_NUMBER, FVector3f::ZAxisVector);
		if (!Normal.ContainsNaN())
		{
			return Normal;
		}
	}

	return FVector3f::ZAxisVector;
}

FVector2f GetSafeBakeUV(const FGPUTessellatedMeshData& MeshData, uint32 VertexIndex)
{
	return MeshData.UVs.IsValidIndex((int32)VertexIndex) ? MeshData.UVs[(int32)VertexIndex] : FVector2f::ZeroVector;
}

FVector3f MakeFallbackTangent(const FVector3f& Normal)
{
	const FVector3f Reference = FMath::Abs(Normal.Z) < 0.99f ? FVector3f::ZAxisVector : FVector3f::XAxisVector;
	return FVector3f::CrossProduct(Reference, Normal).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::YAxisVector);
}

void AddBakeTriangle(
	FMeshDescription& MeshDescription,
	TVertexInstanceAttributesRef<FVector3f>& VertexInstanceNormals,
	TVertexInstanceAttributesRef<FVector3f>& VertexInstanceTangents,
	TVertexInstanceAttributesRef<float>& VertexInstanceBinormalSigns,
	TVertexInstanceAttributesRef<FVector4f>& VertexInstanceColors,
	TVertexInstanceAttributesRef<FVector2f>& VertexInstanceUVs,
	const TArray<FVertexID>& VertexIDs,
	const FGPUTessellatedMeshData& MeshData,
	FPolygonGroupID PolygonGroupID,
	uint32 Index0,
	uint32 Index1,
	uint32 Index2)
{
	const FVertexInstanceID Instance0 = MeshDescription.CreateVertexInstance(VertexIDs[(int32)Index0]);
	const FVertexInstanceID Instance1 = MeshDescription.CreateVertexInstance(VertexIDs[(int32)Index1]);
	const FVertexInstanceID Instance2 = MeshDescription.CreateVertexInstance(VertexIDs[(int32)Index2]);

	const FVector3f Normal0 = GetSafeBakeNormal(MeshData, Index0);
	const FVector3f Normal1 = GetSafeBakeNormal(MeshData, Index1);
	const FVector3f Normal2 = GetSafeBakeNormal(MeshData, Index2);

	VertexInstanceNormals.Set(Instance0, Normal0);
	VertexInstanceNormals.Set(Instance1, Normal1);
	VertexInstanceNormals.Set(Instance2, Normal2);

	VertexInstanceTangents.Set(Instance0, MakeFallbackTangent(Normal0));
	VertexInstanceTangents.Set(Instance1, MakeFallbackTangent(Normal1));
	VertexInstanceTangents.Set(Instance2, MakeFallbackTangent(Normal2));
	VertexInstanceBinormalSigns.Set(Instance0, 1.0f);
	VertexInstanceBinormalSigns.Set(Instance1, 1.0f);
	VertexInstanceBinormalSigns.Set(Instance2, 1.0f);

	VertexInstanceColors.Set(Instance0, FVector4f(1.0f, 1.0f, 1.0f, 1.0f));
	VertexInstanceColors.Set(Instance1, FVector4f(1.0f, 1.0f, 1.0f, 1.0f));
	VertexInstanceColors.Set(Instance2, FVector4f(1.0f, 1.0f, 1.0f, 1.0f));

	VertexInstanceUVs.Set(Instance0, 0, GetSafeBakeUV(MeshData, Index0));
	VertexInstanceUVs.Set(Instance1, 0, GetSafeBakeUV(MeshData, Index1));
	VertexInstanceUVs.Set(Instance2, 0, GetSafeBakeUV(MeshData, Index2));

	const FPolygonID PolygonID = MeshDescription.CreatePolygon(PolygonGroupID, { Instance0, Instance1, Instance2 });
	MeshDescription.ComputePolygonTriangulation(PolygonID);
}
}

UStaticMesh* BakeGPUTessellationMeshDataToStaticMesh(
	const UObject* SourceObject,
	const FGPUTessellatedMeshData& MeshData,
	const TArray<FGPUTessellationStaticMeshBakeSection>& Sections,
	const TArray<UMaterialInterface*>& Materials,
	const FGPUTessellationStaticMeshBakeOptions& Options)
{
	if (!MeshData.IsValid() || MeshData.Vertices.Num() <= 0 || MeshData.Indices.Num() < 3)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: No valid mesh data to bake."));
		return nullptr;
	}

	const FString Directory = NormalizeBakePackageDirectory(Options.AssetDirectory);
	if (Directory.IsEmpty())
	{
		return nullptr;
	}

	const FString BaseAssetName = SanitizeBakeAssetName(Options.AssetName, SourceObject);
	FString PackageName;
	FString AssetName;
	MakeUniqueBakePackageName(Directory, BaseAssetName, PackageName, AssetName);

	FText InvalidPackageReason;
	if (!FPackageName::IsValidLongPackageName(PackageName, false, &InvalidPackageReason))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Invalid static mesh package name '%s': %s."),
			*PackageName,
			*InvalidPackageReason.ToString());
		return nullptr;
	}

	FMeshDescription MeshDescription;
	FStaticMeshAttributes Attributes(MeshDescription);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> PolygonGroupSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	VertexInstanceUVs.SetNumChannels(1);

	MeshDescription.ReserveNewVertices(MeshData.Vertices.Num());
	MeshDescription.ReserveNewVertexInstances(MeshData.Indices.Num());
	MeshDescription.ReserveNewEdges(MeshData.Indices.Num());
	MeshDescription.ReserveNewPolygons(MeshData.Indices.Num() / 3);

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(MeshData.Vertices.Num());
	for (const FVector3f& Vertex : MeshData.Vertices)
	{
		const FVertexID VertexID = MeshDescription.CreateVertex();
		VertexPositions[VertexID] = Vertex;
		VertexIDs.Add(VertexID);
	}

	TArray<FGPUTessellationStaticMeshBakeSection> BakeSections = Sections;
	if (BakeSections.Num() == 0)
	{
		FGPUTessellationStaticMeshBakeSection& Section = BakeSections.AddDefaulted_GetRef();
		Section.FirstIndex = 0;
		Section.NumTriangles = MeshData.Indices.Num() / 3;
		Section.MaterialIndex = 0;
	}

	int32 MaxMaterialIndex = 0;
	for (const FGPUTessellationStaticMeshBakeSection& Section : BakeSections)
	{
		MaxMaterialIndex = FMath::Max(MaxMaterialIndex, Section.MaterialIndex);
	}

	TArray<FStaticMaterial> StaticMaterials;
	StaticMaterials.Reserve(MaxMaterialIndex + 1);
	for (int32 MaterialIndex = 0; MaterialIndex <= MaxMaterialIndex; ++MaterialIndex)
	{
		const FName SlotName(*FString::Printf(TEXT("Material_%d"), MaterialIndex));
		UMaterialInterface* Material = Materials.IsValidIndex(MaterialIndex) ? Materials[MaterialIndex] : nullptr;
		StaticMaterials.Add(FStaticMaterial(Material, SlotName, SlotName));
	}

	TMap<int32, FPolygonGroupID> MaterialToPolygonGroup;
	for (int32 MaterialIndex = 0; MaterialIndex <= MaxMaterialIndex; ++MaterialIndex)
	{
		const FName SlotName = StaticMaterials[MaterialIndex].MaterialSlotName;
		const FPolygonGroupID PolygonGroupID = MeshDescription.CreatePolygonGroup();
		PolygonGroupSlotNames[PolygonGroupID] = SlotName;
		MaterialToPolygonGroup.Add(MaterialIndex, PolygonGroupID);
	}

	int32 AddedTriangleCount = 0;
	for (const FGPUTessellationStaticMeshBakeSection& Section : BakeSections)
	{
		const FPolygonGroupID* PolygonGroupID = MaterialToPolygonGroup.Find(Section.MaterialIndex);
		if (!PolygonGroupID)
		{
			continue;
		}

		const int32 FirstIndex = FMath::Clamp(Section.FirstIndex, 0, MeshData.Indices.Num());
		const int32 LastIndexExclusive = FMath::Clamp(FirstIndex + Section.NumTriangles * 3, 0, MeshData.Indices.Num());
		for (int32 Index = FirstIndex; Index + 2 < LastIndexExclusive; Index += 3)
		{
			const uint32 Index0 = MeshData.Indices[Index + 0];
			const uint32 Index1 = MeshData.Indices[Index + 1];
			const uint32 Index2 = MeshData.Indices[Index + 2];
			if (Index0 == Index1 || Index1 == Index2 || Index2 == Index0)
			{
				continue;
			}
			if (!VertexIDs.IsValidIndex((int32)Index0) || !VertexIDs.IsValidIndex((int32)Index1) || !VertexIDs.IsValidIndex((int32)Index2))
			{
				continue;
			}

			AddBakeTriangle(
				MeshDescription,
				VertexInstanceNormals,
				VertexInstanceTangents,
				VertexInstanceBinormalSigns,
				VertexInstanceColors,
				VertexInstanceUVs,
				VertexIDs,
				MeshData,
				*PolygonGroupID,
				Index0,
				Index1,
				Index2);
			++AddedTriangleCount;
		}
	}

	if (AddedTriangleCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Mesh data contained no valid triangles."));
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return nullptr;
	}
	Package->FullyLoad();

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!StaticMesh)
	{
		return nullptr;
	}

	StaticMesh->InitResources();
	StaticMesh->SetLightingGuid();
	StaticMesh->SetStaticMaterials(StaticMaterials);
	StaticMesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);
	StaticMesh->SetLightMapCoordinateIndex(0);

	StaticMesh->SetNumSourceModels(1);
	FMeshBuildSettings& BuildSettings = StaticMesh->GetSourceModel(0).BuildSettings;
	BuildSettings.bRecomputeNormals = false;
	BuildSettings.bRecomputeTangents = true;
	BuildSettings.bGenerateLightmapUVs = false;
	BuildSettings.bRemoveDegenerates = false;
	BuildSettings.bUseFullPrecisionUVs = true;

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bFastBuild = false;
	BuildParams.bUseHashAsGuid = true;
	BuildParams.bMarkPackageDirty = true;
	BuildParams.bCommitMeshDescription = true;
	BuildParams.bAllowCpuAccess = Options.bAllowCPUAccess;
	if (!StaticMesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Static mesh build failed for '%s'."), *PackageName);
		return nullptr;
	}

	if (Options.bUseComplexAsSimpleCollision)
	{
		StaticMesh->CreateBodySetup();
		if (UBodySetup* BodySetup = StaticMesh->GetBodySetup())
		{
			BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
		}
	}

	StaticMesh->PostEditChange();
	StaticMesh->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(StaticMesh);

	if (Options.bAutoSaveAsset)
	{
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, StaticMesh, *PackageFilename, SaveArgs))
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Static mesh was created but could not be saved to '%s'."), *PackageFilename);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("GPUTessellation Bake: Created static mesh '%s' with %d vertices and %d triangles."),
		*StaticMesh->GetPathName(),
		MeshData.Vertices.Num(),
		AddedTriangleCount);

	return StaticMesh;
}

UTexture2D* BakeGPUTessellationHeightNormalMapToTexture(
	const UObject* SourceObject,
	UTexture* HeightTexture,
	UTexture* SubtractTexture,
	const FGPUTessellationNormalMapBakeOptions& Options)
{
	FGPUTessellationScalarTextureData HeightData;
	if (!ReadTextureToScalarData(HeightTexture, HeightData))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Could not read height texture for normal map bake."));
		return nullptr;
	}

	FGPUTessellationScalarTextureData SubtractData;
	FGPUTessellationScalarTextureData* SubtractDataPtr = nullptr;
	if (SubtractTexture && ReadTextureToScalarData(SubtractTexture, SubtractData))
	{
		SubtractDataPtr = &SubtractData;
	}

	const FString Directory = NormalizeBakePackageDirectory(Options.AssetDirectory);
	if (Directory.IsEmpty())
	{
		return nullptr;
	}

	const FString BaseAssetName = SanitizeBakeAssetName(
		Options.AssetName.IsEmpty() ? FString::Printf(TEXT("%s_BakedNormal"), SourceObject ? *SourceObject->GetName() : TEXT("GPUTessellation")) : Options.AssetName,
		SourceObject);

	FString PackageName;
	FString AssetName;
	MakeUniqueBakePackageName(Directory, BaseAssetName, PackageName, AssetName);

	FText InvalidPackageReason;
	if (!FPackageName::IsValidLongPackageName(PackageName, false, &InvalidPackageReason))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Invalid normal map package name '%s': %s."),
			*PackageName,
			*InvalidPackageReason.ToString());
		return nullptr;
	}

	const int32 Width = HeightData.SizeX;
	const int32 Height = HeightData.SizeY;
	const float TexelStep = FMath::Max(Options.TexelStep, 0.25f);
	const FVector2f DetailUVStep(
		TexelStep / (float)FMath::Max(1, Width),
		TexelStep / (float)FMath::Max(1, Height));
	const float PlaneSizeX = FMath::Max(Options.PlaneSizeX, 1.0f);
	const float PlaneSizeY = FMath::Max(Options.PlaneSizeY, 1.0f);
	const float StepX = FMath::Max(DetailUVStep.X * PlaneSizeX, 1.0e-4f);
	const float StepY = FMath::Max(DetailUVStep.Y * PlaneSizeY, 1.0e-4f);

	TArray<FColor> NormalPixels;
	NormalPixels.SetNumUninitialized(Width * Height);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const float U = ((float)X + 0.5f) / (float)Width;
			const float V = ((float)Y + 0.5f) / (float)Height;
			const float HeightL = SampleBakedHeight(HeightData, SubtractDataPtr, U - DetailUVStep.X, V, Options.DisplacementIntensity);
			const float HeightR = SampleBakedHeight(HeightData, SubtractDataPtr, U + DetailUVStep.X, V, Options.DisplacementIntensity);
			const float HeightD = SampleBakedHeight(HeightData, SubtractDataPtr, U, V - DetailUVStep.Y, Options.DisplacementIntensity);
			const float HeightU = SampleBakedHeight(HeightData, SubtractDataPtr, U, V + DetailUVStep.Y, Options.DisplacementIntensity);

			const FVector3f TangentX(2.0f * StepX, 0.0f, HeightR - HeightL);
			const FVector3f TangentY(0.0f, 2.0f * StepY, HeightU - HeightD);
			const FVector3f HeightNormal = FVector3f::CrossProduct(TangentX, TangentY).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::ZAxisVector);
			const FVector3f StrengthNormal = ApplyBakedNormalStrength(HeightNormal, Options.Strength);
			NormalPixels[Y * Width + X] = EncodeTangentSpaceNormal(StrengthNormal);
		}
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return nullptr;
	}
	Package->FullyLoad();

	UTexture2D* NormalTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!NormalTexture)
	{
		return nullptr;
	}

	NormalTexture->PreEditChange(nullptr);
	NormalTexture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(NormalPixels.GetData()));
	NormalTexture->CompressionSettings = TC_Normalmap;
	NormalTexture->SRGB = false;
	NormalTexture->LODGroup = TEXTUREGROUP_WorldNormalMap;
	NormalTexture->MipGenSettings = TMGS_FromTextureGroup;
	NormalTexture->PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
	NormalTexture->UpdateResource();
	NormalTexture->PostEditChange();
	NormalTexture->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NormalTexture);

	if (Options.bAutoSaveAsset)
	{
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, NormalTexture, *PackageFilename, SaveArgs))
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUTessellation Bake: Normal map was created but could not be saved to '%s'."), *PackageFilename);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("GPUTessellation Bake: Created normal map '%s' from height texture '%s' at %dx%d."),
		*NormalTexture->GetPathName(),
		*GetNameSafe(HeightTexture),
		Width,
		Height);

	return NormalTexture;
}

#endif
