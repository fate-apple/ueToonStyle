// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenSceneRendering.cpp
=============================================================================*/

#include "LumenSceneRendering.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "MeshPassProcessor.inl"
#include "MeshCardRepresentation.h"
#include "GPUScene.h"
#include "Rendering/NaniteResources.h"
#include "Nanite/Nanite.h"
#include "PixelShaderUtils.h"
#include "Lumen.h"
#include "LumenMeshCards.h"
#include "LumenSurfaceCacheFeedback.h"
#include "LumenSceneLighting.h"
#include "LumenTracingUtils.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "HAL/LowLevelMemStats.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

int32 GLumenSupported = 1;
FAutoConsoleVariableRef CVarLumenSupported(
	TEXT("r.Lumen.Supported"),
	GLumenSupported,
	TEXT("Whether Lumen is supported at all for the project, regardless of platform.  This can be used to avoid compiling shaders and other load time overhead."),
	ECVF_ReadOnly
);

int32 GLumenFastCameraMode = 0;
FAutoConsoleVariableRef CVarLumenFastCameraMode(
	TEXT("r.LumenScene.FastCameraMode"),
	GLumenFastCameraMode,
	TEXT("Whether to update the Lumen Scene for fast camera movement - lower quality, faster updates so lighting can keep up with the camera."),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneParallelUpdate = 1;
FAutoConsoleVariableRef CVarLumenSceneParallelUpdate(
	TEXT("r.LumenScene.ParallelUpdate"),
	GLumenSceneParallelUpdate,
	TEXT("Whether to run the Lumen Scene update in parallel."),
	ECVF_RenderThreadSafe
);

int32 GLumenScenePrimitivesPerTask = 128;
FAutoConsoleVariableRef CVarLumenScenePrimitivePerTask(
	TEXT("r.LumenScene.PrimitivesPerTask"),
	GLumenScenePrimitivesPerTask,
	TEXT("How many primitives to process per single surface cache update task."),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneMeshCardsPerTask = 128;
FAutoConsoleVariableRef CVarLumenSceneMeshCardsPerTask(
	TEXT("r.LumenScene.MeshCardsPerTask"),
	GLumenSceneMeshCardsPerTask,
	TEXT("How many mesh cards to process per single surface cache update task."),
	ECVF_RenderThreadSafe
);

int32 GLumenGIMaxConeSteps = 1000;
FAutoConsoleVariableRef CVarLumenGIMaxConeSteps(
	TEXT("r.Lumen.MaxConeSteps"),
	GLumenGIMaxConeSteps,
	TEXT("Maximum steps to use for Cone Stepping of proxy cards."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenSurfaceCacheFreeze = 0;
FAutoConsoleVariableRef CVarLumenSceneSurfaceCacheFreeze(
	TEXT("r.LumenScene.SurfaceCache.Freeze"),
	GLumenSurfaceCacheFreeze,
	TEXT("Freeze surface cache updates for debugging.\n"),
	ECVF_RenderThreadSafe
);

int32 GLumenSurfaceCacheFreezeUpdateFrame = 0;
FAutoConsoleVariableRef CVarLumenSceneSurfaceCacheFreezeUpdateFrame(
	TEXT("r.LumenScene.SurfaceCache.FreezeUpdateFrame"),
	GLumenSurfaceCacheFreezeUpdateFrame,
	TEXT("Keep updating the same subset of surface cache for debugging and profiling.\n"),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneSurfaceCacheReset = 0;
FAutoConsoleVariableRef CVarLumenSceneSurfaceCacheReset(
	TEXT("r.LumenScene.SurfaceCache.Reset"),
	GLumenSceneSurfaceCacheReset,
	TEXT("Reset all atlases and captured cards.\n"),	
	ECVF_RenderThreadSafe
);

int32 GLumenSceneSurfaceCacheResetEveryNthFrame = 0;
FAutoConsoleVariableRef CVarLumenSceneSurfaceCacheResetEveryNthFrame(
	TEXT("r.LumenScene.SurfaceCache.ResetEveryNthFrame"),
	GLumenSceneSurfaceCacheResetEveryNthFrame,
	TEXT("Continuosly reset all atlases and captured cards every N-th frame.\n"),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneCardCapturesPerFrame = 300;
FAutoConsoleVariableRef CVarLumenSceneCardCapturesPerFrame(
	TEXT("r.LumenScene.SurfaceCache.CardCapturesPerFrame"),
	GLumenSceneCardCapturesPerFrame,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenSceneCardCaptureFactor = 64;
FAutoConsoleVariableRef CVarLumenSceneCardCaptureFactor(
	TEXT("r.LumenScene.SurfaceCache.CardCaptureFactor"),
	GLumenSceneCardCaptureFactor,
	TEXT("Controls how many texels can be captured per frame. Texels = SurfaceCacheTexels / Factor."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

TAutoConsoleVariable<float> CVarLumenSceneCardCaptureRefreshFraction(
	TEXT("r.LumenScene.SurfaceCache.CardCaptureRefreshFraction"),
	0.125f,
	TEXT("Fraction of card capture budget allowed to be spent on re-capturing existing pages in order to refresh surface cache materials.\n")
	TEXT("0 disables card refresh."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenSceneCardCaptureMargin = 0.0f;
FAutoConsoleVariableRef CVarLumenSceneCardCaptureMargin(
	TEXT("r.LumenScene.SurfaceCache.CardCaptureMargin"),
	GLumenSceneCardCaptureMargin,
	TEXT("How far from Lumen scene range start to capture cards."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenSceneCardFixedDebugResolution = -1;
FAutoConsoleVariableRef CVarLumenSceneCardFixedDebugResolution(
	TEXT("r.LumenScene.SurfaceCache.CardFixedDebugResolution"),
	GLumenSceneCardFixedDebugResolution,
	TEXT("Lumen card resolution"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenSceneCardCameraDistanceTexelDensityScale = 100;
FAutoConsoleVariableRef CVarLumenSceneCardCameraDistanceTexelDensityScale(
	TEXT("r.LumenScene.SurfaceCache.CardCameraDistanceTexelDensityScale"),
	GLumenSceneCardCameraDistanceTexelDensityScale,
	TEXT("Lumen card texels per world space distance"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenSceneCardMaxTexelDensity = .2f;
FAutoConsoleVariableRef CVarLumenSceneCardMaxTexelDensity(
	TEXT("r.LumenScene.SurfaceCache.CardMaxTexelDensity"),
	GLumenSceneCardMaxTexelDensity,
	TEXT("Lumen card texels per world space distance"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenSceneCardMinResolution = 4;
FAutoConsoleVariableRef CVarLumenSceneCardMinResolution(
	TEXT("r.LumenScene.SurfaceCache.CardMinResolution"),
	GLumenSceneCardMinResolution,
	TEXT("Minimum mesh card size resolution to be visible in Lumen Scene"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenSceneCardMaxResolution = 512;
FAutoConsoleVariableRef CVarLumenSceneCardMaxResolution(
	TEXT("r.LumenScene.SurfaceCache.CardMaxResolution"),
	GLumenSceneCardMaxResolution,
	TEXT("Maximum card resolution in Lumen Scene"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GSurfaceCacheNumFramesToKeepUnusedPages = 256;
FAutoConsoleVariableRef CVarLumenSceneSurfaceCacheNumFramesToKeepUnusedPages(
	TEXT("r.LumenScene.SurfaceCache.NumFramesToKeepUnusedPages"),
	GSurfaceCacheNumFramesToKeepUnusedPages,
	TEXT("Num frames to keep unused pages in surface cache."),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneForceEvictHiResPages = 0;
FAutoConsoleVariableRef CVarLumenSceneForceEvictHiResPages(
	TEXT("r.LumenScene.SurfaceCache.ForceEvictHiResPages"),
	GLumenSceneForceEvictHiResPages,
	TEXT("Evict all optional hi-res surface cache pages."),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneRecaptureLumenSceneEveryFrame = 0;
FAutoConsoleVariableRef CVarLumenGIRecaptureLumenSceneEveryFrame(
	TEXT("r.LumenScene.SurfaceCache.RecaptureEveryFrame"),
	GLumenSceneRecaptureLumenSceneEveryFrame,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneNaniteMultiViewRaster = 1;
FAutoConsoleVariableRef CVarLumenSceneNaniteMultiViewRaster(
	TEXT("r.LumenScene.SurfaceCache.NaniteMultiViewRaster"),
	GLumenSceneNaniteMultiViewRaster,
	TEXT("Toggle multi view Lumen Nanite Card rasterization for debugging."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
		{
			Lumen::DebugResetSurfaceCache();
		}),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneNaniteMultiViewCapture = 1;
FAutoConsoleVariableRef CVarLumenSceneNaniteMultiViewCapture(
	TEXT("r.LumenScene.SurfaceCache.NaniteMultiViewCapture"),
	GLumenSceneNaniteMultiViewCapture,
	TEXT("Toggle multi view Lumen Nanite Card capture for debugging."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
		{
			Lumen::DebugResetSurfaceCache();
		}),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneGlobalDFResolution = 224;
FAutoConsoleVariableRef CVarLumenSceneGlobalDFResolution(
	TEXT("r.LumenScene.GlobalSDF.Resolution"),
	GLumenSceneGlobalDFResolution,
	TEXT(""),
	ECVF_RenderThreadSafe
);

float GLumenSceneGlobalDFClipmapExtent = 2500.0f;
FAutoConsoleVariableRef CVarLumenSceneGlobalDFClipmapExtent(
	TEXT("r.LumenScene.GlobalSDF.ClipmapExtent"),
	GLumenSceneGlobalDFClipmapExtent,
	TEXT(""),
	ECVF_RenderThreadSafe
);

float GLumenSceneFarFieldTexelDensity = 0.001f;
FAutoConsoleVariableRef CVarLumenSceneFarFieldTexelDensity(
	TEXT("r.LumenScene.SurfaceCache.FarField.TexelDensity"),
	GLumenSceneFarFieldTexelDensity,
	TEXT("Far Field Lumen card texels per world space unit"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenSceneFarFieldDistance = 40000.00f;
FAutoConsoleVariableRef CVarLumenSceneFarFieldDistance(
	TEXT("r.LumenScene.SurfaceCache.FarField.Distance"),
	GLumenSceneFarFieldDistance,
	TEXT("Far Field Lumen card culling distance"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenSceneSurfaceCacheLogUpdates = 0;
FAutoConsoleVariableRef CVarLumenSceneSurfaceCacheLogUpdates(
	TEXT("r.LumenScene.SurfaceCache.LogUpdates"),
	GLumenSceneSurfaceCacheLogUpdates,
	TEXT("Whether to log Lumen surface cache updates.\n")
	TEXT("2 - will log mesh names."),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneSurfaceCacheResampleLighting = 1;
FAutoConsoleVariableRef CVarLumenSceneSurfaceCacheResampleLighting(
	TEXT("r.LumenScene.SurfaceCache.ResampleLighting"),
	GLumenSceneSurfaceCacheResampleLighting,
	TEXT("Whether to resample card lighting when cards are reallocated.  This is needed for Radiosity temporal accumulation but can be disabled for debugging."),
	ECVF_RenderThreadSafe
);

#if ENABLE_LOW_LEVEL_MEM_TRACKER
DECLARE_LLM_MEMORY_STAT(TEXT("Lumen"), STAT_LumenLLM, STATGROUP_LLMFULL);
DECLARE_LLM_MEMORY_STAT(TEXT("Lumen"), STAT_LumenSummaryLLM, STATGROUP_LLM);
LLM_DEFINE_TAG(Lumen, NAME_None, NAME_None, GET_STATFNAME(STAT_LumenLLM), GET_STATFNAME(STAT_LumenSummaryLLM));
#endif // ENABLE_LOW_LEVEL_MEM_TRACKER

extern int32 GAllowLumenDiffuseIndirect;
extern int32 GAllowLumenReflections;

namespace LumenSurfaceCache
{
	int32 GetMinCardResolution()
	{
		 return FMath::Clamp(GLumenSceneCardMinResolution, 1, 1024);
	}
};

namespace LumenLandscape
{
	constexpr int32 CardCaptureLOD = 0;
};

void Lumen::DebugResetSurfaceCache()
{
	GLumenSceneSurfaceCacheReset = 1;
}

bool Lumen::IsSurfaceCacheFrozen()
{
	return GLumenSurfaceCacheFreeze != 0;
}

bool Lumen::IsSurfaceCacheUpdateFrameFrozen()
{
	return GLumenSurfaceCacheFreeze != 0 || GLumenSurfaceCacheFreezeUpdateFrame != 0;
}

namespace Lumen
{
	bool AnyLumenHardwareRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View)
	{
#if RHI_RAYTRACING

		const bool bLumenGI = ShouldRenderLumenDiffuseGI(Scene, View);
		const bool bLumenReflections = ShouldRenderLumenReflections(View);

		if (bLumenGI
			&& (UseHardwareRayTracedScreenProbeGather() || UseHardwareRayTracedRadianceCache() || UseHardwareRayTracedDirectLighting()))
		{
			return true;
		}

		if (bLumenReflections
			&& UseHardwareRayTracedReflections())
		{
			return true;
		}

		if ((bLumenGI || bLumenReflections) && Lumen::ShouldVisualizeHardwareRayTracing(*View.Family))
		{
			return true;
		}

		if ((bLumenGI || bLumenReflections) && Lumen::ShouldRenderRadiosityHardwareRayTracing(*View.Family))
		{
			return true;
		}
#endif
		return false;
	}

	bool AnyLumenHardwareInlineRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View)
	{
		if (!AnyLumenHardwareRayTracingPassEnabled(Scene, View))
		{
			return false;
		}

		return Lumen::UseHardwareInlineRayTracing();
	}
}

bool Lumen::ShouldHandleSkyLight(const FScene* Scene, const FSceneViewFamily& ViewFamily)
{
	return Scene->SkyLight
		&& (Scene->SkyLight->ProcessedTexture || Scene->SkyLight->bRealTimeCaptureEnabled)
		&& ViewFamily.EngineShowFlags.SkyLighting
		&& Scene->GetFeatureLevel() >= ERHIFeatureLevel::SM5
		&& !IsAnyForwardShadingEnabled(Scene->GetShaderPlatform())
		&& !ViewFamily.EngineShowFlags.VisualizeLightCulling;
}

bool ShouldRenderLumenForViewFamily(const FScene* Scene, const FSceneViewFamily& ViewFamily, bool bSkipProjectCheck)
{
	return Scene
		&& Scene->LumenSceneData
		&& ViewFamily.Views.Num() == 1
		&& DoesPlatformSupportLumenGI(Scene->GetShaderPlatform(), bSkipProjectCheck);
}

bool Lumen::IsSoftwareRayTracingSupported()
{
	return DoesProjectSupportDistanceFields();
}

bool Lumen::IsLumenFeatureAllowedForView(const FScene* Scene, const FSceneView& View, bool bSkipTracingDataCheck, bool bSkipProjectCheck)
{
	return View.Family
		&& ShouldRenderLumenForViewFamily(Scene, *View.Family, bSkipProjectCheck)
		// Don't update scene lighting for secondary views
		&& !View.bIsPlanarReflection
		&& !View.bIsSceneCapture
		&& !View.bIsReflectionCapture
		&& View.State
		&& (bSkipTracingDataCheck || Lumen::UseHardwareRayTracing() || IsSoftwareRayTracingSupported());
}

int32 Lumen::GetGlobalDFResolution()
{
	return GLumenSceneGlobalDFResolution;
}

float Lumen::GetGlobalDFClipmapExtent()
{
	return GLumenSceneGlobalDFClipmapExtent;
}

float GetCardCameraDistanceTexelDensityScale()
{
	return GLumenSceneCardCameraDistanceTexelDensityScale * (GLumenFastCameraMode ? .2f : 1.0f);
}

int32 GetCardMaxResolution()
{
	if (GLumenFastCameraMode)
	{
		return GLumenSceneCardMaxResolution / 2;
	}

	return GLumenSceneCardMaxResolution;
}

int32 GetMaxLumenSceneCardCapturesPerFrame()
{
	return FMath::Max(GLumenSceneCardCapturesPerFrame * (GLumenFastCameraMode ? 2 : 1), 0);
}

int32 GetMaxMeshCardsToAddPerFrame()
{
	return 2 * GetMaxLumenSceneCardCapturesPerFrame();
}

int32 GetMaxTileCapturesPerFrame()
{
	if (Lumen::IsSurfaceCacheFrozen())
	{
		return 0;
	}

	if (GLumenSceneRecaptureLumenSceneEveryFrame != 0)
	{
		return INT32_MAX;
	}

	return GetMaxLumenSceneCardCapturesPerFrame();
}

uint32 FLumenSceneData::GetSurfaceCacheUpdateFrameIndex() const
{
	return SurfaceCacheUpdateFrameIndex;
}

void FLumenSceneData::IncrementSurfaceCacheUpdateFrameIndex()
{
	if (!Lumen::IsSurfaceCacheUpdateFrameFrozen())
	{
		++SurfaceCacheUpdateFrameIndex;
		if (SurfaceCacheUpdateFrameIndex == 0)
		{
			++SurfaceCacheUpdateFrameIndex;
		}
	}
}

DECLARE_GPU_STAT(LumenSceneUpdate);
DECLARE_GPU_STAT(UpdateLumenSceneBuffers);

IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FLumenCardPassUniformParameters, "LumenCardPass", SceneTextures);

class FLumenCardVS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FLumenCardVS, MeshMaterial);

protected:

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		//@todo DynamicGI - filter
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	FLumenCardVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{}

	FLumenCardVS() = default;
};


IMPLEMENT_MATERIAL_SHADER_TYPE(, FLumenCardVS, TEXT("/Engine/Private/Lumen/LumenCardVertexShader.usf"), TEXT("Main"), SF_Vertex);

template<bool bMultiViewCapture>
class FLumenCardPS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FLumenCardPS, MeshMaterial);

public:
	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		if (Parameters.VertexFactoryType->SupportsNaniteRendering() != bMultiViewCapture)
		{
			return false;
		}

		//@todo DynamicGI - filter
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	FLumenCardPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{}

	FLumenCardPS() = default;

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("LUMEN_MULTI_VIEW_CAPTURE"), bMultiViewCapture);
	}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FLumenCardPS<false>, TEXT("/Engine/Private/Lumen/LumenCardPixelShader.usf"), TEXT("Main"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FLumenCardPS<true>, TEXT("/Engine/Private/Lumen/LumenCardPixelShader.usf"), TEXT("Main"), SF_Pixel);

class FLumenCardMeshProcessor : public FMeshPassProcessor
{
public:

	FLumenCardMeshProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, const FMeshPassProcessorRenderState& InPassDrawRenderState, FMeshPassDrawListContext* InDrawListContext);

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final;

	FMeshPassProcessorRenderState PassDrawRenderState;
};

bool GetLumenCardShaders(
	const FMaterial& Material,
	FVertexFactoryType* VertexFactoryType,
	TShaderRef<FLumenCardVS>& VertexShader,
	TShaderRef<FLumenCardPS<false>>& PixelShader)
{
	FMaterialShaderTypes ShaderTypes;
	ShaderTypes.AddShaderType<FLumenCardVS>();
	ShaderTypes.AddShaderType<FLumenCardPS<false>>();

	FMaterialShaders Shaders;
	if (!Material.TryGetShaders(ShaderTypes, VertexFactoryType, Shaders))
	{
		return false;
	}

	Shaders.TryGetVertexShader(VertexShader);
	Shaders.TryGetPixelShader(PixelShader);
	return true;
}

void FLumenCardMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	LLM_SCOPE_BYTAG(Lumen);

	if (MeshBatch.bUseForMaterial
		&& DoesPlatformSupportLumenGI(GetFeatureLevelShaderPlatform(FeatureLevel))
		&& (PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderInMainPass() && PrimitiveSceneProxy->AffectsDynamicIndirectLighting()))
	{
		const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
		while (MaterialRenderProxy)
		{
			const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
			if (Material)
			{
				auto TryAddMeshBatch = [this](const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId, const FMaterialRenderProxy& MaterialRenderProxy, const FMaterial& Material) -> bool
				{
					const EBlendMode BlendMode = Material.GetBlendMode();
					const FMaterialShadingModelField ShadingModels = Material.GetShadingModels();
					const bool bIsTranslucent = IsTranslucentBlendMode(BlendMode);
					const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
					const ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, Material, OverrideSettings);
					const ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, Material, OverrideSettings);

					if (!bIsTranslucent
						&& ShouldIncludeDomainInMeshPass(Material.GetMaterialDomain()))
					{
						const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;
						FVertexFactoryType* VertexFactoryType = VertexFactory->GetType();
						constexpr bool bMultiViewCapture = false;

						TMeshProcessorShaders<
							FLumenCardVS,
							FLumenCardPS<bMultiViewCapture>> PassShaders;

						if (!GetLumenCardShaders(
							Material,
							VertexFactory->GetType(),
							PassShaders.VertexShader,
							PassShaders.PixelShader))
						{
							return false;
						}

						FMeshMaterialShaderElementData ShaderElementData;
						ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

						const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(PassShaders.VertexShader, PassShaders.PixelShader);

						BuildMeshDrawCommands(
							MeshBatch,
							BatchElementMask,
							PrimitiveSceneProxy,
							MaterialRenderProxy,
							Material,
							PassDrawRenderState,
							PassShaders,
							MeshFillMode,
							MeshCullMode,
							SortKey,
							EMeshPassFeatures::Default,
							ShaderElementData);
					}

					return true;
				};

				if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, *MaterialRenderProxy, *Material))
				{
					break;
				}
			};

			MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
		}
	}
}

FLumenCardMeshProcessor::FLumenCardMeshProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, const FMeshPassProcessorRenderState& InPassDrawRenderState, FMeshPassDrawListContext* InDrawListContext)
	: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	, PassDrawRenderState(InPassDrawRenderState)
{}

FMeshPassProcessor* CreateLumenCardCapturePassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	LLM_SCOPE_BYTAG(Lumen);

	FMeshPassProcessorRenderState PassState;

	// Write and test against depth
	PassState.SetDepthStencilState(TStaticDepthStencilState<true, CF_Greater>::GetRHI());

	PassState.SetBlendState(TStaticBlendState<>::GetRHI());

	return new(FMemStack::Get()) FLumenCardMeshProcessor(Scene, InViewIfDynamicMeshCommand, PassState, InDrawListContext);
}

FRegisterPassProcessorCreateFunction RegisterLumenCardCapturePass(&CreateLumenCardCapturePassProcessor, EShadingPath::Deferred, EMeshPass::LumenCardCapture, EMeshPassFlags::CachedMeshCommands);

class FLumenCardNaniteMeshProcessor : public FMeshPassProcessor
{
public:

	FLumenCardNaniteMeshProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, const FMeshPassProcessorRenderState& InPassDrawRenderState, FMeshPassDrawListContext* InDrawListContext);

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final;

	FMeshPassProcessorRenderState PassDrawRenderState;

private:
	bool TryAddMeshBatch(
		const FMeshBatch& RESTRICT MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		int32 StaticMeshId,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material);
};

FLumenCardNaniteMeshProcessor::FLumenCardNaniteMeshProcessor(
	const FScene* InScene,
	const FSceneView* InViewIfDynamicMeshCommand,
	const FMeshPassProcessorRenderState& InDrawRenderState,
	FMeshPassDrawListContext* InDrawListContext
) :
	FMeshPassProcessor(InScene, InScene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext),
	PassDrawRenderState(InDrawRenderState)
{
}

using FLumenCardNanitePassShaders = TMeshProcessorShaders<FNaniteMultiViewMaterialVS, FLumenCardPS<true>>;

void FLumenCardNaniteMeshProcessor::AddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId /*= -1 */
)
{
	LLM_SCOPE_BYTAG(Lumen);

	checkf(Lumen::HasPrimitiveNaniteMeshBatches(PrimitiveSceneProxy) && DoesPlatformSupportLumenGI(GetFeatureLevelShaderPlatform(FeatureLevel)),
		TEXT("Logic in BuildNaniteDrawCommands() should not have allowed an unqualifying mesh batch to be added"));

	const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
	while (MaterialRenderProxy)
	{
		const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
		if (Material)
		{
			if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, *MaterialRenderProxy, *Material))
			{
				break;
			}
		}

		MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
	}
}

bool FLumenCardNaniteMeshProcessor::TryAddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy& MaterialRenderProxy,
	const FMaterial& Material)
{
	const EBlendMode BlendMode = Material.GetBlendMode();

	check(BlendMode == BLEND_Opaque);
	check(Material.GetMaterialDomain() == MD_Surface);

	TShaderMapRef<FNaniteMultiViewMaterialVS> VertexShader(GetGlobalShaderMap(FeatureLevel));

	FLumenCardNanitePassShaders PassShaders;
	PassShaders.VertexShader = VertexShader;

	const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;
	FVertexFactoryType* VertexFactoryType = VertexFactory->GetType();
	constexpr bool bMultiViewCapture = true;

	FMaterialShaderTypes ShaderTypes;
	ShaderTypes.AddShaderType<FLumenCardPS<bMultiViewCapture>>();

	FMaterialShaders Shaders;
	if (!Material.TryGetShaders(ShaderTypes, VertexFactoryType, Shaders))
	{
		return false;
	}

	Shaders.TryGetPixelShader(PassShaders.PixelShader);

	FMeshMaterialShaderElementData ShaderElementData;
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

	BuildMeshDrawCommands(
		MeshBatch,
		BatchElementMask,
		PrimitiveSceneProxy,
		MaterialRenderProxy,
		Material,
		PassDrawRenderState,
		PassShaders,
		FM_Solid,
		CM_None,
		FMeshDrawCommandSortKey::Default,
		EMeshPassFeatures::Default,
		ShaderElementData
	);

	return true;
}

FMeshPassProcessor* CreateLumenCardNaniteMeshProcessor(
	const FScene* Scene,
	const FSceneView* InViewIfDynamicMeshCommand,
	FMeshPassDrawListContext* InDrawListContext)
{
	LLM_SCOPE_BYTAG(Lumen);

	FMeshPassProcessorRenderState PassState;
	PassState.SetNaniteUniformBuffer(Scene->UniformBuffers.NaniteUniformBuffer);

	PassState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Equal, true, CF_Equal>::GetRHI());
	PassState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
	PassState.SetStencilRef(STENCIL_SANDBOX_MASK);
	PassState.SetBlendState(TStaticBlendState<>::GetRHI());

	return new(FMemStack::Get()) FLumenCardNaniteMeshProcessor(Scene, InViewIfDynamicMeshCommand, PassState, InDrawListContext);
}

FCardPageRenderData::FCardPageRenderData(const FViewInfo& InMainView,
	const FLumenCard& InLumenCard,
	FVector4f InCardUVRect,
	FIntRect InCardCaptureAtlasRect,
	FIntRect InSurfaceCacheAtlasRect,
	int32 InPrimitiveGroupIndex,
	int32 InCardIndex,
	int32 InPageTableIndex,
	bool bInResampleLastLighting)
	: PrimitiveGroupIndex(InPrimitiveGroupIndex)
	, CardIndex(InCardIndex)
	, PageTableIndex(InPageTableIndex)
	, bDistantScene(InLumenCard.bDistantScene)
	, CardUVRect(InCardUVRect)
	, CardCaptureAtlasRect(InCardCaptureAtlasRect)
	, SurfaceCacheAtlasRect(InSurfaceCacheAtlasRect)
	, CardWorldOBB(InLumenCard.WorldOBB)
	, bResampleLastLighting(bInResampleLastLighting)
{
	ensure(CardIndex >= 0 && PageTableIndex >= 0);

	if (InLumenCard.bDistantScene)
	{
		NaniteLODScaleFactor = Lumen::GetDistanceSceneNaniteLODScaleFactor();
	}

	UpdateViewMatrices(InMainView);
}

void FCardPageRenderData::UpdateViewMatrices(const FViewInfo& MainView)
{
	ensureMsgf(FVector3f::DotProduct(CardWorldOBB.AxisX, FVector3f::CrossProduct(CardWorldOBB.AxisY, CardWorldOBB.AxisZ)) < 0.0f, TEXT("Card has wrong handedness"));

	FMatrix ViewRotationMatrix = FMatrix::Identity;
	ViewRotationMatrix.SetColumn(0, (FVector)CardWorldOBB.AxisX);
	ViewRotationMatrix.SetColumn(1, (FVector)CardWorldOBB.AxisY);
	ViewRotationMatrix.SetColumn(2, (FVector)-CardWorldOBB.AxisZ);

	FVector ViewLocation(CardWorldOBB.Origin);
	FVector FaceLocalExtent(CardWorldOBB.Extent);
	// Pull the view location back so the entire box is in front of the near plane
	ViewLocation += FVector(FaceLocalExtent.Z * CardWorldOBB.AxisZ);

	const float NearPlane = 0.0f;
	const float FarPlane = NearPlane + FaceLocalExtent.Z * 2.0f;

	const float ZScale = 1.0f / (FarPlane - NearPlane);
	const float ZOffset = -NearPlane;

	const FVector4f ProjectionRect = FVector4f(2.0f, 2.0f, 2.0f, 2.0f) * CardUVRect - FVector4f(1.0f, 1.0f, 1.0f, 1.0f);

	const float ProjectionL = ProjectionRect.X * 0.5f * FaceLocalExtent.X;
	const float ProjectionR = ProjectionRect.Z * 0.5f * FaceLocalExtent.X;

	const float ProjectionB = -ProjectionRect.W * 0.5f * FaceLocalExtent.Y;
	const float ProjectionT = -ProjectionRect.Y * 0.5f * FaceLocalExtent.Y;

	const FMatrix ProjectionMatrix = FReversedZOrthoMatrix(
		ProjectionL,
		ProjectionR,
		ProjectionB,
		ProjectionT,
		ZScale,
		ZOffset);

	ProjectionMatrixUnadjustedForRHI = ProjectionMatrix;

	FViewMatrices::FMinimalInitializer Initializer;
	Initializer.ViewRotationMatrix = ViewRotationMatrix;
	Initializer.ViewOrigin = ViewLocation;
	Initializer.ProjectionMatrix = ProjectionMatrix;
	Initializer.ConstrainedViewRect = MainView.SceneViewInitOptions.GetConstrainedViewRect();
	Initializer.StereoPass = MainView.SceneViewInitOptions.StereoPass;
#if WITH_EDITOR
	Initializer.bUseFauxOrthoViewPos = MainView.SceneViewInitOptions.bUseFauxOrthoViewPos;
#endif

	ViewMatrices = FViewMatrices(Initializer);
}

void FCardPageRenderData::PatchView(FRHICommandList& RHICmdList, const FScene* Scene, FViewInfo* View) const
{
	View->ProjectionMatrixUnadjustedForRHI = ProjectionMatrixUnadjustedForRHI;
	View->ViewMatrices = ViewMatrices;
	View->ViewRect = CardCaptureAtlasRect;

	FBox VolumeBounds[TVC_MAX];
	View->SetupUniformBufferParameters(
		VolumeBounds,
		TVC_MAX,
		*View->CachedViewUniformShaderParameters);

	View->CachedViewUniformShaderParameters->NearPlane = 0;
	View->CachedViewUniformShaderParameters->FarShadowStaticMeshLODBias = 0;
	View->CachedViewUniformShaderParameters->OverrideLandscapeLOD = LumenLandscape::CardCaptureLOD;
}

void AddCardCaptureDraws(const FScene* Scene,
	FRHICommandListImmediate& RHICmdList,
	FCardPageRenderData& CardPageRenderData,
	const FLumenPrimitiveGroup& PrimitiveGroup,
	TConstArrayView<const FPrimitiveSceneInfo*> SceneInfoPrimitives,
	FMeshCommandOneFrameArray& VisibleMeshCommands,
	TArray<int32, SceneRenderingAllocator>& PrimitiveIds)
{
	LLM_SCOPE_BYTAG(Lumen);

	const EMeshPass::Type MeshPass = EMeshPass::LumenCardCapture;
	const ENaniteMeshPass::Type NaniteMeshPass = ENaniteMeshPass::LumenCardCapture;
	const FBox WorldSpaceCardBox = CardPageRenderData.CardWorldOBB.GetBox();

	uint32 MaxVisibleMeshDrawCommands = 0;
	for (const FPrimitiveSceneInfo* PrimitiveSceneInfo : SceneInfoPrimitives)
	{
		if (PrimitiveSceneInfo
			&& PrimitiveSceneInfo->Proxy->AffectsDynamicIndirectLighting()
			&& WorldSpaceCardBox.Intersect(PrimitiveSceneInfo->Proxy->GetBounds().GetBox())
			&& !PrimitiveSceneInfo->Proxy->IsNaniteMesh())
		{
			MaxVisibleMeshDrawCommands += PrimitiveSceneInfo->StaticMeshRelevances.Num();
		}
	}
	CardPageRenderData.InstanceRuns.Reserve(2 * MaxVisibleMeshDrawCommands);

	for (const FPrimitiveSceneInfo* PrimitiveSceneInfo : SceneInfoPrimitives)
	{
		if (PrimitiveSceneInfo
			&& PrimitiveSceneInfo->Proxy->AffectsDynamicIndirectLighting()
			&& WorldSpaceCardBox.Intersect(PrimitiveSceneInfo->Proxy->GetBounds().GetBox()))
		{
			if (PrimitiveSceneInfo->Proxy->IsNaniteMesh())
			{
				if (PrimitiveGroup.PrimitiveInstanceIndex >= 0)
				{
					CardPageRenderData.NaniteInstanceIds.Add(PrimitiveSceneInfo->GetInstanceSceneDataOffset() + PrimitiveGroup.PrimitiveInstanceIndex);
				}
				else
				{
					// Render all instances
					const int32 NumInstances = PrimitiveSceneInfo->GetNumInstanceSceneDataEntries();

					for (int32 InstanceIndex = 0; InstanceIndex < NumInstances; ++InstanceIndex)
					{
						CardPageRenderData.NaniteInstanceIds.Add(PrimitiveSceneInfo->GetInstanceSceneDataOffset() + InstanceIndex);
					}
				}

				for (const FNaniteCommandInfo& CommandInfo : PrimitiveSceneInfo->NaniteCommandInfos[NaniteMeshPass])
				{
					CardPageRenderData.NaniteCommandInfos.Add(CommandInfo);
				}
			}
			else
			{
				FLODMask LODToRender;

				if (PrimitiveGroup.bHeightfield)
				{
					// Landscape can't use last LOD, as it's a single quad with only 4 distinct heightfield values
					// Also selected LOD needs to to match FLandscapeSectionLODUniformParameters uniform buffers
					LODToRender.SetLOD(LumenLandscape::CardCaptureLOD);
				}
				else
				{
					int32 MaxLOD = 0;
					for (int32 MeshIndex = 0; MeshIndex < PrimitiveSceneInfo->StaticMeshRelevances.Num(); ++MeshIndex)
					{
						const FStaticMeshBatchRelevance& Mesh = PrimitiveSceneInfo->StaticMeshRelevances[MeshIndex];
						if (Mesh.ScreenSize > 0.0f)
						{
							//todo DynamicGI artist control - last LOD is sometimes billboard
							MaxLOD = FMath::Max(MaxLOD, (int32)Mesh.LODIndex);
						}
					}
					LODToRender.SetLOD(MaxLOD);
				}

				FMeshDrawCommandPrimitiveIdInfo IdInfo(PrimitiveSceneInfo->GetIndex(), PrimitiveSceneInfo->GetInstanceSceneDataOffset());

				for (int32 MeshIndex = 0; MeshIndex < PrimitiveSceneInfo->StaticMeshRelevances.Num(); MeshIndex++)
				{
					const FStaticMeshBatchRelevance& StaticMeshRelevance = PrimitiveSceneInfo->StaticMeshRelevances[MeshIndex];
					const FStaticMeshBatch& StaticMesh = PrimitiveSceneInfo->StaticMeshes[MeshIndex];

					if (StaticMeshRelevance.bUseForMaterial && LODToRender.ContainsLOD(StaticMeshRelevance.LODIndex))
					{
						const int32 StaticMeshCommandInfoIndex = StaticMeshRelevance.GetStaticMeshCommandInfoIndex(MeshPass);
						if (StaticMeshCommandInfoIndex >= 0)
						{
							const FCachedMeshDrawCommandInfo& CachedMeshDrawCommand = PrimitiveSceneInfo->StaticMeshCommandInfos[StaticMeshCommandInfoIndex];
							const FCachedPassMeshDrawList& SceneDrawList = Scene->CachedDrawLists[MeshPass];

							const FMeshDrawCommand* MeshDrawCommand = nullptr;
							if (CachedMeshDrawCommand.StateBucketId >= 0)
							{
								MeshDrawCommand = &Scene->CachedMeshDrawCommandStateBuckets[MeshPass].GetByElementId(CachedMeshDrawCommand.StateBucketId).Key;
							}
							else
							{
								MeshDrawCommand = &SceneDrawList.MeshDrawCommands[CachedMeshDrawCommand.CommandIndex];
							}

							const uint32* InstanceRunArray = nullptr;
							uint32 NumInstanceRuns = 0;

							if (MeshDrawCommand->NumInstances > 1 && PrimitiveGroup.PrimitiveInstanceIndex >= 0)
							{
								// Render only a single specified instance, by specifying an inclusive [x;x] range

								ensure(CardPageRenderData.InstanceRuns.Num() + 2 <= CardPageRenderData.InstanceRuns.Max());
								InstanceRunArray = CardPageRenderData.InstanceRuns.GetData() + CardPageRenderData.InstanceRuns.Num();
								NumInstanceRuns = 1;

								CardPageRenderData.InstanceRuns.Add(PrimitiveGroup.PrimitiveInstanceIndex);
								CardPageRenderData.InstanceRuns.Add(PrimitiveGroup.PrimitiveInstanceIndex);
							}

							FVisibleMeshDrawCommand NewVisibleMeshDrawCommand;

							NewVisibleMeshDrawCommand.Setup(
								MeshDrawCommand,
								IdInfo,								
								CachedMeshDrawCommand.StateBucketId,
								CachedMeshDrawCommand.MeshFillMode,
								CachedMeshDrawCommand.MeshCullMode,
								CachedMeshDrawCommand.Flags,
								CachedMeshDrawCommand.SortKey,
								InstanceRunArray,
								NumInstanceRuns);

							VisibleMeshCommands.Add(NewVisibleMeshDrawCommand);
							PrimitiveIds.Add(PrimitiveSceneInfo->GetIndex());
						}
					}
				}
			}
		}
	}
}

struct FMeshCardsAdd
{
	int32 PrimitiveGroupIndex;
	float DistanceSquared;
};

struct FMeshCardsRemove
{
	int32 PrimitiveGroupIndex;
};

struct FCardAllocationOutput
{
	bool bVisible = false;
	int32 ResLevel = -1;
};

// Loop over Lumen primitives and output FMeshCards adds and removes
//通过与 Camera 的距离和 World Bounding 的 Extent 估算 MeshCards 的分辨率，如果没有超过最大更新距离并且没有太小，则增加 MeshCardsAdds 列表，否则增加 MeshCardsRemoves 列表。
struct FLumenSurfaceCacheUpdatePrimitivesTask
{
public:
	FLumenSurfaceCacheUpdatePrimitivesTask(
		const TSparseSpanArray<FLumenPrimitiveGroup>& InPrimitiveGroups,
		const TArray<FVector, TInlineAllocator<2>>& InViewOrigins,
		float InLumenSceneDetail,
		float InMaxDistanceFromCamera,
		int32 InFirstPrimitiveGroupIndex,
		int32 InNumPrimitiveGroupsPerPacket)
		: PrimitiveGroups(InPrimitiveGroups)
		, ViewOrigins(InViewOrigins)
		, FirstPrimitiveGroupIndex(InFirstPrimitiveGroupIndex)
		, NumPrimitiveGroupsPerPacket(InNumPrimitiveGroupsPerPacket)
		, LumenSceneDetail(InLumenSceneDetail)
		, MaxDistanceFromCamera(InMaxDistanceFromCamera)
		, TexelDensityScale(GetCardCameraDistanceTexelDensityScale())
	{
	}

	// Output
	TArray<FMeshCardsAdd> MeshCardsAdds;
	TArray<FMeshCardsRemove> MeshCardsRemoves;
	TArray<FPrimitiveSceneInfo*> LandscapePrimitivesInRange;

	void AnyThreadTask()
	{
		const int32 MinCardResolution = FMath::Clamp(FMath::RoundToInt(LumenSurfaceCache::GetMinCardResolution() / LumenSceneDetail), 1, 1024);
		const int32 LastPrimitiveGroupIndex = FMath::Min(FirstPrimitiveGroupIndex + NumPrimitiveGroupsPerPacket, PrimitiveGroups.Num());

		for (int32 PrimitiveGroupIndex = FirstPrimitiveGroupIndex; PrimitiveGroupIndex < LastPrimitiveGroupIndex; ++PrimitiveGroupIndex)
		{
			if (PrimitiveGroups.IsAllocated(PrimitiveGroupIndex))
			{
				const FLumenPrimitiveGroup& PrimitiveGroup = PrimitiveGroups[PrimitiveGroupIndex];

				// Rough card min resolution test
				float CardMaxDistanceSq = MaxDistanceFromCamera * MaxDistanceFromCamera;
				float DistanceSquared = FLT_MAX;

				for (FVector ViewOrigin : ViewOrigins)
				{
					DistanceSquared = FMath::Min(DistanceSquared, ComputeSquaredDistanceFromBoxToPoint(FVector(PrimitiveGroup.WorldSpaceBoundingBox.Min), FVector(PrimitiveGroup.WorldSpaceBoundingBox.Max), ViewOrigin));
				}

				const float MaxCardExtent = PrimitiveGroup.WorldSpaceBoundingBox.GetExtent().GetMax();
				float MaxCardResolution = (TexelDensityScale * MaxCardExtent) / FMath::Sqrt(FMath::Max(DistanceSquared, 1.0f)) + 0.01f;

				// Far field cards have constant resolution over entire range
				if (PrimitiveGroup.bFarField)
				{
					CardMaxDistanceSq = GLumenSceneFarFieldDistance * GLumenSceneFarFieldDistance;
					MaxCardResolution = MaxCardExtent * GLumenSceneFarFieldTexelDensity;
				}

				if (DistanceSquared <= CardMaxDistanceSq && MaxCardResolution >= (PrimitiveGroup.bEmissiveLightSource ? 1.0f : MinCardResolution))
				{
					if (PrimitiveGroup.MeshCardsIndex == -1 && PrimitiveGroup.bValidMeshCards)
					{
						FMeshCardsAdd Add;
						Add.PrimitiveGroupIndex = PrimitiveGroupIndex;
						Add.DistanceSquared = DistanceSquared;
						MeshCardsAdds.Add(Add);
					}

					if (PrimitiveGroup.bHeightfield)
					{
						LandscapePrimitivesInRange.Append(PrimitiveGroup.Primitives);
					}
				}
				else if (PrimitiveGroup.MeshCardsIndex >= 0)
				{
					FMeshCardsRemove Remove;
					Remove.PrimitiveGroupIndex = PrimitiveGroupIndex;
					MeshCardsRemoves.Add(Remove);
				}
			}
		}
	}

	const TSparseSpanArray<FLumenPrimitiveGroup>& PrimitiveGroups;
	TArray<FVector, TInlineAllocator<2>> ViewOrigins;
	int32 FirstPrimitiveGroupIndex;
	int32 NumPrimitiveGroupsPerPacket;
	float LumenSceneDetail;
	float MaxDistanceFromCamera;
	float TexelDensityScale;
};

struct FSurfaceCacheRemove
{
public:
	int32 LumenCardIndex;
};

// Loop over Lumen mesh cards and output card updates
//断每个 LumenCard 的可见性，如果可见，则生成分配请求 FSurfaceCacheRequest
//更重要的是计算 Mipmap Level，这是执行 Surface Cache 的 Page 分配的关键
struct FLumenSurfaceCacheUpdateMeshCardsTask
{
public:
	FLumenSurfaceCacheUpdateMeshCardsTask(
		const TSparseSpanArray<FLumenMeshCards>& InLumenMeshCards,
		const TSparseSpanArray<FLumenCard>& InLumenCards,
		const TArray<FVector, TInlineAllocator<2>>& InViewOrigins,
		float InLumenSceneDetail,
		float InMaxDistanceFromCamera,
		int32 InFirstMeshCardsIndex,
		int32 InNumMeshCardsPerPacket)
		: LumenMeshCards(InLumenMeshCards)
		, LumenCards(InLumenCards)
		, ViewOrigins(InViewOrigins)
		, LumenSceneDetail(InLumenSceneDetail)
		, FirstMeshCardsIndex(InFirstMeshCardsIndex)
		, NumMeshCardsPerPacket(InNumMeshCardsPerPacket)
		, MaxDistanceFromCamera(InMaxDistanceFromCamera)
		, TexelDensityScale(GetCardCameraDistanceTexelDensityScale())
		, MaxTexelDensity(GLumenSceneCardMaxTexelDensity)
	{
	}

	// Output
	TArray<FSurfaceCacheRequest> SurfaceCacheRequests;
	TArray<int32> CardsToHide;

	void AnyThreadTask()
	{
		const int32 LastLumenMeshCardsIndex = FMath::Min(FirstMeshCardsIndex + NumMeshCardsPerPacket, LumenMeshCards.Num());
		const int32 MinCardResolution = FMath::Clamp(FMath::RoundToInt(LumenSurfaceCache::GetMinCardResolution() / LumenSceneDetail), 1, 1024);

		for (int32 MeshCardsIndex = FirstMeshCardsIndex; MeshCardsIndex < LastLumenMeshCardsIndex; ++MeshCardsIndex)
		{
			if (LumenMeshCards.IsAllocated(MeshCardsIndex))
			{
				const FLumenMeshCards& MeshCardsInstance = LumenMeshCards[MeshCardsIndex];

				for (uint32 CardIndex = MeshCardsInstance.FirstCardIndex; CardIndex < MeshCardsInstance.FirstCardIndex + MeshCardsInstance.NumCards; ++CardIndex)
				{
					const FLumenCard& LumenCard = LumenCards[CardIndex];

					float CardMaxDistance = MaxDistanceFromCamera;
					float ViewerDistance = FLT_MAX;

					for (FVector ViewOrigin : ViewOrigins)
					{
						//1. 计算 Card 对应的 Card 的 World OBB 到视点的距离，最大不超过 100。
						ViewerDistance = FMath::Min(ViewerDistance, FMath::Max(FMath::Sqrt(LumenCard.WorldOBB.ComputeSquaredDistanceToPoint((FVector3f)ViewOrigin)), 100.0f));
					}

					// Compute resolution based on its largest extent
					//2. 取 WorldOBB XY 平面（水平平面，UE Z 轴朝上）的最大值。
					float MaxExtent = FMath::Max(LumenCard.WorldOBB.Extent.X, LumenCard.WorldOBB.Extent.Y);
					//3. 根据前两步的结果和 Card 的 ResolutionScale 计算最大投影大小 MaxProjectedSize。
					float MaxProjectedSize = FMath::Min(TexelDensityScale * MaxExtent * LumenCard.ResolutionScale / ViewerDistance, GLumenSceneCardMaxTexelDensity * MaxExtent);

					// Far field cards have constant resolution over entire range
					if (MeshCardsInstance.bFarField)
					{
						CardMaxDistance = GLumenSceneFarFieldDistance;
						MaxProjectedSize = GLumenSceneFarFieldTexelDensity * MaxExtent * LumenCard.ResolutionScale;
					}

					if (GLumenSceneCardFixedDebugResolution > 0)
					{
						MaxProjectedSize = GLumenSceneCardFixedDebugResolution;
					}

					const int32 MinCardResolutionForMeshCards = MeshCardsInstance.bEmissiveLightSource ? 1 : MinCardResolution;
					//4. 计算 2 的 MaxProjectedSize 次幂得到最大的分辨率 MaxSnappedRes
					const int32 MaxSnappedRes = FMath::RoundUpToPowerOfTwo(FMath::Min(FMath::TruncToInt(MaxProjectedSize), GetCardMaxResolution()));
					//5. 如果 Card 在最大视野范围内且 MaxSnappedRes 大于等于最小分辨率，则认为 Card 可见。
					const bool bVisible = ViewerDistance < CardMaxDistance && MaxSnappedRes >= MinCardResolutionForMeshCards;
					//6. 最后保证 MaxSnappedRes 不小于 Card 的最小分辨率（8，见 MinCardResolution ），计算 2 的幂次得到最终的 Mipmap Level。
					const int32 ResLevel = FMath::FloorLog2(FMath::Max<uint32>(MaxSnappedRes, Lumen::MinCardResolution));

					if (!bVisible && LumenCard.bVisible)
					{
						CardsToHide.Add(CardIndex);
					}
					else if (bVisible && ResLevel != LumenCard.DesiredLockedResLevel)
					{
						float Distance = ViewerDistance;

						if (LumenCard.bVisible && LumenCard.DesiredLockedResLevel != ResLevel)
						{
							// Make reallocation less important than capturing new cards
							const float ResLevelDelta = FMath::Abs((int32)LumenCard.DesiredLockedResLevel - ResLevel);
							Distance += (1.0f - FMath::Clamp((ResLevelDelta + 1.0f) / 3.0f, 0.0f, 1.0f)) * 2500.0f;
						}

						FSurfaceCacheRequest Request;
						Request.ResLevel = ResLevel;
						Request.CardIndex = CardIndex;
						Request.LocalPageIndex = UINT16_MAX;
						Request.Distance = Distance;
						SurfaceCacheRequests.Add(Request);

						ensure(Request.IsLockedMip());
					}
				}
			}
		}
	}

	const TSparseSpanArray<FLumenMeshCards>& LumenMeshCards;
	const TSparseSpanArray<FLumenCard>& LumenCards;
	TArray<FVector, TInlineAllocator<2>> ViewOrigins;
	float LumenSceneDetail;
	int32 FirstMeshCardsIndex;
	int32 NumMeshCardsPerPacket;
	float MaxDistanceFromCamera;
	float TexelDensityScale;
	float MaxTexelDensity;
};

float ComputeMaxCardUpdateDistanceFromCamera(float LumenSceneViewDistance)
{
	float MaxCardDistanceFromCamera = 0.0f;
	
	// Limit to voxel clipmap range
	extern int32 GLumenSceneClipmapResolution;
	if (GetNumLumenVoxelClipmaps(LumenSceneViewDistance) > 0 && GLumenSceneClipmapResolution > 0)
	{
		const float LastClipmapExtent = Lumen::GetFirstClipmapWorldExtent() * (float)(1 << (GetNumLumenVoxelClipmaps(LumenSceneViewDistance) - 1));
		MaxCardDistanceFromCamera = LastClipmapExtent;
	}

#if RHI_RAYTRACING
	// Limit to ray tracing culling radius if ray tracing is used
	if (Lumen::UseHardwareRayTracing() && GetRayTracingCulling() != 0)
	{
		MaxCardDistanceFromCamera = GetRayTracingCullingRadius();
	}
#endif

	return MaxCardDistanceFromCamera + GLumenSceneCardCaptureMargin;
}

/**
 * Make sure that all mesh rendering data is prepared before we render this primitive group
 * @return Will return true it primitive group is ready to render or we need to wait until next frame
 */
bool UpdateStaticMeshes(FLumenPrimitiveGroup& PrimitiveGroup)
{
	bool bReadyToRender = true;

	for (FPrimitiveSceneInfo* PrimitiveSceneInfo : PrimitiveGroup.Primitives)
	{
		if (PrimitiveSceneInfo && PrimitiveSceneInfo->Proxy->AffectsDynamicIndirectLighting())
		{
			if (PrimitiveSceneInfo->NeedsUniformBufferUpdate())
			{
				PrimitiveSceneInfo->UpdateUniformBuffer(FRHICommandListExecutor::GetImmediateCommandList());
			}

			if (PrimitiveSceneInfo->NeedsUpdateStaticMeshes())
			{
				// Need to defer to next InitViews, as main view visible primitives are processed on parallel tasks and calling 
				// CacheMeshDrawCommands may resize CachedDrawLists/CachedMeshDrawCommandStateBuckets causing a crash.
				PrimitiveSceneInfo->BeginDeferredUpdateStaticMeshesWithoutVisibilityCheck();
				bReadyToRender = false;
			}

			if (PrimitiveGroup.bHeightfield && PrimitiveSceneInfo->Proxy->HeightfieldHasPendingStreaming())
			{
				bReadyToRender = false;
			}
		}
	}

	return bReadyToRender;
}

/**
 * Process a throttled number of Lumen surface cache add requests
 * It will make virtual and physical allocations, and evict old pages as required
 */
void FLumenSceneData::ProcessLumenSurfaceCacheRequests(
	const FViewInfo& MainView,
	float MaxCardUpdateDistanceFromCamera,
	int32 MaxTileCapturesPerFrame,
	FLumenCardRenderer& LumenCardRenderer,
	FRHIGPUMask GPUMask,
	const TArray<FSurfaceCacheRequest, SceneRenderingAllocator>& SurfaceCacheRequests)
{
	QUICK_SCOPE_CYCLE_COUNTER(ProcessLumenSurfaceCacheRequests);

	TArray<FCardPageRenderData, SceneRenderingAllocator>& CardPagesToRender = LumenCardRenderer.CardPagesToRender;

	TArray<FVirtualPageIndex, SceneRenderingAllocator> HiResPagesToMap;
	TSparseUniqueList<int32, SceneRenderingAllocator> DirtyCards;

	FLumenSurfaceCacheAllocator CaptureAtlasAllocator;
	//1、根据 Capture Atlas 大小初始化 Capture Atlas Allocator
	CaptureAtlasAllocator.Init(GetCardCaptureAtlasSizeInPages());

	for (int32 RequestIndex = 0; RequestIndex < SurfaceCacheRequests.Num(); ++RequestIndex)
	{
		const FSurfaceCacheRequest& Request = SurfaceCacheRequests[RequestIndex];

		if (Request.IsLockedMip())
		{
			// Update low-res locked (always resident) pages
			//执行常驻的低分辨率 Page 分配
			FLumenCard& Card = Cards[Request.CardIndex];

			//2、遍历每个 SurfaceCacheRequests，获取 Request，根据 Reguest 的 CardIndex 获取 LumenCard 并判断请求的分辨率级别是否一致，如果不一致则继续。
			if (Card.DesiredLockedResLevel != Request.ResLevel)
			{
				// Check if we can make this allocation at all
				bool bCanAlloc = true;

				uint8 NewLockedAllocationResLevel = Request.ResLevel;
				//3. 判断 Surface Atlas（通过 SurfaceCacheAllocator）是否有可分配空间，如果没有则换出最旧的空间
				while (!IsPhysicalSpaceAvailable(Card, NewLockedAllocationResLevel, /*bSinglePage*/ false))
				{
					const int32 MaxFramesSinceLastUsed = 2;

					if (!EvictOldestAllocation(/*MaxFramesSinceLastUsed*/ MaxFramesSinceLastUsed, DirtyCards))
					{
						bCanAlloc = false;
						break;
					}
				}

				// Try to decrease resolution if allocation still can't be made
				//4. 继续判断是否可分配，如果还没有空间，则尝试降低请求的目标分辨率级别，判断是否可分配
				while (!bCanAlloc && NewLockedAllocationResLevel > Lumen::MinResLevel)
				{
					--NewLockedAllocationResLevel;
					bCanAlloc = IsPhysicalSpaceAvailable(Card, NewLockedAllocationResLevel, /*bSinglePage*/ false);
				}

				// Can we fit this card into the temporary card capture allocator?
				//5、判断 Capture Atlas 是否可分配：
				if (!CaptureAtlasAllocator.IsSpaceAvailable(Card, NewLockedAllocationResLevel, /*bSinglePage*/ false))
				{
					bCanAlloc = false;
				}

				const FLumenMeshCards& MeshCardsElement = MeshCards[Card.MeshCardsIndex];
				//6、如果可分配，获取 Card 对应的 LumenMeshCards，并确保 LumenMeshCards 对应的 StaticMesh 数据都已准备好，
				//如果是 Landscape Mesh，则需要在第一次准备好时使 Voxel Vis-Buffer 失效
				if (bCanAlloc && UpdateStaticMeshes(PrimitiveGroups[MeshCardsElement.PrimitiveGroupIndex]))
				{
					// Landscape traces card representation, so need to invalidate voxel vis buffer when it's ready for the first time
					if (MeshCardsElement.bHeightfield && Card.DesiredLockedResLevel == 0)
					{
						PrimitiveModifiedBounds.Add(MeshCardsElement.GetWorldSpaceBounds());
					}

					Card.bVisible = true;
					Card.DesiredLockedResLevel = Request.ResLevel;

					const bool bResampleLastLighting = Card.IsAllocated();

					// Free previous MinAllocatedResLevel
					//7、释放 Card 之前已分配的对应的小于等于最小分配大小的 Surface Cache：
					FreeVirtualSurface(Card, Card.MinAllocatedResLevel, Card.MinAllocatedResLevel);

					// Free anything lower res than the new res level
					FreeVirtualSurface(Card, Card.MinAllocatedResLevel, NewLockedAllocationResLevel - 1);


					const bool bLockPages = true;
					//8、调用 ReallocVirtualSurface 执行重新分配：
					ReallocVirtualSurface(Card, Request.CardIndex, NewLockedAllocationResLevel, bLockPages);

					// Map and update all pages
					//9、获取 Card 对应的 Mipmap，遍历 Mipmap 对应的所有 FLumenPageTableEntry，
					//如果当前 page 没有映射过，则将 Page 映射到 Card Atlas 上，同时分配 Capture Atlas 空间，最后将这些数据加入到 LumenCardRenderer 的 CardPagesToRender 列表中
					FLumenSurfaceMipMap& MipMap = Card.GetMipMap(Card.MinAllocatedResLevel);
					for (int32 LocalPageIndex = 0; LocalPageIndex < MipMap.SizeInPagesX * MipMap.SizeInPagesY; ++LocalPageIndex)
					{
						const int32 PageIndex = MipMap.GetPageTableIndex(LocalPageIndex);
						FLumenPageTableEntry& PageTableEntry = GetPageTableEntry(PageIndex);

						if (!PageTableEntry.IsMapped())
						{
							MapSurfaceCachePage(MipMap, PageIndex, GPUMask);
							check(PageTableEntry.IsMapped());

							// Allocate space in temporary allocation atlas
							FLumenSurfaceCacheAllocator::FAllocation CardCaptureAllocation;
							CaptureAtlasAllocator.Allocate(PageTableEntry, CardCaptureAllocation);
							check(CardCaptureAllocation.PhysicalPageCoord.X >= 0);

							CardPagesToRender.Add(FCardPageRenderData(
								MainView,
								Card,
								PageTableEntry.CardUVRect,
								CardCaptureAllocation.PhysicalAtlasRect,
								PageTableEntry.PhysicalAtlasRect,
								MeshCardsElement.PrimitiveGroupIndex,
								Request.CardIndex,
								PageIndex,
								bResampleLastLighting));

							for (uint32 GPUIndex : GPUMask)
							{
								LastCapturedPageHeap[GPUIndex].Update(GetSurfaceCacheUpdateFrameIndex(), PageIndex);
							}
							LumenCardRenderer.NumCardTexelsToCapture += PageTableEntry.PhysicalAtlasRect.Area();
						}
					}

					DirtyCards.Add(Request.CardIndex);
				}
			}
		}
		else
		{
			// Hi-Res
			if (Cards.IsAllocated(Request.CardIndex))
			{
				FLumenCard& Card = Cards[Request.CardIndex];

				if (Card.bVisible && Card.MinAllocatedResLevel >= 0 && Request.ResLevel > Card.MinAllocatedResLevel)
				{
					HiResPagesToMap.Add(FVirtualPageIndex(Request.CardIndex, Request.ResLevel, Request.LocalPageIndex));
				}
			}
		}

		if (CardPagesToRender.Num() + HiResPagesToMap.Num() >= MaxTileCapturesPerFrame)
		{
			break;
		}
	}

	// Process hi-res optional pages after locked low res ones are done
	for (const FVirtualPageIndex& VirtualPageIndex : HiResPagesToMap)
	{
		FLumenCard& Card = Cards[VirtualPageIndex.CardIndex];

		if (VirtualPageIndex.ResLevel > Card.MinAllocatedResLevel)
		{
			// Make room for new physical allocations
			bool bCanAlloc = true;
			while (!IsPhysicalSpaceAvailable(Card, VirtualPageIndex.ResLevel, /*bSinglePage*/ true))
			{
				// Don't want to evict pages which may be picked up a jittering tile feedback
				const int32 MaxFramesSinceLastUsed = Lumen::GetFeedbackBufferTileSize() * Lumen::GetFeedbackBufferTileSize();

				if (!EvictOldestAllocation(MaxFramesSinceLastUsed, DirtyCards))
				{
					bCanAlloc = false;
					break;
				}
			}

			// Can we fit this card into the temporary card capture allocator?
			if (!CaptureAtlasAllocator.IsSpaceAvailable(Card, VirtualPageIndex.ResLevel, /*bSinglePage*/ true))
			{
				bCanAlloc = false;
			}

			const FLumenMeshCards& MeshCardsElement = MeshCards[Card.MeshCardsIndex];
			if (bCanAlloc && UpdateStaticMeshes(PrimitiveGroups[MeshCardsElement.PrimitiveGroupIndex]))
			{
				const bool bLockPages = false;
				const bool bResampleLastLighting = Card.IsAllocated();

				ReallocVirtualSurface(Card, VirtualPageIndex.CardIndex, VirtualPageIndex.ResLevel, bLockPages);

				FLumenSurfaceMipMap& MipMap = Card.GetMipMap(VirtualPageIndex.ResLevel);
				const int32 PageIndex = MipMap.GetPageTableIndex(VirtualPageIndex.LocalPageIndex);
				FLumenPageTableEntry& PageTableEntry = GetPageTableEntry(PageIndex);

				if (!PageTableEntry.IsMapped())
				{
					MapSurfaceCachePage(MipMap, PageIndex, GPUMask);
					check(PageTableEntry.IsMapped());

					// Allocate space in temporary allocation atlas
					FLumenSurfaceCacheAllocator::FAllocation CardCaptureAllocation;
					CaptureAtlasAllocator.Allocate(PageTableEntry, CardCaptureAllocation);
					check(CardCaptureAllocation.PhysicalPageCoord.X >= 0);

					CardPagesToRender.Add(FCardPageRenderData(
						MainView,
						Card,
						PageTableEntry.CardUVRect,
						CardCaptureAllocation.PhysicalAtlasRect,
						PageTableEntry.PhysicalAtlasRect,
						MeshCardsElement.PrimitiveGroupIndex,
						VirtualPageIndex.CardIndex,
						PageIndex,
						bResampleLastLighting));

					for (uint32 GPUIndex : GPUMask)
					{
						LastCapturedPageHeap[GPUIndex].Update(GetSurfaceCacheUpdateFrameIndex(), PageIndex);
					}
					LumenCardRenderer.NumCardTexelsToCapture += PageTableEntry.PhysicalAtlasRect.Area();
					DirtyCards.Add(VirtualPageIndex.CardIndex);
				}
			}
		}
	}

	// Finally process card refresh to capture any material updates, or render cards that need to be initialized for the first time on
	// a given GPU in multi-GPU scenarios.  Uninitialized cards on a particular GPU will have a zero captured frame index set when the
	// card was allocated.  A zero frame index otherwise can't occur on a card, because the constructor sets SurfaceCacheUpdateFrameIndex
	// to 1, and IncrementSurfaceCacheUpdateFrameIndex skips over zero if it happens to wrap around.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SceneCardCaptureRefresh);

		int32 NumTexelsLeftToRefresh = GetCardCaptureRefreshNumTexels();
		int32 NumPagesLeftToRefesh = FMath::Min<int32>((int32)GetCardCaptureRefreshNumPages(), MaxTileCapturesPerFrame - CardPagesToRender.Num());

		FBinaryHeap<uint32,uint32>& PageHeap = LastCapturedPageHeap[GPUMask.GetFirstIndex()];

		bool bCanCapture = true;
		while (PageHeap.Num() > 0 && bCanCapture)
		{
			bCanCapture = false;

			const uint32 PageTableIndex = PageHeap.Top();
			const uint32 CapturedSurfaceCacheFrameIndex = PageHeap.GetKey(PageTableIndex);

			const int32 FramesSinceLastUpdated = GetSurfaceCacheUpdateFrameIndex() - CapturedSurfaceCacheFrameIndex;
			if (FramesSinceLastUpdated > 0)
			{
				FLumenPageTableEntry& PageTableEntry = GetPageTableEntry(PageTableIndex);
				const FLumenCard& Card = Cards[PageTableEntry.CardIndex];
				const FLumenMeshCards& MeshCardsElement = MeshCards[Card.MeshCardsIndex];

#if WITH_MGPU
				// Limit number of re-captured texels and pages per frame, except always allow captures of uninitialized
				// cards where the captured frame index is zero (don't count them against the throttled limits).
				// Uninitialized cards on a particular GPU will always be at the front of the heap, due to the zero index,
				// so even if the limits are set to zero, we'll still process them if needed (the limit comparisons below
				// are >= 0, and will pass if nothing has been decremented from the limits yet).
				if ((CapturedSurfaceCacheFrameIndex != 0) || (GNumExplicitGPUsForRendering == 1))
#endif
				{
					FLumenMipMapDesc MipMapDesc;
					Card.GetMipMapDesc(PageTableEntry.ResLevel, MipMapDesc);
					NumTexelsLeftToRefresh -= MipMapDesc.PageResolution.X * MipMapDesc.PageResolution.Y;
					NumPagesLeftToRefesh -= 1;
				}

				if (NumTexelsLeftToRefresh >= 0 && NumPagesLeftToRefesh >= 0)
				{
					// Can we fit this card into the temporary card capture allocator?
					if (CaptureAtlasAllocator.IsSpaceAvailable(Card, PageTableEntry.ResLevel, /*bSinglePage*/ true))
					{
						// Allocate space in temporary allocation atlas
						FLumenSurfaceCacheAllocator::FAllocation CardCaptureAllocation;
						CaptureAtlasAllocator.Allocate(PageTableEntry, CardCaptureAllocation);
						check(CardCaptureAllocation.PhysicalPageCoord.X >= 0);

						CardPagesToRender.Add(FCardPageRenderData(
							MainView,
							Card,
							PageTableEntry.CardUVRect,
							CardCaptureAllocation.PhysicalAtlasRect,
							PageTableEntry.PhysicalAtlasRect,
							MeshCardsElement.PrimitiveGroupIndex,
							PageTableEntry.CardIndex,
							PageTableIndex,
							/*bResampleLastLighting*/ true));

						for (uint32 GPUIndex : GPUMask)
						{
							LastCapturedPageHeap[GPUIndex].Update(GetSurfaceCacheUpdateFrameIndex(), PageTableIndex);
						}
						LumenCardRenderer.NumCardTexelsToCapture += PageTableEntry.PhysicalAtlasRect.Area();
						bCanCapture = true;
					}
				}
			}
		}
	}

	// Evict pages which weren't used recently
	if (!Lumen::IsSurfaceCacheFrozen())
	{
		uint32 MaxFramesSinceLastUsed = FMath::Max(GSurfaceCacheNumFramesToKeepUnusedPages, 0);
		while (EvictOldestAllocation(MaxFramesSinceLastUsed, DirtyCards))
		{
		}
	}

	for (int32 CardIndex : DirtyCards.Array)
	{
		FLumenCard& Card = Cards[CardIndex];
		//更新 Card Mipmap 层次结构，此函数作用有 2 个:
		//1、去除未分配的 Miplevel
		//2、未 Map 的 Mipmap 重用低分辨率的 Mipmap
		UpdateCardMipMapHierarchy(Card);
		CardIndicesToUpdateInBuffer.Add(CardIndex);
	}
}

//使用 UE 的 Task 系统并行处理所有的 PrimitiveGroups，由 FLumenSurfaceCacheUpdatePrimitivesTask 实现处理逻辑
void UpdateSurfaceCachePrimitives(
	FLumenSceneData& LumenSceneData,
	const TArray<FVector, TInlineAllocator<2>>& LumenSceneCameraOrigins,
	float LumenSceneDetail,
	float MaxCardUpdateDistanceFromCamera,
	FLumenCardRenderer& LumenCardRenderer)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateSurfaceCachePrimitives);

	const int32 NumPrimitivesPerTask = FMath::Max(GLumenScenePrimitivesPerTask, 1);
	const int32 NumTasks = FMath::DivideAndRoundUp(LumenSceneData.PrimitiveGroups.Num(), GLumenScenePrimitivesPerTask);

	TArray<FLumenSurfaceCacheUpdatePrimitivesTask, SceneRenderingAllocator> Tasks;
	Tasks.Reserve(NumTasks);

	for (int32 TaskIndex = 0; TaskIndex < NumTasks; ++TaskIndex)
	{
		Tasks.Emplace(
			LumenSceneData.PrimitiveGroups,
			LumenSceneCameraOrigins,
			LumenSceneDetail,
			MaxCardUpdateDistanceFromCamera,
			TaskIndex * NumPrimitivesPerTask,
			NumPrimitivesPerTask);
	}

	const bool bExecuteInParallel = FApp::ShouldUseThreadingForPerformance() && GLumenSceneParallelUpdate != 0;

	ParallelFor(Tasks.Num(),
		[&Tasks](int32 Index)
		{
			Tasks[Index].AnyThreadTask();
		},
		!bExecuteInParallel);

	TArray<FMeshCardsAdd, SceneRenderingAllocator> MeshCardsAdds;

	for (int32 TaskIndex = 0; TaskIndex < Tasks.Num(); ++TaskIndex)
	{
		const FLumenSurfaceCacheUpdatePrimitivesTask& Task = Tasks[TaskIndex];
		LumenSceneData.NumMeshCardsToAdd += Task.MeshCardsAdds.Num();

		// Append requests to the global array
		{
			MeshCardsAdds.Reserve(MeshCardsAdds.Num() + Task.MeshCardsAdds.Num());

			for (int32 RequestIndex = 0; RequestIndex < Task.MeshCardsAdds.Num(); ++RequestIndex)
			{
				MeshCardsAdds.Add(Task.MeshCardsAdds[RequestIndex]);
			}
		}

		// MeshCardsRemoves 列表调用 RemoveMeshCards 函数删除 MeshCards 以及 MeshCards 包含的 Card 对象，同样这里最为关键的是释放 Surface Cache
		for (const FMeshCardsRemove& MeshCardsRemove : Task.MeshCardsRemoves)
		{
			FLumenPrimitiveGroup& PrimitiveGroup = LumenSceneData.PrimitiveGroups[MeshCardsRemove.PrimitiveGroupIndex];
			LumenSceneData.RemoveMeshCards(PrimitiveGroup);
		}

		LumenCardRenderer.LandscapePrimitivesInRange.Append(Task.LandscapePrimitivesInRange);
	}

	if (MeshCardsAdds.Num() > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SortAdds);

		struct FSortBySmallerDistance
		{
			FORCEINLINE bool operator()(const FMeshCardsAdd& A, const FMeshCardsAdd& B) const
			{
				return A.DistanceSquared < B.DistanceSquared;
			}
		};

		//将 MeshCardsAdds 列表汇总并按照 Camera 距离由近到远排序，
		MeshCardsAdds.Sort(FSortBySmallerDistance());
	}

	const int32 MeshCardsToAddPerFrame = GetMaxMeshCardsToAddPerFrame();
	
	//调用 AddMeshCards 函数增加 MeshCards，这里最终是调用 AddMeshCardsFromBuildData 函数根据 Offline 生成的 FMeshCardsBuildData 中创建，并且这里也会增加 Card。
	for (int32 MeshCardsIndex = 0; MeshCardsIndex < FMath::Min(MeshCardsAdds.Num(), MeshCardsToAddPerFrame); ++MeshCardsIndex)
	{
		const FMeshCardsAdd& MeshCardsAdd = MeshCardsAdds[MeshCardsIndex];
		LumenSceneData.AddMeshCards(MeshCardsAdd.PrimitiveGroupIndex);
	}
}

////生成 Surface Cache 分配请求
void UpdateSurfaceCacheMeshCards(
	FLumenSceneData& LumenSceneData,
	const TArray<FVector, TInlineAllocator<2>>& LumenSceneCameraOrigins,
	float LumenSceneDetail,
	float MaxCardUpdateDistanceFromCamera,
	TArray<FSurfaceCacheRequest, SceneRenderingAllocator>& SurfaceCacheRequests)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateMeshCards);

	const int32 NumMeshCardsPerTask = FMath::Max(GLumenSceneMeshCardsPerTask, 1);
	const int32 NumTasks = FMath::DivideAndRoundUp(LumenSceneData.MeshCards.Num(), NumMeshCardsPerTask);

	TArray<FLumenSurfaceCacheUpdateMeshCardsTask, SceneRenderingAllocator> Tasks;
	Tasks.Reserve(NumTasks);

	for (int32 TaskIndex = 0; TaskIndex < NumTasks; ++TaskIndex)
	{
		Tasks.Emplace(
			LumenSceneData.MeshCards,
			LumenSceneData.Cards,
			LumenSceneCameraOrigins,
			LumenSceneDetail,
			MaxCardUpdateDistanceFromCamera,
			TaskIndex * NumMeshCardsPerTask,
			NumMeshCardsPerTask);
	}

	const bool bExecuteInParallel = FApp::ShouldUseThreadingForPerformance() && GLumenSceneParallelUpdate != 0;

	ParallelFor(Tasks.Num(),
		[&Tasks](int32 Index)
		{
			Tasks[Index].AnyThreadTask();
		},
		!bExecuteInParallel);

	for (int32 TaskIndex = 0; TaskIndex < Tasks.Num(); ++TaskIndex)
	{
		const FLumenSurfaceCacheUpdateMeshCardsTask& Task = Tasks[TaskIndex];
		LumenSceneData.NumLockedCardsToUpdate += Task.SurfaceCacheRequests.Num();

		// Append requests to the global array
		{
			SurfaceCacheRequests.Reserve(SurfaceCacheRequests.Num() + Task.SurfaceCacheRequests.Num());

			for (int32 RequestIndex = 0; RequestIndex < Task.SurfaceCacheRequests.Num(); ++RequestIndex)
			{
				SurfaceCacheRequests.Add(Task.SurfaceCacheRequests[RequestIndex]);
			}
		}

		for (int32 CardIndex : Task.CardsToHide)
		{
			FLumenCard& Card = LumenSceneData.Cards[CardIndex];

			if (Card.bVisible)
			{
				LumenSceneData.RemoveCardFromAtlas(CardIndex);
				Card.bVisible = false;
			}
		}
	}

	LumenSceneData.UpdateSurfaceCacheFeedback(LumenSceneCameraOrigins, SurfaceCacheRequests);

	if (SurfaceCacheRequests.Num() > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SortRequests);

		struct FSortBySmallerDistance
		{
			FORCEINLINE bool operator()(const FSurfaceCacheRequest& A, const FSurfaceCacheRequest& B) const
			{
				return A.Distance < B.Distance;
			}
		};

		// FSurfaceCacheRequest 列表按照 Camera 距离由近到远排序
		SurfaceCacheRequests.Sort(FSortBySmallerDistance());
	}
}

extern void UpdateLumenScenePrimitives(FScene* Scene);

void AllocateResampledCardCaptureAtlas(FRDGBuilder& GraphBuilder, FIntPoint CardCaptureAtlasSize, FResampledCardCaptureAtlas& CardCaptureAtlas)
{
	CardCaptureAtlas.Size = CardCaptureAtlasSize;

	CardCaptureAtlas.DirectLighting = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			CardCaptureAtlasSize,
			Lumen::GetDirectLightingAtlasFormat(),
			FClearValueBinding::Green,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear),
		TEXT("Lumen.ResampledCardCaptureDirectLighting"));

	CardCaptureAtlas.IndirectLighting = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			CardCaptureAtlasSize,
			Lumen::GetIndirectLightingAtlasFormat(),
			FClearValueBinding::Green,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear),
		TEXT("Lumen.ResampledCardCaptureIndirectLighting"));

	CardCaptureAtlas.NumFramesAccumulated = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			CardCaptureAtlasSize,
			Lumen::GetNumFramesAccumulatedAtlasFormat(),
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear),
		TEXT("Lumen.ResampledCardCaptureNumFramesAccumulated"));
}

