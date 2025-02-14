// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	DeferredShadingRenderer.cpp: Top level rendering loop for deferred shading
=============================================================================*/

#include "DeferredShadingRenderer.h"
#include "VelocityRendering.h"
#include "SingleLayerWaterRendering.h"
#include "SkyAtmosphereRendering.h"
#include "VolumetricCloudRendering.h"
#include "VolumetricRenderTarget.h"
#include "ScenePrivate.h"
#include "SceneOcclusion.h"
#include "ScreenRendering.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PostProcess/PostProcessSubsurface.h"
#include "PostProcess/PostProcessVisualizeCalibrationMaterial.h"
#include "PostProcess/TemporalAA.h"
#include "CompositionLighting/CompositionLighting.h"
#include "FXSystem.h"
#include "OneColorShader.h"
#include "CompositionLighting/PostProcessDeferredDecals.h"
#include "CompositionLighting/PostProcessAmbientOcclusion.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "GlobalDistanceField.h"
#include "PostProcess/PostProcessing.h"
#include "DistanceFieldAtlas.h"
#include "EngineModule.h"
#include "SceneViewExtension.h"
#include "GPUSkinCache.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "RendererModule.h"
#include "VT/VirtualTextureFeedback.h"
#include "VT/VirtualTextureSystem.h"
#include "GPUScene.h"
#include "RayTracing/RayTracingMaterialHitShaders.h"
#include "RayTracing/RayTracingLighting.h"
#include "RayTracing/RayTracingScene.h"
#include "RayTracingDynamicGeometryCollection.h"
#include "RayTracingSkinnedGeometry.h"
#include "SceneTextureParameters.h"
#include "ScreenSpaceDenoise.h"
#include "ScreenSpaceRayTracing.h"
#include "RayTracing/RaytracingOptions.h"
#include "RayTracingDefinitions.h"
#include "RayTracingInstance.h"
#include "ShaderPrint.h"
#include "ShaderDebug.h"
#include "GPUSortManager.h"
#include "HairStrands/HairStrandsRendering.h"
#include "HairStrands/HairStrandsData.h"
#include "PhysicsField/PhysicsFieldComponent.h"
#include "PhysicsFieldRendering.h"
#include "NaniteVisualizationData.h"
#include "Rendering/NaniteResources.h"
#include "Rendering/NaniteStreamingManager.h"
#include "Rendering/NaniteCoarseMeshStreamingManager.h"
#include "SceneTextureReductions.h"
#include "VirtualShadowMaps/VirtualShadowMapCacheManager.h"
#include "Strata/Strata.h"
#include "Lumen/Lumen.h"
#include "Experimental/Containers/SherwoodHashTable.h"
#include "RayTracingGeometryManager.h"
#include "InstanceCulling/InstanceCullingManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Engine/SubsurfaceProfile.h"
#include "SceneCaptureRendering.h"
#include "NaniteSceneProxy.h"
#include "RayTracing/RayTracingInstanceCulling.h"
#include "GPUMessaging.h"

extern int32 GNaniteShowStats;

static TAutoConsoleVariable<int32> CVarClearCoatNormal(
	TEXT("r.ClearCoatNormal"),
	0,
	TEXT("0 to disable clear coat normal.\n")
	TEXT(" 0: off\n")
	TEXT(" 1: on"),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarIrisNormal(
	TEXT("r.IrisNormal"),
	0,
	TEXT("0 to disable iris normal.\n")
	TEXT(" 0: off\n")
	TEXT(" 1: on"),
	ECVF_ReadOnly);

int32 GbEnableAsyncComputeTranslucencyLightingVolumeClear = 0; // @todo: disabled due to GPU crashes
static FAutoConsoleVariableRef CVarEnableAsyncComputeTranslucencyLightingVolumeClear(
	TEXT("r.EnableAsyncComputeTranslucencyLightingVolumeClear"),
	GbEnableAsyncComputeTranslucencyLightingVolumeClear,
	TEXT("Whether to clear the translucency lighting volume using async compute.\n"),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static int32 GRayTracing = 0;
static TAutoConsoleVariable<int32> CVarRayTracing(
	TEXT("r.RayTracing"),
	GRayTracing,
	TEXT("0 to disable ray tracing.\n")
	TEXT(" 0: off\n")
	TEXT(" 1: on"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

int32 GRayTracingUseTextureLod = 0;
static TAutoConsoleVariable<int32> CVarRayTracingTextureLod(
	TEXT("r.RayTracing.UseTextureLod"),
	GRayTracingUseTextureLod,
	TEXT("Enable automatic texture mip level selection in ray tracing material shaders.\n")
	TEXT(" 0: highest resolution mip level is used for all texture (default).\n")
	TEXT(" 1: texture LOD is approximated based on total ray length, output resolution and texel density at hit point (ray cone method)."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static int32 GForceAllRayTracingEffects = -1;
static TAutoConsoleVariable<int32> CVarForceAllRayTracingEffects(
	TEXT("r.RayTracing.ForceAllRayTracingEffects"),
	GForceAllRayTracingEffects,
	TEXT("Force all ray tracing effects ON/OFF.\n")
	TEXT(" -1: Do not force (default) \n")
	TEXT(" 0: All ray tracing effects disabled\n")
	TEXT(" 1: All ray tracing effects enabled"),
	ECVF_RenderThreadSafe);

static int32 GRayTracingAllowInline = 1;
static TAutoConsoleVariable<int32> CVarRayTracingAllowInline(
	TEXT("r.RayTracing.AllowInline"),
	GRayTracingAllowInline,
	TEXT("Allow use of Inline Ray Tracing if supported (default=1)."),	
	ECVF_RenderThreadSafe);

static int32 GRayTracingAllowPipeline = 1;
static TAutoConsoleVariable<int32> CVarRayTracingAllowPipeline(
	TEXT("r.RayTracing.AllowPipeline"),
	GRayTracingAllowPipeline,
	TEXT("Allow use of Ray Tracing pipelines if supported (default=1)."),
	ECVF_RenderThreadSafe);

static int32 GRayTracingSceneCaptures = -1;
static FAutoConsoleVariableRef CVarRayTracingSceneCaptures(
	TEXT("r.RayTracing.SceneCaptures"),
	GRayTracingSceneCaptures,
	TEXT("Enable ray tracing in scene captures.\n")
	TEXT(" -1: Use scene capture settings (default) \n")
	TEXT(" 0: off \n")
	TEXT(" 1: on"),
	ECVF_RenderThreadSafe);

static int32 GRayTracingExcludeDecals = 0;
static FAutoConsoleVariableRef CRayTracingExcludeDecals(
	TEXT("r.RayTracing.ExcludeDecals"),
	GRayTracingExcludeDecals,
	TEXT("A toggle that modifies the inclusion of decals in the ray tracing BVH.\n")
	TEXT(" 0: Decals included in the ray tracing BVH (default)\n")
	TEXT(" 1: Decals excluded from the ray tracing BVH"),
	ECVF_RenderThreadSafe);

static int32 GRayTracingExcludeTranslucent = 0;
static FAutoConsoleVariableRef CRayTracingExcludeTranslucent(
	TEXT("r.RayTracing.ExcludeTranslucent"),
	GRayTracingExcludeTranslucent,
	TEXT("A toggle that modifies the inclusion of translucent objects in the ray tracing scene.\n")
	TEXT(" 0: Translucent objects included in the ray tracing scene (default)\n")
	TEXT(" 1: Translucent objects excluded from the ray tracing scene"),
	ECVF_RenderThreadSafe);

static int32 GRayTracingExcludeSky = 1;
static FAutoConsoleVariableRef CRayTracingExcludeSky(
	TEXT("r.RayTracing.ExcludeSky"),
	GRayTracingExcludeSky,
	TEXT("A toggle that controls inclusion of sky geometry in the ray tracing scene (excluding sky can make ray tracing faster).\n")
	TEXT(" 0: Sky objects included in the ray tracing scene\n")
	TEXT(" 1: Sky objects excluded from the ray tracing scene (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingAsyncBuild(
	TEXT("r.RayTracing.AsyncBuild"),
	0,
	TEXT("Whether to build ray tracing acceleration structures on async compute queue.\n"),
	ECVF_RenderThreadSafe
);

static int32 GRayTracingParallelMeshBatchSetup = 1;
static FAutoConsoleVariableRef CRayTracingParallelMeshBatchSetup(
	TEXT("r.RayTracing.ParallelMeshBatchSetup"),
	GRayTracingParallelMeshBatchSetup,
	TEXT("Whether to setup ray tracing materials via parallel jobs."),
	ECVF_RenderThreadSafe);

static int32 GRayTracingParallelMeshBatchSize = 1024;
static FAutoConsoleVariableRef CRayTracingParallelMeshBatchSize(
	TEXT("r.RayTracing.ParallelMeshBatchSize"),
	GRayTracingParallelMeshBatchSize,
	TEXT("Batch size for ray tracing materials parallel jobs."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRayTracingDynamicGeometryLastRenderTimeUpdateDistance(
	TEXT("r.RayTracing.DynamicGeometryLastRenderTimeUpdateDistance"),
	5000.0f,
	TEXT("Dynamic geometries within this distance will have their LastRenderTime updated, so that visibility based ticking (like skeletal mesh) can work when the component is not directly visible in the view (but reflected)."));

static TAutoConsoleVariable<int32> CVarRayTracingAutoInstance(
	TEXT("r.RayTracing.AutoInstance"),
	1,
	TEXT("Whether to auto instance static meshes\n"),
	ECVF_RenderThreadSafe
);

static int32 GRayTracingDebugDisableTriangleCull = 0;
static FAutoConsoleVariableRef CVarRayTracingDebugDisableTriangleCull(
	TEXT("r.RayTracing.DebugDisableTriangleCull"),
	GRayTracingDebugDisableTriangleCull,
	TEXT("Forces all ray tracing geometry instances to be double-sided by disabling back-face culling. This is useful for debugging and profiling. (default = 0)")
);


static int32 GRayTracingDebugForceOpaque = 0;
static FAutoConsoleVariableRef CVarRayTracingDebugForceOpaque(
	TEXT("r.RayTracing.DebugForceOpaque"),
	GRayTracingDebugForceOpaque,
	TEXT("Forces all ray tracing geometry instances to be opaque, effectively disabling any-hit shaders. This is useful for debugging and profiling. (default = 0)")
);

static int32 GNumLODTasksToInline = 10;
FAutoConsoleVariableRef CVarNumLODTasksToInline(
	TEXT("r.RayTracing.GatherWorldInstancingInlineThreshold"),
	GNumLODTasksToInline,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe);


#if !UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarForceBlackVelocityBuffer(
	TEXT("r.Test.ForceBlackVelocityBuffer"), 0,
	TEXT("Force the velocity buffer to have no motion vector for debugging purpose."),
	ECVF_RenderThreadSafe);
#endif

static TAutoConsoleVariable<int32> CVarNaniteViewMeshLODBiasEnable(
	TEXT("r.Nanite.ViewMeshLODBias.Enable"), 1,
	TEXT("Whether LOD offset to apply for rasterized Nanite meshes for the main viewport should be based off TSR's ScreenPercentage (Enabled by default)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNaniteViewMeshLODBiasOffset(
	TEXT("r.Nanite.ViewMeshLODBias.Offset"), 0.0f,
	TEXT("LOD offset to apply for rasterized Nanite meshes for the main viewport when using TSR (Default = 0)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNaniteViewMeshLODBiasMin(
	TEXT("r.Nanite.ViewMeshLODBias.Min"), -2.0f,
	TEXT("Minimum LOD offset for rasterizing Nanite meshes for the main viewport (Default = -2)."),
	ECVF_RenderThreadSafe);


namespace Lumen
{
	extern bool AnyLumenHardwareRayTracingPassEnabled();
}
namespace Nanite
{
	extern bool IsStatFilterActive(const FString& FilterName);
	extern void ListStatFilters(FSceneRenderer* SceneRenderer);
}

DECLARE_CYCLE_STAT(TEXT("InitViews Intentional Stall"), STAT_InitViews_Intentional_Stall, STATGROUP_InitViews);

DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer UpdateDownsampledDepthSurface"), STAT_FDeferredShadingSceneRenderer_UpdateDownsampledDepthSurface, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer Render Init"), STAT_FDeferredShadingSceneRenderer_Render_Init, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer FGlobalDynamicVertexBuffer Commit"), STAT_FDeferredShadingSceneRenderer_FGlobalDynamicVertexBuffer_Commit, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer FXSystem PreRender"), STAT_FDeferredShadingSceneRenderer_FXSystem_PreRender, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer AllocGBufferTargets"), STAT_FDeferredShadingSceneRenderer_AllocGBufferTargets, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer DBuffer"), STAT_FDeferredShadingSceneRenderer_DBuffer, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer ResolveDepth After Basepass"), STAT_FDeferredShadingSceneRenderer_ResolveDepth_After_Basepass, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer Resolve After Basepass"), STAT_FDeferredShadingSceneRenderer_Resolve_After_Basepass, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer FXSystem PostRenderOpaque"), STAT_FDeferredShadingSceneRenderer_FXSystem_PostRenderOpaque, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer AfterBasePass"), STAT_FDeferredShadingSceneRenderer_AfterBasePass, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer Lighting"), STAT_FDeferredShadingSceneRenderer_Lighting, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer RenderLightShaftOcclusion"), STAT_FDeferredShadingSceneRenderer_RenderLightShaftOcclusion, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer RenderAtmosphere"), STAT_FDeferredShadingSceneRenderer_RenderAtmosphere, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer RenderSkyAtmosphere"), STAT_FDeferredShadingSceneRenderer_RenderSkyAtmosphere, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer RenderFog"), STAT_FDeferredShadingSceneRenderer_RenderFog, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer RenderLightShaftBloom"), STAT_FDeferredShadingSceneRenderer_RenderLightShaftBloom, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer RenderFinish"), STAT_FDeferredShadingSceneRenderer_RenderFinish, STATGROUP_SceneRendering);

DECLARE_GPU_STAT(RayTracingScene);
DECLARE_GPU_STAT(RayTracingGeometry);

DECLARE_GPU_STAT(Postprocessing);
DECLARE_GPU_STAT(VisibilityCommands);
DECLARE_GPU_STAT(RenderDeferredLighting);
DECLARE_GPU_STAT(AllocateRendertargets);
DECLARE_GPU_STAT(FrameRenderFinish);
DECLARE_GPU_STAT(SortLights);
DECLARE_GPU_STAT(PostRenderOpsFX);
DECLARE_GPU_STAT(GPUSceneUpdate);
DECLARE_GPU_STAT_NAMED(Unaccounted, TEXT("[unaccounted]"));
DECLARE_GPU_DRAWCALL_STAT(WaterRendering);
DECLARE_GPU_STAT(HairRendering);
DEFINE_GPU_DRAWCALL_STAT(VirtualTextureUpdate);
DECLARE_GPU_STAT(UploadDynamicBuffers);
DECLARE_GPU_STAT(PostOpaqueExtensions);

CSV_DEFINE_CATEGORY(LightCount, true);

/*-----------------------------------------------------------------------------
	Global Illumination Plugin Function Delegates
-----------------------------------------------------------------------------*/

static FGlobalIlluminationPluginDelegates::FAnyRayTracingPassEnabled GIPluginAnyRaytracingPassEnabledDelegate;
FGlobalIlluminationPluginDelegates::FAnyRayTracingPassEnabled& FGlobalIlluminationPluginDelegates::AnyRayTracingPassEnabled()
{
	return GIPluginAnyRaytracingPassEnabledDelegate;
}

static FGlobalIlluminationPluginDelegates::FPrepareRayTracing GIPluginPrepareRayTracingDelegate;
FGlobalIlluminationPluginDelegates::FPrepareRayTracing& FGlobalIlluminationPluginDelegates::PrepareRayTracing()
{
	return GIPluginPrepareRayTracingDelegate;
}

static FGlobalIlluminationPluginDelegates::FRenderDiffuseIndirectLight GIPluginRenderDiffuseIndirectLightDelegate;
FGlobalIlluminationPluginDelegates::FRenderDiffuseIndirectLight& FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight()
{
	return GIPluginRenderDiffuseIndirectLightDelegate;
}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
static FGlobalIlluminationPluginDelegates::FRenderDiffuseIndirectVisualizations GIPluginRenderDiffuseIndirectVisualizationsDelegate;
FGlobalIlluminationPluginDelegates::FRenderDiffuseIndirectVisualizations& FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectVisualizations()
{
	return GIPluginRenderDiffuseIndirectVisualizationsDelegate;
}
#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)

const TCHAR* GetDepthPassReason(bool bDitheredLODTransitionsUseStencil, EShaderPlatform ShaderPlatform)
{
	if (IsForwardShadingEnabled(ShaderPlatform))
	{
		return TEXT("(Forced by ForwardShading)");
	}

	bool bUseNanite = UseNanite(ShaderPlatform);

	if (bUseNanite)
	{
		return TEXT("(Forced by Nanite)");
	}

	bool bDBufferAllowed = IsUsingDBuffers(ShaderPlatform);

	if (bDBufferAllowed)
	{
		return TEXT("(Forced by DBuffer)");
	}

	if (bDitheredLODTransitionsUseStencil)
	{
		return TEXT("(Forced by StencilLODDither)");
	}

	return TEXT("");
}

/*-----------------------------------------------------------------------------
	FDeferredShadingSceneRenderer
-----------------------------------------------------------------------------*/

FDeferredShadingSceneRenderer::FDeferredShadingSceneRenderer(const FSceneViewFamily* InViewFamily,FHitProxyConsumer* HitProxyConsumer)
	: FSceneRenderer(InViewFamily, HitProxyConsumer)
	, DepthPass(GetDepthPassInfo(Scene))
	, bAreLightsInLightGrid(false)
{}

/** 
* Renders the view family. 
*/

DEFINE_STAT(STAT_CLM_PrePass);
DECLARE_CYCLE_STAT(TEXT("FXPreRender"), STAT_CLM_FXPreRender, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("AfterPrePass"), STAT_CLM_AfterPrePass, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Lighting"), STAT_CLM_Lighting, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("AfterLighting"), STAT_CLM_AfterLighting, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("WaterPass"), STAT_CLM_WaterPass, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Translucency"), STAT_CLM_Translucency, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Distortion"), STAT_CLM_Distortion, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("AfterTranslucency"), STAT_CLM_AfterTranslucency, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("RenderDistanceFieldLighting"), STAT_CLM_RenderDistanceFieldLighting, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("LightShaftBloom"), STAT_CLM_LightShaftBloom, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("PostProcessing"), STAT_CLM_PostProcessing, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Velocity"), STAT_CLM_Velocity, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("AfterVelocity"), STAT_CLM_AfterVelocity, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("TranslucentVelocity"), STAT_CLM_TranslucentVelocity, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("RenderFinish"), STAT_CLM_RenderFinish, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("AfterFrame"), STAT_CLM_AfterFrame, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Wait RayTracing Add Mesh Batch"), STAT_WaitRayTracingAddMesh, STATGROUP_SceneRendering);

FGraphEventRef FDeferredShadingSceneRenderer::TranslucencyTimestampQuerySubmittedFence[FOcclusionQueryHelpers::MaxBufferedOcclusionFrames + 1];
FGlobalDynamicIndexBuffer FDeferredShadingSceneRenderer::DynamicIndexBufferForInitViews;
FGlobalDynamicIndexBuffer FDeferredShadingSceneRenderer::DynamicIndexBufferForInitShadows;
FGlobalDynamicVertexBuffer FDeferredShadingSceneRenderer::DynamicVertexBufferForInitViews;
FGlobalDynamicVertexBuffer FDeferredShadingSceneRenderer::DynamicVertexBufferForInitShadows;
TGlobalResource<FGlobalDynamicReadBuffer> FDeferredShadingSceneRenderer::DynamicReadBufferForInitShadows;
TGlobalResource<FGlobalDynamicReadBuffer> FDeferredShadingSceneRenderer::DynamicReadBufferForInitViews;

/**
 * Returns true if the depth Prepass needs to run
 */
bool FDeferredShadingSceneRenderer::ShouldRenderPrePass() const
{
	return (DepthPass.EarlyZPassMode != DDM_None || DepthPass.bEarlyZPassMovable != 0);
}

bool FDeferredShadingSceneRenderer::RenderHzb(FRDGBuilder& GraphBuilder, FRDGTextureRef SceneDepthTexture)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, HZB);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

		FSceneViewState* ViewState = View.ViewState;
		const FPerViewPipelineState& ViewPipelineState = *ViewPipelineStates[ViewIndex];


		if (ViewPipelineState.bClosestHZB || ViewPipelineState.bFurthestHZB)
		{
			RDG_EVENT_SCOPE(GraphBuilder, "BuildHZB(ViewId=%d)", ViewIndex);

			FRDGTextureRef ClosestHZBTexture = nullptr;
			FRDGTextureRef FurthestHZBTexture = nullptr;

			BuildHZB(
				GraphBuilder,
				SceneDepthTexture,
				/* VisBufferTexture = */ nullptr,
				View.ViewRect,
				View.GetFeatureLevel(),
				View.GetShaderPlatform(),
				TEXT("HZBClosest"),
				/* OutClosestHZBTexture = */ ViewPipelineState.bClosestHZB ? &ClosestHZBTexture : nullptr,
				TEXT("HZBFurthest"),
				/* OutFurthestHZBTexture = */ &FurthestHZBTexture);

			// Update the view.
			{
				View.HZBMipmap0Size = FurthestHZBTexture->Desc.Extent;
				View.HZB = FurthestHZBTexture;

				// Extract furthest HZB texture.
				if (View.ViewState)
				{
					if (IsNaniteEnabled() || FInstanceCullingContext::IsOcclusionCullingEnabled())
					{
						GraphBuilder.QueueTextureExtraction(FurthestHZBTexture, &View.ViewState->PrevFrameViewInfo.HZB);
					}
					else
					{
						View.ViewState->PrevFrameViewInfo.HZB = nullptr;
					}
				}

				// Extract closest HZB texture.
				if (ViewPipelineState.bClosestHZB)
				{
					View.ClosestHZB = ClosestHZBTexture;
				}
			}
		}

		if (FamilyPipelineState->bHZBOcclusion && ViewState && ViewState->HZBOcclusionTests.GetNum() != 0)
		{
			check(ViewState->HZBOcclusionTests.IsValidFrame(ViewState->OcclusionFrameCounter));
			ViewState->HZBOcclusionTests.Submit(GraphBuilder, View);
		}
	}

	return FamilyPipelineState->bHZBOcclusion;
}

