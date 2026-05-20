// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture.h"
#include "GPUMeshTessellationComponent.generated.h"

class FGPUMeshTessellationSceneProxy;
class UStaticMesh;

UENUM(BlueprintType)
enum class EGPUMeshTessellationLODMode : uint8
{
	Disabled UMETA(DisplayName = "No LOD"),
	DistanceBased UMETA(DisplayName = "Distance-Based Tessellation Factor"),
	DistanceBasedStaticLODs UMETA(DisplayName = "Distance-Based Static LODs (0-5)")
};

struct FGPUMeshTessellationVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	FVector3f Normal = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f DisplacementNormal = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f Tangent = FVector3f(1.0f, 0.0f, 0.0f);
	float TangentSign = 1.0f;
	FVector2f UV = FVector2f::ZeroVector;
};

static_assert(sizeof(FGPUMeshTessellationVertex) == 60, "FGPUMeshTessellationVertex must match GPUMeshTessellation.usf.");

struct FGPUMeshTessellationSeamEdge
{
	uint32 TriangleA = 0;
	uint32 EdgeA = 0;
	uint32 TriangleB = 0;
	uint32 EdgeBAndFlags = 0;
};

static_assert(sizeof(FGPUMeshTessellationSeamEdge) == 16, "FGPUMeshTessellationSeamEdge must match GPUMeshTessellation.usf uint4.");

struct FGPUMeshTessellationSection
{
	uint32 FirstIndex = 0;
	uint32 NumTriangles = 0;
	int32 MaterialIndex = 0;
};

struct FGPUMeshTessellationBuildData
{
	TArray<FGPUMeshTessellationVertex> Vertices;
	TArray<uint32> Indices;
	TArray<FGPUMeshTessellationSeamEdge> SeamEdges;
	TArray<FGPUMeshTessellationSection> Sections;
	FBoxSphereBounds LocalBounds = FBoxSphereBounds(EForceInit::ForceInit);
	int32 TessellationFactor = 1;
	int32 VerticesPerTriangle = 3;
	int32 IndicesPerTriangle = 3;
	int32 OutputVertexCount = 0;
	int32 OutputIndexCount = 0;
	bool bEnableDisplacement = true;
	bool bGenerateNormals = true;
	float DisplacementIntensity = 10.0f;
	float DisplacementOffset = 0.0f;

	bool IsValid() const
	{
		return Vertices.Num() > 0 && Indices.Num() >= 3 && Sections.Num() > 0 && OutputVertexCount > 0 && OutputIndexCount > 0;
	}
};