class FResampleLightingHistoryToCardCaptureAtlasPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FResampleLightingHistoryToCardCaptureAtlasPS);
	SHADER_USE_PARAMETER_STRUCT(FResampleLightingHistoryToCardCaptureAtlasPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OpacityAtlas)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DirectLightingAtlas)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IndirectLightingAtlas)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadiosityNumFramesAccumulatedAtlas)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, NewCardPageResampleData)
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FResampleLightingHistoryToCardCaptureAtlasPS, "/Engine/Private/Lumen/LumenSceneLighting.usf", "ResampleLightingHistoryToCardCaptureAtlasPS", SF_Pixel);

BEGIN_SHADER_PARAMETER_STRUCT(FResampleLightingHistoryToCardCaptureParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FPixelShaderUtils::FRasterizeToRectsVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FResampleLightingHistoryToCardCaptureAtlasPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void FLumenSceneData::CopyBuffersForResample(FRDGBuilder& GraphBuilder, FShaderResourceViewRHIRef& LastCardBufferForResampleSRV, FShaderResourceViewRHIRef& LastPageTableBufferForResampleSRV)
{
	if (LastPageTableBufferForResample.NumBytes != PageTableBuffer.NumBytes)
	{
		LastPageTableBufferForResample.Initialize(TEXT("Lumen.LastPageBufferForResample"), PageTableBuffer.NumBytes, BUF_Static);
	}

	{
		GraphBuilder.RHICmdList.Transition(FRHITransitionInfo(LastPageTableBufferForResample.UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

		FMemcpyResourceParams MemcpyParams;
		MemcpyParams.Count = PageTableBuffer.NumBytes / sizeof(uint32);
		MemcpyParams.SrcOffset = 0;
		MemcpyParams.DstOffset = 0;
		MemcpyResource(GraphBuilder.RHICmdList, LastPageTableBufferForResample, PageTableBuffer, MemcpyParams);
	}

	const int32 NumBytesPerElement = sizeof(FVector4f);

	if (LastCardBufferForResample.NumBytes != CardBuffer.NumBytes)
	{
		LastCardBufferForResample.Initialize(TEXT("Lumen.LastCardsForResample"), NumBytesPerElement, CardBuffer.NumBytes / NumBytesPerElement, BUF_Static);
	}
	else
	{
		GraphBuilder.RHICmdList.Transition(FRHITransitionInfo(LastCardBufferForResample.UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	}

	//@todo - copy just the needed cards, instead of the entire scene
	{
		FMemcpyResourceParams MemcpyParams;
		MemcpyParams.Count = CardBuffer.NumBytes / NumBytesPerElement;
		MemcpyParams.SrcOffset = 0;
		MemcpyParams.DstOffset = 0;
		MemcpyResource(GraphBuilder.RHICmdList, LastCardBufferForResample, CardBuffer, MemcpyParams);
	}

	FRHITransitionInfo Transitions[2] =
	{
		FRHITransitionInfo(LastPageTableBufferForResample.UAV, ERHIAccess::Unknown, ERHIAccess::SRVMask),
		FRHITransitionInfo(LastCardBufferForResample.UAV, ERHIAccess::Unknown, ERHIAccess::SRVMask)
	};
	GraphBuilder.RHICmdList.Transition(Transitions);

	LastCardBufferForResampleSRV = LastCardBufferForResample.SRV;
	LastPageTableBufferForResampleSRV = LastPageTableBufferForResample.SRV;
}

// Try to resample direct lighting and indirect lighting (radiosity) from existing surface cache to new captured cards
void ResampleLightingHistory(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FScene* Scene,
	const TArray<FCardPageRenderData, SceneRenderingAllocator>& CardPagesToRender,
	FLumenSceneData& LumenSceneData,
	FResampledCardCaptureAtlas& CardCaptureAtlas)
{
	if (GLumenSceneSurfaceCacheResampleLighting
		&& LumenSceneData.GetPageTableBufferSRV()
		&& LumenSceneData.CardBuffer.SRV)
	{
		AllocateResampledCardCaptureAtlas(GraphBuilder, LumenSceneData.GetCardCaptureAtlasSize(), CardCaptureAtlas);

		// Because LumenSceneData.UploadPageTable will not be deferred by RDG, we have to make a copy of the old buffers for our pass which will be deferred by RDG
		FShaderResourceViewRHIRef LastCardBufferForResampleSRV;
		FShaderResourceViewRHIRef LastPageTableBufferForResampleSRV;
		LumenSceneData.CopyBuffersForResample(GraphBuilder, LastCardBufferForResampleSRV, LastPageTableBufferForResampleSRV);

		FRDGUploadData<FUintVector4> CardCaptureRectArray(GraphBuilder, CardPagesToRender.Num());
		FRDGUploadData<FUintVector4> CardPageResampleDataArray(GraphBuilder, CardPagesToRender.Num() * 2);

		for (int32 Index = 0; Index < CardPagesToRender.Num(); Index++)
		{
			const FCardPageRenderData& CardPageRenderData = CardPagesToRender[Index];

			FUintVector4& Rect = CardCaptureRectArray[Index];
			Rect.X = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Min.X, 0);
			Rect.Y = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Min.Y, 0);
			Rect.Z = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Max.X, 0);
			Rect.W = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Max.Y, 0);

			FUintVector4& CardPageResampleData0 = CardPageResampleDataArray[Index * 2 + 0];
			FUintVector4& CardPageResampleData1 = CardPageResampleDataArray[Index * 2 + 1];

			CardPageResampleData0.X = CardPageRenderData.bResampleLastLighting ? CardPageRenderData.CardIndex : -1;
			CardPageResampleData1 = FUintVector4(
				*(const uint32*)&CardPageRenderData.CardUVRect.X,
				*(const uint32*)&CardPageRenderData.CardUVRect.Y,
				*(const uint32*)&CardPageRenderData.CardUVRect.Z,
				*(const uint32*)&CardPageRenderData.CardUVRect.W);
		}

		FRDGBufferRef CardCaptureRectBuffer = CreateUploadBuffer(GraphBuilder, TEXT("Lumen.CardCaptureRects"),
			sizeof(FUintVector4), FMath::RoundUpToPowerOfTwo(CardPagesToRender.Num()),
			CardCaptureRectArray);
		FRDGBufferSRVRef CardCaptureRectBufferSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CardCaptureRectBuffer, PF_R32G32B32A32_UINT));

		FRDGBufferRef NewCardPageResampleDataBuffer = CreateUploadBuffer(GraphBuilder, TEXT("Lumen.CardPageResampleDataBuffer"),
			sizeof(FUintVector4), FMath::RoundUpToPowerOfTwo(CardPagesToRender.Num() * 2),
			CardPageResampleDataArray);
		FRDGBufferSRVRef NewCardPageResampleDataSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NewCardPageResampleDataBuffer, PF_R32G32B32A32_UINT));

		{
			FResampleLightingHistoryToCardCaptureParameters* PassParameters = GraphBuilder.AllocParameters<FResampleLightingHistoryToCardCaptureParameters>();

			PassParameters->RenderTargets[0] = FRenderTargetBinding(CardCaptureAtlas.DirectLighting, ERenderTargetLoadAction::ENoAction);
			PassParameters->RenderTargets[1] = FRenderTargetBinding(CardCaptureAtlas.IndirectLighting, ERenderTargetLoadAction::ENoAction);
			PassParameters->RenderTargets[2] = FRenderTargetBinding(CardCaptureAtlas.NumFramesAccumulated, ERenderTargetLoadAction::ENoAction);

			PassParameters->PS.View = View.ViewUniformBuffer;

			{
				FLumenCardScene* LumenCardSceneParameters = GraphBuilder.AllocParameters<FLumenCardScene>();
				SetupLumenCardSceneParameters(GraphBuilder, Scene, *LumenCardSceneParameters);
				LumenCardSceneParameters->CardData = LastCardBufferForResampleSRV;
				LumenCardSceneParameters->PageTableBuffer = LastPageTableBufferForResampleSRV;
				PassParameters->PS.LumenCardScene = GraphBuilder.CreateUniformBuffer(LumenCardSceneParameters);
			}

			PassParameters->PS.OpacityAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.OpacityAtlas);
			PassParameters->PS.DirectLightingAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.DirectLightingAtlas);
			PassParameters->PS.IndirectLightingAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.IndirectLightingAtlas);
			PassParameters->PS.RadiosityNumFramesAccumulatedAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.RadiosityNumFramesAccumulatedAtlas);
			PassParameters->PS.NewCardPageResampleData = NewCardPageResampleDataSRV;

			FResampleLightingHistoryToCardCaptureAtlasPS::FPermutationDomain PermutationVector;
			auto PixelShader = View.ShaderMap->GetShader<FResampleLightingHistoryToCardCaptureAtlasPS>(PermutationVector);

			FPixelShaderUtils::AddRasterizeToRectsPass<FResampleLightingHistoryToCardCaptureAtlasPS>(
				GraphBuilder,
				View.ShaderMap,
				RDG_EVENT_NAME("ResampleLightingHistoryToCardCaptureAtlas"),
				PixelShader,
				PassParameters,
				CardCaptureAtlas.Size,
				CardCaptureRectBufferSRV,
				CardPagesToRender.Num(),
				TStaticBlendState<>::GetRHI(),
				TStaticRasterizerState<>::GetRHI(),
				TStaticDepthStencilState<false, CF_Always>::GetRHI());
		}
	}
}