BEGIN_SHADER_PARAMETER_STRUCT(FRenderOpaqueFXPassParameters, )
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
END_SHADER_PARAMETER_STRUCT()

static void RenderOpaqueFX(
	FRDGBuilder& GraphBuilder,
	TArrayView<const FViewInfo> Views,
	FFXSystemInterface* FXSystem,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer)
{
	// Notify the FX system that opaque primitives have been rendered and we now have a valid depth buffer.
	if (FXSystem && Views.Num() > 0)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, PostRenderOpsFX);
		RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderOpaqueFX);

		const ERDGPassFlags UBPassFlags = ERDGPassFlags::Compute | ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass | ERDGPassFlags::NeverCull;

		// Add a pass which extracts the RHI handle from the scene textures UB and sends it to the FX system.
		FRenderOpaqueFXPassParameters* ExtractUBPassParameters = GraphBuilder.AllocParameters<FRenderOpaqueFXPassParameters>();
		ExtractUBPassParameters->SceneTextures = SceneTexturesUniformBuffer;
		GraphBuilder.AddPass(RDG_EVENT_NAME("SetSceneTexturesUniformBuffer"), ExtractUBPassParameters, UBPassFlags, [ExtractUBPassParameters, FXSystem](FRHICommandListImmediate&)
		{
			FXSystem->SetSceneTexturesUniformBuffer(ExtractUBPassParameters->SceneTextures->GetRHIRef());
		});

		FXSystem->PostRenderOpaque(GraphBuilder, Views, true /*bAllowGPUParticleUpdate*/);

		// Clear the scene textures UB pointer on the FX system. Use the same pass parameters to extend resource lifetimes.
		GraphBuilder.AddPass(RDG_EVENT_NAME("UnsetSceneTexturesUniformBuffer"), ExtractUBPassParameters, UBPassFlags, [FXSystem](FRHICommandListImmediate&)
		{
			FXSystem->SetSceneTexturesUniformBuffer(nullptr);
		});

		if (FGPUSortManager* GPUSortManager = FXSystem->GetGPUSortManager())
		{
			GPUSortManager->OnPostRenderOpaque(GraphBuilder);
		}

		GraphBuilder.AddDispatchHint();
	}
}

#if RHI_RAYTRACING

static void AddDebugRayTracingInstanceFlags(ERayTracingInstanceFlags& InOutFlags)
{
	if (GRayTracingDebugForceOpaque)
	{
		InOutFlags |= ERayTracingInstanceFlags::ForceOpaque;
	}
	if (GRayTracingDebugDisableTriangleCull)
	{
		InOutFlags |= ERayTracingInstanceFlags::TriangleCullDisable;
	}
}

