// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUMeshTessellationComponent.h"
#include "GPUMeshTessellationSceneProxy.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshResources.h"

#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#include "GPUMeshTessellationMeshBuilder.h"
#include "GPUTessellationStaticMeshBaker.h"
#include "RenderingThread.h"
#endif

namespace
{
	void GPUMeshTessellationForceTextureResident(UTexture* Texture)
	{
		if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
		{
			Texture2D->bForceMiplevelsToBeResident = true;
			Texture2D->SetForceMipLevelsToBeResident(30.0f, 0);
#if WITH_EDITOR
			Texture2D->WaitForStreaming();
#endif
		}
	}

	FIntVector GPUMeshTessellationQuantizePosition(const FVector3f& Position, float Tolerance)
	{
		const float SafeTolerance = FMath::Max(Tolerance, 0.0001f);
		return FIntVector(
			FMath::RoundToInt(Position.X / SafeTolerance),
			FMath::RoundToInt(Position.Y / SafeTolerance),
			FMath::RoundToInt(Position.Z / SafeTolerance));
	}

	bool GPUMeshTessellationKeyLess(const FIntVector& A, const FIntVector& B)
	{
		if (A.X != B.X)
		{
			return A.X < B.X;
		}
		if (A.Y != B.Y)
		{
			return A.Y < B.Y;
		}
		return A.Z < B.Z;
	}

	struct FGPUMeshTessellationEdgeKey
	{
		FIntVector A = FIntVector(0, 0, 0);
		FIntVector B = FIntVector(0, 0, 0);

		friend bool operator==(const FGPUMeshTessellationEdgeKey& Left, const FGPUMeshTessellationEdgeKey& Right)
		{
			return Left.A == Right.A && Left.B == Right.B;
		}
	};

	uint32 GetTypeHash(const FGPUMeshTessellationEdgeKey& Key)
	{
		return HashCombine(GetTypeHash(Key.A), GetTypeHash(Key.B));
	}

	FGPUMeshTessellationEdgeKey GPUMeshTessellationMakeEdgeKey(const FIntVector& Start, const FIntVector& End)
	{
		return GPUMeshTessellationKeyLess(End, Start)
			? FGPUMeshTessellationEdgeKey{ End, Start }
			: FGPUMeshTessellationEdgeKey{ Start, End };
	}

	struct FGPUMeshTessellationSourceEdge
	{
		uint32 TriangleIndex = 0;
		uint32 EdgeIndex = 0;
		uint32 VertexA = 0;
		uint32 VertexB = 0;
		FIntVector StartKey = FIntVector(0, 0, 0);
		FIntVector EndKey = FIntVector(0, 0, 0);
	};

	void GPUMeshTessellationApplyWeldedDisplacementNormals(TArray<FGPUMeshTessellationVertex>& Vertices, float Tolerance)
	{
		TMap<FIntVector, FVector3f> AccumulatedNormals;
		AccumulatedNormals.Reserve(Vertices.Num());

		for (const FGPUMeshTessellationVertex& Vertex : Vertices)
		{
			const FIntVector Key = GPUMeshTessellationQuantizePosition(Vertex.Position, Tolerance);
			if (FVector3f* ExistingNormal = AccumulatedNormals.Find(Key))
			{
				*ExistingNormal += Vertex.Normal;
			}
			else
			{
				AccumulatedNormals.Add(Key, Vertex.Normal);
			}
		}

		for (FGPUMeshTessellationVertex& Vertex : Vertices)
		{
			const FIntVector Key = GPUMeshTessellationQuantizePosition(Vertex.Position, Tolerance);
			if (const FVector3f* AccumulatedNormal = AccumulatedNormals.Find(Key))
			{
				Vertex.DisplacementNormal = AccumulatedNormal->GetSafeNormal(UE_SMALL_NUMBER, Vertex.Normal);
			}
		}
	}

