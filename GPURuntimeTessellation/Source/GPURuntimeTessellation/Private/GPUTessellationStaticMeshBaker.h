// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "GPUTessellationMeshBuilder.h"

class UMaterialInterface;
class UStaticMesh;
class UTexture;
class UTexture2D;

struct FGPUTessellationStaticMeshBakeSection
{
	int32 FirstIndex = 0;
	int32 NumTriangles = 0;
	int32 MaterialIndex = 0;
};

struct FGPUTessellationStaticMeshBakeOptions
{
	FString AssetDirectory = TEXT("/Game/GPUTessellationBakes");
	FString AssetName;
	bool bAllowCPUAccess = false;
	bool bUseComplexAsSimpleCollision = true;
	bool bAutoSaveAsset = false;
};

struct FGPUTessellationNormalMapBakeOptions
{
	FString AssetDirectory = TEXT("/Game/GPUTessellationBakes");
	FString AssetName;
	float PlaneSizeX = 1000.0f;
	float PlaneSizeY = 1000.0f;
	float DisplacementIntensity = 100.0f;
	float TexelStep = 1.0f;
	float Strength = 1.0f;
	bool bAutoSaveAsset = false;
};

UStaticMesh* BakeGPUTessellationMeshDataToStaticMesh(
	const UObject* SourceObject,
	const FGPUTessellatedMeshData& MeshData,
	const TArray<FGPUTessellationStaticMeshBakeSection>& Sections,
	const TArray<UMaterialInterface*>& Materials,
	const FGPUTessellationStaticMeshBakeOptions& Options);

UTexture2D* BakeGPUTessellationHeightNormalMapToTexture(
	const UObject* SourceObject,
	UTexture* HeightTexture,
	UTexture* SubtractTexture,
	const FGPUTessellationNormalMapBakeOptions& Options);

#endif