bool FDeferredShadingSceneRenderer::GatherRayTracingWorldInstancesForView(FRDGBuilder& GraphBuilder, FViewInfo& View, FRayTracingScene& RayTracingScene)
{
	if (!IsRayTracingEnabled())
	{
		return false;
	}

	bool bAnyRayTracingPassEnabled = false;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		bAnyRayTracingPassEnabled |= AnyRayTracingPassEnabled(Scene, Views[ViewIndex]);
	}

	if (!bAnyRayTracingPassEnabled)
	{
		return false;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FDeferredShadingSceneRenderer::GatherRayTracingWorldInstances);
	SCOPE_CYCLE_COUNTER(STAT_GatherRayTracingWorldInstances);

	RayTracingCollector.ClearViewMeshArrays();

	FGPUScenePrimitiveCollector DummyDynamicPrimitiveCollector;

	RayTracingCollector.AddViewMeshArrays(
		&View,
		&View.RayTracedDynamicMeshElements,
		&View.SimpleElementCollector,
		&DummyDynamicPrimitiveCollector,
		ViewFamily.GetFeatureLevel(),
		&DynamicIndexBufferForInitViews,
		&DynamicVertexBufferForInitViews,
		&DynamicReadBufferForInitViews
		);

	View.DynamicRayTracingMeshCommandStorage.Reserve(Scene->Primitives.Num());
	View.VisibleRayTracingMeshCommands.Reserve(Scene->Primitives.Num());

	extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

	for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
	{
		Extension->BeginRenderView(&View);
	}

	View.RayTracingMeshResourceCollector = MakeUnique<FRayTracingMeshResourceCollector>(
		Scene->GetFeatureLevel(),
		&DynamicIndexBufferForInitViews,
		&DynamicVertexBufferForInitViews,
		&DynamicReadBufferForInitViews);

	View.RayTracingCullingParameters.Init(View);

	FRayTracingMaterialGatheringContext MaterialGatheringContext
	{
		Scene,
		&View,
		ViewFamily,
		GraphBuilder,
		*View.RayTracingMeshResourceCollector
	};

	const float CurrentWorldTime = View.Family->Time.GetWorldTimeSeconds();

	struct FRelevantPrimitive
	{
		FRHIRayTracingGeometry* RayTracingGeometryRHI = nullptr;
		TArrayView<const int32> CachedRayTracingMeshCommandIndices;
		uint64 StateHash = 0;
		int32 PrimitiveIndex = -1;
		int8 LODIndex = -1;
		uint8 InstanceMask = 0;
		bool bStatic = false;
		bool bAllSegmentsOpaque = true;
		bool bAnySegmentsCastShadow = false;
		bool bAnySegmentsDecal = false;
		bool bTwoSided = false;
		bool bIsSky = false;
		bool bAllSegmentsTranslucent = true;

		uint64 InstancingKey() const
		{
			uint64 Key = StateHash;
			Key ^= uint64(InstanceMask) << 32;
			Key ^= bAllSegmentsOpaque ? 0x1ull << 40 : 0x0;
			Key ^= bAnySegmentsCastShadow ? 0x1ull << 41 : 0x0;
			Key ^= bAnySegmentsDecal ? 0x1ull << 42 : 0x0;
			Key ^= bTwoSided ? 0x1ull << 43 : 0x0;
			Key ^= bIsSky ? 0x1ull << 44 : 0x0;
			Key ^= bAllSegmentsTranslucent ? 0x1ull << 45 : 0x0;
			return Key ^ reinterpret_cast<uint64>(RayTracingGeometryRHI);
		}
	};

	// Unified array is used for static and dynamic primitives because we don't know ahead of time how many we'll have of each.
	TArray<FRelevantPrimitive> RelevantPrimitives;
	RelevantPrimitives.Reserve(Scene->PrimitiveSceneProxies.Num());

	TArray<FPrimitiveSceneInfo*> DirtyCachedRayTracingPrimitives;
	DirtyCachedRayTracingPrimitives.Reserve(Scene->PrimitiveSceneProxies.Num());

	int32 VisiblePrimitives = 0;
	bool bPerformRayTracing = View.State != nullptr && !View.bIsReflectionCapture && View.bAllowRayTracing;
	if (bPerformRayTracing)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GatherRayTracingWorldInstances_RelevantPrimitives);

		int32 BroadIndex = 0;

		for (int PrimitiveIndex = 0; PrimitiveIndex < Scene->PrimitiveSceneProxies.Num(); PrimitiveIndex++)
		{
			while (PrimitiveIndex >= int(Scene->TypeOffsetTable[BroadIndex].Offset))
			{
				BroadIndex++;
			}

			// Skip before dereferencing SceneInfo
			if (EnumHasAnyFlags(Scene->PrimitiveRayTracingFlags[PrimitiveIndex], ERayTracingPrimitiveFlags::UnsupportedProxyType))
			{
				//skip over unsupported SceneProxies (warning don't make IsRayTracingRelevant data dependent other than the vtable)
				PrimitiveIndex = Scene->TypeOffsetTable[BroadIndex].Offset - 1;
				continue;
			}

			// Get primitive visibility state from culling
			if (!View.PrimitiveRayTracingVisibilityMap[PrimitiveIndex])
			{
				continue;
			}

			const FPrimitiveSceneInfo* SceneInfo = Scene->Primitives[PrimitiveIndex];

			// #dxr_todo: ray tracing in scene captures should re-use the persistent RT scene. (UE-112448)
			bool bShouldRayTraceSceneCapture = GRayTracingSceneCaptures > 0
				|| (GRayTracingSceneCaptures == -1 && View.bSceneCaptureUsesRayTracing);

			if (View.bIsSceneCapture && (!bShouldRayTraceSceneCapture || !SceneInfo->bIsVisibleInSceneCaptures))
			{
				continue;
			}

			// Marked visible and used after point, check if streaming then mark as used in the TLAS (so it can be streamed in)
			if (EnumHasAnyFlags(Scene->PrimitiveRayTracingFlags[PrimitiveIndex], ERayTracingPrimitiveFlags::Streaming))
			{
				// Is the cached data dirty?
				if (SceneInfo->bCachedRaytracingDataDirty)
				{
					DirtyCachedRayTracingPrimitives.Add(Scene->Primitives[PrimitiveIndex]);
				}

				check(SceneInfo->CoarseMeshStreamingHandle != INDEX_NONE);
				RayTracingScene.UsedCoarseMeshStreamingHandles.Add(SceneInfo->CoarseMeshStreamingHandle);
			}

			VisiblePrimitives++;

			//#dxr_todo UE-68621  The Raytracing code path does not support ShowFlags since data moved to the SceneInfo. 
			//Touching the SceneProxy to determine this would simply cost too much
			static const auto RayTracingStaticMeshesCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RayTracing.Geometry.StaticMeshes"));

			FRelevantPrimitive Item;
			Item.PrimitiveIndex = PrimitiveIndex;

			if (EnumHasAnyFlags(Scene->PrimitiveRayTracingFlags[PrimitiveIndex], ERayTracingPrimitiveFlags::StaticMesh)
				&& View.Family->EngineShowFlags.StaticMeshes
				&& RayTracingStaticMeshesCVar && RayTracingStaticMeshesCVar->GetValueOnRenderThread() > 0)
			{
				Item.bStatic = true;
				RelevantPrimitives.Add(Item);
			}
			else if (View.Family->EngineShowFlags.SkeletalMeshes)
			{
				Item.bStatic = false;
				RelevantPrimitives.Add(Item);
			}
		}
	}

	INC_DWORD_STAT_BY(STAT_VisibleRayTracingPrimitives, VisiblePrimitives);

	FPrimitiveSceneInfo::UpdateCachedRaytracingData(Scene, DirtyCachedRayTracingPrimitives);

	FGraphEventArray LODTaskList;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GatherRayTracingWorldInstances_ComputeLOD);

		static const auto ICVarStaticMeshLODDistanceScale = IConsoleManager::Get().FindConsoleVariable(TEXT("r.StaticMeshLODDistanceScale"));
		const float LODScaleCVarValue = ICVarStaticMeshLODDistanceScale->GetFloat();
		const int32 ForcedLODLevel = GetCVarForceLOD();

		const uint32 NumTotalItems = RelevantPrimitives.Num();
		const uint32 TargetItemsPerTask = 1024; // Granularity based on profiling Infiltrator scene
		const uint32 NumTasks = FMath::Max(1u, FMath::DivideAndRoundUp(NumTotalItems, TargetItemsPerTask));
		const uint32 ItemsPerTask = FMath::DivideAndRoundUp(NumTotalItems, NumTasks); // Evenly divide commands between tasks (avoiding potential short last task)

		auto ComputeLOD =
			[	&View,
				Scene = this->Scene,
				LODScaleCVarValue,
				ForcedLODLevel
			](FRelevantPrimitive* Items, uint32 NumItems)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GatherRayTracingWorldInstances_ComputeLOD_Task);

				for (uint32 i = 0; i < NumItems; ++i)
				{
					FRelevantPrimitive& RelevantPrimitive = Items[i];
					if (!RelevantPrimitive.bStatic)
					{
						continue; // skip dynamic primitives
					}

					const int32 PrimitiveIndex = RelevantPrimitive.PrimitiveIndex;
					const FPrimitiveSceneInfo* SceneInfo = Scene->Primitives[PrimitiveIndex];

					int8 LODIndex = 0;

					if (EnumHasAnyFlags(Scene->PrimitiveRayTracingFlags[PrimitiveIndex], ERayTracingPrimitiveFlags::ComputeLOD))
					{
						const FPrimitiveBounds& Bounds = Scene->PrimitiveBounds[PrimitiveIndex];
						const FPrimitiveSceneInfo* RESTRICT PrimitiveSceneInfo = Scene->Primitives[PrimitiveIndex];

						FLODMask LODToRender;

						const int8 CurFirstLODIdx = PrimitiveSceneInfo->Proxy->GetCurrentFirstLODIdx_RenderThread();
						check(CurFirstLODIdx >= 0);

						float MeshScreenSizeSquared = 0;
						float LODScale = LODScaleCVarValue * View.LODDistanceFactor;
						LODToRender = ComputeLODForMeshes(SceneInfo->StaticMeshRelevances, View, Bounds.BoxSphereBounds.Origin, Bounds.BoxSphereBounds.SphereRadius, ForcedLODLevel, MeshScreenSizeSquared, CurFirstLODIdx, LODScale, true);

						LODIndex = LODToRender.GetRayTracedLOD();
					}

					if (!EnumHasAllFlags(Scene->PrimitiveRayTracingFlags[PrimitiveIndex], ERayTracingPrimitiveFlags::CacheInstances))
					{
						FRHIRayTracingGeometry* RayTracingGeometryInstance = SceneInfo->GetStaticRayTracingGeometryInstance(LODIndex);
						if (RayTracingGeometryInstance == nullptr)
						{
							continue;
						}

						// Sometimes LODIndex is out of range because it is clamped by ClampToFirstLOD, like the requested LOD is being streamed in and hasn't been available
						// According to InitViews, we should hide the static mesh instance
						if (SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.IsValidIndex(LODIndex))
						{
							RelevantPrimitive.LODIndex = LODIndex;
							RelevantPrimitive.RayTracingGeometryRHI = SceneInfo->GetStaticRayTracingGeometryInstance(LODIndex);

							RelevantPrimitive.CachedRayTracingMeshCommandIndices = SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[LODIndex];
							RelevantPrimitive.StateHash = SceneInfo->CachedRayTracingMeshCommandsHashPerLOD[LODIndex];

							for (int32 CommandIndex : RelevantPrimitive.CachedRayTracingMeshCommandIndices)
							{
								if (CommandIndex >= 0)
								{
									const FRayTracingMeshCommand& RayTracingMeshCommand = Scene->CachedRayTracingMeshCommands[CommandIndex];

									RelevantPrimitive.InstanceMask |= RayTracingMeshCommand.InstanceMask;
									RelevantPrimitive.bAllSegmentsOpaque &= RayTracingMeshCommand.bOpaque;
									RelevantPrimitive.bAnySegmentsCastShadow |= RayTracingMeshCommand.bCastRayTracedShadows;
									RelevantPrimitive.bAnySegmentsDecal |= RayTracingMeshCommand.bDecal;
									RelevantPrimitive.bTwoSided |= RayTracingMeshCommand.bTwoSided;
									RelevantPrimitive.bIsSky |= RayTracingMeshCommand.bIsSky;
									RelevantPrimitive.bAllSegmentsTranslucent &= RayTracingMeshCommand.bIsTranslucent;
								}
								else
								{
									// CommandIndex == -1 indicates that the mesh batch has been filtered by FRayTracingMeshProcessor (like the shadow depth pass batch)
									// Do nothing in this case
								}
							}

							RelevantPrimitive.InstanceMask |= RelevantPrimitive.bAnySegmentsCastShadow ? RAY_TRACING_MASK_SHADOW : 0;

							if (EnumHasAllFlags(Scene->PrimitiveRayTracingFlags[PrimitiveIndex], ERayTracingPrimitiveFlags::FarField))
							{
								RelevantPrimitive.InstanceMask = RAY_TRACING_MASK_FAR_FIELD;
							}
						}
					}
				}
			};

		if (NumTasks > (uint32)GNumLODTasksToInline)
		{
			SCOPED_NAMED_EVENT(DispatchParallelComputeLOD, FColor::Red);
			
			LODTaskList.Reserve(NumTasks);

			for (uint32 TaskIndex = 0; TaskIndex < NumTasks; ++TaskIndex)
			{
				const uint32 FirstTaskItemIndex = TaskIndex * ItemsPerTask;

				LODTaskList.Add(FFunctionGraphTask::CreateAndDispatchWhenReady(
					[	FirstTaskItemIndex,
						Items = RelevantPrimitives.GetData() + FirstTaskItemIndex,
						NumItems = FMath::Min(ItemsPerTask, NumTotalItems - FirstTaskItemIndex),
						ComputeLOD
					]()
					{
						ComputeLOD(Items, NumItems);
					},
					TStatId(), nullptr, ENamedThreads::AnyNormalThreadHiPriTask));
			}
		}
		else
		{
			SCOPED_NAMED_EVENT(ComputeLOD, FColor::Magenta);
			
			ComputeLOD(RelevantPrimitives.GetData(), NumTotalItems);
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GatherRayTracingWorldInstances_DynamicElements);

		const bool bParallelMeshBatchSetup = GRayTracingParallelMeshBatchSetup && FApp::ShouldUseThreadingForPerformance();

		const int64 SharedBufferGenerationID = Scene->GetRayTracingDynamicGeometryCollection()->BeginUpdate();

		struct FRayTracingMeshBatchWorkItem
		{
			const FPrimitiveSceneProxy* SceneProxy = nullptr;
			TArray<FMeshBatch> MeshBatchesOwned;
			TArrayView<const FMeshBatch> MeshBatchesView;
			uint32 InstanceIndex = 0;

			TArrayView<const FMeshBatch> GetMeshBatches() const
			{
				if (MeshBatchesOwned.Num())
				{
					check(MeshBatchesView.Num() == 0);
					return TArrayView<const FMeshBatch>(MeshBatchesOwned);
				}
				else
				{
					check(MeshBatchesOwned.Num() == 0);
					return MeshBatchesView;
				}
			}
		};

		static constexpr uint32 MaxWorkItemsPerPage = 128; // Try to keep individual pages small to avoid slow-path memory allocations
		struct FRayTracingMeshBatchTaskPage
		{
			FRayTracingMeshBatchWorkItem WorkItems[MaxWorkItemsPerPage];
			uint32 NumWorkItems = 0;
			FRayTracingMeshBatchTaskPage* Next = nullptr;
		};

		FRayTracingMeshBatchTaskPage* MeshBatchTaskHead = nullptr;
		FRayTracingMeshBatchTaskPage* MeshBatchTaskPage = nullptr;
		uint32 NumPendingMeshBatches = 0;
		const uint32 RayTracingParallelMeshBatchSize = GRayTracingParallelMeshBatchSize;

		auto KickRayTracingMeshBatchTask = [&View, &MeshBatchTaskHead, &MeshBatchTaskPage, &NumPendingMeshBatches, Scene = this->Scene]()
		{
			if (MeshBatchTaskHead)
			{
				FDynamicRayTracingMeshCommandStorage* TaskDynamicCommandStorage = new(FMemStack::Get()) FDynamicRayTracingMeshCommandStorage;
				View.DynamicRayTracingMeshCommandStoragePerTask.Add(TaskDynamicCommandStorage);

				FRayTracingMeshCommandOneFrameArray* TaskVisibleCommands = new(FMemStack::Get()) FRayTracingMeshCommandOneFrameArray;
				TaskVisibleCommands->Reserve(NumPendingMeshBatches);
				View.VisibleRayTracingMeshCommandsPerTask.Add(TaskVisibleCommands);

				View.AddRayTracingMeshBatchTaskList.Add(FFunctionGraphTask::CreateAndDispatchWhenReady(
					[TaskDataHead = MeshBatchTaskHead, &View, Scene, TaskDynamicCommandStorage, TaskVisibleCommands]()
				{
					FTaskTagScope TaskTagScope(ETaskTag::EParallelRenderingThread);
					TRACE_CPUPROFILER_EVENT_SCOPE(RayTracingMeshBatchTask);
					FRayTracingMeshBatchTaskPage* Page = TaskDataHead;
					const int32 ExpectedMaxVisibieCommands = TaskVisibleCommands->Max();
					while (Page)
					{
						for (uint32 ItemIndex = 0; ItemIndex < Page->NumWorkItems; ++ItemIndex)
						{
							const FRayTracingMeshBatchWorkItem& WorkItem = Page->WorkItems[ItemIndex];
							TArrayView<const FMeshBatch> MeshBatches = WorkItem.GetMeshBatches();
							for (int32 SegmentIndex = 0; SegmentIndex < MeshBatches.Num(); SegmentIndex++)
							{
								const FMeshBatch& MeshBatch = MeshBatches[SegmentIndex];
								FDynamicRayTracingMeshCommandContext CommandContext(
									*TaskDynamicCommandStorage, *TaskVisibleCommands,
									SegmentIndex, WorkItem.InstanceIndex);
								FMeshPassProcessorRenderState PassDrawRenderState(Scene->UniformBuffers.ViewUniformBuffer);
								FRayTracingMeshProcessor RayTracingMeshProcessor(&CommandContext, Scene, &View, PassDrawRenderState, Scene->CachedRayTracingMeshCommandsMode);
								RayTracingMeshProcessor.AddMeshBatch(MeshBatch, 1, WorkItem.SceneProxy);
							}
						}
						FRayTracingMeshBatchTaskPage* NextPage = Page->Next;
						Page->~FRayTracingMeshBatchTaskPage();
						Page = NextPage;
					}
					check(ExpectedMaxVisibieCommands <= TaskVisibleCommands->Max());
				}, TStatId(), nullptr, ENamedThreads::AnyThread));
			}

			MeshBatchTaskHead = nullptr;
			MeshBatchTaskPage = nullptr;
			NumPendingMeshBatches = 0;
		};

		// Local temporary array of instances used for GetDynamicRayTracingInstances()
		TArray<FRayTracingInstance> TempRayTracingInstances;

		for (const FRelevantPrimitive& RelevantPrimitive : RelevantPrimitives)
		{
			if (RelevantPrimitive.bStatic)
			{
				continue;
			}

			const int32 PrimitiveIndex = RelevantPrimitive.PrimitiveIndex;
			FPrimitiveSceneInfo* SceneInfo = Scene->Primitives[PrimitiveIndex];

			FPrimitiveSceneProxy* SceneProxy = Scene->PrimitiveSceneProxies[PrimitiveIndex];
			TempRayTracingInstances.Reset();
			MaterialGatheringContext.DynamicRayTracingGeometriesToUpdate.Reset();

			SceneProxy->GetDynamicRayTracingInstances(MaterialGatheringContext, TempRayTracingInstances);

			for (auto DynamicRayTracingGeometryUpdate : MaterialGatheringContext.DynamicRayTracingGeometriesToUpdate)
			{
				Scene->GetRayTracingDynamicGeometryCollection()->AddDynamicMeshBatchForGeometryUpdate(
					Scene,
					&View,
					SceneProxy,
					DynamicRayTracingGeometryUpdate,
					PrimitiveIndex
				);
			}

			if (TempRayTracingInstances.Num() > 0)
			{
				for (FRayTracingInstance& Instance : TempRayTracingInstances)
				{
					const FRayTracingGeometry* Geometry = Instance.Geometry;

					if (!ensureMsgf(Geometry->DynamicGeometrySharedBufferGenerationID == FRayTracingGeometry::NonSharedVertexBuffers
						|| Geometry->DynamicGeometrySharedBufferGenerationID == SharedBufferGenerationID,
						TEXT("GenerationID %lld, but expected to be %lld or %lld. Geometry debug name: '%s'. ")
						TEXT("When shared vertex buffers are used, the contents is expected to be written every frame. ")
						TEXT("Possibly AddDynamicMeshBatchForGeometryUpdate() was not called for this geometry."),
						Geometry->DynamicGeometrySharedBufferGenerationID, SharedBufferGenerationID, FRayTracingGeometry::NonSharedVertexBuffers,
						*Geometry->Initializer.DebugName.ToString()))
					{
						continue;
					}

					// If geometry still has pending build request then add to list which requires a force build
					if (Geometry->HasPendingBuildRequest())
					{
						RayTracingScene.GeometriesToBuild.Add(Geometry);
					}

					// Validate the material/segment counts
					if (!ensureMsgf(Instance.GetMaterials().Num() == Geometry->Initializer.Segments.Num() ||
						(Geometry->Initializer.Segments.Num() == 0 && Instance.GetMaterials().Num() == 1),
						TEXT("Ray tracing material assignment validation failed for geometry '%s'. "
							"Instance.GetMaterials().Num() = %d, Geometry->Initializer.Segments.Num() = %d, Instance.Mask = 0x%X."),
						*Geometry->Initializer.DebugName.ToString(), Instance.GetMaterials().Num(),
						Geometry->Initializer.Segments.Num(), Instance.Mask))
					{
						continue;
					}

					const uint32 InstanceIndex = RayTracingScene.Instances.Num();

					FRayTracingGeometryInstance& RayTracingInstance = RayTracingScene.Instances.AddDefaulted_GetRef();
					RayTracingInstance.GeometryRHI = Geometry->RayTracingGeometryRHI;
					checkf(RayTracingInstance.GeometryRHI, TEXT("Ray tracing instance must have a valid geometry."));

					RayTracingInstance.DefaultUserData = PrimitiveIndex;
					RayTracingInstance.Mask = Instance.Mask;
					if (Instance.bForceOpaque)
					{
						RayTracingInstance.Flags |= ERayTracingInstanceFlags::ForceOpaque;
					}
					if (Instance.bDoubleSided)
					{
						RayTracingInstance.Flags |= ERayTracingInstanceFlags::TriangleCullDisable;
					}
					AddDebugRayTracingInstanceFlags(RayTracingInstance.Flags);

					if (Instance.InstanceGPUTransformsSRV.IsValid())
					{
						RayTracingInstance.NumTransforms = Instance.NumTransforms;
						RayTracingInstance.GPUTransformsSRV = Instance.InstanceGPUTransformsSRV;
					}
					else 
					{
						if (Instance.OwnsTransforms())
						{
							// Slow path: copy transforms to the owned storage
							checkf(Instance.InstanceTransformsView.Num() == 0, TEXT("InstanceTransformsView is expected to be empty if using InstanceTransforms"));
							TArrayView<FMatrix> SceneOwnedTransforms = RayTracingScene.Allocate<FMatrix>(Instance.InstanceTransforms.Num());
							FMemory::Memcpy(SceneOwnedTransforms.GetData(), Instance.InstanceTransforms.GetData(), Instance.InstanceTransforms.Num() * sizeof(RayTracingInstance.Transforms[0]));
							static_assert(TIsSame<decltype(SceneOwnedTransforms[0]), decltype(Instance.InstanceTransforms[0])>::Value, "Unexpected transform type");

							RayTracingInstance.NumTransforms = SceneOwnedTransforms.Num();
							RayTracingInstance.Transforms = SceneOwnedTransforms;
						}
						else
						{
							// Fast path: just reference persistently-allocated transforms and avoid a copy
							checkf(Instance.InstanceTransforms.Num() == 0, TEXT("InstanceTransforms is expected to be empty if using InstanceTransformsView"));
							RayTracingInstance.NumTransforms = Instance.InstanceTransformsView.Num();
							RayTracingInstance.Transforms = Instance.InstanceTransformsView;
						}
					}

					if (bParallelMeshBatchSetup)
					{
						if (NumPendingMeshBatches >= RayTracingParallelMeshBatchSize)
						{
							KickRayTracingMeshBatchTask();
						}

						if (MeshBatchTaskPage == nullptr || MeshBatchTaskPage->NumWorkItems == MaxWorkItemsPerPage)
						{
							FRayTracingMeshBatchTaskPage* NextPage = new(FMemStack::Get()) FRayTracingMeshBatchTaskPage;
							if (MeshBatchTaskHead == nullptr)
							{
								MeshBatchTaskHead = NextPage;
							}
							if (MeshBatchTaskPage)
							{
								MeshBatchTaskPage->Next = NextPage;
							}
							MeshBatchTaskPage = NextPage;
						}

						FRayTracingMeshBatchWorkItem& WorkItem = MeshBatchTaskPage->WorkItems[MeshBatchTaskPage->NumWorkItems];
						MeshBatchTaskPage->NumWorkItems++;

						NumPendingMeshBatches += Instance.GetMaterials().Num();

						if (Instance.OwnsMaterials())
						{
							Swap(WorkItem.MeshBatchesOwned, Instance.Materials);
						}
						else
						{
							WorkItem.MeshBatchesView = Instance.MaterialsView;
						}

						WorkItem.SceneProxy = SceneProxy;
						WorkItem.InstanceIndex = InstanceIndex;
					}
					else
					{
						TArrayView<const FMeshBatch> InstanceMaterials = Instance.GetMaterials();
						for (int32 SegmentIndex = 0; SegmentIndex < InstanceMaterials.Num(); SegmentIndex++)
						{
							const FMeshBatch& MeshBatch = InstanceMaterials[SegmentIndex];
							FDynamicRayTracingMeshCommandContext CommandContext(View.DynamicRayTracingMeshCommandStorage, View.VisibleRayTracingMeshCommands, SegmentIndex, InstanceIndex);
							FMeshPassProcessorRenderState PassDrawRenderState(Scene->UniformBuffers.ViewUniformBuffer);
							FRayTracingMeshProcessor RayTracingMeshProcessor(&CommandContext, Scene, &View, PassDrawRenderState, Scene->CachedRayTracingMeshCommandsMode);
							RayTracingMeshProcessor.AddMeshBatch(MeshBatch, 1, SceneProxy);
						}
					}
				}

				if (CVarRayTracingDynamicGeometryLastRenderTimeUpdateDistance.GetValueOnRenderThread() > 0.0f)
				{
					if (FVector::Distance(SceneProxy->GetActorPosition(), View.ViewMatrices.GetViewOrigin()) < CVarRayTracingDynamicGeometryLastRenderTimeUpdateDistance.GetValueOnRenderThread())
					{
						// Update LastRenderTime for components so that visibility based ticking (like skeletal meshes) can get updated
						// We are only doing this for dynamic geometries now
						SceneInfo->LastRenderTime = CurrentWorldTime;
						SceneInfo->UpdateComponentLastRenderTime(CurrentWorldTime, /*bUpdateLastRenderTimeOnScreen=*/true);
						SceneInfo->ConditionalUpdateUniformBuffer(GraphBuilder.RHICmdList);
					}
				}
			}
		}

		KickRayTracingMeshBatchTask();
	}

	//

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GatherRayTracingWorldInstances_AddInstances);

		const bool bAutoInstance = CVarRayTracingAutoInstance.GetValueOnRenderThread() != 0;

		if (LODTaskList.Num() > 0)
		{
			SCOPED_NAMED_EVENT(WaitForParallelComputeLOD, FColor::Red);
			TRACE_CPUPROFILER_EVENT_SCOPE(WaitForLODTasks);
			FTaskGraphInterface::Get().WaitUntilTasksComplete(LODTaskList, ENamedThreads::GetRenderThread_Local());
		}

		struct FAutoInstanceBatch
		{
			int32 Index = INDEX_NONE;

			// Copies the next InstanceSceneDataOffset and user data into the current batch, returns true if arrays were re-allocated.
			bool Add(FRayTracingScene& RayTracingScene, uint32 InInstanceSceneDataOffset, uint32 InUserData)
			{
				// Adhoc TArray-like resize behavior, in lieu of support for using a custom FMemStackBase in TArray.
				// Idea for future: if batch becomes large enough, we could actually split it into multiple instances to avoid memory waste.

				const bool bNeedReallocation = Cursor == InstanceSceneDataOffsets.Num();

				if (bNeedReallocation)
				{
					int32 PrevCount = InstanceSceneDataOffsets.Num();
					int32 NextCount = FMath::Max(PrevCount * 2, 1);

					TArrayView<uint32> NewInstanceSceneDataOffsets = RayTracingScene.Allocate<uint32>(NextCount);
					if (PrevCount)
					{
						FMemory::Memcpy(NewInstanceSceneDataOffsets.GetData(), InstanceSceneDataOffsets.GetData(), InstanceSceneDataOffsets.GetTypeSize() * InstanceSceneDataOffsets.Num());
					}
					InstanceSceneDataOffsets = NewInstanceSceneDataOffsets;

					TArrayView<uint32> NewUserData = RayTracingScene.Allocate<uint32>(NextCount);
					if (PrevCount)
					{
						FMemory::Memcpy(NewUserData.GetData(), UserData.GetData(), UserData.GetTypeSize() * UserData.Num());
					}
					UserData = NewUserData;
				}

				InstanceSceneDataOffsets[Cursor] = InInstanceSceneDataOffset;
				UserData[Cursor] = InUserData;

				++Cursor;

				return bNeedReallocation;
			}

			bool IsValid() const
			{
				return InstanceSceneDataOffsets.Num() != 0;
			}

			TArrayView<uint32> InstanceSceneDataOffsets;
			TArrayView<uint32> UserData;
			uint32 Cursor = 0;
		};

		Experimental::TSherwoodMap<uint64, FAutoInstanceBatch> InstanceBatches;

		InstanceBatches.Reserve(RelevantPrimitives.Num());

		TArray<FRayTracingCullPrimitiveInstancesClosure> CullInstancesClosures;
		if (View.RayTracingCullingParameters.CullInRayTracing > 0 && GetRayTracingCullingPerInstance())
		{
			CullInstancesClosures.Reserve(RelevantPrimitives.Num());
			View.RayTracingPerInstanceCullingTaskList.Reserve(RelevantPrimitives.Num() / 256 + 1);
		}

		// scan relevant primitives computing hash data to look for duplicate instances
		for (const FRelevantPrimitive& RelevantPrimitive : RelevantPrimitives)
		{
			const int32 PrimitiveIndex = RelevantPrimitive.PrimitiveIndex;
			FPrimitiveSceneInfo* SceneInfo = Scene->Primitives[PrimitiveIndex];
			ERayTracingPrimitiveFlags Flags = Scene->PrimitiveRayTracingFlags[PrimitiveIndex];

			if (EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::CacheInstances))
			{
				// TODO: support GRayTracingExcludeDecals, but not in the form of RayTracingMeshCommand.bDecal as that requires looping over all cached MDCs
				// Instead, either make r.RayTracing.ExcludeDecals read only or request a recache of all ray tracing commands during which decals are excluded

				const int32 NewInstanceIndex = RayTracingScene.Instances.Num();

				// At the moment we only support SM & ISMs on this path
				check(EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::CacheMeshCommands));
				if (SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.Num() > 0 && SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[0].Num() > 0)
				{
					for (int32 CommandIndex : SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[0])
					{
						FVisibleRayTracingMeshCommand NewVisibleMeshCommand;

						NewVisibleMeshCommand.RayTracingMeshCommand = &Scene->CachedRayTracingMeshCommands[CommandIndex];
						NewVisibleMeshCommand.InstanceIndex = NewInstanceIndex;
						View.VisibleRayTracingMeshCommands.Add(NewVisibleMeshCommand);
					}
				}

				checkf(SceneInfo->CachedRayTracingInstance.GeometryRHI, TEXT("Ray tracing instance must have a valid geometry."));
				SceneInfo->UpdateCachedRayTracingInstanceWorldTransforms();
				RayTracingScene.Instances.Add(SceneInfo->CachedRayTracingInstance);

				if (View.RayTracingCullingParameters.CullInRayTracing > 0 && GetRayTracingCullingPerInstance() && SceneInfo->CachedRayTracingInstance.NumTransforms > 1)
				{
					FRayTracingGeometryInstance& NewInstance = RayTracingScene.Instances.Last();

					const bool bIsFarFieldPrimitive = EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::FarField);

					TArrayView<uint32> InstanceActivationMask = RayTracingScene.Allocate<uint32>(FMath::DivideAndRoundUp(NewInstance.NumTransforms, 32u));

					NewInstance.ActivationMask = InstanceActivationMask;

					FRayTracingCullPrimitiveInstancesClosure Closure;
					Closure.Scene = Scene;
					Closure.SceneInfo = SceneInfo;
					Closure.PrimitiveIndex = PrimitiveIndex;
					Closure.bIsFarFieldPrimitive = bIsFarFieldPrimitive;
					Closure.CullingParameters = &View.RayTracingCullingParameters;
					Closure.OutInstanceActivationMask = InstanceActivationMask;

					CullInstancesClosures.Add(MoveTemp(Closure));

					if (CullInstancesClosures.Num() >= 256)
					{
						View.RayTracingPerInstanceCullingTaskList.Add(FFunctionGraphTask::CreateAndDispatchWhenReady([CullInstancesClosures = MoveTemp(CullInstancesClosures)]()
						{
							for (auto& Closure : CullInstancesClosures)
							{
								Closure();
							}
						}, TStatId(), nullptr, ENamedThreads::AnyThread));
					}
				}

				AddDebugRayTracingInstanceFlags(RayTracingScene.Instances.Last().Flags);
			}
			else
			{
				const int8 LODIndex = RelevantPrimitive.LODIndex;

				if (LODIndex < 0 || !RelevantPrimitive.bStatic)
				{
					continue; // skip dynamic primitives and other 
				}

				if ((GRayTracingExcludeDecals && RelevantPrimitive.bAnySegmentsDecal)
					|| (GRayTracingExcludeTranslucent && RelevantPrimitive.bAllSegmentsTranslucent)
					|| (GRayTracingExcludeSky && RelevantPrimitive.bIsSky))
				{
					continue;
				}

				// location if this is a new entry
				const int32 NewInstanceIndex = RayTracingScene.Instances.Num();
				const uint64 InstanceKey = RelevantPrimitive.InstancingKey();

				FAutoInstanceBatch DummyInstanceBatch = { NewInstanceIndex };
				FAutoInstanceBatch& InstanceBatch = bAutoInstance ? InstanceBatches.FindOrAdd(InstanceKey, DummyInstanceBatch) : DummyInstanceBatch;

				if (InstanceBatch.Index != NewInstanceIndex)
				{
					// Reusing a previous entry, just append to the instance list.

					FRayTracingGeometryInstance& RayTracingInstance = RayTracingScene.Instances[InstanceBatch.Index];
					bool bReallocated = InstanceBatch.Add(RayTracingScene, SceneInfo->GetInstanceSceneDataOffset(), (uint32)PrimitiveIndex);

					++RayTracingInstance.NumTransforms;
					check(RayTracingInstance.NumTransforms == InstanceBatch.Cursor); // sanity check

					if (bReallocated)
					{
						RayTracingInstance.InstanceSceneDataOffsets = InstanceBatch.InstanceSceneDataOffsets;
						RayTracingInstance.UserData = InstanceBatch.UserData;
					}
				}
				else
				{
					// Starting new instance batch

					for (int32 CommandIndex : RelevantPrimitive.CachedRayTracingMeshCommandIndices)
					{
						if (CommandIndex >= 0)
						{
							FVisibleRayTracingMeshCommand NewVisibleMeshCommand;

							NewVisibleMeshCommand.RayTracingMeshCommand = &Scene->CachedRayTracingMeshCommands[CommandIndex];
							NewVisibleMeshCommand.InstanceIndex = NewInstanceIndex;
							View.VisibleRayTracingMeshCommands.Add(NewVisibleMeshCommand);
						}
						else
						{
							// CommandIndex == -1 indicates that the mesh batch has been filtered by FRayTracingMeshProcessor (like the shadow depth pass batch)
							// Do nothing in this case
						}
					}

					FRayTracingGeometryInstance& RayTracingInstance = RayTracingScene.Instances.AddDefaulted_GetRef();

					RayTracingInstance.GeometryRHI = RelevantPrimitive.RayTracingGeometryRHI;
					checkf(RayTracingInstance.GeometryRHI, TEXT("Ray tracing instance must have a valid geometry."));

					InstanceBatch.Add(RayTracingScene, SceneInfo->GetInstanceSceneDataOffset(), (uint32)PrimitiveIndex);
					RayTracingInstance.InstanceSceneDataOffsets = InstanceBatch.InstanceSceneDataOffsets;
					RayTracingInstance.UserData = InstanceBatch.UserData;
					RayTracingInstance.NumTransforms = 1;

					RayTracingInstance.Mask = RelevantPrimitive.InstanceMask; // When no cached command is found, InstanceMask == 0 and the instance is effectively filtered out

					if (RelevantPrimitive.bAllSegmentsOpaque)
					{
						RayTracingInstance.Flags |= ERayTracingInstanceFlags::ForceOpaque;
					}
					if (RelevantPrimitive.bTwoSided)
					{
						RayTracingInstance.Flags |= ERayTracingInstanceFlags::TriangleCullDisable;
					}
					AddDebugRayTracingInstanceFlags(RayTracingInstance.Flags);
				}
			}
		}

		View.RayTracingPerInstanceCullingTaskList.Add(FFunctionGraphTask::CreateAndDispatchWhenReady([CullInstancesClosures = MoveTemp(CullInstancesClosures)]()
		{
			for (auto& Closure : CullInstancesClosures)
			{
				Closure();
			}
		}, TStatId(), nullptr, ENamedThreads::AnyThread));
	}

	// Inform the coarse mesh streaming manager about all the used streamable render assets in the scene
	Nanite::FCoarseMeshStreamingManager* CoarseMeshSM = IStreamingManager::Get().GetNaniteCoarseMeshStreamingManager();
	if (CoarseMeshSM)
	{
		CoarseMeshSM->AddUsedStreamingHandles(RayTracingScene.UsedCoarseMeshStreamingHandles);
	}

	return true;
}