void FDeferredShadingSceneRenderer::BeginUpdateLumenSceneTasks(FRDGBuilder& GraphBuilder)
{
	LLM_SCOPE_BYTAG(Lumen);

	const FViewInfo& View = Views[0];
	const bool bAnyLumenActive = ShouldRenderLumenDiffuseGI(Scene, View) || ShouldRenderLumenReflections(View);

	LumenCardRenderer.Reset();

	if (bAnyLumenActive
		&& !ViewFamily.EngineShowFlags.HitProxies)
	{
		SCOPED_NAMED_EVENT(FDeferredShadingSceneRenderer_BeginUpdateLumenSceneTasks, FColor::Emerald);
		QUICK_SCOPE_CYCLE_COUNTER(BeginUpdateLumenSceneTasks);

		FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;
		LumenSceneData.bDebugClearAllCachedState = GLumenSceneRecaptureLumenSceneEveryFrame != 0;
		const bool bReallocateAtlas = LumenSceneData.UpdateAtlasSize();

		// Surface cache reset for debugging
		if ((GLumenSceneSurfaceCacheReset != 0)
			|| (GLumenSceneSurfaceCacheResetEveryNthFrame > 0 && (View.Family->FrameNumber % (uint32)GLumenSceneSurfaceCacheResetEveryNthFrame == 0)))
		{
			LumenSceneData.bDebugClearAllCachedState = true;
			GLumenSceneSurfaceCacheReset = 0;
		}

		if (GLumenSceneForceEvictHiResPages != 0)
		{
			LumenSceneData.ForceEvictEntireCache();
			GLumenSceneForceEvictHiResPages = 0;
		}

		LumenSceneData.NumMeshCardsToAdd = 0;
		LumenSceneData.NumLockedCardsToUpdate = 0;
		LumenSceneData.NumHiResPagesToAdd = 0;

		//1.管理 FLumenPrimitiveGroup 对象，包括删除、创建、更新操作。
		// 更新Scene->LumenSceneData->
		UpdateLumenScenePrimitives(Scene);
		UpdateDistantScene(Scene, Views[0]);

		if (LumenSceneData.bDebugClearAllCachedState || bReallocateAtlas)
		{
			LumenSceneData.RemoveAllMeshCards();
		}

		TArray<FVector, TInlineAllocator<2>> LumenSceneCameraOrigins;

		LumenSceneCameraOrigins.Add(GetLumenSceneViewOrigin(View, GetNumLumenVoxelClipmaps(View.FinalPostProcessSettings.LumenSceneViewDistance) - 1));
		const float MaxCardUpdateDistanceFromCamera = ComputeMaxCardUpdateDistanceFromCamera(View.FinalPostProcessSettings.LumenSceneViewDistance);

#if WITH_MGPU
		for (const FVector& MultiViewOrigin : MultiViewFamilyOrigins)
		{
			LumenSceneCameraOrigins.Add(MultiViewOrigin);
		}
#endif

		const int32 MaxTileCapturesPerFrame = GetMaxTileCapturesPerFrame();

		if (MaxTileCapturesPerFrame > 0)
		{
			QUICK_SCOPE_CYCLE_COUNTER(FillCardPagesToRender);

			TArray<FSurfaceCacheRequest, SceneRenderingAllocator> SurfaceCacheRequests;

			const float LumenSceneDetail = FMath::Clamp<float>(View.FinalPostProcessSettings.LumenSceneDetail, .125f, 8.0f);

			//2. 管理 FLumenMeshCards，输出需要创建和删除的 MeshCards 列表
			UpdateSurfaceCachePrimitives(
				LumenSceneData,
				LumenSceneCameraOrigins,
				LumenSceneDetail,
				MaxCardUpdateDistanceFromCamera,
				LumenCardRenderer);
			
			//FLumenMeshCards 生成之后，由 UpdateSurfaceCacheMeshCards 函数生成分配请求 FSurfaceCacheRequest 列表，用于下一步的处理
			UpdateSurfaceCacheMeshCards(
				LumenSceneData,
				LumenSceneCameraOrigins,
				LumenSceneDetail,
				MaxCardUpdateDistanceFromCamera,
				SurfaceCacheRequests);

			//4.处理 Surface Cache 分配请求，分配 Page，生成用于捕获 Material Attributes 的渲染数据
			//		(1. 执行常驻的低分辨率 Page 分配
			//		(2. 执行高分辨率 Page 分配 启用 Surface Cache Feedback （Shader 中 SURFACE_CACHE_FEEDBACK 宏）以及在启用 HW RayTracing 时启用的高分辨率 Page （Shader 中 SURFACE_CACHE_HIGH_RES_PAGES 宏）时使用
			//		(3. 如果还有剩余 Capture Atlas 空间，则执行需要每帧更新材质的 Page 分配
			// allocate Card Atlas  and Capture Atlas ?
			LumenSceneData.ProcessLumenSurfaceCacheRequests(
				View,
				MaxCardUpdateDistanceFromCamera,
				MaxTileCapturesPerFrame,
				LumenCardRenderer,
				GraphBuilder.RHICmdList.GetGPUMask(),
				SurfaceCacheRequests);
		}

		// Atlas reallocation
		{
			if (bReallocateAtlas || !LumenSceneData.AlbedoAtlas)
			{
				LumenSceneData.AllocateCardAtlases(GraphBuilder, View);
			}

			if (LumenSceneData.bDebugClearAllCachedState)
			{
				ClearLumenSurfaceCacheAtlas(GraphBuilder, View);
			}
		}

		TArray<FCardPageRenderData, SceneRenderingAllocator>& CardPagesToRender = LumenCardRenderer.CardPagesToRender;

		if (CardPagesToRender.Num())
		{
			// Before we update the GPU page table, read from the persistent atlases for the card pages we are reallocating, and write it to the card capture atlas
			// This is a resample operation, as the original data may have been at a different mip level, or didn't exist at all
			ResampleLightingHistory(GraphBuilder, View, Scene, CardPagesToRender, LumenSceneData, LumenCardRenderer.ResampledCardCaptureAtlas);
		}

		LumenSceneData.UploadPageTable(GraphBuilder);

		//5. 遍历 CardPagesToRender 列表，依次调用 AddCardCaptureDraws 函数完成
		if (CardPagesToRender.Num() > 0)
		{
			{
				QUICK_SCOPE_CYCLE_COUNTER(MeshPassSetup);

				#if (UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT) && STATS
				if (GLumenSceneSurfaceCacheLogUpdates != 0)
				{
					UE_LOG(LogRenderer, Log, TEXT("Surface Cache Updates: %d"), CardPagesToRender.Num());

					if (GLumenSceneSurfaceCacheLogUpdates > 1)
					{ 
						for (FCardPageRenderData& CardPageRenderData : CardPagesToRender)
						{
							const FLumenPrimitiveGroup& LumenPrimitiveGroup = LumenSceneData.PrimitiveGroups[CardPageRenderData.PrimitiveGroupIndex];

							UE_LOG(LogRenderer, Log, TEXT("%s Instance:%d NumPrimsInGroup: %d"),
								*LumenPrimitiveGroup.Primitives[0]->Proxy->GetStatId().GetName().ToString(),
								LumenPrimitiveGroup.PrimitiveInstanceIndex,
								LumenPrimitiveGroup.Primitives.Num());
						}
					}
				}
				#endif

				for (FCardPageRenderData& CardPageRenderData : CardPagesToRender)
				{
					CardPageRenderData.StartMeshDrawCommandIndex = LumenCardRenderer.MeshDrawCommands.Num();
					CardPageRenderData.NumMeshDrawCommands = 0;
					int32 NumNanitePrimitives = 0;

					const FLumenPrimitiveGroup& PrimitiveGroup = LumenSceneData.PrimitiveGroups[CardPageRenderData.PrimitiveGroupIndex];
					const FLumenCard& Card = LumenSceneData.Cards[CardPageRenderData.CardIndex];
					ensure(Card.bVisible);

					//PrimitiveGroup 只有一个 Instance 且 FLandscapeComponentSceneProxy 且FeatureLevel > ERHIFeatureLevel::ES3_1
					if (PrimitiveGroup.bHeightfield)
					{
						AddCardCaptureDraws(
							Scene,
							GraphBuilder.RHICmdList,
							CardPageRenderData,
							PrimitiveGroup,
							LumenCardRenderer.LandscapePrimitivesInRange,
							LumenCardRenderer.MeshDrawCommands,
							LumenCardRenderer.MeshDrawPrimitiveIds);
					}
					else
					{
						AddCardCaptureDraws(
							Scene,
							GraphBuilder.RHICmdList,
							CardPageRenderData,
							PrimitiveGroup,
							PrimitiveGroup.Primitives,
							LumenCardRenderer.MeshDrawCommands,
							LumenCardRenderer.MeshDrawPrimitiveIds);
					}

					CardPageRenderData.NumMeshDrawCommands = LumenCardRenderer.MeshDrawCommands.Num() - CardPageRenderData.StartMeshDrawCommandIndex;
				}
			}
		}
	}
}

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLumenCardScene, "LumenCardScene");

