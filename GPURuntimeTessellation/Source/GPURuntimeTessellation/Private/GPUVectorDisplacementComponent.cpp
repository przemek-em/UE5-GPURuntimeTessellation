// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPUVectorDisplacementComponent.h"

#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Engine/TextureRenderTarget.h"
#include "PixelFormat.h"
#include "RenderUtils.h"

namespace
{
	void GPUVectorDisplacementForceTextureResident(UTexture* Texture)
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

	bool GPUVectorDisplacementIsGeometryNormalMode(EGPUTessellationNormalMethod NormalMethod)
	{
		return NormalMethod == EGPUTessellationNormalMethod::Disabled ||
			NormalMethod == EGPUTessellationNormalMethod::GeometryBased ||
			NormalMethod == EGPUTessellationNormalMethod::FromNormalMap;
	}
}

UGPUVectorDisplacementComponent::UGPUVectorDisplacementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TessellationSettings.bUseSineWaveDisplacement = false;
	TessellationSettings.NormalCalculationMethod = EGPUTessellationNormalMethod::GeometryBased;

	// Heightfield traces are not reliable when the texture can move vertices sideways.
	CollisionMode = EGPUTessellationCollisionMode::Disabled;
	bEnableHeightFieldLineTrace = false;
}

FGPUTessellationSettings UGPUVectorDisplacementComponent::GetEffectiveTessellationSettings() const
{
	FGPUTessellationSettings EffectiveSettings = Super::GetEffectiveTessellationSettings();
	EffectiveSettings.bUseVectorDisplacement = VectorDisplacementTexture != nullptr;
	EffectiveSettings.VectorDisplacementSpace = VectorDisplacementSpace;
	EffectiveSettings.VectorDisplacementDecodeMode = DecodeMode;
	EffectiveSettings.VectorDisplacementScale = VectorDisplacementScale;
	EffectiveSettings.VectorDisplacementBias = VectorDisplacementBias;
	EffectiveSettings.bUseVectorDisplacementAlphaAsStrength = bUseAlphaAsStrength;
	EffectiveSettings.VectorDisplacementIntensity = GlobalVectorDisplacementIntensity;
	EffectiveSettings.bAddScalarHeightDisplacementToVector = bAddScalarHeightDisplacement;
	EffectiveSettings.VectorDisplacementBoundsPadding = VectorDisplacementBoundsPadding;

	if (EffectiveSettings.bUseVectorDisplacement &&
		bForceGeometryNormalsForVectorDisplacement &&
		!GPUVectorDisplacementIsGeometryNormalMode(EffectiveSettings.NormalCalculationMethod))
	{
		EffectiveSettings.NormalCalculationMethod = EGPUTessellationNormalMethod::GeometryBased;
	}

	return EffectiveSettings;
}

void UGPUVectorDisplacementComponent::SetVectorDisplacementTexture(UTexture* InTexture)
{
	VectorDisplacementTexture = InTexture;
	GPUVectorDisplacementForceTextureResident(VectorDisplacementTexture);

	if (bValidateTextureImportSettings)
	{
		ValidateVectorDisplacementTexture();
	}

	MarkCollisionMeshDirty();
	MarkWaterSurfaceReadbackDirty();
	RebuildCollisionMesh();
	UpdateTessellatedMesh();
}

void UGPUVectorDisplacementComponent::ValidateVectorDisplacementTexture() const
{
	if (!VectorDisplacementTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: No vector displacement texture assigned."));
		return;
	}

	if (const UTexture2D* Texture2D = Cast<UTexture2D>(VectorDisplacementTexture))
	{
		if (Texture2D->SRGB)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: %s has sRGB enabled. Vector displacement should be linear data with sRGB disabled."),
				*GetNameSafe(Texture2D));
		}

		const TextureCompressionSettings CompressionSettings = static_cast<TextureCompressionSettings>(Texture2D->CompressionSettings.GetValue());
		const bool bFloatOrVectorCompression =
			CompressionSettings == TC_HDR ||
			CompressionSettings == TC_HDR_F32 ||
			CompressionSettings == TC_HalfFloat ||
			CompressionSettings == TC_VectorDisplacementmap;
		if (!bFloatOrVectorCompression)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: %s uses compression setting %d. Prefer HDR, HDR_F32, HalfFloat, or VectorDisplacementmap for vector displacement data."),
				*GetNameSafe(Texture2D),
				static_cast<int32>(CompressionSettings));
		}

		const EPixelFormat PixelFormat = Texture2D->GetPixelFormat();
		const bool bFloatRGBA =
			PixelFormat == PF_FloatRGBA ||
			PixelFormat == PF_A32B32G32R32F;
		if (!bFloatRGBA)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: %s runtime pixel format is %s. RGBA16F/FloatRGBA or RGBA32F is recommended for high-quality vector displacement."),
				*GetNameSafe(Texture2D),
				GetPixelFormatString(PixelFormat));
		}

		if (bRequire32BitRuntimeTexture && PixelFormat != PF_A32B32G32R32F)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: %s is not RGBA32F at runtime (%s), but Require 32 Bit Runtime Texture is enabled."),
				*GetNameSafe(Texture2D),
				GetPixelFormatString(PixelFormat));
		}

		if (!FMath::IsPowerOfTwo(Texture2D->GetSizeX()) || !FMath::IsPowerOfTwo(Texture2D->GetSizeY()))
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: %s is %dx%d. Non-power-of-two textures are supported, but power-of-two sizes are safer for streaming/mip workflows."),
				*GetNameSafe(Texture2D),
				Texture2D->GetSizeX(),
				Texture2D->GetSizeY());
		}
	}
	else if (const UTextureRenderTarget* RenderTarget = Cast<UTextureRenderTarget>(VectorDisplacementTexture))
	{
		if (RenderTarget->IsSRGB())
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: %s render target is sRGB. Vector displacement render targets should be linear."),
				*GetNameSafe(RenderTarget));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: %s is not a Texture2D or render target. Compute sampling may fail depending on the texture type."),
			*GetNameSafe(VectorDisplacementTexture));
	}

	if (CollisionMode == EGPUTessellationCollisionMode::HeightFieldTraceOnly ||
		CollisionMode == EGPUTessellationCollisionMode::CoarseHeightFieldMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUVectorDisplacement: Heightfield collision modes ignore lateral X/Y vector offsets. Use Vertex Perfect Mesh or Bake Full Mesh Patch Collision for exact collision."));
	}
}

#if WITH_EDITOR
void UGPUVectorDisplacementComponent::BakeCurrentVectorDisplacementToStaticMesh()
{
	BakeCurrentVectorDisplacementToStaticMeshAsset();
}

UStaticMesh* UGPUVectorDisplacementComponent::BakeCurrentVectorDisplacementToStaticMeshAsset()
{
	return BakeCurrentTessellationToStaticMeshAsset();
}

void UGPUVectorDisplacementComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (bValidateTextureImportSettings && VectorDisplacementTexture)
	{
		ValidateVectorDisplacementTexture();
	}
}
#endif