static void DeduplicateRayGenerationShaders(TArray< FRHIRayTracingShader*>& RayGenShaders)
{
	TSet<FRHIRayTracingShader*> UniqueRayGenShaders;
	for (FRHIRayTracingShader* Shader : RayGenShaders)
	{
		UniqueRayGenShaders.Add(Shader);
	}
	RayGenShaders = UniqueRayGenShaders.Array();
}

BEGIN_SHADER_PARAMETER_STRUCT(FBuildAccelerationStructurePassParams, )
	RDG_BUFFER_ACCESS(RayTracingSceneScratchBuffer, ERHIAccess::UAVCompute)
	RDG_BUFFER_ACCESS(DynamicGeometryScratchBuffer, ERHIAccess::UAVCompute)
	RDG_BUFFER_ACCESS(RayTracingSceneInstanceBuffer, ERHIAccess::SRVCompute)
END_SHADER_PARAMETER_STRUCT()

bool FDeferredShadingSceneRenderer::SetupRayTracingPipelineStates(FRHICommandListImmediate& RHICmdList)
{
	if (!IsRayTracingEnabled() || Views.Num() == 0)
	{
		return false;
	}

	bool bAnyRayTracingPassEnabled = false;
	for (const FViewInfo& View : Views)
	{
		bAnyRayTracingPassEnabled |= AnyRayTracingPassEnabled(Scene, View);
	}

	if (!bAnyRayTracingPassEnabled)
	{
		return false;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FDeferredShadingSceneRenderer::SetupRayTracingPipelineStates);

	const int32 ReferenceViewIndex = 0;
	FViewInfo& ReferenceView = Views[ReferenceViewIndex];

	if (ReferenceView.AddRayTracingMeshBatchTaskList.Num() > 0)
	{
		SCOPE_CYCLE_COUNTER(STAT_WaitRayTracingAddMesh);

		FTaskGraphInterface::Get().WaitUntilTasksComplete(ReferenceView.AddRayTracingMeshBatchTaskList, ENamedThreads::GetRenderThread_Local());

		for (int32 TaskIndex = 0; TaskIndex < ReferenceView.AddRayTracingMeshBatchTaskList.Num(); TaskIndex++)
		{
			ReferenceView.VisibleRayTracingMeshCommands.Append(*ReferenceView.VisibleRayTracingMeshCommandsPerTask[TaskIndex]);
		}

		ReferenceView.AddRayTracingMeshBatchTaskList.Empty();
	}

	const bool bIsPathTracing = ViewFamily.EngineShowFlags.PathTracing;

	if (GRHISupportsRayTracingShaders)
	{
		// #dxr_todo: UE-72565: refactor ray tracing effects to not be member functions of DeferredShadingRenderer. 
		// Should register each effect at startup and just loop over them automatically to gather all required shaders.

		TArray<FRHIRayTracingShader*> RayGenShaders;

		// We typically see ~120 raygen shaders, but allow some headroom to avoid reallocation if our estimate is wrong.
		RayGenShaders.Reserve(256);

		if (bIsPathTracing)
		{
			// This view only needs the path tracing raygen shaders as all other
			// passes should be disabled.
			PreparePathTracing(ViewFamily, RayGenShaders);
		}
		else
		{
			// Path tracing is disabled, get all other possible raygen shaders
			PrepareRayTracingDebug(ViewFamily, RayGenShaders);

			// These other cases do potentially depend on the camera position since they are
			// driven by FinalPostProcessSettings, which is why we need to merge them across views
			if (!IsForwardShadingEnabled(ShaderPlatform))
			{
				for (const FViewInfo& View : Views)
				{
					PrepareRayTracingReflections(View, *Scene, RayGenShaders);
					PrepareSingleLayerWaterRayTracingReflections(View, *Scene, RayGenShaders);
					PrepareRayTracingShadows(View, RayGenShaders);
					PrepareRayTracingAmbientOcclusion(View, RayGenShaders);
					PrepareRayTracingSkyLight(View, *Scene, RayGenShaders);
					PrepareRayTracingGlobalIllumination(View, RayGenShaders);
					PrepareRayTracingGlobalIlluminationPlugin(View, RayGenShaders);
					PrepareRayTracingTranslucency(View, RayGenShaders);

					if (DoesPlatformSupportLumenGI(ShaderPlatform) && Lumen::UseHardwareRayTracing())
					{
						PrepareLumenHardwareRayTracingScreenProbeGather(View, RayGenShaders);
						PrepareLumenHardwareRayTracingRadianceCache(View, RayGenShaders);
						PrepareLumenHardwareRayTracingTranslucencyVolume(View, RayGenShaders);
						PrepareLumenHardwareRayTracingReflections(View, RayGenShaders);
						PrepareLumenHardwareRayTracingVisualize(View, RayGenShaders);
					}
				}
			}
			DeduplicateRayGenerationShaders(RayGenShaders);
		}

		if (RayGenShaders.Num())
		{
			ReferenceView.RayTracingMaterialPipeline = BindRayTracingMaterialPipeline(RHICmdList, ReferenceView, RayGenShaders);
		}
	}

	// Initialize common resources used for lighting in ray tracing effects

	ReferenceView.RayTracingSubSurfaceProfileTexture = GetSubsurfaceProfileTextureWithFallback();

	ReferenceView.RayTracingSubSurfaceProfileSRV = RHICreateShaderResourceView(ReferenceView.RayTracingSubSurfaceProfileTexture, 0);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		FViewInfo& View = Views[ViewIndex];

		// Send common ray tracing resources from reference view to all others.
		if (ViewIndex != ReferenceViewIndex)
		{
			View.RayTracingSubSurfaceProfileTexture = ReferenceView.RayTracingSubSurfaceProfileTexture;
			View.RayTracingSubSurfaceProfileSRV = ReferenceView.RayTracingSubSurfaceProfileSRV;
			View.RayTracingMaterialPipeline = ReferenceView.RayTracingMaterialPipeline;
		}

		if (bIsPathTracing)
		{
			// Path Tracing currently uses its own code to manage lights, so doesn't need to run this.
			// TODO: merge the lighting representations between ray traced and path traced cases?
		}
		else
		{
			// This light data is a function of the camera position, so must be computed per view.
			View.RayTracingLightData = CreateRayTracingLightData(RHICmdList,
				Scene->Lights, View, EUniformBufferUsage::UniformBuffer_SingleFrame);
		}
	}

	return true;
}

bool FDeferredShadingSceneRenderer::DispatchRayTracingWorldUpdates(FRDGBuilder& GraphBuilder, FRDGBufferRef& OutDynamicGeometryScratchBuffer)
{
	OutDynamicGeometryScratchBuffer = nullptr;

	bool bAnyRayTracingPassEnabled = false;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		bAnyRayTracingPassEnabled |= AnyRayTracingPassEnabled(Scene, Views[ViewIndex]);
	}

	if (!IsRayTracingEnabled() || !bAnyRayTracingPassEnabled || Views.Num() == 0)
	{
		// This needs to happen even when ray tracing is not enabled
		// because importers might batch BVH creation requests that need to be resolved in any case
		GRayTracingGeometryManager.ProcessBuildRequests(GraphBuilder.RHICmdList);
		return false;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FDeferredShadingSceneRenderer::DispatchRayTracingWorldUpdates);

	// Make sure there are no pending skin cache builds and updates anymore:
	// FSkeletalMeshObjectGPUSkin::UpdateDynamicData_RenderThread could have enqueued build operations which might not have
	// been processed by CommitRayTracingGeometryUpdates. 
	// All pending builds should be done before adding them to the top level BVH.
	if (FRayTracingSkinnedGeometryUpdateQueue* RayTracingSkinnedGeometryUpdateQueue = Scene->GetRayTracingSkinnedGeometryUpdateQueue())
	{
		RayTracingSkinnedGeometryUpdateQueue->Commit(GraphBuilder);
	}

	GRayTracingGeometryManager.ProcessBuildRequests(GraphBuilder.RHICmdList);

	const int32 ReferenceViewIndex = 0;
	FViewInfo& ReferenceView = Views[ReferenceViewIndex];
	FRayTracingScene& RayTracingScene = Scene->RayTracingScene;

	if (RayTracingScene.GeometriesToBuild.Num() > 0)
	{
		// Force update all the collected geometries (use stack allocator?)
		GRayTracingGeometryManager.ForceBuildIfPending(GraphBuilder.RHICmdList, RayTracingScene.GeometriesToBuild);
	}

	FTaskGraphInterface::Get().WaitUntilTasksComplete(ReferenceView.RayTracingPerInstanceCullingTaskList, ENamedThreads::GetRenderThread_Local());
	ReferenceView.RayTracingPerInstanceCullingTaskList.Empty();

	RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());

	RayTracingScene.Create(GraphBuilder, Scene->GPUScene);

	const uint32 BLASScratchSize = Scene->GetRayTracingDynamicGeometryCollection()->ComputeScratchBufferSize();
	if (BLASScratchSize > 0)
	{
		const uint32 ScratchAlignment = GRHIRayTracingAccelerationStructureAlignment;
		FRDGBufferDesc ScratchBufferDesc;
		ScratchBufferDesc.UnderlyingType = FRDGBufferDesc::EUnderlyingType::StructuredBuffer;
		ScratchBufferDesc.Usage = BUF_RayTracingScratch;
		ScratchBufferDesc.BytesPerElement = ScratchAlignment;
		ScratchBufferDesc.NumElements = FMath::DivideAndRoundUp(BLASScratchSize, ScratchAlignment);

		OutDynamicGeometryScratchBuffer = GraphBuilder.CreateBuffer(ScratchBufferDesc, TEXT("DynamicGeometry.BLASSharedScratchBuffer"));
	}

	const bool bRayTracingAsyncBuild = CVarRayTracingAsyncBuild.GetValueOnRenderThread() != 0 && GRHISupportsRayTracingAsyncBuildAccelerationStructure;
	const ERDGPassFlags ComputePassFlags = bRayTracingAsyncBuild ? ERDGPassFlags::AsyncCompute : ERDGPassFlags::Compute;

	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingScene);

		FBuildAccelerationStructurePassParams* PassParams = GraphBuilder.AllocParameters<FBuildAccelerationStructurePassParams>();
		PassParams->RayTracingSceneScratchBuffer = Scene->RayTracingScene.BuildScratchBuffer;
		PassParams->RayTracingSceneInstanceBuffer = Scene->RayTracingScene.InstanceBuffer;
		PassParams->DynamicGeometryScratchBuffer = OutDynamicGeometryScratchBuffer;

		// Use ERDGPassFlags::NeverParallel so the pass never runs off the render thread and we always get the following order of execution on the CPU:
		// BuildTLASInstanceBuffer, RayTracingScene, EndUpdate, ..., ReleaseRayTracingResources		
		GraphBuilder.AddPass(RDG_EVENT_NAME("RayTracingScene"), PassParams, ComputePassFlags | ERDGPassFlags::NeverCull | ERDGPassFlags::NeverParallel,
			[this, PassParams, bRayTracingAsyncBuild](FRHIComputeCommandList& RHICmdList)
		{
			FRHIBuffer* DynamicGeometryScratchBuffer = PassParams->DynamicGeometryScratchBuffer ? PassParams->DynamicGeometryScratchBuffer->GetRHI() : nullptr;
			Scene->GetRayTracingDynamicGeometryCollection()->DispatchUpdates(RHICmdList, DynamicGeometryScratchBuffer);

			FRHIRayTracingScene* RayTracingSceneRHI = Scene->RayTracingScene.GetRHIRayTracingSceneChecked();
			FRHIBuffer* AccelerationStructureBuffer = Scene->RayTracingScene.GetBufferChecked();
			FRHIBuffer* ScratchBuffer = PassParams->RayTracingSceneScratchBuffer->GetRHI();
			FRHIBuffer* InstanceBuffer = PassParams->RayTracingSceneInstanceBuffer->GetRHI();

			FRayTracingSceneBuildParams BuildParams;
			BuildParams.Scene = RayTracingSceneRHI;
			BuildParams.ScratchBuffer = ScratchBuffer;
			BuildParams.ScratchBufferOffset = 0;
			BuildParams.InstanceBuffer = InstanceBuffer;
			BuildParams.InstanceBufferOffset = 0;

			// Sanity check acceleration structure buffer sizes
		#if DO_CHECK
			{
				FRayTracingAccelerationStructureSize SizeInfo = RHICalcRayTracingSceneSize(
					RayTracingSceneRHI->GetInitializer().NumNativeInstances, ERayTracingAccelerationStructureFlags::FastTrace);

				check(SizeInfo.ResultSize <= Scene->RayTracingScene.SizeInfo.ResultSize);
				check(SizeInfo.BuildScratchSize <= Scene->RayTracingScene.SizeInfo.BuildScratchSize);
				check(SizeInfo.ResultSize <= AccelerationStructureBuffer->GetSize());
				check(SizeInfo.BuildScratchSize <= ScratchBuffer->GetSize());
			}
		#endif // DO_CHECK

			RHICmdList.BindAccelerationStructureMemory(RayTracingSceneRHI, AccelerationStructureBuffer, 0);
			RHICmdList.BuildAccelerationStructure(BuildParams);

			if (!bRayTracingAsyncBuild)
			{
				// Submit potentially expensive BVH build commands to the GPU as soon as possible.
				// Avoids a GPU bubble in some CPU-limited cases.
				RHICmdList.SubmitCommandsHint();
			}
		});
	}

	AddPass(GraphBuilder, RDG_EVENT_NAME("EndUpdate"), [this](FRHICommandListImmediate& RHICmdList)
	{
		Scene->GetRayTracingDynamicGeometryCollection()->EndUpdate(RHICmdList);
	});

	return true;
}

static void ReleaseRaytracingResources(FRDGBuilder& GraphBuilder, TArrayView<FViewInfo> Views, FRayTracingScene &RayTracingScene)
{
	AddPass(GraphBuilder, RDG_EVENT_NAME("ReleaseRayTracingResources"), [Views, &RayTracingScene](FRHICommandListImmediate& RHICmdList)
	{
		if (RayTracingScene.IsCreated())
		{
			RHICmdList.ClearRayTracingBindings(RayTracingScene.GetRHIRayTracingScene());

			// If we did not end up rendering anything this frame, then release all ray tracing scene resources.
			if (RayTracingScene.Instances.Num() == 0)
			{
				RayTracingScene.ResetAndReleaseResources();
			}
		}

		// Release resources that were bound to the ray tracing scene to allow them to be immediately recycled.
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			FViewInfo& View = Views[ViewIndex];

			// Release common lighting resources
			View.RayTracingSubSurfaceProfileSRV.SafeRelease();
			View.RayTracingSubSurfaceProfileTexture.SafeRelease();

			View.RayTracingLightData.LightBufferSRV.SafeRelease();
			View.RayTracingLightData.LightBuffer.SafeRelease();
			View.RayTracingLightData.LightCullVolumeSRV.SafeRelease();
			View.RayTracingLightData.LightCullVolume.SafeRelease();
			View.RayTracingLightData.LightIndices.Release();
			View.RayTracingLightData.UniformBuffer.SafeRelease();
		}
	});
}