void SetupLumenCardSceneParameters(FRDGBuilder& GraphBuilder, const FScene* Scene, FLumenCardScene& OutParameters)
{
	FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;

	OutParameters.NumCards = LumenSceneData.Cards.Num();
	OutParameters.NumMeshCards = LumenSceneData.MeshCards.Num();
	OutParameters.NumCardPages = LumenSceneData.GetNumCardPages();
	OutParameters.MaxConeSteps = GLumenGIMaxConeSteps;	
	OutParameters.PhysicalAtlasSize = LumenSceneData.GetPhysicalAtlasSize();
	OutParameters.InvPhysicalAtlasSize = FVector2f(1.0f) / OutParameters.PhysicalAtlasSize;
	OutParameters.IndirectLightingAtlasDownsampleFactor = Lumen::GetRadiosityAtlasDownsampleFactor();
	OutParameters.NumDistantCards = LumenSceneData.DistantCardIndices.Num();
	extern float GLumenDistantSceneMaxTraceDistance;
	OutParameters.DistantSceneMaxTraceDistance = GLumenDistantSceneMaxTraceDistance;
	OutParameters.DistantSceneDirection = FVector3f::ZeroVector;

	if (Scene->DirectionalLights.Num() > 0)
	{
		OutParameters.DistantSceneDirection = (FVector3f)-Scene->DirectionalLights[0]->Proxy->GetDirection();
	}
	
	for (int32 i = 0; i < LumenSceneData.DistantCardIndices.Num(); i++)
	{
		GET_SCALAR_ARRAY_ELEMENT(OutParameters.DistantCardIndices, i) = LumenSceneData.DistantCardIndices[i];
	}

	OutParameters.CardData = LumenSceneData.CardBuffer.SRV;
	OutParameters.MeshCardsData = LumenSceneData.MeshCardsBuffer.SRV;
	OutParameters.CardPageData = LumenSceneData.CardPageBuffer.SRV;
	OutParameters.PageTableBuffer = LumenSceneData.GetPageTableBufferSRV();
	OutParameters.SceneInstanceIndexToMeshCardsIndexBuffer = LumenSceneData.SceneInstanceIndexToMeshCardsIndexBuffer.SRV;

	OutParameters.HeightfieldData = LumenSceneData.HeightfieldBuffer.SRV;
	OutParameters.NumHeightfields = LumenSceneData.Heightfields.Num();

	if (LumenSceneData.AlbedoAtlas.IsValid())
	{
		OutParameters.OpacityAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.OpacityAtlas, TEXT("Lumen.SceneOpacity"));
		OutParameters.AlbedoAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.AlbedoAtlas, TEXT("Lumen.SceneAlbedo"));
		OutParameters.NormalAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.NormalAtlas, TEXT("Lumen.SceneNormal"));
		OutParameters.EmissiveAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.EmissiveAtlas, TEXT("Lumen.SceneEmissive"));
		OutParameters.DepthAtlas = GraphBuilder.RegisterExternalTexture(LumenSceneData.DepthAtlas, TEXT("Lumen.SceneDepth"));
	}
	else
	{
		FRDGTextureRef BlackDummyTextureRef = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy, TEXT("Lumen.BlackDummy"));
		OutParameters.OpacityAtlas = BlackDummyTextureRef;
		OutParameters.AlbedoAtlas = BlackDummyTextureRef;
		OutParameters.NormalAtlas = BlackDummyTextureRef;
		OutParameters.EmissiveAtlas = BlackDummyTextureRef;
		OutParameters.DepthAtlas = BlackDummyTextureRef;
	}
}