	void GPUMeshTessellationBuildSeamEdges(
		const TArray<FGPUMeshTessellationVertex>& Vertices,
		const TArray<uint32>& Indices,
		float Tolerance,
		TArray<FGPUMeshTessellationSeamEdge>& OutSeamEdges)
	{
		TMap<FGPUMeshTessellationEdgeKey, TArray<FGPUMeshTessellationSourceEdge>> EdgesByPosition;
		const int32 SourceTriangleCount = Indices.Num() / 3;
		EdgesByPosition.Reserve(SourceTriangleCount * 3);

		for (int32 TriangleIndex = 0; TriangleIndex < SourceTriangleCount; ++TriangleIndex)
		{
			const uint32 TriangleVertices[3] =
			{
				Indices[TriangleIndex * 3 + 0],
				Indices[TriangleIndex * 3 + 1],
				Indices[TriangleIndex * 3 + 2]
			};

			const uint32 EdgeVertices[3][2] =
			{
				{ TriangleVertices[0], TriangleVertices[1] },
				{ TriangleVertices[1], TriangleVertices[2] },
				{ TriangleVertices[2], TriangleVertices[0] }
			};

			for (uint32 EdgeIndex = 0; EdgeIndex < 3; ++EdgeIndex)
			{
				const uint32 VertexA = EdgeVertices[EdgeIndex][0];
				const uint32 VertexB = EdgeVertices[EdgeIndex][1];
				if (!Vertices.IsValidIndex(VertexA) || !Vertices.IsValidIndex(VertexB))
				{
					continue;
				}

				FGPUMeshTessellationSourceEdge SourceEdge;
				SourceEdge.TriangleIndex = (uint32)TriangleIndex;
				SourceEdge.EdgeIndex = EdgeIndex;
				SourceEdge.VertexA = VertexA;
				SourceEdge.VertexB = VertexB;
				SourceEdge.StartKey = GPUMeshTessellationQuantizePosition(Vertices[VertexA].Position, Tolerance);
				SourceEdge.EndKey = GPUMeshTessellationQuantizePosition(Vertices[VertexB].Position, Tolerance);

				EdgesByPosition.FindOrAdd(GPUMeshTessellationMakeEdgeKey(SourceEdge.StartKey, SourceEdge.EndKey)).Add(SourceEdge);
			}
		}

		for (const TPair<FGPUMeshTessellationEdgeKey, TArray<FGPUMeshTessellationSourceEdge>>& EdgeGroup : EdgesByPosition)
		{
			const TArray<FGPUMeshTessellationSourceEdge>& SourceEdges = EdgeGroup.Value;
			if (SourceEdges.Num() < 2)
			{
				continue;
			}

			for (int32 EdgeAIndex = 0; EdgeAIndex < SourceEdges.Num() - 1; ++EdgeAIndex)
			{
				for (int32 EdgeBIndex = EdgeAIndex + 1; EdgeBIndex < SourceEdges.Num(); ++EdgeBIndex)
				{
					const FGPUMeshTessellationSourceEdge& EdgeA = SourceEdges[EdgeAIndex];
					const FGPUMeshTessellationSourceEdge& EdgeB = SourceEdges[EdgeBIndex];
					if (EdgeA.TriangleIndex == EdgeB.TriangleIndex)
					{
						continue;
					}

					const bool bSameSourceEdge =
						(EdgeA.VertexA == EdgeB.VertexA && EdgeA.VertexB == EdgeB.VertexB) ||
						(EdgeA.VertexA == EdgeB.VertexB && EdgeA.VertexB == EdgeB.VertexA);
					if (bSameSourceEdge)
					{
						continue;
					}

					const bool bReverseB = EdgeA.StartKey == EdgeB.EndKey && EdgeA.EndKey == EdgeB.StartKey;
					FGPUMeshTessellationSeamEdge& SeamEdge = OutSeamEdges.AddDefaulted_GetRef();
					SeamEdge.TriangleA = EdgeA.TriangleIndex;
					SeamEdge.EdgeA = EdgeA.EdgeIndex;
					SeamEdge.TriangleB = EdgeB.TriangleIndex;
					SeamEdge.EdgeBAndFlags = EdgeB.EdgeIndex | (bReverseB ? (1u << 8) : 0u);
				}
			}
		}
	}
}

UGPUMeshTessellationComponent::UGPUMeshTessellationComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, CurrentLODLevel(4.0f)
	, LastAppliedTessFactor(4)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
#if WITH_EDITOR
	bTickInEditor = true;
#endif
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = true;
	bAffectDistanceFieldLighting = true;
}

void UGPUMeshTessellationComponent::SetDisplacementTexture(UTexture* InTexture)
{
	DisplacementTexture = InTexture;
	GPUMeshTessellationForceTextureResident(DisplacementTexture);
	MarkTessellationDataDirty();
}

