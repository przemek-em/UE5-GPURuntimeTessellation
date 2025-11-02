// Copyright Epic Games, Inc. All Rights Reserved.

#include "GPURuntimeTessellation.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FGPURuntimeTessellationModule"

void FGPURuntimeTessellationModule::StartupModule()
{
	// Map shader directory for plugin shaders
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("GPURuntimeTessellation"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/GPURuntimeTessellation"), PluginShaderDir);
	
	UE_LOG(LogTemp, Log, TEXT("GPURuntimeTessellation: Module started, shader directory mapped to: %s"), *PluginShaderDir);
}

void FGPURuntimeTessellationModule::ShutdownModule()
{
	UE_LOG(LogTemp, Log, TEXT("GPURuntimeTessellation: Module shutdown"));
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FGPURuntimeTessellationModule, GPURuntimeTessellation)