void FDeferredShadingSceneRenderer::WaitForRayTracingScene(FRDGBuilder& GraphBuilder, FRDGBufferRef DynamicGeometryScratchBuffer)
{
	bool bAnyRayTracingPassEnabled = false;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		bAnyRayTracingPassEnabled |= AnyRayTracingPassEnabled(Scene, Views[ViewIndex]);
	}

	if (!bAnyRayTracingPassEnabled)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FDeferredShadingSceneRenderer::WaitForRayTracingScene);

	RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());

	SetupRayTracingPipelineStates(GraphBuilder.RHICmdList);

	bool bAnyInlineRayTracingPassEnabled = false;
	for (const FViewInfo& View : Views)
	{
		bAnyInlineRayTracingPassEnabled |= Lumen::AnyLumenHardwareInlineRayTracingPassEnabled(Scene, View);
	}

	if (bAnyInlineRayTracingPassEnabled)
	{
		const int32 ReferenceViewIndex = 0;
		FViewInfo& ReferenceView = Views[ReferenceViewIndex];

		SetupLumenHardwareRayTracingHitGroupBuffer(ReferenceView);
	}

	// Scratch buffer must be referenced in this pass, as it must live until the BVH build is complete.
	FBuildAccelerationStructurePassParams* PassParams = GraphBuilder.AllocParameters<FBuildAccelerationStructurePassParams>();
	PassParams->RayTracingSceneScratchBuffer = Scene->RayTracingScene.BuildScratchBuffer;
	PassParams->DynamicGeometryScratchBuffer = DynamicGeometryScratchBuffer;

	GraphBuilder.AddPass(RDG_EVENT_NAME("WaitForRayTracingScene"), PassParams, ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		[this, PassParams](FRHICommandListImmediate& RHICmdList)
	{
		const int32 ReferenceViewIndex = 0;
		FViewInfo& ReferenceView = Views[ReferenceViewIndex];

		const bool bIsPathTracing = ViewFamily.EngineShowFlags.PathTracing;

		check(ReferenceView.RayTracingMaterialPipeline || ReferenceView.RayTracingMaterialBindings.Num() == 0);

		if (ReferenceView.RayTracingMaterialPipeline && ReferenceView.RayTracingMaterialBindings.Num())
		{
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(ReferenceView.RayTracingMaterialBindingsTask, ENamedThreads::GetRenderThread_Local());

			// Gather bindings from all chunks and submit them all as a single batch to allow RHI to bind all shader parameters in parallel.

			uint32 NumTotalBindings = 0;

			for (FRayTracingLocalShaderBindingWriter* BindingWriter : ReferenceView.RayTracingMaterialBindings)
			{
				const FRayTracingLocalShaderBindingWriter::FChunk* Chunk = BindingWriter->GetFirstChunk();
				while (Chunk)
				{
					NumTotalBindings += Chunk->Num;
					Chunk = Chunk->Next;
				}
			}

			const uint32 MergedBindingsSize = sizeof(FRayTracingLocalShaderBindings) * NumTotalBindings;
			FRayTracingLocalShaderBindings* MergedBindings = (FRayTracingLocalShaderBindings*)(RHICmdList.Bypass()
				? FMemStack::Get().Alloc(MergedBindingsSize, alignof(FRayTracingLocalShaderBindings))
				: RHICmdList.Alloc(MergedBindingsSize, alignof(FRayTracingLocalShaderBindings)));

			uint32 MergedBindingIndex = 0;
			for (FRayTracingLocalShaderBindingWriter* BindingWriter : ReferenceView.RayTracingMaterialBindings)
			{
				const FRayTracingLocalShaderBindingWriter::FChunk* Chunk = BindingWriter->GetFirstChunk();
				while (Chunk)
				{
					const uint32 Num = Chunk->Num;
					for (uint32_t i = 0; i < Num; ++i)
					{
						MergedBindings[MergedBindingIndex] = Chunk->Bindings[i];
						MergedBindingIndex++;
					}
					Chunk = Chunk->Next;
				}
			}

			const bool bCopyDataToInlineStorage = false; // Storage is already allocated from RHICmdList, no extra copy necessary
			RHICmdList.SetRayTracingHitGroups(
				ReferenceView.GetRayTracingSceneChecked(),
				ReferenceView.RayTracingMaterialPipeline,
				NumTotalBindings, MergedBindings,
				bCopyDataToInlineStorage);

			if (!bIsPathTracing)
			{
				TArray<FRHIRayTracingShader*> DeferredMaterialRayGenShaders;
				if (!IsForwardShadingEnabled(ShaderPlatform))
				{
					for (const FViewInfo& View : Views)
					{
						PrepareRayTracingReflectionsDeferredMaterial(View, *Scene, DeferredMaterialRayGenShaders);
						PrepareRayTracingDeferredReflectionsDeferredMaterial(View, *Scene, DeferredMaterialRayGenShaders);
						PrepareRayTracingGlobalIlluminationDeferredMaterial(View, DeferredMaterialRayGenShaders);
						if (DoesPlatformSupportLumenGI(ShaderPlatform))
						{
							PrepareLumenHardwareRayTracingReflectionsDeferredMaterial(View, DeferredMaterialRayGenShaders);
							PrepareLumenHardwareRayTracingRadianceCacheDeferredMaterial(View, DeferredMaterialRayGenShaders);
							PrepareLumenHardwareRayTracingScreenProbeGatherDeferredMaterial(View, DeferredMaterialRayGenShaders);
							PrepareLumenHardwareRayTracingVisualizeDeferredMaterial(View, DeferredMaterialRayGenShaders);
						}
					}
				}
				DeduplicateRayGenerationShaders(DeferredMaterialRayGenShaders);

				if (DeferredMaterialRayGenShaders.Num())
				{
					ReferenceView.RayTracingMaterialGatherPipeline = BindRayTracingDeferredMaterialGatherPipeline(RHICmdList, ReferenceView, DeferredMaterialRayGenShaders);
				}

				// Add Lumen hardware ray tracing materials
				TArray<FRHIRayTracingShader*> LumenHardwareRayTracingRayGenShaders;
				if (DoesPlatformSupportLumenGI(ShaderPlatform))
				{
					for (const FViewInfo& View : Views)
					{
						PrepareLumenHardwareRayTracingVisualizeLumenMaterial(View, LumenHardwareRayTracingRayGenShaders);
						PrepareLumenHardwareRayTracingRadianceCacheLumenMaterial(View, LumenHardwareRayTracingRayGenShaders);
						PrepareLumenHardwareRayTracingTranslucencyVolume(View, LumenHardwareRayTracingRayGenShaders);
						PrepareLumenHardwareRayTracingRadiosityLumenMaterial(View, LumenHardwareRayTracingRayGenShaders);
						PrepareLumenHardwareRayTracingReflectionsLumenMaterial(View, LumenHardwareRayTracingRayGenShaders);
						PrepareLumenHardwareRayTracingScreenProbeGatherLumenMaterial(View, LumenHardwareRayTracingRayGenShaders);
						PrepareLumenHardwareRayTracingDirectLightingLumenMaterial(View, LumenHardwareRayTracingRayGenShaders);
					}
				}
				DeduplicateRayGenerationShaders(DeferredMaterialRayGenShaders);

				if (LumenHardwareRayTracingRayGenShaders.Num())
				{
					ReferenceView.LumenHardwareRayTracingMaterialPipeline = BindLumenHardwareRayTracingMaterialPipeline(RHICmdList, ReferenceView, LumenHardwareRayTracingRayGenShaders, ReferenceView.LumenHardwareRayTracingHitDataBuffer);
				}
			}

			// Move the ray tracing binding container ownership to the command list, so that memory will be
			// released on the RHI thread timeline, after the commands that reference it are processed.
			RHICmdList.EnqueueLambda([Ptrs = MoveTemp(ReferenceView.RayTracingMaterialBindings)](FRHICommandListImmediate&)
			{
				for (auto Ptr : Ptrs)
				{
					delete Ptr;
				}
			});

			// Send ray tracing resources from reference view to all others.
			for (int32 ViewIndex = 1; ViewIndex < Views.Num(); ++ViewIndex)
			{
				FViewInfo& View = Views[ViewIndex];
				View.RayTracingMaterialGatherPipeline = ReferenceView.RayTracingMaterialGatherPipeline;
				View.LumenHardwareRayTracingMaterialPipeline = ReferenceView.LumenHardwareRayTracingMaterialPipeline;
			}

			if (!bIsPathTracing)
			{
				SetupRayTracingLightingMissShader(RHICmdList, ReferenceView);
			}
		}

		if (RayTracingDynamicGeometryUpdateEndTransition)
		{
			RHICmdList.EndTransition(RayTracingDynamicGeometryUpdateEndTransition);
			RayTracingDynamicGeometryUpdateEndTransition = nullptr;
		}

		FRHIRayTracingScene* RayTracingScene = ReferenceView.GetRayTracingSceneChecked();
		RHICmdList.Transition(FRHITransitionInfo(RayTracingScene, ERHIAccess::BVHWrite, ERHIAccess::BVHRead));

		if (ReferenceView.LumenHardwareRayTracingHitDataBuffer)
		{
			RHICmdList.Transition(FRHITransitionInfo(ReferenceView.LumenHardwareRayTracingHitDataBuffer, ERHIAccess::None, ERHIAccess::SRVMask));
		}			
	});
}

#endif // RHI_RAYTRACING

static TAutoConsoleVariable<float> CVarStallInitViews(
	TEXT("CriticalPathStall.AfterInitViews"),
	0.0f,
	TEXT("Sleep for the given time after InitViews. Time is given in ms. This is a debug option used for critical path analysis and forcing a change in the critical path."));

void FDeferredShadingSceneRenderer::CommitFinalPipelineState()
{
	ViewPipelineStates.SetNum(Views.Num());

	// Family pipeline state
	{
		FamilyPipelineState.Set(&FFamilyPipelineState::bNanite, UseNanite(ShaderPlatform)); // TODO: Should this respect ViewFamily.EngineShowFlags.NaniteMeshes?

		static const auto ICVarHZBOcc = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HZBOcclusion"));
		FamilyPipelineState.Set(&FFamilyPipelineState::bHZBOcclusion, ICVarHZBOcc->GetInt() != 0);	
	}

	CommitIndirectLightingState();

	// Views pipeline states
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		TPipelineState<FPerViewPipelineState>& ViewPipelineState = ViewPipelineStates[ViewIndex];

		// Commit HZB state
		{
			const bool bHasSSGI = ViewPipelineState[&FPerViewPipelineState::DiffuseIndirectMethod] == EDiffuseIndirectMethod::SSGI;
			const bool bUseLumen = ViewPipelineState[&FPerViewPipelineState::DiffuseIndirectMethod] == EDiffuseIndirectMethod::Lumen 
				|| ViewPipelineState[&FPerViewPipelineState::ReflectionsMethod] == EReflectionsMethod::Lumen;

			// Requires FurthestHZB
			ViewPipelineState.Set(&FPerViewPipelineState::bFurthestHZB,
				FamilyPipelineState[&FFamilyPipelineState::bHZBOcclusion] ||
				FamilyPipelineState[&FFamilyPipelineState::bNanite] ||
				ViewPipelineState[&FPerViewPipelineState::bUseLumenProbeHierarchy] ||
				ViewPipelineState[&FPerViewPipelineState::AmbientOcclusionMethod] == EAmbientOcclusionMethod::SSAO ||
				ViewPipelineState[&FPerViewPipelineState::ReflectionsMethod] == EReflectionsMethod::SSR ||
				bHasSSGI || bUseLumen);

			ViewPipelineState.Set(&FPerViewPipelineState::bClosestHZB, 
				bHasSSGI || bUseLumen);
		}
	}

	// Commit all the pipeline states.
	{
		for (TPipelineState<FPerViewPipelineState>& ViewPipelineState : ViewPipelineStates)
		{
			ViewPipelineState.Commit();
		}
		FamilyPipelineState.Commit();
	} 
}

bool FDeferredShadingSceneRenderer::IsNaniteEnabled() const
{
	return UseNanite(ShaderPlatform) && ViewFamily.EngineShowFlags.NaniteMeshes && Nanite::GStreamingManager.HasResourceEntries();
}

#if WITH_MGPU
BEGIN_SHADER_PARAMETER_STRUCT(FGBufferTemporalTextureParams, )
	RDG_TEXTURE_ACCESS(DepthResolve, ERHIAccess::CopySrc)
	RDG_TEXTURE_ACCESS(GBufferA, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()
#endif

void FDeferredShadingSceneRenderer::Render(FRDGBuilder& GraphBuilder)
{
	const bool bNaniteEnabled = IsNaniteEnabled();

	GPU_MESSAGE_SCOPE(GraphBuilder);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

		ShaderPrint::BeginView(GraphBuilder, View);
		ShaderDrawDebug::BeginView(GraphBuilder, View);
		ShadingEnergyConservation::Init(GraphBuilder, View);
	}
	Scene->UpdateAllPrimitiveSceneInfos(GraphBuilder, true);

#if RHI_RAYTRACING
	// Now that we have updated all the PrimitiveSceneInfos, update the RayTracing mesh commands cache if needed
	{
		ERayTracingMeshCommandsMode CurrentMode = ViewFamily.EngineShowFlags.PathTracing ? ERayTracingMeshCommandsMode::PATH_TRACING : ERayTracingMeshCommandsMode::RAY_TRACING;
		bool bNaniteCoarseMeshStreamingModeChanged = false;
#if WITH_EDITOR
		bNaniteCoarseMeshStreamingModeChanged = Nanite::FCoarseMeshStreamingManager::CheckStreamingMode();
#endif // WITH_EDITOR

		if (CurrentMode != Scene->CachedRayTracingMeshCommandsMode || bNaniteCoarseMeshStreamingModeChanged)
		{
			// If we change to or from a path traced render, we need to refresh the cached ray tracing mesh commands
			// because they contain data about the currently bound shader. This operation is a bit expensive but
			// only happens once as we transition between modes which should be rare.
			Scene->CachedRayTracingMeshCommandsMode = CurrentMode;
			Scene->RefreshRayTracingMeshCommandCache();
		}

		static FVector FarFieldReferencePosLast = Lumen::GetFarFieldReferencePos();
		FVector FarFieldReferencePos = Lumen::GetFarFieldReferencePos();
		if (FarFieldReferencePosLast.Z != FarFieldReferencePos.Z)
		{
			FarFieldReferencePosLast = FarFieldReferencePos;
			Scene->RefreshRayTracingInstances();
		}

	}
#endif


	FGPUSceneScopeBeginEndHelper GPUSceneScopeBeginEndHelper(Scene->GPUScene, GPUSceneDynamicContext, Scene);

	bool bUpdateNaniteStreaming = false;
	bool bVisualizeNanite = false;
	if (bNaniteEnabled)
	{
		Nanite::GGlobalResources.Update(GraphBuilder);

		// Only update Nanite streaming residency for the first view when multiple view rendering (nDisplay) is enabled.
		// Streaming requests are still accumulated from the remaining views.
		bUpdateNaniteStreaming =  !ViewFamily.bIsMultipleViewFamily || ViewFamily.bIsFirstViewInMultipleViewFamily;
		if(bUpdateNaniteStreaming)
		{
			Nanite::GStreamingManager.BeginAsyncUpdate(GraphBuilder);
		}

		FNaniteVisualizationData& NaniteVisualization = GetNaniteVisualizationData();
		if (Views.Num() > 0)
		{
			const FName& NaniteViewMode = Views[0].CurrentNaniteVisualizationMode;
			if (NaniteVisualization.Update(NaniteViewMode))
			{
				// When activating the view modes from the command line, automatically enable the VisualizeNanite show flag for convenience.
				ViewFamily.EngineShowFlags.SetVisualizeNanite(true);
			}
			bVisualizeNanite = NaniteVisualization.IsActive() && ViewFamily.EngineShowFlags.VisualizeNanite;
		}
	}

	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderOther);

	// Setups the final FViewInfo::ViewRect.
	PrepareViewRectsForRendering(GraphBuilder.RHICmdList);

	if (ShouldRenderSkyAtmosphere(Scene, ViewFamily.EngineShowFlags))
	{
		for (int32 LightIndex = 0; LightIndex < NUM_ATMOSPHERE_LIGHTS; ++LightIndex)
		{
			if (Scene->AtmosphereLights[LightIndex])
			{
				PrepareSunLightProxy(*Scene->GetSkyAtmosphereSceneInfo(),LightIndex, *Scene->AtmosphereLights[LightIndex]);
			}
		}
	}
	else
	{
		Scene->ResetAtmosphereLightsProperties();
	}

	SCOPED_NAMED_EVENT(FDeferredShadingSceneRenderer_Render, FColor::Emerald);

#if WITH_MGPU
	const FRHIGPUMask RenderTargetGPUMask = ComputeGPUMasks(GraphBuilder.RHICmdList);
#endif // WITH_MGPU

	// By default, limit our GPU usage to only GPUs specified in the view masks.
	RDG_GPU_MASK_SCOPE(GraphBuilder, AllViewsGPUMask);

	WaitOcclusionTests(GraphBuilder.RHICmdList);

	if (!ViewFamily.EngineShowFlags.Rendering)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "Scene");
	RDG_GPU_STAT_SCOPE_VERBOSE(GraphBuilder, Unaccounted, *ViewFamily.ProfileDescription);
	
	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_Render_Init);
		RDG_RHI_GPU_STAT_SCOPE(GraphBuilder, AllocateRendertargets);

		// Initialize global system textures (pass-through if already initialized).
		GSystemTextures.InitializeTextures(GraphBuilder.RHICmdList, FeatureLevel);

		// Force the subsurface profile texture to be updated.
		UpdateSubsurfaceProfileTexture(GraphBuilder, ShaderPlatform);
	}

	const FSceneTexturesConfig SceneTexturesConfig = FSceneTexturesConfig::Create(ViewFamily);
	FSceneTexturesConfig::Set(SceneTexturesConfig);

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Create(GraphBuilder);

	const bool bHasRayTracedOverlay = HasRayTracedOverlay(ViewFamily);
	const bool bAllowStaticLighting = !bHasRayTracedOverlay && IsStaticLightingAllowed();

	const bool bUseVirtualTexturing = UseVirtualTexturing(FeatureLevel);
	if (bUseVirtualTexturing)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, VirtualTextureUpdate);
		// AllocateResources needs to be called before RHIBeginScene
		FVirtualTextureSystem::Get().AllocateResources(GraphBuilder, FeatureLevel);
		FVirtualTextureSystem::Get().CallPendingCallbacks();
		VirtualTextureFeedbackBegin(GraphBuilder, Views, SceneTexturesConfig.Extent);
	}

	// Important that this uses consistent logic throughout the frame, so evaluate once and pass in the flag from here
	// NOTE: Must be done after  system texture initialization
	VirtualShadowMapArray.Initialize(GraphBuilder, Scene->VirtualShadowMapArrayCacheManager, UseVirtualShadowMaps(ShaderPlatform, FeatureLevel));

	// if DDM_AllOpaqueNoVelocity was used, then velocity should have already been rendered as well
	const bool bIsEarlyDepthComplete = (DepthPass.EarlyZPassMode == DDM_AllOpaque || DepthPass.EarlyZPassMode == DDM_AllOpaqueNoVelocity);

	// Use read-only depth in the base pass if we have a full depth prepass.
	const bool bAllowReadOnlyDepthBasePass = bIsEarlyDepthComplete
		&& !ViewFamily.EngineShowFlags.ShaderComplexity
		&& !ViewFamily.UseDebugViewPS()
		&& !ViewFamily.EngineShowFlags.Wireframe
		&& !ViewFamily.EngineShowFlags.LightMapDensity;

	const FExclusiveDepthStencil::Type BasePassDepthStencilAccess =
		bAllowReadOnlyDepthBasePass
		? FExclusiveDepthStencil::DepthRead_StencilWrite
		: FExclusiveDepthStencil::DepthWrite_StencilWrite;

	FILCUpdatePrimTaskData ILCTaskData;

	// Find the visible primitives.
	GraphBuilder.RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	FInstanceCullingManager& InstanceCullingManager = *GraphBuilder.AllocObject<FInstanceCullingManager>(Scene->GPUScene.IsEnabled(), GraphBuilder);

	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, VisibilityCommands);
		InitViews(GraphBuilder, SceneTexturesConfig, BasePassDepthStencilAccess, ILCTaskData, InstanceCullingManager);
	}

	// Compute & commit the final state of the entire dependency topology of the renderer.
	CommitFinalPipelineState();

#if !UE_BUILD_SHIPPING
	if (CVarStallInitViews.GetValueOnRenderThread() > 0.0f)
	{
		SCOPE_CYCLE_COUNTER(STAT_InitViews_Intentional_Stall);
		FPlatformProcess::Sleep(CVarStallInitViews.GetValueOnRenderThread() / 1000.0f);
	}
#endif

	extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

	for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
	{
		Extension->BeginFrame();

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			// Must happen before RHI thread flush so any tasks we dispatch here can land in the idle gap during the flush
			Extension->PrepareView(&Views[ViewIndex]);
		}
	}