void UGPUMeshTessellationComponent::UpdateTessellatedMesh()
{
	MarkTessellationDataDirty();
}

#if WITH_EDITOR
void UGPUMeshTessellationComponent::BakeCurrentTessellationToStaticMesh()
{
	BakeCurrentTessellationToStaticMeshAsset();
}

UStaticMesh* UGPUMeshTessellationComponent::BakeCurrentTessellationToStaticMeshAsset()
{
	GPUMeshTessellationForceTextureResident(DisplacementTexture);

	FGPUMeshTessellationBuildData BuildData;
	if (!BuildMeshTessellationData(BuildData))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUMeshTessellation Bake: Failed to build source data for %s."), *GetName());
		return nullptr;
	}

	const UObject* SourceObject = GetOwner() ? static_cast<const UObject*>(GetOwner()) : static_cast<const UObject*>(this);
	if (bBakeNormalMapTexture && bBakeNormalMapOnly)
	{
		const FVector LocalExtent = BuildData.LocalBounds.BoxExtent;
		FGPUTessellationNormalMapBakeOptions NormalMapOptions;
		NormalMapOptions.AssetDirectory = BakeAssetDirectory;
		NormalMapOptions.AssetName = BakeNormalMapAssetName;
		NormalMapOptions.PlaneSizeX = FMath::Max((float)LocalExtent.X * 2.0f, 1.0f);
		NormalMapOptions.PlaneSizeY = FMath::Max((float)LocalExtent.Y * 2.0f, 1.0f);
		NormalMapOptions.DisplacementIntensity = DisplacementIntensity;
		NormalMapOptions.TexelStep = BakeNormalMapTexelStep;
		NormalMapOptions.Strength = BakeNormalMapStrength;
		NormalMapOptions.bAutoSaveAsset = bBakeMeshAutoSaveAsset;
		BakeGPUTessellationHeightNormalMapToTexture(SourceObject, DisplacementTexture, nullptr, NormalMapOptions);
		return nullptr;
	}

	FGPUTessellatedMeshData MeshData;
	UTexture* BakeDisplacementTexture = DisplacementTexture;
	ENQUEUE_RENDER_COMMAND(BakeGPUMeshTessellationToStaticMesh)(
		[BuildData, BakeDisplacementTexture, &MeshData](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FGPUMeshTessellationMeshBuilder MeshBuilder;
			MeshBuilder.ExecuteTessellationPipeline(GraphBuilder, BuildData, BakeDisplacementTexture, MeshData);
			GraphBuilder.Execute();
		});
	FlushRenderingCommands();

	if (!MeshData.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUMeshTessellation Bake: GPU readback produced no valid mesh for %s."), *GetName());
		return nullptr;
	}

	TArray<FGPUTessellationStaticMeshBakeSection> BakeSections;
	BakeSections.Reserve(BuildData.Sections.Num());
	int32 MaxMaterialIndex = 0;
	for (const FGPUMeshTessellationSection& SourceSection : BuildData.Sections)
	{
		FGPUTessellationStaticMeshBakeSection& BakeSection = BakeSections.AddDefaulted_GetRef();
		BakeSection.FirstIndex = (int32)SourceSection.FirstIndex;
		BakeSection.NumTriangles = (int32)SourceSection.NumTriangles;
		BakeSection.MaterialIndex = FMath::Max(0, SourceSection.MaterialIndex);
		MaxMaterialIndex = FMath::Max(MaxMaterialIndex, BakeSection.MaterialIndex);
	}

	TArray<UMaterialInterface*> Materials;
	Materials.Reserve(MaxMaterialIndex + 1);
	for (int32 MaterialIndex = 0; MaterialIndex <= MaxMaterialIndex; ++MaterialIndex)
	{
		Materials.Add(GetMaterial(MaterialIndex));
	}

	FGPUTessellationStaticMeshBakeOptions Options;
	Options.AssetDirectory = BakeAssetDirectory;
	Options.AssetName = BakeAssetName;
	Options.bAllowCPUAccess = bBakeMeshAllowCPUAccess;
	Options.bUseComplexAsSimpleCollision = bBakeMeshUseComplexCollision;
	Options.bAutoSaveAsset = bBakeMeshAutoSaveAsset;

	UStaticMesh* BakedMesh = BakeGPUTessellationMeshDataToStaticMesh(SourceObject, MeshData, BakeSections, Materials, Options);

	if (BakedMesh && bBakeNormalMapTexture)
	{
		const FVector LocalExtent = BuildData.LocalBounds.BoxExtent;
		FGPUTessellationNormalMapBakeOptions NormalMapOptions;
		NormalMapOptions.AssetDirectory = BakeAssetDirectory;
		NormalMapOptions.AssetName = BakeNormalMapAssetName.IsEmpty()
			? FString::Printf(TEXT("%s_N"), *BakedMesh->GetName())
			: BakeNormalMapAssetName;
		NormalMapOptions.PlaneSizeX = FMath::Max((float)LocalExtent.X * 2.0f, 1.0f);
		NormalMapOptions.PlaneSizeY = FMath::Max((float)LocalExtent.Y * 2.0f, 1.0f);
		NormalMapOptions.DisplacementIntensity = DisplacementIntensity;
		NormalMapOptions.TexelStep = BakeNormalMapTexelStep;
		NormalMapOptions.Strength = BakeNormalMapStrength;
		NormalMapOptions.bAutoSaveAsset = bBakeMeshAutoSaveAsset;
		BakeGPUTessellationHeightNormalMapToTexture(SourceObject, DisplacementTexture, nullptr, NormalMapOptions);
	}

	return BakedMesh;
}
#endif