/**
 * Experimental static mesh component that renders an arbitrary mesh through the
 * GPU Runtime Tessellation compute path.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class GPURUNTIMETESSELLATION_API UGPUMeshTessellationComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UGPUMeshTessellationComponent(const FObjectInitializer& ObjectInitializer);

	/** Uniform subdivision factor per source triangle. One source triangle emits Factor^2 triangles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation", meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "16"))
	int32 TessellationFactor = 4;

	/** Height/displacement texture sampled with the selected source mesh UV channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation")
	TObjectPtr<UTexture> DisplacementTexture;

	/** Height multiplier in local mesh units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation", meta = (ClampMin = "0.0"))
	float DisplacementIntensity = 10.0f;

	/** Constant offset applied along interpolated vertex normals when displacement is enabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation")
	float DisplacementOffset = 0.0f;

	/** Source static mesh UV channel used for displacement sampling and material UV0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation", meta = (ClampMin = "0", ClampMax = "7", UIMin = "0", UIMax = "3"))
	int32 UVChannel = 0;

	/** Disable to render tessellated but undisplaced geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation")
	bool bEnableDisplacement = true;

	/** Recompute vertex normals from the GPU-generated displaced triangles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Normals")
	bool bGenerateNormals = true;

	/** Average duplicate-position normals before displacement so hard-edge meshes stay visually closed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Seams")
	bool bWeldDisplacementNormals = true;

	/** Emit two-sided bridge triangles between duplicate-position seam edges to hide displacement cracks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Seams")
	bool bGenerateSeamStitching = true;

	/** Position tolerance used for hard-edge seam welding, in local mesh units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Seams", meta = (ClampMin = "0.0001", UIMin = "0.001", UIMax = "1.0"))
	float SeamWeldTolerance = 0.001f;

	/** Optional distance LOD that regenerates this actor with a lower or higher uniform tessellation factor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD")
	EGPUMeshTessellationLODMode LODMode = EGPUMeshTessellationLODMode::Disabled;

	/** Use closest point on mesh bounds instead of component pivot for distance LOD. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD", meta = (EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBased || LODMode == EGPUMeshTessellationLODMode::DistanceBasedStaticLODs", EditConditionHides))
	bool bUseDistanceToBounds = true;

	/** Tessellation factor at close range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD", meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "32", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBased", EditConditionHides))
	int32 MaxTessellationFactor = 16;

	/** Tessellation factor at far range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD", meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "16", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBased", EditConditionHides))
	int32 MinTessellationFactor = 2;

	/** Within this distance, distance LOD uses MaxTessellationFactor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD", meta = (ClampMin = "0.0", UIMin = "0.0", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBased", EditConditionHides))
	float MinTessellationDistance = 1000.0f;

	/** Beyond this distance, distance LOD uses MinTessellationFactor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD", meta = (ClampMin = "1.0", UIMin = "100.0", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBased", EditConditionHides))
	float MaxTessellationDistance = 50000.0f;

	/** Smooth transition speed between LOD factors. Set to 0 for immediate changes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBased", EditConditionHides))
	float LODTransitionSpeed = 2.0f;

	/** Minimum tessellation-factor difference before regenerating the actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBased", EditConditionHides))
	int32 LODHysteresis = 1;

	/** Static LOD tessellation factors from LOD0 (near) to LOD5 (far). At most the first six entries are used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD|Static", meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "32", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBasedStaticLODs", EditConditionHides))
	TArray<int32> StaticLODTessellationFactors = { 16, 12, 8, 4, 2, 1 };

	/** Distance thresholds for switching from LOD0 to later static LODs. Needs one fewer entry than StaticLODTessellationFactors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|LOD|Static", meta = (ClampMin = "0.0", UIMin = "0.0", EditCondition = "LODMode == EGPUMeshTessellationLODMode::DistanceBasedStaticLODs", EditConditionHides))
	TArray<float> StaticLODDistances = { 1000.0f, 2500.0f, 5000.0f, 10000.0f, 20000.0f };

	/** Safety cap for generated vertices to avoid accidental huge GPU allocations. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Safety", meta = (ClampMin = "3", UIMin = "1000"))
	int32 MaxGeneratedVertices = 4000000;

	/** Safety cap for generated triangles to avoid accidental huge GPU allocations. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Safety", meta = (ClampMin = "1", UIMin = "1000"))
	int32 MaxGeneratedTriangles = 8000000;

#if WITH_EDITORONLY_DATA
	/** Content Browser folder for baked static mesh assets. Must be under /Game. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake")
	FString BakeAssetDirectory = TEXT("/Game/GPUTessellationBakes");

	/** Optional baked static mesh asset name. Empty auto-generates from the actor/component name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake")
	FString BakeAssetName;

	/** Allow CPU access on the generated static mesh render buffers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake")
	bool bBakeMeshAllowCPUAccess = false;

	/** Use the generated render triangles as simple collision on the baked static mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake")
	bool bBakeMeshUseComplexCollision = true;

	/** Save the generated asset package immediately. Disabled leaves the asset dirty for manual saving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake")
	bool bBakeMeshAutoSaveAsset = false;

	/** Also bake a tangent-space normal map texture from the displacement height texture or render target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake")
	bool bBakeNormalMapTexture = false;

	/** Skip static mesh creation and bake only the normal map texture when pressing the bake button. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake", meta = (EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	bool bBakeNormalMapOnly = false;

	/** Optional normal map asset name. Empty derives a name from the baked mesh, or actor/component when baking only normals. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake", meta = (EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	FString BakeNormalMapAssetName;

	/** Height texture texel distance used for central-difference normal generation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake", meta = (ClampMin = "0.25", ClampMax = "16.0", UIMin = "0.5", UIMax = "4.0", EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	float BakeNormalMapTexelStep = 1.0f;

	/** Slope multiplier applied to the baked normal map. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU Mesh Tessellation|Bake", meta = (ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0", EditCondition = "bBakeNormalMapTexture", EditConditionHides))
	float BakeNormalMapStrength = 1.0f;
#endif

	UFUNCTION(BlueprintCallable, Category = "GPU Mesh Tessellation")
	void SetDisplacementTexture(UTexture* InTexture);

	UFUNCTION(BlueprintCallable, Category = "GPU Mesh Tessellation")
	void UpdateTessellatedMesh();

#if WITH_EDITOR
	/** Bake the current GPU-generated tessellated geometry into a new Static Mesh asset. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GPU Mesh Tessellation", meta = (DisplayName = "Bake Current Tessellation To Static Mesh"))
	void BakeCurrentTessellationToStaticMesh();

	/** Bake the current GPU-generated tessellated geometry and return the created Static Mesh asset. */
	UFUNCTION(BlueprintCallable, Category = "GPU Mesh Tessellation")
	UStaticMesh* BakeCurrentTessellationToStaticMeshAsset();
#endif

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual bool SetStaticMesh(UStaticMesh* NewMesh) override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	bool BuildMeshTessellationData(FGPUMeshTessellationBuildData& OutData) const;

private:
	int32 GetActiveTessellationFactor() const;
	int32 GetStaticLODTessellationFactor(int32 LODIndex) const;
	int32 CalculateDistanceTessellationFactor(float Distance) const;
	bool TryGetLODViewPosition(FVector& OutViewPosition) const;
	float CalculateLODDistance(const FVector& ViewPosition) const;
	void UpdateDistanceBasedLOD(float DeltaTime);
	bool BuildMeshTessellationDataForFactor(FGPUMeshTessellationBuildData& OutData, int32 InTessellationFactor) const;
	void MarkTessellationDataDirty();

private:
	float CurrentLODLevel = 4.0f;
	int32 LastAppliedTessFactor = 4;
};