#if RHI_RAYTRACING

	// Gather mesh instances, shaders, resources, parameters, etc. and build ray tracing acceleration structure

	FRayTracingScene& RayTracingScene = Scene->RayTracingScene;
	RayTracingScene.Reset(); // Resets the internal arrays, but does not release any resources.

	const int32 ReferenceViewIndex = 0;
	FViewInfo& ReferenceView = Views[ReferenceViewIndex];

	// Prepare the scene for rendering this frame.
	GatherRayTracingWorldInstancesForView(GraphBuilder, ReferenceView, RayTracingScene);

#endif // RHI_RAYTRACING

	// Dynamic vertex and index buffers need to be committed before rendering.
	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_FGlobalDynamicVertexBuffer_Commit);
		DynamicIndexBufferForInitViews.Commit();
		DynamicVertexBufferForInitViews.Commit();
		DynamicReadBufferForInitViews.Commit();
	}

	// Notify the FX system that the scene is about to be rendered.
	if (FXSystem && Views.IsValidIndex(0))
	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_FXSystem_PreRender);
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_FXPreRender));
		FXSystem->PreRender(GraphBuilder, Views, true /*bAllowGPUParticleUpdate*/);
		if (FGPUSortManager* GPUSortManager = FXSystem->GetGPUSortManager())
		{
			GPUSortManager->OnPreRender(GraphBuilder);
		}
	}

	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, GPUSceneUpdate);

		if (!ViewFamily.bIsRenderedImmediatelyAfterAnotherViewFamily)
		{
			GraphBuilder.SetFlushResourcesRHI();
		}

		Scene->GPUScene.Update(GraphBuilder, *Scene);

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			FViewInfo& View = Views[ViewIndex];
			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

			Scene->GPUScene.UploadDynamicPrimitiveShaderDataForView(GraphBuilder, Scene, View);

			Scene->GPUScene.DebugRender(GraphBuilder, *Scene, View);
		}

		InstanceCullingManager.BeginDeferredCulling(GraphBuilder, Scene->GPUScene);

		if (Views.Num() > 0)
		{
			FViewInfo& View = Views[0];
			Scene->UpdatePhysicsField(GraphBuilder, View);
		}
	}

	FSceneTextures& SceneTextures = FSceneTextures::Create(GraphBuilder, SceneTexturesConfig); 

	// Note, should happen after the GPU-Scene update to ensure rendering to runtime virtual textures is using the correctly updated scene
	if (bUseVirtualTexturing)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, VirtualTextureUpdate);
		FVirtualTextureSystem::Get().Update(GraphBuilder, FeatureLevel, Scene);
	}

	const bool bUseGBuffer = IsUsingGBuffers(ShaderPlatform);
	
	const bool bRenderDeferredLighting = ViewFamily.EngineShowFlags.Lighting
		&& FeatureLevel >= ERHIFeatureLevel::SM5
		&& ViewFamily.EngineShowFlags.DeferredLighting
		&& bUseGBuffer
		&& !bHasRayTracedOverlay;

	bool bComputeLightGrid = false;
	bool bAnyLumenEnabled = false;
	// Simple forward shading doesn't support local lights. No need to compute light grid
	if (!IsSimpleForwardShadingEnabled(ShaderPlatform))
	{
		if (bUseGBuffer)
		{
			bComputeLightGrid = bRenderDeferredLighting;
		}
		else
		{
			bComputeLightGrid = ViewFamily.EngineShowFlags.Lighting;
		}

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			FViewInfo& View = Views[ViewIndex];
			bAnyLumenEnabled = bAnyLumenEnabled 
				|| GetViewPipelineState(View).DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen
				|| GetViewPipelineState(View).ReflectionsMethod == EReflectionsMethod::Lumen;
		}

		bComputeLightGrid |= (
			ShouldRenderVolumetricFog() ||
			VolumetricCloudWantsToSampleLocalLights(Scene, ViewFamily.EngineShowFlags) ||
			ViewFamily.ViewMode != VMI_Lit ||
			bAnyLumenEnabled ||
			VirtualShadowMapArray.IsEnabled());
	}

	// force using occ queries for wireframe if rendering is parented or frozen in the first view
	check(Views.Num());
	#if (UE_BUILD_SHIPPING || UE_BUILD_TEST)
		const bool bIsViewFrozen = false;
		const bool bHasViewParent = false;
	#else
		const bool bIsViewFrozen = Views[0].State && ((FSceneViewState*)Views[0].State)->bIsFrozen;
		const bool bHasViewParent = Views[0].State && ((FSceneViewState*)Views[0].State)->HasViewParent();
	#endif

	
	const bool bIsOcclusionTesting = DoOcclusionQueries() && !ViewFamily.EngineShowFlags.DisableOcclusionQueries
		&& (!ViewFamily.EngineShowFlags.Wireframe || bIsViewFrozen || bHasViewParent);
	const bool bNeedsPrePass = ShouldRenderPrePass();

	GEngine->GetPreRenderDelegateEx().Broadcast(GraphBuilder);

	// Strata initialisation is always run even when not enabled.
	const bool bStrataEnabled = Strata::IsStrataEnabled();
	Strata::InitialiseStrataFrameSceneData(*this, GraphBuilder);

	if (DepthPass.IsComputeStencilDitherEnabled())
	{
		AddDitheredStencilFillPass(GraphBuilder, Views, SceneTextures.Depth.Target, DepthPass);
	}

	AddPass(GraphBuilder, RDG_EVENT_NAME("GPUSkinCache::Transitions"), [this](FRHICommandList& InRHICmdList)
	{
		RunGPUSkinCacheTransition(InRHICmdList, Scene, EGPUSkinCacheTransition::Renderer);
	});

	FHairStrandsBookmarkParameters& HairStrandsBookmarkParameters = *GraphBuilder.AllocObject<FHairStrandsBookmarkParameters>();
	if (IsHairStrandsEnabled(EHairStrandsShaderType::All, Scene->GetShaderPlatform()))
	{
		HairStrandsBookmarkParameters = CreateHairStrandsBookmarkParameters(Scene, Views[0]);
		RunHairStrandsBookmark(GraphBuilder, EHairStrandsBookmark::ProcessTasks, HairStrandsBookmarkParameters);

		// Interpolation needs to happen after the skin cache run as there is a dependency 
		// on the skin cache output.
		const bool bRunHairStrands = HairStrandsBookmarkParameters.HasInstances() && (Views.Num() > 0);
		if (bRunHairStrands)
		{
			if (IsHairStrandsEnabled(EHairStrandsShaderType::Strands, Scene->GetShaderPlatform()))
			{
				RunHairStrandsBookmark(GraphBuilder, EHairStrandsBookmark::ProcessGatherCluster, HairStrandsBookmarkParameters);

				FHairCullingParams CullingParams;
				CullingParams.bCullingProcessSkipped = false;
				ComputeHairStrandsClustersCulling(GraphBuilder, *HairStrandsBookmarkParameters.ShaderMap, Views, CullingParams, HairStrandsBookmarkParameters.HairClusterData);
			}

			RunHairStrandsBookmark(GraphBuilder, EHairStrandsBookmark::ProcessStrandsInterpolation, HairStrandsBookmarkParameters);
		}
		else
		{
			for (FViewInfo& View : Views)
			{
				View.HairStrandsViewData.UniformBuffer = HairStrands::CreateDefaultHairStrandsViewUniformBuffer(GraphBuilder, View);
			}
		}
	}

	if (bNaniteEnabled)
	{
		Nanite::ListStatFilters(this);

		// Must happen before any Nanite rendering in the frame
		if (bUpdateNaniteStreaming)
		{
			Nanite::GStreamingManager.EndAsyncUpdate(GraphBuilder);
		}
	}

	{
		RDG_RHI_GPU_STAT_SCOPE(GraphBuilder, GPUSceneUpdate);
		PrepareDistanceFieldScene(GraphBuilder, false);
	}

	const bool bShouldRenderVelocities = ShouldRenderVelocities();
	const bool bBasePassCanOutputVelocity = FVelocityRendering::BasePassCanOutputVelocity(FeatureLevel);
	const bool bUseSelectiveBasePassOutputs = IsUsingSelectiveBasePassOutputs(ShaderPlatform);
	const bool bHairStrandsEnable = HairStrandsBookmarkParameters.HasInstances() && Views.Num() > 0 && IsHairStrandsEnabled(EHairStrandsShaderType::Strands, Views[0].GetShaderPlatform());

	{
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_PrePass));

		// Both compute approaches run earlier, so skip clearing stencil here, just load existing.
		const ERenderTargetLoadAction StencilLoadAction = DepthPass.IsComputeStencilDitherEnabled()
			? ERenderTargetLoadAction::ELoad
			: ERenderTargetLoadAction::EClear;

		const ERenderTargetLoadAction DepthLoadAction = ERenderTargetLoadAction::EClear;
		AddClearDepthStencilPass(GraphBuilder, SceneTextures.Depth.Target, DepthLoadAction, StencilLoadAction);

		// Draw the scene pre-pass / early z pass, populating the scene depth buffer and HiZ
		//开启PrePass需要满足以下两个条件：
		//非硬件Tiled的GPU。现代移动端GPU通常自带Tiled，且是TBDR架构，已经在GPU层做了Early-Z，无需再显式绘制。
		//指定了有效的EarlyZPassMode或者渲染器的bEarlyZPassMovable不为0。
		if (bNeedsPrePass)
		{
			RenderPrePass(GraphBuilder, SceneTextures.Depth.Target, InstanceCullingManager);
		}
		else
		{
			// We didn't do the prepass, but we still want the HMD mask if there is one
			RenderPrePassHMD(GraphBuilder, SceneTextures.Depth.Target);
		}

		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_AfterPrePass));

		// special pass for DDM_AllOpaqueNoVelocity, which uses the velocity pass to finish the early depth pass write
		if (bShouldRenderVelocities && Scene->EarlyZPassMode == DDM_AllOpaqueNoVelocity)
		{
			// Render the velocities of movable objects
			GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_Velocity));
			RenderVelocities(GraphBuilder, SceneTextures, EVelocityPass::Opaque, bHairStrandsEnable);
			GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_AfterVelocity));
		}
	}

	{
		{
			RDG_RHI_GPU_STAT_SCOPE(GraphBuilder, VisibilityCommands);
			InitViewsAfterPrepass(GraphBuilder, ILCTaskData, InstanceCullingManager);
		}

		{
			SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_FGlobalDynamicVertexBuffer_Commit);
			DynamicVertexBufferForInitShadows.Commit();
			DynamicIndexBufferForInitShadows.Commit();
			DynamicReadBufferForInitShadows.Commit();
		}
	}

	TArray<Nanite::FRasterResults, TInlineAllocator<2>> NaniteRasterResults;
	if (bNaniteEnabled && Views.Num() > 0)
	{
		LLM_SCOPE_BYTAG(Nanite);
		TRACE_CPUPROFILER_EVENT_SCOPE(InitNaniteRaster);

		NaniteRasterResults.AddDefaulted(Views.Num());

		RDG_GPU_STAT_SCOPE(GraphBuilder, NaniteRaster);
		const FIntPoint RasterTextureSize = SceneTextures.Depth.Target->Desc.Extent;
		
		// Primary raster view
		{
			Nanite::FSharedContext SharedContext{};
			SharedContext.FeatureLevel = Scene->GetFeatureLevel();
			SharedContext.ShaderMap = GetGlobalShaderMap(SharedContext.FeatureLevel);
			SharedContext.Pipeline = Nanite::EPipeline::Primary;

			Nanite::FRasterState RasterState;
			Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(GraphBuilder, SharedContext, RasterTextureSize, ViewFamily.EngineShowFlags.VisualizeNanite);

			Nanite::FCullingContext::FConfiguration CullingConfig = { 0 };
			CullingConfig.bTwoPassOcclusion					= true;
			CullingConfig.bUpdateStreaming					= true;
			CullingConfig.bPrimaryContext					= true;
			CullingConfig.bForceHWRaster					= RasterContext.RasterScheduling == Nanite::ERasterScheduling::HardwareOnly;

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				const FViewInfo& View = Views[ViewIndex];
				CullingConfig.SetViewFlags(View);

				Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(
					GraphBuilder,
					SharedContext,
					*Scene,
					!bIsEarlyDepthComplete ? View.PrevViewInfo.NaniteHZB : View.PrevViewInfo.HZB,
					View.ViewRect,
					CullingConfig
				);

				static FString EmptyFilterName = TEXT(""); // Empty filter represents primary view.
				const bool bExtractStats = Nanite::IsStatFilterActive(EmptyFilterName);

				float LODScaleFactor = 1.0f;
				if (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale &&
					CVarNaniteViewMeshLODBiasEnable.GetValueOnRenderThread() != 0)
				{
					float TemporalUpscaleFactor = float(View.GetSecondaryViewRectSize().X) / float(View.ViewRect.Width());

					LODScaleFactor = TemporalUpscaleFactor * FMath::Exp2(-CVarNaniteViewMeshLODBiasOffset.GetValueOnRenderThread());
					LODScaleFactor = FMath::Min(LODScaleFactor, FMath::Exp2(-CVarNaniteViewMeshLODBiasMin.GetValueOnRenderThread()));
				}

				Nanite::FPackedView PackedView = Nanite::CreatePackedViewFromViewInfo(
					View,
					RasterTextureSize,
					NANITE_VIEW_FLAG_HZBTEST,
					/* StreamingPriorityCategory = */ 3,
					/* MinBoundsRadius = */ 0.0f,
					LODScaleFactor);

				Nanite::CullRasterize(
					GraphBuilder,
					*Scene,
					View,
					{ PackedView },
					SharedContext,
					CullingContext,
					RasterContext,
					RasterState,
					/*OptionalInstanceDraws*/ nullptr,
					bExtractStats
				);

				Nanite::FRasterResults& RasterResults = NaniteRasterResults[ViewIndex];

				if (bNeedsPrePass)
				{
					// Emit velocity with depth if not writing it in base pass.
					FRDGTexture* VelocityBuffer = !IsUsingBasePassVelocity(ShaderPlatform) ? SceneTextures.Velocity : nullptr;

					const bool bEmitStencilMask = NANITE_MATERIAL_STENCIL != 0;

					Nanite::EmitDepthTargets(
						GraphBuilder,
						*Scene,
						Views[ViewIndex],
						CullingContext.PageConstants,
						CullingContext.VisibleClustersSWHW,
						CullingContext.ViewsBuffer,
						SceneTextures.Depth.Target,
						RasterContext.VisBuffer64,
						VelocityBuffer,
						RasterResults.MaterialDepth,
						RasterResults.MaterialResolve,
						bNeedsPrePass,
						bEmitStencilMask
					);
				}

				if (!bIsEarlyDepthComplete && CullingConfig.bTwoPassOcclusion && View.ViewState)
				{
					// Won't have a complete SceneDepth for post pass so can't use complete HZB for main pass or it will poke holes in the post pass HZB killing occlusion culling.
					RDG_EVENT_SCOPE(GraphBuilder, "Nanite::BuildHZB");

					FRDGTextureRef SceneDepth = SystemTextures.Black;
					FRDGTextureRef GraphHZB = nullptr;

					const FIntRect PrimaryViewRect = View.GetPrimaryView()->ViewRect;

					BuildHZBFurthest(
						GraphBuilder,
						SceneDepth,
						RasterContext.VisBuffer64,
						PrimaryViewRect,
						FeatureLevel,
						ShaderPlatform,
						TEXT("Nanite.HZB"),
						/* OutFurthestHZBTexture = */ &GraphHZB );
					
					GraphBuilder.QueueTextureExtraction( GraphHZB, &View.ViewState->PrevFrameViewInfo.NaniteHZB );
				}

				Nanite::ExtractResults(GraphBuilder, CullingContext, RasterContext, RasterResults);

				if (GNaniteShowStats != 0 && IStereoRendering::IsAPrimaryView(View))
				{
					Nanite::PrintStats(GraphBuilder, View);
				}
			}
		}
	}

	SceneTextures.SetupMode = ESceneTextureSetupMode::SceneDepth;
	SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);

	AddResolveSceneDepthPass(GraphBuilder, Views, SceneTextures.Depth);

	// NOTE: The ordering of the lights is used to select sub-sets for different purposes, e.g., those that support clustered deferred.
	FSortedLightSetSceneInfo& SortedLightSet = *GraphBuilder.AllocObject<FSortedLightSetSceneInfo>();
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, SortLights);
		GatherLightsAndComputeLightGrid(GraphBuilder, bComputeLightGrid, SortedLightSet);
	}

	CSV_CUSTOM_STAT(LightCount, All,  float(SortedLightSet.SortedLights.Num()), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(LightCount, Batched, float(SortedLightSet.UnbatchedLightStart), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(LightCount, Unbatched, float(SortedLightSet.SortedLights.Num()) - float(SortedLightSet.UnbatchedLightStart), ECsvCustomStatOp::Set);

	FCompositionLighting CompositionLighting(Views, SceneTextures, [this] (int32 ViewIndex)
	{
		return ViewPipelineStates[ViewIndex]->AmbientOcclusionMethod == EAmbientOcclusionMethod::SSAO;
	});

	const auto RenderOcclusionLambda = [&]()
	{
		RenderOcclusion(GraphBuilder, SceneTextures, bIsOcclusionTesting);

		CompositionLighting.ProcessAfterOcclusion(GraphBuilder);
	};

	// Early occlusion queries
	const bool bOcclusionBeforeBasePass = ((DepthPass.EarlyZPassMode == EDepthDrawingMode::DDM_AllOccluders) || bIsEarlyDepthComplete);

#if RHI_RAYTRACING
	bool bRayTracingSceneReady = false;
#endif

	if (bOcclusionBeforeBasePass)
	{
		RenderOcclusionLambda();
	}

	// End early occlusion queries

	BeginAsyncDistanceFieldShadowProjections(GraphBuilder, SceneTextures);

	const bool bShouldRenderSkyAtmosphere = ShouldRenderSkyAtmosphere(Scene, ViewFamily.EngineShowFlags);
	const bool bShouldRenderVolumetricCloudBase = ShouldRenderVolumetricCloud(Scene, ViewFamily.EngineShowFlags);
	const bool bShouldRenderVolumetricCloud = bShouldRenderVolumetricCloudBase && !ViewFamily.EngineShowFlags.VisualizeVolumetricCloudConservativeDensity;
	const bool bShouldVisualizeVolumetricCloud = bShouldRenderVolumetricCloudBase && !!ViewFamily.EngineShowFlags.VisualizeVolumetricCloudConservativeDensity;
	bool bAsyncComputeVolumetricCloud = IsVolumetricRenderTargetEnabled() && IsVolumetricRenderTargetAsyncCompute();
	bool bHasHalfResCheckerboardMinMaxDepth = false;
	bool bVolumetricRenderTargetRequired = bShouldRenderVolumetricCloud && !bHasRayTracedOverlay;

	if (bShouldRenderVolumetricCloudBase)
	{
		InitVolumetricRenderTargetForViews(GraphBuilder, Views);
	}

	InitVolumetricCloudsForViews(GraphBuilder, bShouldRenderVolumetricCloudBase, InstanceCullingManager);

	// Generate sky LUTs
	// TODO: Valid shadow maps (for volumetric light shafts) have not yet been generated at this point in the frame. Need to resolve dependency ordering!
	// This also must happen before the BasePass for Sky material to be able to sample valid LUTs.
	if (bShouldRenderSkyAtmosphere)
	{
		// Generate the Sky/Atmosphere look up tables
		RenderSkyAtmosphereLookUpTables(GraphBuilder);
	}

	// Capture the SkyLight using the SkyAtmosphere and VolumetricCloud component if available.
	const bool bRealTimeSkyCaptureEnabled = Scene->SkyLight && Scene->SkyLight->bRealTimeCaptureEnabled && Views.Num() > 0 && ViewFamily.EngineShowFlags.SkyLighting;
	if (bRealTimeSkyCaptureEnabled)
	{
		FViewInfo& MainView = Views[0];
		Scene->AllocateAndCaptureFrameSkyEnvMap(GraphBuilder, *this, MainView, bShouldRenderSkyAtmosphere, bShouldRenderVolumetricCloud, InstanceCullingManager);
	}

	const ECustomDepthPassLocation CustomDepthPassLocation = GetCustomDepthPassLocation(ShaderPlatform);
	if (CustomDepthPassLocation == ECustomDepthPassLocation::BeforeBasePass)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_CustomDepthPass_BeforeBasePass);
		if (RenderCustomDepthPass(GraphBuilder, SceneTextures.CustomDepth, SceneTextures.GetSceneTextureShaderParameters(FeatureLevel)))
		{
			SceneTextures.SetupMode |= ESceneTextureSetupMode::CustomDepth;
			SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);
		}
	}

	//据 Nanite 和 Non-Nanite Mesh 执行各自的渲染逻辑
	//Nanite Mesh 是 Nanite 的标准 Cull Rasterize 流程
	//Non-Nanite Mesh 的捕获发生在 MeshCardCapture Pass 中，执行的是 Mesh Drawing Pipeline 流程，对应的 Shader 是 LumenCardVertexShader.usf、LumenCardPixelShader.usf（Nanite Mesh 使用的也是同样的 Shader）
	//VS 就是普通顶点变换，PS 则从 Material 中获取各个 Attribute 写入到对应的 RT 中，
	//需要说明的是，由于 Lumen 主要用于计算 Indirect Diffuse，Lighting 只考虑 Diffuse 项，将 Material 视为完全粗糙的电介质材料，通过为移动平台极致优化的近似环境 BRDF （Roughness=1 和 N=V（1））计算最终的 Albedo，另外为了后续便于 Lighting 计算，这里 Normal 也转换到 Card Space
	UpdateLumenScene(GraphBuilder);

	FRDGTextureRef HalfResolutionDepthCheckerboardMinMaxTexture = nullptr;

	// Kick off async compute cloud eraly if all depth has been written in the prepass
	if (bShouldRenderVolumetricCloud && bAsyncComputeVolumetricCloud && DepthPass.EarlyZPassMode == DDM_AllOpaque && !bHasRayTracedOverlay)
	{
		HalfResolutionDepthCheckerboardMinMaxTexture = CreateHalfResolutionDepthCheckerboardMinMax(GraphBuilder, Views, SceneTextures.Depth.Resolve);
		bHasHalfResCheckerboardMinMaxDepth = true;

		bool bSkipVolumetricRenderTarget = false;
		bool bSkipPerPixelTracing = true;
		bAsyncComputeVolumetricCloud = RenderVolumetricCloud(GraphBuilder, SceneTextures, bSkipVolumetricRenderTarget, bSkipPerPixelTracing, HalfResolutionDepthCheckerboardMinMaxTexture, true, InstanceCullingManager);
	}
	
	FRDGTextureRef ForwardScreenSpaceShadowMaskTexture = nullptr;
	FRDGTextureRef ForwardScreenSpaceShadowMaskHairTexture = nullptr;
	if (IsForwardShadingEnabled(ShaderPlatform))
	{
		// With forward shading we need to render shadow maps early
		ensureMsgf(!VirtualShadowMapArray.IsEnabled(), TEXT("Virtual shadow maps are not supported in the forward shading path"));
		RenderShadowDepthMaps(GraphBuilder, InstanceCullingManager);

		if (bHairStrandsEnable && !bHasRayTracedOverlay)
		{
			RenderHairPrePass(GraphBuilder, Scene, Views, InstanceCullingManager);
			RenderHairBasePass(GraphBuilder, Scene, SceneTextures, Views, InstanceCullingManager);
		}

		RenderForwardShadowProjections(GraphBuilder, SceneTextures, ForwardScreenSpaceShadowMaskTexture, ForwardScreenSpaceShadowMaskHairTexture);

		// With forward shading we need to render volumetric fog before the base pass
		ComputeVolumetricFog(GraphBuilder, SceneTextures);
	}

	FDBufferTextures DBufferTextures = CreateDBufferTextures(GraphBuilder, SceneTextures.Config.Extent, ShaderPlatform);

	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(DeferredShadingSceneRenderer_DBuffer);
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_DBuffer);
		CompositionLighting.ProcessBeforeBasePass(GraphBuilder, DBufferTextures);
	}
	
	if (IsForwardShadingEnabled(ShaderPlatform) && bAllowStaticLighting)
	{
		RenderIndirectCapsuleShadows(GraphBuilder, SceneTextures);
	}

	FTranslucencyLightingVolumeTextures TranslucencyLightingVolumeTextures;

	if (bRenderDeferredLighting && GbEnableAsyncComputeTranslucencyLightingVolumeClear && GSupportsEfficientAsyncCompute)
	{
		InitTranslucencyLightingVolumeTextures(GraphBuilder, Views, ERDGPassFlags::AsyncCompute, TranslucencyLightingVolumeTextures);
	}