FPrimitiveSceneProxy* UGPUMeshTessellationComponent::CreateSceneProxy()
{
	TArray<FGPUMeshTessellationBuildData> BuildDataLODs;
	TArray<float> LODDistanceThresholds;

	if (LODMode == EGPUMeshTessellationLODMode::DistanceBasedStaticLODs)
	{
		const int32 StaticLODCount = FMath::Clamp(StaticLODTessellationFactors.Num(), 1, 6);
		BuildDataLODs.Reserve(StaticLODCount);

		for (int32 LODIndex = 0; LODIndex < StaticLODCount; ++LODIndex)
		{
			FGPUMeshTessellationBuildData LODBuildData;
			if (BuildMeshTessellationDataForFactor(LODBuildData, GetStaticLODTessellationFactor(LODIndex)))
			{
				BuildDataLODs.Add(MoveTemp(LODBuildData));
			}
		}

		LODDistanceThresholds = StaticLODDistances;
	}
	else
	{
		FGPUMeshTessellationBuildData BuildData;
		if (BuildMeshTessellationData(BuildData))
		{
			BuildDataLODs.Add(MoveTemp(BuildData));
		}
	}

	if (BuildDataLODs.Num() == 0)
	{
		return nullptr;
	}

	return new FGPUMeshTessellationSceneProxy(this, MoveTemp(BuildDataLODs), MoveTemp(LODDistanceThresholds), bUseDistanceToBounds);
}

FBoxSphereBounds UGPUMeshTessellationComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	UStaticMesh* SourceStaticMesh = GetStaticMesh();
	if (!SourceStaticMesh)
	{
		return FBoxSphereBounds(FBox(FVector::ZeroVector, FVector::ZeroVector)).TransformBy(LocalToWorld);
	}

	const float MaxLocalDisplacement = bEnableDisplacement
		? FMath::Abs(DisplacementIntensity) + FMath::Abs(DisplacementOffset)
		: 0.0f;
	return SourceStaticMesh->GetBounds().ExpandBy(MaxLocalDisplacement).TransformBy(LocalToWorld);
}

bool UGPUMeshTessellationComponent::SetStaticMesh(UStaticMesh* NewMesh)
{
	const bool bChanged = Super::SetStaticMesh(NewMesh);
	if (bChanged)
	{
		MarkTessellationDataDirty();
	}
	return bChanged;
}

void UGPUMeshTessellationComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (LODMode == EGPUMeshTessellationLODMode::DistanceBased)
	{
		UpdateDistanceBasedLOD(DeltaTime);
	}
}