DECLARE_GPU_STAT(UpdateCardSceneBuffer);

class FClearLumenCardCapturePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearLumenCardCapturePS);
	SHADER_USE_PARAMETER_STRUCT(FClearLumenCardCapturePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FClearLumenCardCapturePS, "/Engine/Private/Lumen/LumenSceneLighting.usf", "ClearLumenCardCapturePS", SF_Pixel);

BEGIN_SHADER_PARAMETER_STRUCT(FClearLumenCardCaptureParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FPixelShaderUtils::FRasterizeToRectsVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FClearLumenCardCapturePS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void ClearLumenCardCapture(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FCardCaptureAtlas& Atlas,
	FRDGBufferSRVRef RectCoordBufferSRV,
	uint32 NumRects)
{
	FClearLumenCardCaptureParameters* PassParameters = GraphBuilder.AllocParameters<FClearLumenCardCaptureParameters>();

	PassParameters->RenderTargets[0] = FRenderTargetBinding(Atlas.Albedo, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[1] = FRenderTargetBinding(Atlas.Normal, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[2] = FRenderTargetBinding(Atlas.Emissive, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(Atlas.DepthStencil, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilWrite);

	auto PixelShader = View.ShaderMap->GetShader<FClearLumenCardCapturePS>();

	FPixelShaderUtils::AddRasterizeToRectsPass<FClearLumenCardCapturePS>(
		GraphBuilder,
		View.ShaderMap,
		RDG_EVENT_NAME("ClearCardCapture"),
		PixelShader,
		PassParameters,
		Atlas.Size,
		RectCoordBufferSRV,
		NumRects,
		TStaticBlendState<>::GetRHI(),
		TStaticRasterizerState<>::GetRHI(),
		TStaticDepthStencilState<true, CF_Always,
		true, CF_Always, SO_Replace, SO_Replace, SO_Replace,
		false, CF_Always, SO_Replace, SO_Replace, SO_Replace,
		0xff, 0xff>::GetRHI());
}

BEGIN_SHADER_PARAMETER_STRUCT(FLumenCardPassParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenCardPassUniformParameters, CardPass)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

//Capture Atlas 并不是 Surface Cache 的真正的物理存储所在，而只是捕获 Material Attribute 流程中的临时资源，无需持久化，因此是每帧按需分配大小
FIntPoint FLumenSceneData::GetCardCaptureAtlasSizeInPages() const
{
	const float MultPerComponent = 1.0f / FMath::Sqrt(FMath::Clamp(GLumenSceneCardCaptureFactor, 1.0f, 1024.0f));

	FIntPoint CaptureAtlasSizeInPages;
	CaptureAtlasSizeInPages.X = FMath::DivideAndRoundUp<uint32>(PhysicalAtlasSize.X * MultPerComponent + 0.5f, Lumen::PhysicalPageSize);
	CaptureAtlasSizeInPages.Y = FMath::DivideAndRoundUp<uint32>(PhysicalAtlasSize.Y * MultPerComponent + 0.5f, Lumen::PhysicalPageSize);
	return CaptureAtlasSizeInPages;
}

FIntPoint FLumenSceneData::GetCardCaptureAtlasSize() const 
{
	return GetCardCaptureAtlasSizeInPages() * Lumen::PhysicalPageSize;
}

uint32 FLumenSceneData::GetCardCaptureRefreshNumTexels() const
{
	const float CardCaptureRefreshFraction = FMath::Clamp(CVarLumenSceneCardCaptureRefreshFraction.GetValueOnRenderThread(), 0.0f, 1.0f);
	if (CardCaptureRefreshFraction > 0.0f)
	{
		// Allow to capture at least 1 full physical page
		FIntPoint CardCaptureAtlasSize = GetCardCaptureAtlasSize();
		return FMath::Max(CardCaptureAtlasSize.X * CardCaptureAtlasSize.Y * CardCaptureRefreshFraction, Lumen::PhysicalPageSize * Lumen::PhysicalPageSize);
	}

	return 0;
}

uint32 FLumenSceneData::GetCardCaptureRefreshNumPages() const
{
	const float CardCaptureRefreshFraction = FMath::Clamp(CVarLumenSceneCardCaptureRefreshFraction.GetValueOnRenderThread(), 0.0f, 1.0f);
	if (CardCaptureRefreshFraction > 0.0f)
	{
		// Allow to capture at least 1 full physical page
		return FMath::Clamp(GetMaxTileCapturesPerFrame() * CardCaptureRefreshFraction, 1, GetMaxTileCapturesPerFrame());
	}

	return 0;
}

void AllocateCardCaptureAtlas(FRDGBuilder& GraphBuilder, FIntPoint CardCaptureAtlasSize, FCardCaptureAtlas& CardCaptureAtlas)
{
	CardCaptureAtlas.Size = CardCaptureAtlasSize;

	CardCaptureAtlas.Albedo = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			CardCaptureAtlasSize,
			PF_R8G8B8A8,
			FClearValueBinding::Green,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear),
		TEXT("Lumen.CardCaptureAlbedoAtlas"));

	CardCaptureAtlas.Normal = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			CardCaptureAtlasSize,
			PF_R8G8,
			FClearValueBinding::Green,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear),
		TEXT("Lumen.CardCaptureNormalAtlas"));

	CardCaptureAtlas.Emissive = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			CardCaptureAtlasSize,
			PF_FloatR11G11B10,
			FClearValueBinding::Green,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear),
		TEXT("Lumen.CardCaptureEmissiveAtlas"));

	CardCaptureAtlas.DepthStencil = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			CardCaptureAtlasSize,
			PF_DepthStencil,
			FClearValueBinding::DepthZero,
			TexCreate_ShaderResource | TexCreate_DepthStencilTargetable | TexCreate_NoFastClear),
		TEXT("Lumen.CardCaptureDepthStencilAtlas"));
}