#if RHI_RAYTRACING
	// Async AS builds can potentially overlap with BasePass
	FRDGBufferRef DynamicGeometryScratchBuffer;
	DispatchRayTracingWorldUpdates(GraphBuilder, DynamicGeometryScratchBuffer);
#endif

	{
		RenderBasePass(GraphBuilder, SceneTextures, DBufferTextures, BasePassDepthStencilAccess, ForwardScreenSpaceShadowMaskTexture, InstanceCullingManager, bNaniteEnabled, NaniteRasterResults);
		GraphBuilder.AddDispatchHint();

		if (!bAllowReadOnlyDepthBasePass)
		{
			AddResolveSceneDepthPass(GraphBuilder, Views, SceneTextures.Depth);
		}

#if WITH_MGPU
		if (SceneTextures.Depth.Resolve && SceneTextures.GBufferA)
		{
			FGBufferTemporalTextureParams* PassParameters = GraphBuilder.AllocParameters<FGBufferTemporalTextureParams>();

			PassParameters->DepthResolve = SceneTextures.Depth.Resolve;
			PassParameters->GBufferA = SceneTextures.GBufferA;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GBuffer Temporal Copy"),
				PassParameters,
				ERDGPassFlags::Copy | ERDGPassFlags::NeverCull,
				[PassParameters](FRHIComputeCommandList& RHICmdList)
				{
					FName GBufferTemporalEffect("GBufferTemporalCopy");

					RHICmdList.WaitForTemporalEffect(GBufferTemporalEffect);

					const uint32 NumGBufferTemporalTextures = 2;
					TStaticArray<FRHITexture*, NumGBufferTemporalTextures> GBufferTemporalTexturesRHI;
					GBufferTemporalTexturesRHI[0] = PassParameters->DepthResolve->GetRHI();
					GBufferTemporalTexturesRHI[1] = PassParameters->GBufferA->GetRHI();

					RHICmdList.BroadcastTemporalEffect(
						GBufferTemporalEffect, MakeArrayView(GBufferTemporalTexturesRHI.GetData(), GBufferTemporalTexturesRHI.Num()));
				});
		}
#endif  // WITH_MGPU

		if (bVisualizeNanite)
		{
			Nanite::AddVisualizationPasses(
				GraphBuilder,
				Scene,
				SceneTextures,
				ViewFamily.EngineShowFlags,
				Views,
				NaniteRasterResults
			);
		}

		// VisualizeVirtualShadowMap TODO
	}

	if (ViewFamily.EngineShowFlags.VisualizeLightCulling)
	{
		FRDGTextureRef VisualizeLightCullingTexture = GraphBuilder.CreateTexture(SceneTextures.Color.Target->Desc, TEXT("SceneColorVisualizeLightCulling"));
		AddClearRenderTargetPass(GraphBuilder, VisualizeLightCullingTexture, FLinearColor::Transparent);
		SceneTextures.Color.Target = VisualizeLightCullingTexture;

		// When not in MSAA, assign to both targets.
		if (SceneTexturesConfig.NumSamples == 1)
		{
			SceneTextures.Color.Resolve = SceneTextures.Color.Target;
		}
	}

	if (bUseGBuffer)
	{
		// mark GBufferA for saving for next frame if it's needed
		ExtractNormalsForNextFrameReprojection(GraphBuilder, SceneTextures, Views);
	}

	// Rebuild scene textures to include GBuffers.
	SceneTextures.SetupMode |= ESceneTextureSetupMode::GBuffers;
	SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);

	if (bRealTimeSkyCaptureEnabled)
	{
		Scene->ValidateSkyLightRealTimeCapture(GraphBuilder, Views[0], SceneTextures.Color.Target);
	}

	VisualizeVolumetricLightmap(GraphBuilder, SceneTextures);

	// Occlusion after base pass
	if (!bOcclusionBeforeBasePass)
	{
		RenderOcclusionLambda();
	}

	// End occlusion after base

	if (!bUseGBuffer)
	{
		AddResolveSceneColorPass(GraphBuilder, Views, SceneTextures.Color);
	}

	// Render hair
	if (bHairStrandsEnable && !IsForwardShadingEnabled(ShaderPlatform) && !bHasRayTracedOverlay)
	{
		RenderHairPrePass(GraphBuilder, Scene, Views, InstanceCullingManager);
		RenderHairBasePass(GraphBuilder, Scene, SceneTextures, Views, InstanceCullingManager);
	}

	FLumenSceneFrameTemporaries LumenFrameTemporaries;

	// Shadows, lumen and fog after base pass
	if (!bHasRayTracedOverlay)
	{
		// If forward shading is enabled, we rendered shadow maps earlier already
		if (!IsForwardShadingEnabled(ShaderPlatform))
		{
			if (VirtualShadowMapArray.IsEnabled())
			{
				ensureMsgf(AreLightsInLightGrid(), TEXT("Virtual shadow map setup requires local lights to be injected into the light grid (this may be caused by 'r.LightCulling.Quality=0')."));
				VirtualShadowMapArray.BuildPageAllocations(GraphBuilder, SceneTextures, Views, ViewFamily.EngineShowFlags, SortedLightSet, VisibleLightInfos, NaniteRasterResults, *Scene);
			}

			RenderShadowDepthMaps(GraphBuilder, InstanceCullingManager);
		}
		CheckShadowDepthRenderCompleted();

#if RHI_RAYTRACING
		// Lumen scene lighting requires ray tracing scene to be ready if HWRT shadows are desired
		if (Lumen::UseHardwareRayTracedSceneLighting(ViewFamily))
		{
			WaitForRayTracingScene(GraphBuilder, DynamicGeometryScratchBuffer);
			bRayTracingSceneReady = true;
		}
#endif // RHI_RAYTRACING

		{
			LLM_SCOPE_BYTAG(Lumen);
			RenderLumenSceneLighting(GraphBuilder, Views[0], LumenFrameTemporaries);
		}

		// If forward shading is enabled, we computed fog earlier already
		if (!IsForwardShadingEnabled(ShaderPlatform))
		{
			ComputeVolumetricFog(GraphBuilder, SceneTextures);
		}
	}
	// End shadow and fog after base pass

	if (bUpdateNaniteStreaming)
	{
		Nanite::GStreamingManager.SubmitFrameStreamingRequests(GraphBuilder);
	}

	if (Scene->VirtualShadowMapArrayCacheManager)
	{
		// Do this even if VSMs are disabled this frame to clean up any previously extracted data
		Scene->VirtualShadowMapArrayCacheManager->ExtractFrameData(
			GraphBuilder,				
			VirtualShadowMapArray,
			*this,
			ViewFamily.EngineShowFlags.VirtualShadowMapCaching);
	}

	// If not all depth is written during the prepass, kick off async compute cloud after basepass
	if (bShouldRenderVolumetricCloud && bAsyncComputeVolumetricCloud && DepthPass.EarlyZPassMode != DDM_AllOpaque && !bHasRayTracedOverlay)
	{
		HalfResolutionDepthCheckerboardMinMaxTexture = CreateHalfResolutionDepthCheckerboardMinMax(GraphBuilder, Views, SceneTextures.Depth.Resolve);
		bHasHalfResCheckerboardMinMaxDepth = true;

		bool bSkipVolumetricRenderTarget = false;
		bool bSkipPerPixelTracing = true;
		bAsyncComputeVolumetricCloud = RenderVolumetricCloud(GraphBuilder, SceneTextures, bSkipVolumetricRenderTarget, bSkipPerPixelTracing, HalfResolutionDepthCheckerboardMinMaxTexture, true, InstanceCullingManager);
	}

	if (CustomDepthPassLocation == ECustomDepthPassLocation::AfterBasePass)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_CustomDepthPass_AfterBasePass);
		if (RenderCustomDepthPass(GraphBuilder, SceneTextures.CustomDepth, SceneTextures.GetSceneTextureShaderParameters(FeatureLevel)))
		{
			SceneTextures.SetupMode |= ESceneTextureSetupMode::CustomDepth;
			SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);
		}
	}

	// TODO: Keeping the velocities here for testing, but if that works, this pass will be remove and DDM_AllOpaqueNoVelocity will be the only option with
	// DBuffer decals enabled.

	// If bBasePassCanOutputVelocity is set, basepass fully writes the velocity buffer unless bUseSelectiveBasePassOutputs is enabled.
	if (bShouldRenderVelocities && (!bBasePassCanOutputVelocity || bUseSelectiveBasePassOutputs) && (Scene->EarlyZPassMode != DDM_AllOpaqueNoVelocity))
	{
		// Render the velocities of movable objects
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_Velocity));
		RenderVelocities(GraphBuilder, SceneTextures, EVelocityPass::Opaque, bHairStrandsEnable);
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_AfterVelocity));

		// TODO: Populate velocity buffer from Nanite visibility buffer.
	}

	// Copy lighting channels out of stencil before deferred decals which overwrite those values
	FRDGTextureRef LightingChannelsTexture = CopyStencilToLightingChannelTexture(GraphBuilder, SceneTextures.Stencil);

	// Post base pass for material classification
	if (Strata::IsStrataEnabled())
	{
		Strata::AddStrataMaterialClassificationPass(GraphBuilder, SceneTextures, Views);
	}

	// Pre-lighting composition lighting stage
	// e.g. deferred decals, SSAO
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(AfterBasePass);
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_AfterBasePass);

		if (!IsForwardShadingEnabled(ShaderPlatform))
		{
			AddResolveSceneDepthPass(GraphBuilder, Views, SceneTextures.Depth);
		}

		CompositionLighting.ProcessAfterBasePass(GraphBuilder);
	}

	// Rebuild scene textures to include velocity, custom depth, and SSAO.
	SceneTextures.SetupMode |= ESceneTextureSetupMode::All;
	SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);

	if (!IsForwardShadingEnabled(ShaderPlatform))
	{
		// Clear stencil to 0 now that deferred decals are done using what was setup in the base pass.
		AddClearStencilPass(GraphBuilder, SceneTextures.Depth.Target);
	}

#if RHI_RAYTRACING
	// If Lumen did not force an earlier ray tracing scene sync, we must wait for it here.
	if (!bRayTracingSceneReady)
	{
		WaitForRayTracingScene(GraphBuilder, DynamicGeometryScratchBuffer);
		bRayTracingSceneReady = true;
	}
#endif // RHI_RAYTRACING

	// 渲染延迟管线的光照.
	if (bRenderDeferredLighting)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, RenderDeferredLighting);
		RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderLighting);
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_Lighting);
		
		BeginGatheringLumenSurfaceCacheFeedback(GraphBuilder, Views[0], LumenFrameTemporaries);

		// 渲染非直接漫反射和AO.
		FRDGTextureRef DynamicBentNormalAOTexture = nullptr;
		RenderDiffuseIndirectAndAmbientOcclusion(GraphBuilder, SceneTextures, LumenFrameTemporaries, LightingChannelsTexture, /* bIsVisualizePass = */ false);

		// These modulate the scenecolor output from the basepass, which is assumed to be indirect lighting
		if (bAllowStaticLighting)
		{
			// 渲染非直接胶囊阴影. 会修改从Base Pass输出的已经添加了非直接光照的场景颜色.
			RenderIndirectCapsuleShadows(GraphBuilder, SceneTextures);
		}

		// These modulate the scene color output from the base pass, which is assumed to be indirect lighting
		RenderDFAOAsIndirectShadowing(GraphBuilder, SceneTextures, DynamicBentNormalAOTexture);

		// Clear the translucent lighting volumes before we accumulate
		if ((GbEnableAsyncComputeTranslucencyLightingVolumeClear && GSupportsEfficientAsyncCompute) == false)
		{
			InitTranslucencyLightingVolumeTextures(GraphBuilder, Views, ERDGPassFlags::Compute, TranslucencyLightingVolumeTextures);
		}

#if RHI_RAYTRACING
		if (IsRayTracingEnabled())
		{
			RenderDitheredLODFadingOutMask(GraphBuilder, Views[0], SceneTextures.Depth.Target);
		}
#endif

		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_Lighting));
		RenderLights(GraphBuilder, SceneTextures, TranslucencyLightingVolumeTextures, LightingChannelsTexture, SortedLightSet);
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_AfterLighting));

		InjectTranslucencyLightingVolumeAmbientCubemap(GraphBuilder, Views, TranslucencyLightingVolumeTextures);
		FilterTranslucencyLightingVolume(GraphBuilder, Views, TranslucencyLightingVolumeTextures);

		// Render diffuse sky lighting and reflections that only operate on opaque pixels
		RenderDeferredReflectionsAndSkyLighting(GraphBuilder, SceneTextures, DynamicBentNormalAOTexture);
		
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		// Renders debug visualizations for global illumination plugins
		RenderGlobalIlluminationPluginVisualizations(GraphBuilder, LightingChannelsTexture);