#if WITH_EDITOR
void UGPUMeshTessellationComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, TessellationFactor) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, DisplacementTexture) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, DisplacementIntensity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, DisplacementOffset) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, UVChannel) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, bEnableDisplacement) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, bGenerateNormals) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, bWeldDisplacementNormals) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, bGenerateSeamStitching) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, SeamWeldTolerance) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, LODMode) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, bUseDistanceToBounds) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, MaxTessellationFactor) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, MinTessellationFactor) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, MinTessellationDistance) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, MaxTessellationDistance) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, LODTransitionSpeed) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, LODHysteresis) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, StaticLODTessellationFactors) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, StaticLODDistances) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, MaxGeneratedVertices) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, MaxGeneratedTriangles))
	{
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, LODMode) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, MaxTessellationFactor) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, StaticLODTessellationFactors) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGPUMeshTessellationComponent, TessellationFactor))
		{
			if (LODMode == EGPUMeshTessellationLODMode::DistanceBased)
			{
				LastAppliedTessFactor = FMath::Clamp(MaxTessellationFactor, 1, 32);
			}
			else if (LODMode == EGPUMeshTessellationLODMode::DistanceBasedStaticLODs)
			{
				LastAppliedTessFactor = GetStaticLODTessellationFactor(0);
			}
			else
			{
				LastAppliedTessFactor = FMath::Clamp(TessellationFactor, 1, 32);
			}
			CurrentLODLevel = (float)LastAppliedTessFactor;
		}

		GPUMeshTessellationForceTextureResident(DisplacementTexture);
		MarkTessellationDataDirty();
	}
}
#endif

bool UGPUMeshTessellationComponent::BuildMeshTessellationData(FGPUMeshTessellationBuildData& OutData) const
{
	return BuildMeshTessellationDataForFactor(OutData, GetActiveTessellationFactor());
}

