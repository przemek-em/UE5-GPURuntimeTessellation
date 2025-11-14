// Licensed under the MIT License. See LICENSE file in the project root.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * GPU Runtime Tessellation Module
 * 
 * Pure compute shader-based tessellation system that replaces Hull/Domain shaders
 * with compute shaders for universal platform support.
 */
class FGPURuntimeTessellationModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
