// Copyright Epic Games, Inc. All Rights Reserved.

#include "GPUTessellationComputeShaders.h"
#include "ShaderCompilerCore.h"

// Implement all compute shaders
IMPLEMENT_GLOBAL_SHADER(FGPUTessellationFactorCS, "/Plugin/GPURuntimeTessellation/Private/GPUTessellationFactor.usf", "CalculateTessellationFactors", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPUVertexGenerationCS, "/Plugin/GPURuntimeTessellation/Private/GPUVertexGeneration.usf", "GenerateVertices", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPUDisplacementCS, "/Plugin/GPURuntimeTessellation/Private/GPUDisplacement.usf", "ApplyDisplacement", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPUNormalCalculationCS, "/Plugin/GPURuntimeTessellation/Private/GPUNormalCalculation.usf", "CalculateNormals", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPUIndexGenerationCS, "/Plugin/GPURuntimeTessellation/Private/GPUIndexGeneration.usf", "GenerateIndices", SF_Compute);