bool UGPUMeshTessellationComponent::BuildMeshTessellationDataForFactor(FGPUMeshTessellationBuildData& OutData, int32 InTessellationFactor) const
{
	OutData = FGPUMeshTessellationBuildData();

	UStaticMesh* SourceStaticMesh = GetStaticMesh();
	if (!SourceStaticMesh || !SourceStaticMesh->GetRenderData() || SourceStaticMesh->GetRenderData()->LODResources.Num() == 0)
	{
		return false;
	}

	const int32 LODIndex = FMath::Clamp(SourceStaticMesh->GetMinLODIdx(), 0, SourceStaticMesh->GetRenderData()->LODResources.Num() - 1);
	const FStaticMeshLODResources& LODResource = SourceStaticMesh->GetRenderData()->LODResources[LODIndex];
	const FPositionVertexBuffer& PositionVertexBuffer = LODResource.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& StaticMeshVertexBuffer = LODResource.VertexBuffers.StaticMeshVertexBuffer;
	const int32 VertexCount = PositionVertexBuffer.GetNumVertices();
	const FIndexArrayView IndexView = LODResource.IndexBuffer.GetArrayView();
	const int32 SourceTriangleCount = IndexView.Num() / 3;

	if (VertexCount <= 0 || SourceTriangleCount <= 0)
	{
		return false;
	}

	const int32 SafeTessellationFactor = FMath::Clamp(InTessellationFactor, 1, 32);
	const int64 VerticesPerTriangle64 = ((int64)SafeTessellationFactor + 1) * ((int64)SafeTessellationFactor + 2) / 2;
	const int64 BaseOutputTriangles64 = (int64)SourceTriangleCount * SafeTessellationFactor * SafeTessellationFactor;
	const int64 BaseOutputIndices64 = BaseOutputTriangles64 * 3;
	const int64 OutputVertices64 = (int64)SourceTriangleCount * VerticesPerTriangle64;

	const int32 NumTexCoords = (int32)StaticMeshVertexBuffer.GetNumTexCoords();
	const int32 SafeUVChannel = NumTexCoords > 0 ? FMath::Clamp(UVChannel, 0, NumTexCoords - 1) : 0;

	OutData.Vertices.SetNumUninitialized(VertexCount);
	for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		FGPUMeshTessellationVertex& Vertex = OutData.Vertices[VertexIndex];
		const FVector4f TangentX = StaticMeshVertexBuffer.VertexTangentX(VertexIndex);
		const FVector4f TangentZ = StaticMeshVertexBuffer.VertexTangentZ(VertexIndex);

		Vertex.Position = PositionVertexBuffer.VertexPosition(VertexIndex);
		Vertex.Normal = FVector3f(TangentZ).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));
		Vertex.DisplacementNormal = Vertex.Normal;
		Vertex.Tangent = FVector3f(TangentX).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1.0f, 0.0f, 0.0f));
		Vertex.TangentSign = TangentZ.W < 0.0f ? -1.0f : 1.0f;
		Vertex.UV = NumTexCoords > 0 ? StaticMeshVertexBuffer.GetVertexUV(VertexIndex, SafeUVChannel) : FVector2f::ZeroVector;
	}

	if (bWeldDisplacementNormals)
	{
		GPUMeshTessellationApplyWeldedDisplacementNormals(OutData.Vertices, SeamWeldTolerance);
	}

	OutData.Indices.SetNumUninitialized(SourceTriangleCount * 3);
	for (int32 Index = 0; Index < SourceTriangleCount * 3; ++Index)
	{
		OutData.Indices[Index] = IndexView[Index];
	}

	if (bGenerateSeamStitching)
	{
		GPUMeshTessellationBuildSeamEdges(OutData.Vertices, OutData.Indices, SeamWeldTolerance, OutData.SeamEdges);
	}

	const int64 SeamTriangles64 = (int64)OutData.SeamEdges.Num() * SafeTessellationFactor * 4;
	const int64 OutputTriangles64 = BaseOutputTriangles64 + SeamTriangles64;
	const int64 OutputIndices64 = BaseOutputIndices64 + SeamTriangles64 * 3;

	if (OutputVertices64 > MaxGeneratedVertices || OutputTriangles64 > MaxGeneratedTriangles ||
		OutputVertices64 > MAX_int32 || OutputIndices64 > MAX_int32)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("GPUMeshTessellation: %s would generate %lld vertices / %lld triangles at factor %d. Raise safety caps or lower tessellation."),
			*GetName(),
			OutputVertices64,
			OutputTriangles64,
			SafeTessellationFactor);
		return false;
	}

	const int32 IndicesPerTriangle = 3 * SafeTessellationFactor * SafeTessellationFactor;
	if (LODResource.Sections.Num() > 0)
	{
		OutData.Sections.Reserve(LODResource.Sections.Num());
		for (const FStaticMeshSection& SourceSection : LODResource.Sections)
		{
			if (SourceSection.NumTriangles <= 0)
			{
				continue;
			}

			FGPUMeshTessellationSection& Section = OutData.Sections.AddDefaulted_GetRef();
			Section.FirstIndex = (uint32)((SourceSection.FirstIndex / 3) * IndicesPerTriangle);
			Section.NumTriangles = (uint32)(SourceSection.NumTriangles * SafeTessellationFactor * SafeTessellationFactor);
			Section.MaterialIndex = SourceSection.MaterialIndex;
		}
	}

	if (OutData.Sections.Num() == 0)
	{
		FGPUMeshTessellationSection& Section = OutData.Sections.AddDefaulted_GetRef();
		Section.FirstIndex = 0;
		Section.NumTriangles = (uint32)BaseOutputTriangles64;
		Section.MaterialIndex = 0;
	}

	if (SeamTriangles64 > 0)
	{
		FGPUMeshTessellationSection& SeamSection = OutData.Sections.AddDefaulted_GetRef();
		SeamSection.FirstIndex = (uint32)BaseOutputIndices64;
		SeamSection.NumTriangles = (uint32)SeamTriangles64;
		SeamSection.MaterialIndex = 0;
	}

	const float MaxLocalDisplacement = bEnableDisplacement
		? FMath::Abs(DisplacementIntensity) + FMath::Abs(DisplacementOffset)
		: 0.0f;
	OutData.LocalBounds = SourceStaticMesh->GetBounds().ExpandBy(MaxLocalDisplacement);
	OutData.TessellationFactor = SafeTessellationFactor;
	OutData.VerticesPerTriangle = (int32)VerticesPerTriangle64;
	OutData.IndicesPerTriangle = IndicesPerTriangle;
	OutData.OutputVertexCount = (int32)OutputVertices64;
	OutData.OutputIndexCount = (int32)OutputIndices64;
	OutData.bEnableDisplacement = bEnableDisplacement;
	OutData.bGenerateNormals = bGenerateNormals;
	OutData.DisplacementIntensity = DisplacementIntensity;
	OutData.DisplacementOffset = DisplacementOffset;

	return OutData.IsValid();
}

int32 UGPUMeshTessellationComponent::GetActiveTessellationFactor() const
{
	if (LODMode == EGPUMeshTessellationLODMode::DistanceBased)
	{
		const int32 FallbackFactor = FMath::Clamp(MaxTessellationFactor, 1, 32);
		return FMath::Clamp(LastAppliedTessFactor > 0 ? LastAppliedTessFactor : FallbackFactor, 1, 32);
	}
	if (LODMode == EGPUMeshTessellationLODMode::DistanceBasedStaticLODs)
	{
		return GetStaticLODTessellationFactor(0);
	}

	return FMath::Clamp(TessellationFactor, 1, 32);
}

