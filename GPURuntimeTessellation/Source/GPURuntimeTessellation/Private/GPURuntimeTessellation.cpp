// Licensed under the MIT License. See LICENSE file in the project root.

#include "GPURuntimeTessellation.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "IDetailCustomization.h"
#include "PropertyEditorModule.h"
#endif

#define LOCTEXT_NAMESPACE "FGPURuntimeTessellationModule"

#if WITH_EDITOR
namespace
{
class FGPUTessellationDetailsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FGPUTessellationDetailsCustomization>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
	{
		DetailBuilder.SortCategories(&FGPUTessellationDetailsCustomization::SortCategories);
	}

private:
	static int32 GetCategorySortOrder(const FName CategoryName)
	{
		static const TArray<FName> CategoryOrder =
		{
			TEXT("TransformCommon"),
			TEXT("Transform"),

			TEXT("GPU Vector Displacement"),
			TEXT("GPU Vector Displacement|Decode"),
			TEXT("GPU Vector Displacement|Advanced"),
			TEXT("GPU Vector Displacement|Normals"),
			TEXT("GPU Vector Displacement|Bounds"),
			TEXT("GPU Vector Displacement|Validation"),

			TEXT("GPU Mesh Tessellation"),
			TEXT("GPU Mesh Tessellation|Bake"),
			TEXT("GPU Mesh Tessellation|Normals"),
			TEXT("GPU Mesh Tessellation|Seams"),
			TEXT("GPU Mesh Tessellation|LOD"),
			TEXT("GPU Mesh Tessellation|LOD|Static"),
			TEXT("GPU Mesh Tessellation|Safety"),

			TEXT("GPU Tessellation"),
			TEXT("GPU Tessellation|Advanced"),
			TEXT("GPU Tessellation|Bake"),
			TEXT("GPU Tessellation|Render Target"),
			TEXT("GPU Tessellation|Debug"),

			TEXT("Tessellation"),
			TEXT("Tessellation|Subdivision"),
			TEXT("Geometry"),
			TEXT("Displacement"),
			TEXT("LOD"),
			TEXT("LOD|Discrete"),
			TEXT("LOD|Patches"),
			TEXT("LOD|Patches|Experimental"),
			TEXT("LOD|Quadtree"),
			TEXT("LOD|Quadtree|Critical"),
			TEXT("Normals"),

			TEXT("GPU Tessellation|Collision Mesh"),
			TEXT("GPU Tessellation|Collision Mesh|LOD Rings"),
			TEXT("GPU Tessellation|Collision Mesh|Debug"),

			TEXT("GPU Tessellation|Water Interaction"),
			TEXT("GPU Tessellation|Water Interaction|Buoyancy"),
			TEXT("GPU Tessellation|Water Interaction|Surface Sampling"),
			TEXT("GPU Tessellation|Water Interaction|Debug"),

			TEXT("Ocean"),
			TEXT("Ocean|Wind"),
			TEXT("Ocean|Gerstner"),
			TEXT("Ocean|Perlin"),
			TEXT("Ocean|FFT")
		};

		int32 SortOrder = INDEX_NONE;
		if (CategoryOrder.Find(CategoryName, SortOrder))
		{
			return SortOrder;
		}

		const FString CategoryString = CategoryName.ToString();
		if (CategoryString.StartsWith(TEXT("GPU Vector Displacement|")))
		{
			return 7;
		}
		if (CategoryString.StartsWith(TEXT("GPU Mesh Tessellation|")))
		{
			return 13;
		}
		if (CategoryString.StartsWith(TEXT("GPU Tessellation|Collision Mesh|")))
		{
			return 33;
		}
		if (CategoryString.StartsWith(TEXT("GPU Tessellation|Water Interaction|")))
		{
			return 37;
		}
		if (CategoryString.StartsWith(TEXT("GPU Tessellation|")))
		{
			return 17;
		}
		if (CategoryString.StartsWith(TEXT("LOD|")))
		{
			return 27;
		}
		if (CategoryString.StartsWith(TEXT("Ocean|")))
		{
			return 41;
		}

		return INDEX_NONE;
	}

	static void SortCategories(const TMap<FName, IDetailCategoryBuilder*>& CategoryMap)
	{
		for (const TPair<FName, IDetailCategoryBuilder*>& Pair : CategoryMap)
		{
			if (!Pair.Value)
			{
				continue;
			}

			const int32 DesiredSortOrder = GetCategorySortOrder(Pair.Key);
			Pair.Value->SetSortOrder(DesiredSortOrder == INDEX_NONE ? Pair.Value->GetSortOrder() + 100 : DesiredSortOrder);
		}
	}
};

void RegisterGPUTessellationDetailsCustomization(FPropertyEditorModule& PropertyModule, const FName ClassName)
{
	PropertyModule.RegisterCustomClassLayout(
		ClassName,
		FOnGetDetailCustomizationInstance::CreateStatic(&FGPUTessellationDetailsCustomization::MakeInstance));
}

void UnregisterGPUTessellationDetailsCustomization(FPropertyEditorModule& PropertyModule, const FName ClassName)
{
	PropertyModule.UnregisterCustomClassLayout(ClassName);
}

const TArray<FName>& GetGPUTessellationCustomizedClassNames()
{
	static const TArray<FName> ClassNames =
	{
		TEXT("GPUTessellationComponent"),
		TEXT("GPUTessellationActor"),
		TEXT("GPUVectorDisplacementComponent"),
		TEXT("GPUVectorDisplacementActor"),
		TEXT("GPUMeshTessellationComponent"),
		TEXT("GPUMeshTessellationActor"),
		TEXT("GPUOceanComponent")
	};

	return ClassNames;
}
}
#endif

void FGPURuntimeTessellationModule::StartupModule()
{
	// Map shader directory for plugin shaders
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("GPURuntimeTessellation"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/GPURuntimeTessellation"), PluginShaderDir);

#if WITH_EDITOR
	if (!IsRunningCommandlet())
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		for (const FName ClassName : GetGPUTessellationCustomizedClassNames())
		{
			RegisterGPUTessellationDetailsCustomization(PropertyModule, ClassName);
		}
		PropertyModule.NotifyCustomizationModuleChanged();
	}
#endif
	
	UE_LOG(LogTemp, Log, TEXT("GPURuntimeTessellation: Module started, shader directory mapped to: %s"), *PluginShaderDir);
}

void FGPURuntimeTessellationModule::ShutdownModule()
{
#if WITH_EDITOR
	if (!IsRunningCommandlet())
	{
		if (FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			for (const FName ClassName : GetGPUTessellationCustomizedClassNames())
			{
				UnregisterGPUTessellationDetailsCustomization(*PropertyModule, ClassName);
			}
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}
#endif

	UE_LOG(LogTemp, Log, TEXT("GPURuntimeTessellation: Module shutdown"));
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FGPURuntimeTessellationModule, GPURuntimeTessellation)
