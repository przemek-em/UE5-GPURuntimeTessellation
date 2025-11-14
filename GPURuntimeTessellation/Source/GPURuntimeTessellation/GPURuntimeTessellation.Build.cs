// Licensed under the MIT License. See LICENSE file in the project root.

using UnrealBuildTool;

public class GPURuntimeTessellation : ModuleRules
{
	public GPURuntimeTessellation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// Public include paths
			}
		);
				
		PrivateIncludePaths.AddRange(
			new string[] {
				// Private include paths
			}
		);
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"Renderer",
				"RHI",
				"RHICore"
			}
		);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects"
			}
		);
		
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Slate",
					"SlateCore",
					"UnrealEd",
					"PropertyEditor",
					"EditorStyle",
					"ToolMenus",
					"InputCore",
					"LevelEditor"
				}
			);
		}
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// Dynamic modules
			}
		);
	}
}