#endif
		// 计算SSS效果.
		AddSubsurfacePass(GraphBuilder, SceneTextures, Views);

		{
			RenderHairStrandsSceneColorScattering(GraphBuilder, SceneTextures.Color.Target, Scene, Views);
		}

	#if RHI_RAYTRACING
		if (ShouldRenderRayTracingSkyLight(Scene->SkyLight) 
			//@todo - integrate RenderRayTracingSkyLight into RenderDiffuseIndirectAndAmbientOcclusion
			&& GetViewPipelineState(Views[0]).DiffuseIndirectMethod != EDiffuseIndirectMethod::Lumen
			&& ViewFamily.EngineShowFlags.GlobalIllumination)
		{
			FRDGTextureRef SkyLightTexture = nullptr;
			FRDGTextureRef SkyLightHitDistanceTexture = nullptr;
			RenderRayTracingSkyLight(GraphBuilder, SceneTextures.Color.Target, SkyLightTexture, SkyLightHitDistanceTexture);
			CompositeRayTracingSkyLight(GraphBuilder, SceneTextures, SkyLightTexture, SkyLightHitDistanceTexture);
		}
	#endif
	}
	else if (HairStrands::HasViewHairStrandsData(Views) && ViewFamily.EngineShowFlags.Lighting)
	{
		RenderLightsForHair(GraphBuilder, SceneTextures, SortedLightSet, ForwardScreenSpaceShadowMaskHairTexture, LightingChannelsTexture);
		RenderDeferredReflectionsAndSkyLightingHair(GraphBuilder);
	}

	if (bShouldRenderVolumetricCloud && IsVolumetricRenderTargetEnabled() && !bHasHalfResCheckerboardMinMaxDepth && !bHasRayTracedOverlay)
	{
		HalfResolutionDepthCheckerboardMinMaxTexture = CreateHalfResolutionDepthCheckerboardMinMax(GraphBuilder, Views, SceneTextures.Depth.Resolve);
	}

	if (bShouldRenderVolumetricCloud && !bHasRayTracedOverlay)
	{
		if (!bAsyncComputeVolumetricCloud)
		{
			// Generate the volumetric cloud render target
			bool bSkipVolumetricRenderTarget = false;
			bool bSkipPerPixelTracing = true;
			RenderVolumetricCloud(GraphBuilder, SceneTextures, bSkipVolumetricRenderTarget, bSkipPerPixelTracing, HalfResolutionDepthCheckerboardMinMaxTexture, false, InstanceCullingManager);
		}
		// Reconstruct the volumetric cloud render target to be ready to compose it over the scene
		ReconstructVolumetricRenderTarget(GraphBuilder, Views, SceneTextures.Depth.Resolve, HalfResolutionDepthCheckerboardMinMaxTexture, bAsyncComputeVolumetricCloud);
	}

	const bool bShouldRenderTranslucency = !bHasRayTracedOverlay && ShouldRenderTranslucency();

	// Union of all translucency view render flags.
	ETranslucencyView TranslucencyViewsToRender = bShouldRenderTranslucency ? GetTranslucencyViews(Views) : ETranslucencyView::None;

	FTranslucencyPassResourcesMap TranslucencyResourceMap(Views.Num());

	const bool bShouldRenderSingleLayerWater = !bHasRayTracedOverlay && ShouldRenderSingleLayerWater(Views);
	FSceneWithoutWaterTextures SceneWithoutWaterTextures;
	if (bShouldRenderSingleLayerWater)
	{
		if (EnumHasAnyFlags(TranslucencyViewsToRender, ETranslucencyView::UnderWater))
		{
			RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderTranslucency);
			SCOPE_CYCLE_COUNTER(STAT_TranslucencyDrawTime);
			GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_Translucency));
			RenderTranslucency(GraphBuilder, SceneTextures, TranslucencyLightingVolumeTextures, &TranslucencyResourceMap, ETranslucencyView::UnderWater, InstanceCullingManager);
			EnumRemoveFlags(TranslucencyViewsToRender, ETranslucencyView::UnderWater);
		}

		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_WaterPass));
		RenderSingleLayerWater(GraphBuilder, SceneTextures, bShouldRenderVolumetricCloud, SceneWithoutWaterTextures);
	}

	// Rebuild scene textures to include scene color.
	SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);

	FRDGTextureRef LightShaftOcclusionTexture = nullptr;

	// Draw Lightshafts
	if (!bHasRayTracedOverlay && ViewFamily.EngineShowFlags.LightShafts)
	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_RenderLightShaftOcclusion);
		LightShaftOcclusionTexture = RenderLightShaftOcclusion(GraphBuilder, SceneTextures);
	}

	// Draw the sky atmosphere
	if (!bHasRayTracedOverlay && bShouldRenderSkyAtmosphere)
	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_RenderSkyAtmosphere);
		RenderSkyAtmosphere(GraphBuilder, SceneTextures);
	}

	// Draw fog.
	if (!bHasRayTracedOverlay && ShouldRenderFog(ViewFamily))
	{
		RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderFog);
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_RenderFog);
		RenderFog(GraphBuilder, SceneTextures, LightShaftOcclusionTexture);
	}

	// After the height fog, Draw volumetric clouds (having fog applied on them already) when using per pixel tracing,
	if (!bHasRayTracedOverlay && bShouldRenderVolumetricCloud)
	{
		bool bSkipVolumetricRenderTarget = true;
		bool bSkipPerPixelTracing = false;
		RenderVolumetricCloud(GraphBuilder, SceneTextures, bSkipVolumetricRenderTarget, bSkipPerPixelTracing, HalfResolutionDepthCheckerboardMinMaxTexture, false, InstanceCullingManager);
	}

	// or composite the off screen buffer over the scene.
	if (bVolumetricRenderTargetRequired)
	{
		ComposeVolumetricRenderTargetOverScene(GraphBuilder, Views, SceneTextures.Color.Target, SceneTextures.Depth.Target, bShouldRenderSingleLayerWater, SceneWithoutWaterTextures, SceneTextures);
	}

	FRendererModule& RendererModule = static_cast<FRendererModule&>(GetRendererModule());
	RendererModule.RenderPostOpaqueExtensions(GraphBuilder, Views, SceneTextures);

	RenderOpaqueFX(GraphBuilder, Views, FXSystem, SceneTextures.UniformBuffer);

	if (Scene->GPUScene.ExecuteDeferredGPUWritePass(GraphBuilder, Views, EGPUSceneGPUWritePass::PostOpaqueRendering))
	{
		InstanceCullingManager.BeginDeferredCulling(GraphBuilder, Scene->GPUScene);
	}

	if (GetHairStrandsComposition() == EHairStrandsCompositionType::BeforeTranslucent)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, HairRendering);
		RenderHairComposition(GraphBuilder, Views, SceneTextures.Color.Target, SceneTextures.Depth.Target);
	}

	// Draw translucency.
	if (!bHasRayTracedOverlay && TranslucencyViewsToRender != ETranslucencyView::None)
	{
		RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderTranslucency);
		SCOPE_CYCLE_COUNTER(STAT_TranslucencyDrawTime);

		RDG_EVENT_SCOPE(GraphBuilder, "Translucency");

		// Raytracing doesn't need the distortion effect.
		const bool bShouldRenderDistortion = TranslucencyViewsToRender != ETranslucencyView::RayTracing;

#if RHI_RAYTRACING
		if (EnumHasAnyFlags(TranslucencyViewsToRender, ETranslucencyView::RayTracing))
		{
			RenderRayTracingTranslucency(GraphBuilder, SceneTextures.Color);
			EnumRemoveFlags(TranslucencyViewsToRender, ETranslucencyView::RayTracing);
		}
#endif
		// Sort objects' triangles
		for (FViewInfo& View : Views)
		{
			if (OIT::IsEnabled(View))
			{
				OIT::AddSortTrianglesPass(GraphBuilder, View, Scene->OITSceneData, FOITSortingType::BackToFront);
			}
		}

		// Render all remaining translucency views.
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_Translucency));
		RenderTranslucency(GraphBuilder, SceneTextures, TranslucencyLightingVolumeTextures, &TranslucencyResourceMap, TranslucencyViewsToRender, InstanceCullingManager);
		TranslucencyViewsToRender = ETranslucencyView::None;

		// Compose hair before velocity/distortion pass since these pass write depth value, 
		// and this would make the hair composition fails in this cases.
		if (GetHairStrandsComposition() == EHairStrandsCompositionType::AfterTranslucent)
		{
			RDG_GPU_STAT_SCOPE(GraphBuilder, HairRendering);
			RenderHairComposition(GraphBuilder, Views, SceneTextures.Color.Target, SceneTextures.Depth.Target);
		}

		if (bShouldRenderDistortion)
		{
			GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_Distortion));
			RenderDistortion(GraphBuilder, SceneTextures.Color.Target, SceneTextures.Depth.Target);
		}

		if (bShouldRenderVelocities)
		{
			const bool bRecreateSceneTextures = !SceneTextures.Velocity;

			GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_TranslucentVelocity));
			RenderVelocities(GraphBuilder, SceneTextures, EVelocityPass::Translucent, false);

			if (bRecreateSceneTextures)
			{
				// Rebuild scene textures to include newly allocated velocity.
				SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);
			}
		}

		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_AfterTranslucency));
	}
	else if (GetHairStrandsComposition() == EHairStrandsCompositionType::AfterTranslucent)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, HairRendering);
		RenderHairComposition(GraphBuilder, Views, SceneTextures.Color.Target, SceneTextures.Depth.Target);
	}

#if !UE_BUILD_SHIPPING
	if (CVarForceBlackVelocityBuffer.GetValueOnRenderThread())
	{
		SceneTextures.Velocity = SystemTextures.Black;

		// Rebuild the scene texture uniform buffer to include black.
		SceneTextures.UniformBuffer = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SceneTextures.SetupMode);
	}
#endif

	{
		if (HairStrandsBookmarkParameters.HasInstances())
		{
			RenderHairStrandsDebugInfo(GraphBuilder, Scene, Views, HairStrandsBookmarkParameters.HairClusterData, SceneTextures.Color.Target, SceneTextures.Depth.Target);
		}
	}

	if (VirtualShadowMapArray.IsEnabled())
	{
		VirtualShadowMapArray.RenderDebugInfo(GraphBuilder);
		if (Views.Num() > 0)
		{
			VirtualShadowMapArray.PrintStats(GraphBuilder, Views[0]);
		}
	}

	for (FViewInfo& View : Views)
	{
		ShadingEnergyConservation::Debug(GraphBuilder, View, SceneTextures);
	}

	for (FViewInfo& View : Views)
	{
		ShadingEnergyConservation::Debug(GraphBuilder, View, SceneTextures);
	}

	if (!bHasRayTracedOverlay && ViewFamily.EngineShowFlags.LightShafts)
	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_RenderLightShaftBloom);
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_LightShaftBloom));
		RenderLightShaftBloom(GraphBuilder, SceneTextures, /* inout */ TranslucencyResourceMap);
	}

	if (bUseVirtualTexturing)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, VirtualTextureUpdate);
		VirtualTextureFeedbackEnd(GraphBuilder);
	}

#if RHI_RAYTRACING
	if (IsRayTracingEnabled())
	{
		// Path tracer requires the full ray tracing pipeline support, as well as specialized extra shaders.
		// Most of the ray tracing debug visualizations also require the full pipeline, but some support inline mode.
		
		if (ViewFamily.EngineShowFlags.PathTracing 
			&& FDataDrivenShaderPlatformInfo::GetSupportsPathTracing(Scene->GetShaderPlatform()))
		{
			for (const FViewInfo& View : Views)
			{
				RenderPathTracing(GraphBuilder, View, SceneTextures.UniformBuffer, SceneTextures.Color.Target);
			}
		}
		else if (ViewFamily.EngineShowFlags.RayTracingDebug)
		{
			for (const FViewInfo& View : Views)
			{
				RenderRayTracingDebug(GraphBuilder, View, SceneTextures.Color.Target);
			}
		}
	}
#endif

	RendererModule.RenderOverlayExtensions(GraphBuilder, Views, SceneTextures);

	if (ViewFamily.EngineShowFlags.PhysicsField && Scene->PhysicsField)
	{
		RenderPhysicsField(GraphBuilder, Views, Scene->PhysicsField, SceneTextures.Color.Target);
	}

	if (ViewFamily.EngineShowFlags.VisualizeDistanceFieldAO && ShouldRenderDistanceFieldLighting())
	{
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_RenderDistanceFieldLighting));

		// Use the skylight's max distance if there is one, to be consistent with DFAO shadowing on the skylight
		const float OcclusionMaxDistance = Scene->SkyLight && !Scene->SkyLight->bWantsStaticShadowing ? Scene->SkyLight->OcclusionMaxDistance : Scene->DefaultMaxDistanceFieldOcclusionDistance;
		FRDGTextureRef DummyOutput = nullptr;
		RenderDistanceFieldLighting(GraphBuilder, SceneTextures, FDistanceFieldAOParameters(OcclusionMaxDistance), DummyOutput, false, ViewFamily.EngineShowFlags.VisualizeDistanceFieldAO);
	}

	// Draw visualizations just before use to avoid target contamination
	if (ViewFamily.EngineShowFlags.VisualizeMeshDistanceFields || ViewFamily.EngineShowFlags.VisualizeGlobalDistanceField)
	{
		RenderMeshDistanceFieldVisualization(GraphBuilder, SceneTextures, FDistanceFieldAOParameters(Scene->DefaultMaxDistanceFieldOcclusionDistance));
	}

	if (bRenderDeferredLighting)
	{
		RenderLumenMiscVisualizations(GraphBuilder, SceneTextures, LumenFrameTemporaries);
		RenderDiffuseIndirectAndAmbientOcclusion(GraphBuilder, SceneTextures, LumenFrameTemporaries, LightingChannelsTexture, /* bIsVisualizePass = */ true);
	}

	if (ViewFamily.EngineShowFlags.StationaryLightOverlap)
	{
		RenderStationaryLightOverlap(GraphBuilder, SceneTextures, LightingChannelsTexture);
	}

	if (bShouldVisualizeVolumetricCloud && !bHasRayTracedOverlay)
	{
		RenderVolumetricCloud(GraphBuilder, SceneTextures, false, true, HalfResolutionDepthCheckerboardMinMaxTexture, false, InstanceCullingManager);
		ReconstructVolumetricRenderTarget(GraphBuilder, Views, SceneTextures.Depth.Resolve, HalfResolutionDepthCheckerboardMinMaxTexture, false);
		ComposeVolumetricRenderTargetOverSceneForVisualization(GraphBuilder, Views, SceneTextures.Color.Target, SceneTextures);
		RenderVolumetricCloud(GraphBuilder, SceneTextures, true, false, HalfResolutionDepthCheckerboardMinMaxTexture, false, InstanceCullingManager);
	}

	// Resolve the scene color for post processing.
	AddResolveSceneColorPass(GraphBuilder, Views, SceneTextures.Color);

	RendererModule.RenderPostResolvedSceneColorExtension(GraphBuilder, SceneTextures);

	FRDGTextureRef ViewFamilyTexture = TryCreateViewFamilyTexture(GraphBuilder, ViewFamily);

	CopySceneCaptureComponentToTarget(GraphBuilder, SceneTextures, ViewFamilyTexture, ViewFamily, Views);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		const FViewInfo& View = Views[ViewIndex];

		if (((View.FinalPostProcessSettings.DynamicGlobalIlluminationMethod == EDynamicGlobalIlluminationMethod::ScreenSpace && ScreenSpaceRayTracing::ShouldKeepBleedFreeSceneColor(View))
			|| GetViewPipelineState(View).DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen
			|| GetViewPipelineState(View).ReflectionsMethod == EReflectionsMethod::Lumen)
			&& !View.bStatePrevViewInfoIsReadOnly)
		{
			// Keep scene color and depth for next frame screen space ray tracing.
			FSceneViewState* ViewState = View.ViewState;
			GraphBuilder.QueueTextureExtraction(SceneTextures.Depth.Resolve, &ViewState->PrevFrameViewInfo.DepthBuffer);
			GraphBuilder.QueueTextureExtraction(SceneTextures.Color.Resolve, &ViewState->PrevFrameViewInfo.ScreenSpaceRayTracingInput);
		}
	}

	// Finish rendering for each view.
	if (ViewFamily.bResolveScene && ViewFamilyTexture)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "PostProcessing");
		RDG_GPU_STAT_SCOPE(GraphBuilder, Postprocessing);
		SCOPE_CYCLE_COUNTER(STAT_FinishRenderViewTargetTime);

		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_PostProcessing));

		FPostProcessingInputs PostProcessingInputs;
		PostProcessingInputs.ViewFamilyTexture = ViewFamilyTexture;
		PostProcessingInputs.CustomDepthTexture = SceneTextures.CustomDepth.Depth;
		PostProcessingInputs.SceneTextures = SceneTextures.UniformBuffer;

		if (ViewFamily.UseDebugViewPS())
		{
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				const FViewInfo& View = Views[ViewIndex];
   				const Nanite::FRasterResults* NaniteResults = bNaniteEnabled ? &NaniteRasterResults[ViewIndex] : nullptr;
				RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
				RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);
				PostProcessingInputs.TranslucencyViewResourcesMap = FTranslucencyViewResourcesMap(TranslucencyResourceMap, ViewIndex);
				AddDebugViewPostProcessingPasses(GraphBuilder, View, PostProcessingInputs, NaniteResults);
			}
		}
		else
		{
			for (int32 ViewExt = 0; ViewExt < ViewFamily.ViewExtensions.Num(); ++ViewExt)
			{
				for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ++ViewIndex)
				{
					FViewInfo& View = Views[ViewIndex];
					RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
					PostProcessingInputs.TranslucencyViewResourcesMap = FTranslucencyViewResourcesMap(TranslucencyResourceMap, ViewIndex);
					ViewFamily.ViewExtensions[ViewExt]->PrePostProcessPass_RenderThread(GraphBuilder, View, PostProcessingInputs);
				}
			}
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				const FViewInfo& View = Views[ViewIndex];
				const Nanite::FRasterResults* NaniteResults = bNaniteEnabled ? &NaniteRasterResults[ViewIndex] : nullptr;
				RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
				RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);

				PostProcessingInputs.TranslucencyViewResourcesMap = FTranslucencyViewResourcesMap(TranslucencyResourceMap, ViewIndex);

#if !(UE_BUILD_SHIPPING)
				if (IsPostProcessVisualizeCalibrationMaterialEnabled(View))
				{
					const UMaterialInterface* DebugMaterialInterface = GetPostProcessVisualizeCalibrationMaterialInterface(View);
					check(DebugMaterialInterface);

					AddVisualizeCalibrationMaterialPostProcessingPasses(GraphBuilder, View, PostProcessingInputs, DebugMaterialInterface);
				}
				else
#endif
				{
					const FPerViewPipelineState& ViewPipelineState = GetViewPipelineState(View);
					const bool bAnyLumenActive = ViewPipelineState.DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen || ViewPipelineState.ReflectionsMethod == EReflectionsMethod::Lumen;

					AddPostProcessingPasses(GraphBuilder, View, bAnyLumenActive, PostProcessingInputs, NaniteResults, InstanceCullingManager, &VirtualShadowMapArray, LumenFrameTemporaries);
				}
			}
		}
	}

	if (bRenderDeferredLighting)
	{
		// After AddPostProcessingPasses in case of Lumen Visualizations writing to feedback
		FinishGatheringLumenSurfaceCacheFeedback(GraphBuilder, Views[0], LumenFrameTemporaries);
	}

	for (FViewInfo& View : Views)
	{
		ShaderPrint::EndView(View);
		ShaderDrawDebug::EndView(View);
	}

	GEngine->GetPostRenderDelegateEx().Broadcast(GraphBuilder);

#if RHI_RAYTRACING
	ReleaseRaytracingResources(GraphBuilder, Views, Scene->RayTracingScene);
#endif //  RHI_RAYTRACING

#if WITH_MGPU
	DoCrossGPUTransfers(GraphBuilder, RenderTargetGPUMask, ViewFamilyTexture);
#endif

	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_RenderFinish);
		RDG_GPU_STAT_SCOPE(GraphBuilder, FrameRenderFinish);
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_RenderFinish));
		RenderFinish(GraphBuilder, ViewFamilyTexture);
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLM_AfterFrame));
		GraphBuilder.AddDispatchHint();
	}

	QueueSceneTextureExtractions(GraphBuilder, SceneTextures);

	// Release the view's previous frame histories so that their memory can be reused at the graph's execution.
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		Views[ViewIndex].PrevViewInfo = FPreviousViewInfo();
	}
}

#if RHI_RAYTRACING

bool AnyRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View)
{
	if (!IsRayTracingEnabled() || Scene == nullptr)
	{
		return false;
	}

	return ShouldRenderRayTracingAmbientOcclusion(View)
		|| ShouldRenderRayTracingReflections(View)
		|| ShouldRenderRayTracingGlobalIllumination(View)
		|| ShouldRenderRayTracingTranslucency(View)
		|| ShouldRenderRayTracingSkyLight(Scene->SkyLight)
		|| ShouldRenderRayTracingShadows()
		|| Scene->RayTracedLights.Num() > 0
		|| ShouldRenderPluginRayTracingGlobalIllumination(View)
        || Lumen::AnyLumenHardwareRayTracingPassEnabled(Scene, View)
		|| HasRayTracedOverlay(*View.Family);
}

bool ShouldRenderRayTracingEffect(bool bEffectEnabled, ERayTracingPipelineCompatibilityFlags CompatibilityFlags, const FSceneView* View)
{
	if (!IsRayTracingEnabled() || (View && !View->bAllowRayTracing))
	{
		return false;
	}

	const bool bAllowPipeline = GRHISupportsRayTracingShaders && 
								CVarRayTracingAllowPipeline.GetValueOnRenderThread() &&
								EnumHasAnyFlags(CompatibilityFlags, ERayTracingPipelineCompatibilityFlags::FullPipeline);

	const bool bAllowInline = GRHISupportsInlineRayTracing && 
							  CVarRayTracingAllowInline.GetValueOnRenderThread() &&
							  EnumHasAnyFlags(CompatibilityFlags, ERayTracingPipelineCompatibilityFlags::Inline);

	// Disable the effect if current machine does not support the full ray tracing pipeline and the effect can't fall back to inline mode or vice versa.
	if (!bAllowPipeline && !bAllowInline)
	{
		return false;
	}

	const int32 OverrideMode = CVarForceAllRayTracingEffects.GetValueOnRenderThread();

	if (OverrideMode >= 0)
	{
		return OverrideMode > 0;
	}
	else
	{
		return bEffectEnabled;
	}
}

bool HasRayTracedOverlay(const FSceneViewFamily& ViewFamily)
{
	// Return true if a full screen ray tracing pass will be displayed on top of the raster pass
	// This can be used to skip certain calculations
	return
		ViewFamily.EngineShowFlags.PathTracing ||
		ViewFamily.EngineShowFlags.RayTracingDebug;
}
#endif // RHI_RAYTRACING