void UpdateGlobalLightingState(const FScene* Scene, FViewInfo& View, FLumenSceneData& LumenSceneData)
{
	FLumenGlobalLightingState& GlobalLightingState = LumenSceneData.GlobalLightingState;

	bool bModifySceneStateVersion = false;
	const FLightSceneInfo* DirectionalLightSceneInfo = nullptr;

	for (const FLightSceneInfo* LightSceneInfo : Scene->DirectionalLights)
	{
		if (LightSceneInfo->ShouldRenderLightViewIndependent()
			&& LightSceneInfo->ShouldRenderLight(View, true)
			&& LightSceneInfo->Proxy->GetIndirectLightingScale() > 0.0f)
		{
			DirectionalLightSceneInfo = LightSceneInfo;
			break;
		}
	}

	if (DirectionalLightSceneInfo && GlobalLightingState.bDirectionalLightValid)
	{
		const float OldMax = GlobalLightingState.DirectionalLightColor.GetMax();
		const float NewMax = DirectionalLightSceneInfo->Proxy->GetColor().GetMax();
		const float Ratio = OldMax / FMath::Max(NewMax, .00001f);

		if (Ratio > 4.0f || Ratio < .25f)
		{
			bModifySceneStateVersion = true;
		}
	}

	if (DirectionalLightSceneInfo)
	{
		GlobalLightingState.DirectionalLightColor = DirectionalLightSceneInfo->Proxy->GetColor();
		GlobalLightingState.bDirectionalLightValid = true;
	}
	else
	{
		GlobalLightingState.DirectionalLightColor = FLinearColor::Black;
		GlobalLightingState.bDirectionalLightValid = false;
	}

	const FSkyLightSceneProxy* SkyLightProxy = Scene->SkyLight;

	if (SkyLightProxy && GlobalLightingState.bSkyLightValid)
	{
		const float OldMax = GlobalLightingState.SkyLightColor.GetMax();
		const float NewMax = SkyLightProxy->GetEffectiveLightColor().GetMax();
		const float Ratio = OldMax / FMath::Max(NewMax, .00001f);

		if (Ratio > 4.0f || Ratio < .25f)
		{
			bModifySceneStateVersion = true;
		}
	}

	if (SkyLightProxy)
	{
		GlobalLightingState.SkyLightColor = SkyLightProxy->GetEffectiveLightColor();
		GlobalLightingState.bSkyLightValid = true;
	}
	else
	{
		GlobalLightingState.SkyLightColor = FLinearColor::Black;
		GlobalLightingState.bSkyLightValid = false;
	}

	if (bModifySceneStateVersion)
	{
		View.bLumenPropagateGlobalLightingChange = true;
	}
}

