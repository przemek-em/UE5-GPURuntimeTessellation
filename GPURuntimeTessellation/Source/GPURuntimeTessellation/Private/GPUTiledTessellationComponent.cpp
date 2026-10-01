// Licensed under the MIT License. See LICENSE file in the project root.
#include "GPUTiledTessellationComponent.h"
#include "GPUTiledTessellationSceneProxy.h"
#include "GPUTessellationRendering.h"
#include "Engine/Texture2D.h"

UGPUTiledTessellationComponent::UGPUTiledTessellationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetMobility(EComponentMobility::Movable);
	bAffectDistanceFieldLighting = false;
}

int64 UGPUTiledTessellationComponent::GetEstimatedGeometryBytes() const
{
	const int64 Quads = FMath::Clamp(QuadsPerTile, 1, 1024);
	// Positions float3 + normals float3 + UVs float2; one shared uint32 index grid.
	return int64(Tiles.Num()) * (Quads + 1) * (Quads + 1) * 32 + Quads * Quads * 6 * 4;
}

void UGPUTiledTessellationComponent::OnRegister()
{
	Super::OnRegister();
	PrecachePSOs();
}

bool UGPUTiledTessellationComponent::ValidateTiles(FString& OutIssues) const
{
	OutIssues.Reset();
	auto Issue = [&OutIssues](const FString& Text) { OutIssues += Text + TEXT("\n"); };
	if (Tiles.IsEmpty()) { Issue(TEXT("Assign at least one heightmap tile.")); }
	if (TileSize.ContainsNaN() || TileSize.X < 1.0 || TileSize.Y < 1.0
		|| TileSize.X > MAX_flt || TileSize.Y > MAX_flt
		|| !FMath::IsFinite(HeightScale) || !FMath::IsFinite(HeightOffset)
		|| !FMath::IsFinite(HeightOffset + HeightScale))
	{
		Issue(TEXT("Tile size and heights must be finite, with tile dimensions at least 1 cm."));
	}
	if (QuadsPerTile < 1 || QuadsPerTile > 1024) { Issue(TEXT("QuadsPerTile must be between 1 and 1024.")); }
	if (GeometryBudgetMB < 1 || GeometryBudgetMB > 4096
		|| GetEstimatedGeometryBytes() > int64(GeometryBudgetMB) * 1024 * 1024)
	{
		Issue(TEXT("Generated geometry exceeds GeometryBudgetMB, or the budget is outside 1..4096 MB."));
	}
	TSet<FIntPoint> Coordinates;
	FIntPoint Dimensions = FIntPoint::ZeroValue;
	for (int32 Index = 0; Index < Tiles.Num(); ++Index)
	{
		const FGPUTiledHeightmap& Tile = Tiles[Index];
		const FString Label = FString::Printf(TEXT("Tile %d (%d,%d): "), Index, Tile.Coordinate.X, Tile.Coordinate.Y);
		if (Coordinates.Contains(Tile.Coordinate)) { Issue(Label + TEXT("duplicate coordinate.")); }
		Coordinates.Add(Tile.Coordinate);
		if (Tile.Coordinate.X == MIN_int32 || Tile.Coordinate.X == MAX_int32
			|| Tile.Coordinate.Y == MIN_int32 || Tile.Coordinate.Y == MAX_int32
			|| FMath::Abs(double(Tile.Coordinate.X) * TileSize.X) + TileSize.X > MAX_flt
			|| FMath::Abs(double(Tile.Coordinate.Y) * TileSize.Y) + TileSize.Y > MAX_flt)
		{
			Issue(Label + TEXT("coordinate exceeds supported local range."));
		}
		const UTexture2D* Texture = Tile.Heightmap;
		if (!Texture) { Issue(Label + TEXT("missing heightmap.")); continue; }
		const FIntPoint Size(Texture->GetSizeX(), Texture->GetSizeY());
		if (Size.X < 2 || Size.Y < 2) { Issue(Label + TEXT("heightmap needs at least 2x2 samples.")); }
		if (Dimensions == FIntPoint::ZeroValue) { Dimensions = Size; }
		if (Size != Dimensions) { Issue(Label + TEXT("heightmap dimensions must match the other tiles.")); }
		if (Texture->SRGB) { Issue(Label + TEXT("disable sRGB for linear height samples.")); }
		if (Texture->VirtualTextureStreaming) { Issue(Label + TEXT("virtual height textures are not supported in this prototype.")); }
		if (!Texture->NeverStream) { Issue(Label + TEXT("enable Never Stream; automatic heightmap streaming is a future phase.")); }
	}
	return OutIssues.IsEmpty();
}

FPrimitiveSceneProxy* UGPUTiledTessellationComponent::CreateSceneProxy()
{
	// An unconfigured actor is a normal editor state, with nothing to render yet.
	if (Tiles.IsEmpty()) { return nullptr; }
	FString Issues;
	if (!ValidateTiles(Issues))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPU Tiled Tessellation: %s"), *Issues);
		return nullptr;
	}
	return new FGPUTiledTessellationSceneProxy(this);
}

FBoxSphereBounds UGPUTiledTessellationComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBox Box(ForceInit);
	const double MinHeight = FMath::Min(HeightOffset, HeightOffset + HeightScale);
	const double MaxHeight = FMath::Max(HeightOffset, HeightOffset + HeightScale);
	for (const FGPUTiledHeightmap& Tile : Tiles)
	{
		const double X = double(Tile.Coordinate.X) * TileSize.X;
		const double Y = double(Tile.Coordinate.Y) * TileSize.Y;
		Box += FVector(X, Y, MinHeight - 1.0);
		Box += FVector(X + TileSize.X, Y + TileSize.Y, MaxHeight + 1.0);
	}
	if (!Box.IsValid || Box.Min.ContainsNaN() || Box.Max.ContainsNaN())
	{
		Box = FBox(FVector(-1.0), FVector(1.0));
	}
	return FBoxSphereBounds(Box).TransformBy(LocalToWorld);
}

void UGPUTiledTessellationComponent::RebuildTiles()
{
	UpdateBounds();
	PrecachePSOs();
	MarkRenderTransformDirty();
	MarkRenderStateDirty();
}

void UGPUTiledTessellationComponent::CollectPSOPrecacheData(const FPSOPrecacheParams& BaseParams, FMaterialInterfacePSOPrecacheParamsList& OutParams)
{
	CollectGPUTessellationPSOPrecacheData(*this, BaseParams, OutParams);
}

#if WITH_EDITOR
void UGPUTiledTessellationComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RebuildTiles();
}
#endif