int32 UGPUMeshTessellationComponent::GetStaticLODTessellationFactor(int32 LODIndex) const
{
	if (StaticLODTessellationFactors.Num() == 0)
	{
		return FMath::Clamp(TessellationFactor, 1, 32);
	}

	const int32 ClampedIndex = FMath::Clamp(LODIndex, 0, FMath::Min(StaticLODTessellationFactors.Num(), 6) - 1);
	return FMath::Clamp(StaticLODTessellationFactors[ClampedIndex], 1, 32);
}

int32 UGPUMeshTessellationComponent::CalculateDistanceTessellationFactor(float Distance) const
{
	const FVector ComponentScale = GetComponentScale();
	const float MaxScale = FMath::Max3(FMath::Abs(ComponentScale.X), FMath::Abs(ComponentScale.Y), FMath::Abs(ComponentScale.Z));
	const float SafeScale = FMath::Max(MaxScale, 0.0001f);
	const float MinDistance = MinTessellationDistance * SafeScale;
	const float MaxDistance = FMath::Max(MaxTessellationDistance * SafeScale, MinDistance + 1.0f);
	const int32 NearFactor = FMath::Clamp(MaxTessellationFactor, 1, 32);
	const int32 FarFactor = FMath::Clamp(MinTessellationFactor, 1, 32);

	if (Distance <= MinDistance)
	{
		return NearFactor;
	}
	if (Distance >= MaxDistance)
	{
		return FarFactor;
	}

	const float Alpha = (Distance - MinDistance) / (MaxDistance - MinDistance);
	return FMath::Clamp(FMath::RoundToInt(FMath::Lerp((float)NearFactor, (float)FarFactor, Alpha)), 1, 32);
}

bool UGPUMeshTessellationComponent::TryGetLODViewPosition(FVector& OutViewPosition) const
{
	if (const UWorld* World = GetWorld())
	{
		if (APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			FRotator ViewRotation;
			PlayerController->GetPlayerViewPoint(OutViewPosition, ViewRotation);
			return true;
		}
	}

#if WITH_EDITOR
	if (GEditor && GEditor->GetActiveViewport())
	{
		if (FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient()))
		{
			OutViewPosition = ViewportClient->GetViewLocation();
			return true;
		}
	}
#endif

	return false;
}

float UGPUMeshTessellationComponent::CalculateLODDistance(const FVector& ViewPosition) const
{
	if (!bUseDistanceToBounds)
	{
		return FVector::Dist(GetComponentLocation(), ViewPosition);
	}

	const FBoxSphereBounds WorldBounds = CalcBounds(GetComponentTransform());
	return FMath::Sqrt(WorldBounds.ComputeSquaredDistanceFromBoxToPoint(ViewPosition));
}

void UGPUMeshTessellationComponent::UpdateDistanceBasedLOD(float DeltaTime)
{
	FVector ViewPosition = FVector::ZeroVector;
	if (!TryGetLODViewPosition(ViewPosition))
	{
		return;
	}

	if (LastAppliedTessFactor <= 0)
	{
		LastAppliedTessFactor = FMath::Clamp(MaxTessellationFactor, 1, 32);
		CurrentLODLevel = (float)LastAppliedTessFactor;
	}

	const float Distance = CalculateLODDistance(ViewPosition);
	const int32 TargetFactor = CalculateDistanceTessellationFactor(Distance);
	if (LODTransitionSpeed > 0.0f && DeltaTime > 0.0f)
	{
		CurrentLODLevel = FMath::FInterpTo(CurrentLODLevel, (float)TargetFactor, DeltaTime, LODTransitionSpeed);
	}
	else
	{
		CurrentLODLevel = (float)TargetFactor;
	}

	const int32 NewFactor = FMath::Clamp(FMath::RoundToInt(CurrentLODLevel), 1, 32);
	if (FMath::Abs(NewFactor - LastAppliedTessFactor) > LODHysteresis)
	{
		LastAppliedTessFactor = NewFactor;
		MarkTessellationDataDirty();
	}
}

void UGPUMeshTessellationComponent::MarkTessellationDataDirty()
{
	UpdateBounds();
	MarkRenderStateDirty();
}