void FDeferredShadingSceneRenderer::UpdateLumenScene(FRDGBuilder& GraphBuilder)
{
	LLM_SCOPE_BYTAG(Lumen);
	TRACE_CPUPROFILER_EVENT_SCOPE(FDeferredShadingSceneRenderer::UpdateLumenScene);

	FViewInfo& View = Views[0];
	const FPerViewPipelineState& ViewPipelineState = GetViewPipelineState(View);
	const bool bAnyLumenActive = ViewPipelineState.DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen || ViewPipelineState.ReflectionsMethod == EReflectionsMethod::Lumen;

	if (bAnyLumenActive
		// Don't update scene lighting for secondary views
		&& !View.bIsPlanarReflection 
		&& !View.bIsSceneCapture
		&& !View.bIsReflectionCapture
		&& View.ViewState)
	{
		FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;
		TArray<FCardPageRenderData, SceneRenderingAllocator>& CardPagesToRender = LumenCardRenderer.CardPagesToRender;

		QUICK_SCOPE_CYCLE_COUNTER(UpdateLumenScene);
		RDG_RHI_GPU_STAT_SCOPE(GraphBuilder, UpdateLumenSceneBuffers);
		RDG_GPU_STAT_SCOPE(GraphBuilder, LumenSceneUpdate);
		RDG_EVENT_SCOPE(GraphBuilder, "LumenSceneUpdate: %u card captures %.3fM texels", CardPagesToRender.Num(), LumenCardRenderer.NumCardTexelsToCapture / (1024.0f * 1024.0f));

		UpdateGlobalLightingState(Scene, View, LumenSceneData);

		Lumen::UpdateCardSceneBuffer(GraphBuilder, ViewFamily, Scene);

		// Init transient render targets for capturing cards
		FCardCaptureAtlas CardCaptureAtlas;
		AllocateCardCaptureAtlas(GraphBuilder, LumenSceneData.GetCardCaptureAtlasSize(), CardCaptureAtlas);

		if (CardPagesToRender.Num() > 0)
		{
			FRHIBuffer* PrimitiveIdVertexBuffer = nullptr;
			FInstanceCullingResult InstanceCullingResult;
			TUniquePtr<FInstanceCullingContext> InstanceCullingContext;
			if (Scene->GPUScene.IsEnabled())
			{
				InstanceCullingContext = MakeUnique<FInstanceCullingContext>(View.GetFeatureLevel(), nullptr, TArrayView<const int32>(&View.GPUSceneViewId, 1), nullptr);
				
				int32 MaxInstances = 0;
				int32 VisibleMeshDrawCommandsNum = 0;
				int32 NewPassVisibleMeshDrawCommandsNum = 0;
				
				InstanceCullingContext->SetupDrawCommands(LumenCardRenderer.MeshDrawCommands, false, MaxInstances, VisibleMeshDrawCommandsNum, NewPassVisibleMeshDrawCommandsNum);
				// Not supposed to do any compaction here.
				ensure(VisibleMeshDrawCommandsNum == LumenCardRenderer.MeshDrawCommands.Num());

				InstanceCullingContext->BuildRenderingCommands(GraphBuilder, Scene->GPUScene, View.DynamicPrimitiveCollector.GetInstanceSceneDataOffset(), View.DynamicPrimitiveCollector.NumInstances(), InstanceCullingResult);
			}
			else
			{
				// Prepare primitive Id VB for rendering mesh draw commands.
				if (LumenCardRenderer.MeshDrawPrimitiveIds.Num() > 0)
				{
					const uint32 PrimitiveIdBufferDataSize = LumenCardRenderer.MeshDrawPrimitiveIds.Num() * sizeof(int32);

					FPrimitiveIdVertexBufferPoolEntry Entry = GPrimitiveIdVertexBufferPool.Allocate(PrimitiveIdBufferDataSize);
					PrimitiveIdVertexBuffer = Entry.BufferRHI;

					void* RESTRICT Data = RHILockBuffer(PrimitiveIdVertexBuffer, 0, PrimitiveIdBufferDataSize, RLM_WriteOnly);
					FMemory::Memcpy(Data, LumenCardRenderer.MeshDrawPrimitiveIds.GetData(), PrimitiveIdBufferDataSize);
					RHIUnlockBuffer(PrimitiveIdVertexBuffer);

					GPrimitiveIdVertexBufferPool.ReturnToFreeList(Entry);
				}
			}

			FRDGBufferRef CardCaptureRectBuffer = nullptr;
			FRDGBufferSRVRef CardCaptureRectBufferSRV = nullptr;

			{
				FRDGUploadData<FUintVector4> CardCaptureRectArray(GraphBuilder, CardPagesToRender.Num());

				for (int32 Index = 0; Index < CardPagesToRender.Num(); Index++)
				{
					const FCardPageRenderData& CardPageRenderData = CardPagesToRender[Index];

					FUintVector4& Rect = CardCaptureRectArray[Index];
					Rect.X = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Min.X, 0);
					Rect.Y = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Min.Y, 0);
					Rect.Z = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Max.X, 0);
					Rect.W = FMath::Max(CardPageRenderData.CardCaptureAtlasRect.Max.Y, 0);
				}

				CardCaptureRectBuffer =
					CreateUploadBuffer(GraphBuilder, TEXT("Lumen.CardCaptureRects"),
						sizeof(FUintVector4), FMath::RoundUpToPowerOfTwo(CardPagesToRender.Num()),
						CardCaptureRectArray);
				CardCaptureRectBufferSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CardCaptureRectBuffer, PF_R32G32B32A32_UINT));

				ClearLumenCardCapture(GraphBuilder, View, CardCaptureAtlas, CardCaptureRectBufferSRV, CardPagesToRender.Num());
			}

			FViewInfo* SharedView = View.CreateSnapshot();
			{
				SharedView->DynamicPrimitiveCollector = FGPUScenePrimitiveCollector(&GetGPUSceneDynamicContext());
				SharedView->StereoPass = EStereoscopicPass::eSSP_FULL;
				SharedView->DrawDynamicFlags = EDrawDynamicFlags::ForceLowestLOD;

				// Don't do material texture mip biasing in proxy card rendering
				SharedView->MaterialTextureMipBias = 0;

				TRefCountPtr<IPooledRenderTarget> NullRef;
				FPlatformMemory::Memcpy(&SharedView->PrevViewInfo.HZB, &NullRef, sizeof(SharedView->PrevViewInfo.HZB));

				SharedView->CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();
				SharedView->CachedViewUniformShaderParameters->PrimitiveSceneData = Scene->GPUScene.PrimitiveBuffer.SRV;
				SharedView->CachedViewUniformShaderParameters->InstanceSceneData = Scene->GPUScene.InstanceSceneDataBuffer.SRV;
				SharedView->CachedViewUniformShaderParameters->InstancePayloadData = Scene->GPUScene.InstancePayloadDataBuffer.SRV;
				SharedView->CachedViewUniformShaderParameters->LightmapSceneData = Scene->GPUScene.LightmapDataBuffer.SRV;
				SharedView->CachedViewUniformShaderParameters->InstanceSceneDataSOAStride = Scene->GPUScene.InstanceSceneDataSOAStride;

				SharedView->ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*SharedView->CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);
			}

			FLumenCardPassUniformParameters* PassUniformParameters = GraphBuilder.AllocParameters<FLumenCardPassUniformParameters>();
			SetupSceneTextureUniformParameters(GraphBuilder, Scene->GetFeatureLevel(), /*SceneTextureSetupMode*/ ESceneTextureSetupMode::None, PassUniformParameters->SceneTextures);
			PassUniformParameters->EyeAdaptationTexture = GetEyeAdaptationTexture(GraphBuilder, View);

			{
				FLumenCardPassParameters* PassParameters = GraphBuilder.AllocParameters<FLumenCardPassParameters>();
				PassParameters->View = Scene->UniformBuffers.LumenCardCaptureViewUniformBuffer;
				PassParameters->CardPass = GraphBuilder.CreateUniformBuffer(PassUniformParameters);
				PassParameters->RenderTargets[0] = FRenderTargetBinding(CardCaptureAtlas.Albedo, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets[1] = FRenderTargetBinding(CardCaptureAtlas.Normal, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets[2] = FRenderTargetBinding(CardCaptureAtlas.Emissive, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(CardCaptureAtlas.DepthStencil, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilNop);

				InstanceCullingResult.GetDrawParameters(PassParameters->InstanceCullingDrawParams);

				uint32 NumPages = 0;
				uint32 NumDraws = 0;
				uint32 NumInstances = 0;
				uint32 NumTris = 0;

				// Compute some stats about non Nanite meshes which are captured
				#if RDG_EVENTS != RDG_EVENTS_NONE
				{
					for (FCardPageRenderData& CardPageRenderData : CardPagesToRender)
					{
						if (CardPageRenderData.NumMeshDrawCommands > 0)
						{
							NumPages += 1;
							NumDraws += CardPageRenderData.NumMeshDrawCommands;

							for (int32 DrawCommandIndex = CardPageRenderData.StartMeshDrawCommandIndex; DrawCommandIndex < CardPageRenderData.StartMeshDrawCommandIndex + CardPageRenderData.NumMeshDrawCommands; ++DrawCommandIndex)
							{
								const FVisibleMeshDrawCommand& VisibleDrawCommand = LumenCardRenderer.MeshDrawCommands[DrawCommandIndex];
								const FMeshDrawCommand* MeshDrawCommand = VisibleDrawCommand.MeshDrawCommand;

								uint32 NumInstancesPerDraw = 0;

								// Count number of instances to draw
								if (VisibleDrawCommand.NumRuns)
								{
									for (int32 InstanceRunIndex = 0; InstanceRunIndex < VisibleDrawCommand.NumRuns; ++InstanceRunIndex)
									{
										const int32 FirstInstance = VisibleDrawCommand.RunArray[InstanceRunIndex * 2 + 0];
										const int32 LastInstance = VisibleDrawCommand.RunArray[InstanceRunIndex * 2 + 1];
										NumInstancesPerDraw += LastInstance - FirstInstance + 1;
									}
								}
								else
								{
									NumInstancesPerDraw += MeshDrawCommand->NumInstances;
								}

								NumInstances += NumInstancesPerDraw;
								NumTris += MeshDrawCommand->NumPrimitives * NumInstancesPerDraw;
							}
						}
					}
				}
				#endif

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("MeshCardCapture Pages:%u Draws:%u Instances:%u Tris:%u", NumPages, NumDraws, NumInstances, NumTris),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, Scene = Scene, PrimitiveIdVertexBuffer, SharedView, &CardPagesToRender, PassParameters, InstanceCullingContext = MoveTemp(InstanceCullingContext)](FRHICommandListImmediate& RHICmdList)
					{
						QUICK_SCOPE_CYCLE_COUNTER(MeshPass);

						for (FCardPageRenderData& CardPageRenderData : CardPagesToRender)
						{
							if (CardPageRenderData.NumMeshDrawCommands > 0)
							{
								const FIntRect ViewRect = CardPageRenderData.CardCaptureAtlasRect;
								RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

								CardPageRenderData.PatchView(RHICmdList, Scene, SharedView);
								Scene->UniformBuffers.LumenCardCaptureViewUniformBuffer.UpdateUniformBufferImmediate(*SharedView->CachedViewUniformShaderParameters);

								FGraphicsMinimalPipelineStateSet GraphicsMinimalPipelineStateSet;
								if (Scene->GPUScene.IsEnabled())
								{
									FInstanceCullingDrawParams& InstanceCullingDrawParams = PassParameters->InstanceCullingDrawParams;

									InstanceCullingContext->SubmitDrawCommands(
										LumenCardRenderer.MeshDrawCommands,
										GraphicsMinimalPipelineStateSet,
										GetMeshDrawCommandOverrideArgs(PassParameters->InstanceCullingDrawParams),
										CardPageRenderData.StartMeshDrawCommandIndex,
										CardPageRenderData.NumMeshDrawCommands,
										1,
										RHICmdList);
								}
								else
								{
									SubmitMeshDrawCommandsRange(
										LumenCardRenderer.MeshDrawCommands,
										GraphicsMinimalPipelineStateSet,
										PrimitiveIdVertexBuffer,
										FInstanceCullingContext::GetInstanceIdBufferStride(Scene->GetFeatureLevel()),
										0,
										false,
										CardPageRenderData.StartMeshDrawCommandIndex,
										CardPageRenderData.NumMeshDrawCommands,
										1,
										RHICmdList);
								}
							}
						}
					}
				);
			}

			bool bAnyNaniteMeshes = false;

			for (FCardPageRenderData& CardPageRenderData : CardPagesToRender)
			{
				if (CardPageRenderData.NaniteCommandInfos.Num() > 0 && CardPageRenderData.NaniteInstanceIds.Num() > 0)
				{
					bAnyNaniteMeshes = true;
					break;
				}
			}

			if (UseNanite(ShaderPlatform) && ViewFamily.EngineShowFlags.NaniteMeshes && bAnyNaniteMeshes)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(NaniteMeshPass);
				QUICK_SCOPE_CYCLE_COUNTER(NaniteMeshPass);

				const FIntPoint DepthStencilAtlasSize = CardCaptureAtlas.Size;
				const FIntRect DepthAtlasRect = FIntRect(0, 0, DepthStencilAtlasSize.X, DepthStencilAtlasSize.Y);

				Nanite::FSharedContext SharedContext{};
				SharedContext.FeatureLevel = Scene->GetFeatureLevel();
				SharedContext.ShaderMap = GetGlobalShaderMap(SharedContext.FeatureLevel);
				SharedContext.Pipeline = Nanite::EPipeline::Lumen;

				Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(
					GraphBuilder,
					SharedContext,
					DepthStencilAtlasSize,
					false,
					Nanite::EOutputBufferMode::VisBuffer,
					true,
					CardCaptureRectBufferSRV,
					CardPagesToRender.Num());

				Nanite::FCullingContext::FConfiguration CullingConfig = { 0 };
				CullingConfig.bSupportsMultiplePasses	= true;
				CullingConfig.bForceHWRaster			= RasterContext.RasterScheduling == Nanite::ERasterScheduling::HardwareOnly;
				CullingConfig.SetViewFlags(*SharedView);
				CullingConfig.bIsLumenCapture = true;

				Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(
					GraphBuilder,
					SharedContext,
					*Scene,
					nullptr,
					FIntRect(),
					CullingConfig
				);

				if (GLumenSceneNaniteMultiViewRaster != 0)
				{
					// Multi-view rendering path
					const uint32 NumCardPagesToRender = CardPagesToRender.Num();

					uint32 NextCardIndex = 0;
					while(NextCardIndex < NumCardPagesToRender)
					{
						TArray<Nanite::FPackedView, SceneRenderingAllocator> NaniteViews;
						TArray<Nanite::FInstanceDraw, SceneRenderingAllocator> NaniteInstanceDraws;

						while(NextCardIndex < NumCardPagesToRender && NaniteViews.Num() < NANITE_MAX_VIEWS_PER_CULL_RASTERIZE_PASS)
						{
							const FCardPageRenderData& CardPageRenderData = CardPagesToRender[NextCardIndex];

							if(CardPageRenderData.NaniteInstanceIds.Num() > 0)
							{
								for(uint32 InstanceID : CardPageRenderData.NaniteInstanceIds)
								{
									NaniteInstanceDraws.Add(Nanite::FInstanceDraw { InstanceID, (uint32)NaniteViews.Num() });
								}

								Nanite::FPackedViewParams Params;
								Params.ViewMatrices = CardPageRenderData.ViewMatrices;
								Params.PrevViewMatrices = CardPageRenderData.ViewMatrices;
								Params.ViewRect = CardPageRenderData.CardCaptureAtlasRect;
								Params.RasterContextSize = DepthStencilAtlasSize;
								Params.LODScaleFactor = CardPageRenderData.NaniteLODScaleFactor;
								NaniteViews.Add(Nanite::CreatePackedView(Params));
							}

							NextCardIndex++;
						}

						if (NaniteInstanceDraws.Num() > 0)
						{
							RDG_EVENT_SCOPE(GraphBuilder, "Nanite::RasterizeLumenCards");

							Nanite::FRasterState RasterState;
							Nanite::CullRasterize(
								GraphBuilder,
								*Scene,
								*SharedView,
								NaniteViews,
								SharedContext,
								CullingContext,
								RasterContext,
								RasterState,
								&NaniteInstanceDraws
							);
						}
					}
				}
				else
				{
					RDG_EVENT_SCOPE(GraphBuilder, "RenderLumenCardsWithNanite");

					// One draw call per view
					for(FCardPageRenderData& CardPageRenderData : CardPagesToRender)
					{
						if(CardPageRenderData.NaniteInstanceIds.Num() > 0)
						{						
							TArray<Nanite::FInstanceDraw, SceneRenderingAllocator> NaniteInstanceDraws;
							for( uint32 InstanceID : CardPageRenderData.NaniteInstanceIds )
							{
								NaniteInstanceDraws.Add( Nanite::FInstanceDraw { InstanceID, 0u } );
							}
						
							CardPageRenderData.PatchView(GraphBuilder.RHICmdList, Scene, SharedView);
							Nanite::FPackedView PackedView = Nanite::CreatePackedViewFromViewInfo(*SharedView, DepthStencilAtlasSize, 0);

							Nanite::CullRasterize(
								GraphBuilder,
								*Scene,
								*SharedView,
								{ PackedView },
								SharedContext,
								CullingContext,
								RasterContext,
								Nanite::FRasterState(),
								&NaniteInstanceDraws
							);
						}
					}
				}

				extern float GLumenDistantSceneMinInstanceBoundsRadius;

				// Render entire scene for distant cards
				for (FCardPageRenderData& CardPageRenderData : CardPagesToRender)
				{
					if (CardPageRenderData.bDistantScene)
					{
						Nanite::FRasterState RasterState;
						RasterState.bNearClip = false;

						CardPageRenderData.PatchView(GraphBuilder.RHICmdList, Scene, SharedView);
						Nanite::FPackedView PackedView = Nanite::CreatePackedViewFromViewInfo(
							*SharedView,
							DepthStencilAtlasSize,
							/*Flags*/ 0,
							/*StreamingPriorityCategory*/ 0,
							GLumenDistantSceneMinInstanceBoundsRadius,
							Lumen::GetDistanceSceneNaniteLODScaleFactor());

						Nanite::CullRasterize(
							GraphBuilder,
							*Scene,
							*SharedView,
							{ PackedView },
							SharedContext,
							CullingContext,
							RasterContext,
							RasterState);
					}
				}

				if (GLumenSceneNaniteMultiViewCapture != 0)
				{
					Nanite::DrawLumenMeshCapturePass(
						GraphBuilder,
						*Scene,
						SharedView,
						TArrayView<const FCardPageRenderData>(CardPagesToRender),
						CullingContext,
						RasterContext,
						PassUniformParameters,
						CardCaptureRectBufferSRV,
						CardPagesToRender.Num(),
						CardCaptureAtlas.Size,
						CardCaptureAtlas.Albedo,
						CardCaptureAtlas.Normal,
						CardCaptureAtlas.Emissive,
						CardCaptureAtlas.DepthStencil
					);
				}
				else
				{
					// Single capture per card. Slow path, only for debugging.
					for (int32 PageIndex = 0; PageIndex < CardPagesToRender.Num(); ++PageIndex)
					{
						if (CardPagesToRender[PageIndex].NaniteCommandInfos.Num() > 0)
						{
							Nanite::DrawLumenMeshCapturePass(
								GraphBuilder,
								*Scene,
								SharedView,
								TArrayView<const FCardPageRenderData>(&CardPagesToRender[PageIndex], 1),
								CullingContext,
								RasterContext,
								PassUniformParameters,
								CardCaptureRectBufferSRV,
								CardPagesToRender.Num(),
								CardCaptureAtlas.Size,
								CardCaptureAtlas.Albedo,
								CardCaptureAtlas.Normal,
								CardCaptureAtlas.Emissive,
								CardCaptureAtlas.DepthStencil
							);
						}
					}
				}
			}

			UpdateLumenSurfaceCacheAtlas(GraphBuilder, View, CardPagesToRender, CardCaptureRectBufferSRV, CardCaptureAtlas, LumenCardRenderer.ResampledCardCaptureAtlas);
		}
	}

	// Reset arrays, but keep allocated memory for 1024 elements
	FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;
	LumenSceneData.CardIndicesToUpdateInBuffer.Empty(1024);
	LumenSceneData.MeshCardsIndicesToUpdateInBuffer.Empty(1024);
}
