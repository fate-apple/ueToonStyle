// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LightRendering.cpp: Light rendering implementation.
=============================================================================*/

#include "LightRendering.h"
#include "RendererModule.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "Engine/SubsurfaceProfile.h"
#include "ShowFlags.h"
#include "VisualizeTexture.h"
#include "RayTracing/RaytracingOptions.h"
#include "SceneTextureParameters.h"
#include "HairStrands/HairStrandsRendering.h"
#include "ScreenPass.h"
#include "SkyAtmosphereRendering.h"
#include "VolumetricCloudRendering.h"
#include "Strata/Strata.h"
#include "VirtualShadowMaps/VirtualShadowMapProjection.h"
#include "HairStrands/HairStrandsData.h"
#include "AnisotropyRendering.h"
#include "Engine/SubsurfaceProfile.h"

// ENABLE_DEBUG_DISCARD_PROP is used to test the lighting code by allowing to discard lights to see how performance scales
// It ought never to be enabled in a shipping build, and is probably only really useful when woring on the shading code.
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	#define ENABLE_DEBUG_DISCARD_PROP 1
#else // (UE_BUILD_SHIPPING || UE_BUILD_TEST)
	#define ENABLE_DEBUG_DISCARD_PROP 0
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

DECLARE_GPU_STAT(Lights);

IMPLEMENT_TYPE_LAYOUT(FLightFunctionSharedParameters);
IMPLEMENT_TYPE_LAYOUT(FStencilingGeometryShaderParameters);
IMPLEMENT_TYPE_LAYOUT(FOnePassPointShadowProjectionShaderParameters);
IMPLEMENT_TYPE_LAYOUT(FShadowProjectionShaderParameters);

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FDeferredLightUniformStruct, "DeferredLightUniforms");

extern int32 GUseTranslucentLightingVolumes;

extern TAutoConsoleVariable<int32> CVarVirtualShadowOnePassProjection;

static int32 GAllowDepthBoundsTest = 1;
static FAutoConsoleVariableRef CVarAllowDepthBoundsTest(
	TEXT("r.AllowDepthBoundsTest"),
	GAllowDepthBoundsTest,
	TEXT("If true, use enable depth bounds test when rendering defered lights.")
	);

static int32 bAllowSimpleLights = 1;
static FAutoConsoleVariableRef CVarAllowSimpleLights(
	TEXT("r.AllowSimpleLights"),
	bAllowSimpleLights,
	TEXT("If true, we allow simple (ie particle) lights")
);

static TAutoConsoleVariable<int32> CVarRayTracingOcclusion(
	TEXT("r.RayTracing.Shadows"),
	0,
	TEXT("0: use traditional rasterized shadow map (default)\n")
	TEXT("1: use ray tracing shadows"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static int32 GShadowRayTracingSamplesPerPixel = -1;
static FAutoConsoleVariableRef CVarShadowRayTracingSamplesPerPixel(
	TEXT("r.RayTracing.Shadows.SamplesPerPixel"),
	GShadowRayTracingSamplesPerPixel,
	TEXT("Sets the samples-per-pixel for directional light occlusion (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarShadowUseDenoiser(
	TEXT("r.Shadow.Denoiser"),
	2,
	TEXT("Choose the denoising algorithm.\n")
	TEXT(" 0: Disabled (default);\n")
	TEXT(" 1: Forces the default denoiser of the renderer;\n")
	TEXT(" 2: GScreenSpaceDenoiser witch may be overriden by a third party plugin.\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMaxShadowDenoisingBatchSize(
	TEXT("r.Shadow.Denoiser.MaxBatchSize"), 4,
	TEXT("Maximum number of shadow to denoise at the same time."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMaxShadowRayTracingBatchSize(
	TEXT("r.RayTracing.Shadows.MaxBatchSize"), 8,
	TEXT("Maximum number of shadows to trace at the same time."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarAllowClearLightSceneExtentsOnly(
	TEXT("r.AllowClearLightSceneExtentsOnly"), 1,
	TEXT(""),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingShadowsDirectionalLight(
	TEXT("r.RayTracing.Shadows.Lights.Directional"),
	1,
	TEXT("Enables ray tracing shadows for directional lights (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingShadowsPointLight(
	TEXT("r.RayTracing.Shadows.Lights.Point"),
	1,
	TEXT("Enables ray tracing shadows for point lights (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingShadowsSpotLight(
	TEXT("r.RayTracing.Shadows.Lights.Spot"),
	1,
	TEXT("Enables ray tracing shadows for spot lights (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingShadowsRectLight(
	TEXT("r.RayTracing.Shadows.Lights.Rect"),
	1,
	TEXT("Enables ray tracing shadows for rect light (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarAppliedLightFunctionOnHair(
	TEXT("r.HairStrands.LightFunction"),
	1,
	TEXT("Enables Light function on hair"),
	ECVF_RenderThreadSafe);

#if ENABLE_DEBUG_DISCARD_PROP
static float GDebugLightDiscardProp = 0.0f;
static FAutoConsoleVariableRef CVarDebugLightDiscardProp(
	TEXT("r.DebugLightDiscardProp"),
	GDebugLightDiscardProp,
	TEXT("[0,1]: Proportion of lights to discard for debug/performance profiling purposes.")
);
#endif // ENABLE_DEBUG_DISCARD_PROP

#if RHI_RAYTRACING

static bool ShouldRenderRayTracingShadowsForLightType(ELightComponentType LightType)
{
	switch(LightType)
	{
	case LightType_Directional:
		return !!CVarRayTracingShadowsDirectionalLight.GetValueOnRenderThread();
	case LightType_Point:
		return !!CVarRayTracingShadowsPointLight.GetValueOnRenderThread();
	case LightType_Spot:
		return !!CVarRayTracingShadowsSpotLight.GetValueOnRenderThread();
	case LightType_Rect:
		return !!CVarRayTracingShadowsRectLight.GetValueOnRenderThread();
	default:
		return true;	
	}	
}

bool ShouldRenderRayTracingShadows()
{
	const bool bIsStereo = GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled();
	const bool bHairStrands = IsHairStrandsEnabled(EHairStrandsShaderType::Strands);

	return ShouldRenderRayTracingEffect((CVarRayTracingOcclusion.GetValueOnRenderThread() > 0) && !(bIsStereo && bHairStrands), ERayTracingPipelineCompatibilityFlags::FullPipeline, nullptr);
}

bool ShouldRenderRayTracingShadowsForLight(const FLightSceneProxy& LightProxy)
{
	const bool bShadowRayTracingAllowed = ShouldRenderRayTracingEffect(true, ERayTracingPipelineCompatibilityFlags::FullPipeline, nullptr);
	return (LightProxy.CastsRaytracedShadow() == ECastRayTracedShadow::Enabled || (ShouldRenderRayTracingShadows() && LightProxy.CastsRaytracedShadow() == ECastRayTracedShadow::UseProjectSetting))
		&& ShouldRenderRayTracingShadowsForLightType((ELightComponentType)LightProxy.GetLightType())
		&& bShadowRayTracingAllowed;
}

bool ShouldRenderRayTracingShadowsForLight(const FLightSceneInfoCompact& LightInfo)
{
	const bool bShadowRayTracingAllowed = ShouldRenderRayTracingEffect(true, ERayTracingPipelineCompatibilityFlags::FullPipeline, nullptr);
	return (LightInfo.CastRaytracedShadow == ECastRayTracedShadow::Enabled || (ShouldRenderRayTracingShadows() && LightInfo.CastRaytracedShadow == ECastRayTracedShadow::UseProjectSetting))
		&& ShouldRenderRayTracingShadowsForLightType((ELightComponentType)LightInfo.LightType)
		&& bShadowRayTracingAllowed;
}
#endif // RHI_RAYTRACING

static void RenderLight(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	const FLightSceneInfo* LightSceneInfo,
	FRDGTextureRef ScreenShadowMaskTexture,
	FRDGTextureRef LightingChannelsTexture,
	bool bRenderOverlap,
	bool bCloudShadow);

FDeferredLightUniformStruct GetDeferredLightParameters(const FSceneView& View, const FLightSceneInfo& LightSceneInfo)
{
	FDeferredLightUniformStruct Out;

	FLightRenderParameters LightParameters;
	LightSceneInfo.Proxy->GetLightShaderParameters(LightParameters);
	LightParameters.MakeShaderParameters(View.ViewMatrices, Out.LightParameters);

	const bool bIsRayTracedLight = ShouldRenderRayTracingShadowsForLight(*LightSceneInfo.Proxy);

	const FVector2D FadeParams = LightSceneInfo.Proxy->GetDirectionalLightDistanceFadeParameters(View.GetFeatureLevel(), !bIsRayTracedLight && LightSceneInfo.IsPrecomputedLightingValid(), View.MaxShadowCascades);
	
	// use MAD for efficiency in the shader
	Out.DistanceFadeMAD = FVector2f(FadeParams.Y, -FadeParams.X * FadeParams.Y);
	
	int32 ShadowMapChannel = LightSceneInfo.Proxy->GetShadowMapChannel();

	static const auto AllowStaticLightingVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.AllowStaticLighting"));
	const bool bAllowStaticLighting = (!AllowStaticLightingVar || AllowStaticLightingVar->GetValueOnRenderThread() != 0);

	if (!bAllowStaticLighting)
	{
		ShadowMapChannel = INDEX_NONE;
	}

	Out.ShadowMapChannelMask = FVector4f(
		ShadowMapChannel == 0 ? 1 : 0,
		ShadowMapChannel == 1 ? 1 : 0,
		ShadowMapChannel == 2 ? 1 : 0,
		ShadowMapChannel == 3 ? 1 : 0);

	const bool bDynamicShadows = View.Family->EngineShowFlags.DynamicShadows && GetShadowQuality() > 0;
	const bool bHasLightFunction = LightSceneInfo.Proxy->GetLightFunctionMaterial() != NULL;
	Out.ShadowedBits = LightSceneInfo.Proxy->CastsStaticShadow() || bHasLightFunction ? 1 : 0;
	Out.ShadowedBits |= LightSceneInfo.Proxy->CastsDynamicShadow() && View.Family->EngineShowFlags.DynamicShadows ? 3 : 0;

	Out.VolumetricScatteringIntensity = LightSceneInfo.Proxy->GetVolumetricScatteringIntensity();

	static auto* ContactShadowsCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ContactShadows"));
	static auto* IntensityCVar = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.ContactShadows.NonShadowCastingIntensity"));

	Out.ContactShadowLength = 0;
	Out.ContactShadowNonShadowCastingIntensity = 0.0f;

	if (ContactShadowsCVar && ContactShadowsCVar->GetValueOnRenderThread() != 0 && View.Family->EngineShowFlags.ContactShadows)
	{
		Out.ContactShadowLength = LightSceneInfo.Proxy->GetContactShadowLength();
		// Sign indicates if contact shadow length is in world space or screen space.
		// Multiply by 2 for screen space in order to preserve old values after introducing multiply by View.ClipToView[1][1] in shader.
		Out.ContactShadowLength *= LightSceneInfo.Proxy->IsContactShadowLengthInWS() ? -1.0f : 2.0f;

		Out.ContactShadowNonShadowCastingIntensity = IntensityCVar ? IntensityCVar->GetValueOnRenderThread() : 0.0f;
	}

	// When rendering reflection captures, the direct lighting of the light is actually the indirect specular from the main view
	if (View.bIsReflectionCapture)
	{
		Out.LightParameters.Color *= LightSceneInfo.Proxy->GetIndirectLightingScale();
	}

	const ELightComponentType LightType = (ELightComponentType)LightSceneInfo.Proxy->GetLightType();
	if ((LightType == LightType_Point || LightType == LightType_Spot || LightType == LightType_Rect) && View.IsPerspectiveProjection())
	{
		Out.LightParameters.Color *= GetLightFadeFactor(View, LightSceneInfo.Proxy);
	}

	Out.LightingChannelMask = LightSceneInfo.Proxy->GetLightingChannelMask();

	// Ensure the light falloff exponent is set to 0 so that lighting shaders handle it as inverse-squared attenuated light
	if (LightSceneInfo.Proxy->IsInverseSquared())
	{
		Out.LightParameters.FalloffExponent = 0.0f;
	}
	return Out;
}

FDeferredLightUniformStruct GetSimpleDeferredLightParameters(
	const FSceneView& View,
	const FSimpleLightEntry& SimpleLight,
	const FVector& LightWorldPosition)
{
	FDeferredLightUniformStruct Out;
	Out.ShadowMapChannelMask = FVector4f(0, 0, 0, 0);
	Out.DistanceFadeMAD = FVector2f(0, 0);
	Out.ContactShadowLength = 0.0f;
	Out.ContactShadowNonShadowCastingIntensity = 0.f;
	Out.VolumetricScatteringIntensity = SimpleLight.VolumetricScatteringIntensity;
	Out.ShadowedBits = 0;
	Out.LightingChannelMask = 0;

	Out.LightParameters.TranslatedWorldPosition = FVector3f(LightWorldPosition + View.ViewMatrices.GetPreViewTranslation());
	Out.LightParameters.InvRadius = 1.0f / FMath::Max(SimpleLight.Radius, KINDA_SMALL_NUMBER);
	Out.LightParameters.Color = (FVector3f)SimpleLight.Color;
	Out.LightParameters.FalloffExponent = SimpleLight.Exponent;
	Out.LightParameters.Direction = FVector3f(1, 0, 0);
	Out.LightParameters.Tangent = FVector3f(1, 0, 0);
	Out.LightParameters.SpotAngles = FVector2f(-2, 1);
	Out.LightParameters.SpecularScale = 1.0f;
	Out.LightParameters.SourceRadius = 0.0f;
	Out.LightParameters.SoftSourceRadius = 0.0f;
	Out.LightParameters.SourceLength = 0.0f;
	Out.LightParameters.RectLightBarnCosAngle = 0;
	Out.LightParameters.RectLightBarnLength = -2.0f;
	Out.LightParameters.SourceTexture = GWhiteTexture->TextureRHI;
	return Out;
}
FDeferredLightUniformStruct GetSimpleDeferredLightParameters(
	const FSceneView& View,
	const FSimpleLightEntry& SimpleLight,
	const FSimpleLightPerViewEntry& SimpleLightPerViewData)
{
	return GetSimpleDeferredLightParameters(View, SimpleLight, SimpleLightPerViewData.Position);
}

FLightOcclusionType GetLightOcclusionType(const FLightSceneProxy& Proxy)
{
#if RHI_RAYTRACING
	return ShouldRenderRayTracingShadowsForLight(Proxy) ? FLightOcclusionType::Raytraced : FLightOcclusionType::Shadowmap;
#else
	return FLightOcclusionType::Shadowmap;
#endif
}

FLightOcclusionType GetLightOcclusionType(const FLightSceneInfoCompact& LightInfo)
{
#if RHI_RAYTRACING
	return ShouldRenderRayTracingShadowsForLight(LightInfo) ? FLightOcclusionType::Raytraced : FLightOcclusionType::Shadowmap;
#else
	return FLightOcclusionType::Shadowmap;
#endif
}

float GetLightFadeFactor(const FSceneView& View, const FLightSceneProxy* Proxy)
{
	// Distance fade
	FSphere Bounds = Proxy->GetBoundingSphere();

	const float DistanceSquared = (Bounds.Center - View.ViewMatrices.GetViewOrigin()).SizeSquared();
	extern float GMinScreenRadiusForLights;
	float SizeFade = FMath::Square(FMath::Min(0.0002f, GMinScreenRadiusForLights / Bounds.W) * View.LODDistanceFactor) * DistanceSquared;
	SizeFade = FMath::Clamp(6.0f - 6.0f * SizeFade, 0.0f, 1.0f);

	extern float GLightMaxDrawDistanceScale;
	float MaxDist = Proxy->GetMaxDrawDistance() * GLightMaxDrawDistanceScale;
	float Range = Proxy->GetFadeRange();
	float DistanceFade = MaxDist ? (MaxDist - FMath::Sqrt(DistanceSquared)) / Range : 1.0f;
	DistanceFade = FMath::Clamp(DistanceFade, 0.0f, 1.0f);
	return SizeFade * DistanceFade;
}

void StencilingGeometry::DrawSphere(FRHICommandList& RHICmdList)
{
	RHICmdList.SetStreamSource(0, StencilingGeometry::GStencilSphereVertexBuffer.VertexBufferRHI, 0);
	RHICmdList.DrawIndexedPrimitive(StencilingGeometry::GStencilSphereIndexBuffer.IndexBufferRHI, 0, 0,
		StencilingGeometry::GStencilSphereVertexBuffer.GetVertexCount(), 0,
		StencilingGeometry::GStencilSphereIndexBuffer.GetIndexCount() / 3, 1);
}

void StencilingGeometry::DrawVectorSphere(FRHICommandList& RHICmdList)
{
	RHICmdList.SetStreamSource(0, StencilingGeometry::GStencilSphereVectorBuffer.VertexBufferRHI, 0);
	RHICmdList.DrawIndexedPrimitive(StencilingGeometry::GStencilSphereIndexBuffer.IndexBufferRHI, 0, 0,
									StencilingGeometry::GStencilSphereVectorBuffer.GetVertexCount(), 0,
									StencilingGeometry::GStencilSphereIndexBuffer.GetIndexCount() / 3, 1);
}

void StencilingGeometry::DrawCone(FRHICommandList& RHICmdList)
{
	// No Stream Source needed since it will generate vertices on the fly
	RHICmdList.SetStreamSource(0, StencilingGeometry::GStencilConeVertexBuffer.VertexBufferRHI, 0);

	RHICmdList.DrawIndexedPrimitive(StencilingGeometry::GStencilConeIndexBuffer.IndexBufferRHI, 0, 0,
		FStencilConeIndexBuffer::NumVerts, 0, StencilingGeometry::GStencilConeIndexBuffer.GetIndexCount() / 3, 1);
}

/** The stencil sphere vertex buffer. */
TGlobalResource<StencilingGeometry::TStencilSphereVertexBuffer<18, 12, FVector4f> > StencilingGeometry::GStencilSphereVertexBuffer;
TGlobalResource<StencilingGeometry::TStencilSphereVertexBuffer<18, 12, FVector3f> > StencilingGeometry::GStencilSphereVectorBuffer;

/** The stencil sphere index buffer. */
TGlobalResource<StencilingGeometry::TStencilSphereIndexBuffer<18, 12> > StencilingGeometry::GStencilSphereIndexBuffer;

TGlobalResource<StencilingGeometry::TStencilSphereVertexBuffer<4, 4, FVector4f> > StencilingGeometry::GLowPolyStencilSphereVertexBuffer;
TGlobalResource<StencilingGeometry::TStencilSphereIndexBuffer<4, 4> > StencilingGeometry::GLowPolyStencilSphereIndexBuffer;

/** The (dummy) stencil cone vertex buffer. */
TGlobalResource<StencilingGeometry::FStencilConeVertexBuffer> StencilingGeometry::GStencilConeVertexBuffer;

/** The stencil cone index buffer. */
TGlobalResource<StencilingGeometry::FStencilConeIndexBuffer> StencilingGeometry::GStencilConeIndexBuffer;

// Implement a version for directional lights, and a version for point / spot lights
IMPLEMENT_GLOBAL_SHADER(FDeferredLightVS, "/Engine/Private/DeferredLightVertexShaders.usf", "VertexMain", SF_Vertex);

class FDeferredLightHairVS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDeferredLightHairVS, Global);
	SHADER_USE_PARAMETER_STRUCT(FDeferredLightHairVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_HAIR"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDeferredLightHairVS, "/Engine/Private/DeferredLightVertexShaders.usf", "HairVertexMain", SF_Vertex);

enum class ELightSourceShape
{
	Directional,
	Capsule,
	Rect,

	MAX
};


/** A pixel shader for rendering the light in a deferred pass. */
class FDeferredLightPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDeferredLightPS, Global)
	SHADER_USE_PARAMETER_STRUCT(FDeferredLightPS, FGlobalShader);

	class FSourceShapeDim		: SHADER_PERMUTATION_ENUM_CLASS("LIGHT_SOURCE_SHAPE", ELightSourceShape);
	class FSourceTextureDim		: SHADER_PERMUTATION_BOOL("USE_SOURCE_TEXTURE");
	class FIESProfileDim		: SHADER_PERMUTATION_BOOL("USE_IES_PROFILE");
	class FVisualizeCullingDim	: SHADER_PERMUTATION_BOOL("VISUALIZE_LIGHT_CULLING");
	class FLightingChannelsDim	: SHADER_PERMUTATION_BOOL("USE_LIGHTING_CHANNELS");
	class FTransmissionDim		: SHADER_PERMUTATION_BOOL("USE_TRANSMISSION");
	class FHairLighting			: SHADER_PERMUTATION_INT("USE_HAIR_LIGHTING", 2);
	class FAtmosphereTransmittance : SHADER_PERMUTATION_BOOL("USE_ATMOSPHERE_TRANSMITTANCE");
	class FCloudTransmittance 	: SHADER_PERMUTATION_BOOL("USE_CLOUD_TRANSMITTANCE");
	class FAnistropicMaterials 	: SHADER_PERMUTATION_BOOL("SUPPORTS_ANISOTROPIC_MATERIALS");
	class FStrataTileType		: SHADER_PERMUTATION_INT("STRATA_TILETYPE", 3);

	using FPermutationDomain = TShaderPermutationDomain<
		FSourceShapeDim,
		FSourceTextureDim,
		FIESProfileDim,
		FVisualizeCullingDim,
		FLightingChannelsDim,
		FTransmissionDim,
		FHairLighting,
		FAtmosphereTransmittance,
		FCloudTransmittance,
		FAnistropicMaterials,
		FStrataTileType>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FStrataGlobalUniformParameters, Strata)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVolumetricCloudShadowAOParameters, CloudShadowAO)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLightCloudTransmittanceParameters, CloudShadow)
		SHADER_PARAMETER(uint32, CloudShadowEnabled)
		SHADER_PARAMETER(uint32, HairTransmittanceBufferMaxCount)
		SHADER_PARAMETER(uint32, HairShadowMaskValid)
		SHADER_PARAMETER(FVector4f, ShadowChannelMask)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LightAttenuationTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LightAttenuationTextureSampler)
		SHADER_PARAMETER_TEXTURE(Texture2D, IESTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, IESTextureSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LightingChannelsTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LightingChannelsSampler)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, HairTransmittanceBuffer)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ScreenShadowMaskSubPixelTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DummyRectLightTextureForCapsuleCompilerWarning)
		SHADER_PARAMETER_SAMPLER(SamplerState, DummyRectLightSamplerForCapsuleCompilerWarning)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FDeferredLightUniformStruct, DeferredLight)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		// Build FVisualizeCullingDim permutation only for a restricted number of case, as they don't impact the 'estimated cost' of lighting
		if (PermutationVector.Get< FVisualizeCullingDim >() && (
			PermutationVector.Get< FSourceTextureDim >() ||
			PermutationVector.Get< FIESProfileDim >() ||
			PermutationVector.Get< FTransmissionDim >() ||
			PermutationVector.Get< FHairLighting >() ||
			PermutationVector.Get< FAtmosphereTransmittance >() ||
			PermutationVector.Get< FCloudTransmittance >() ||
			PermutationVector.Get< FAnistropicMaterials >()))
		{
			return false;
		}

		if (PermutationVector.Get< FSourceShapeDim >() == ELightSourceShape::Directional && PermutationVector.Get< FIESProfileDim >())
		{
			return false;
		}

		if (PermutationVector.Get< FSourceShapeDim >() != ELightSourceShape::Directional && (PermutationVector.Get<FAtmosphereTransmittance>() || PermutationVector.Get<FCloudTransmittance>()))
		{
			return false;
		}

		if( PermutationVector.Get< FSourceShapeDim >() != ELightSourceShape::Rect && PermutationVector.Get< FSourceTextureDim >())
		{
			return false;
		}

		if (PermutationVector.Get< FHairLighting >() && PermutationVector.Get< FTransmissionDim >())
		{
			return false;
		}

		if (PermutationVector.Get<FDeferredLightPS::FAnistropicMaterials>())
		{
			if (Strata::IsStrataEnabled())
			{
				return false;
			}

			// Anisotropic materials do not currently support rect lights
			if (PermutationVector.Get<FSourceShapeDim>() == ELightSourceShape::Rect || PermutationVector.Get<FSourceTextureDim>())
			{
				return false;
			}

			// (Hair Lighting == 2) has its own BxDF and anisotropic BRDF is only for DefaultLit and ClearCoat materials.
			if (PermutationVector.Get<FHairLighting>() == 2)
			{
				return false;
			}

			if (!FDataDrivenShaderPlatformInfo::GetSupportsAnisotropicMaterials(Parameters.Platform))
			{
				return false;
			}
		}

		if (!Strata::IsStrataEnabled() && PermutationVector.Get<FStrataTileType>() != 0)
		{
			return false;
		}
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		// Build FVisualizeCullingDim permutation only for a restricted number of case, as they don't impact the 'estimated cost' of lighting
		if (PermutationVector.Get< FVisualizeCullingDim >())
		{
			PermutationVector.Set< FSourceTextureDim >(false);
			PermutationVector.Set< FIESProfileDim >(false);
			PermutationVector.Set< FTransmissionDim >(false);
			PermutationVector.Set< FHairLighting >(false);
			PermutationVector.Set< FAtmosphereTransmittance >(false);
			PermutationVector.Set< FCloudTransmittance >(false);
			PermutationVector.Set< FAnistropicMaterials >(false);
		}

		return PermutationVector;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("USE_HAIR_COMPLEX_TRANSMITTANCE"), IsHairStrandsSupported(EHairStrandsShaderType::All, Parameters.Platform) ? 1u : 0u);
		OutEnvironment.SetDefine(TEXT("STRATA_ENABLED"), Strata::IsStrataEnabled() ? 1u : 0u);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDeferredLightPS, "/Engine/Private/DeferredLightPixelShaders.usf", "DeferredLightPixelMain", SF_Pixel);

/** Shader used to visualize stationary light overlap. */
class FDeferredLightOverlapPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDeferredLightOverlapPS, Global)
	SHADER_USE_PARAMETER_STRUCT(FDeferredLightOverlapPS, FGlobalShader);

	class FRadialAttenuation : SHADER_PERMUTATION_BOOL("RADIAL_ATTENUATION");
	using FPermutationDomain = TShaderPermutationDomain<FRadialAttenuation>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FDeferredLightUniformStruct, DeferredLight)
		SHADER_PARAMETER(float, bHasValidChannel)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDeferredLightOverlapPS, "/Engine/Private/StationaryLightOverlapShaders.usf", "OverlapPixelMain", SF_Pixel);

static void SplitSimpleLightsByView(TArrayView<const FViewInfo> Views, const FSimpleLightArray& SimpleLights, TArrayView<FSimpleLightArray> SimpleLightsByView)
{
	check(SimpleLightsByView.Num() == Views.Num());

	for (int32 LightIndex = 0; LightIndex < SimpleLights.InstanceData.Num(); ++LightIndex)
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			FSimpleLightPerViewEntry PerViewEntry = SimpleLights.GetViewDependentData(LightIndex, ViewIndex, Views.Num());
			SimpleLightsByView[ViewIndex].InstanceData.Add(SimpleLights.InstanceData[LightIndex]);
			SimpleLightsByView[ViewIndex].PerViewData.Add(PerViewEntry);
		}
	}
}

/** Gathers simple lights from visible primtives in the passed in views. */
void FSceneRenderer::GatherSimpleLights(const FSceneViewFamily& ViewFamily, const TArray<FViewInfo>& Views, FSimpleLightArray& SimpleLights)
{
	TArray<const FPrimitiveSceneInfo*, SceneRenderingAllocator> PrimitivesWithSimpleLights;

	// Gather visible primitives from all views that might have simple lights
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		for (int32 PrimitiveIndex = 0; PrimitiveIndex < View.VisibleDynamicPrimitivesWithSimpleLights.Num(); PrimitiveIndex++)
		{
			const FPrimitiveSceneInfo* PrimitiveSceneInfo = View.VisibleDynamicPrimitivesWithSimpleLights[PrimitiveIndex];

			// TArray::AddUnique is slow, but not expecting many entries in PrimitivesWithSimpleLights
			PrimitivesWithSimpleLights.AddUnique(PrimitiveSceneInfo);
		}
	}

	// Gather simple lights from the primitives
	for (int32 PrimitiveIndex = 0; PrimitiveIndex < PrimitivesWithSimpleLights.Num(); PrimitiveIndex++)
	{
		const FPrimitiveSceneInfo* Primitive = PrimitivesWithSimpleLights[PrimitiveIndex];
		Primitive->Proxy->GatherSimpleLights(ViewFamily, SimpleLights);
	}
}

/** Gets a readable light name for use with a draw event. */
void FSceneRenderer::GetLightNameForDrawEvent(const FLightSceneProxy* LightProxy, FString& LightNameWithLevel)
{
#if WANTS_DRAW_MESH_EVENTS
	if (GetEmitDrawEvents())
	{
		FString FullLevelName = LightProxy->GetLevelName().ToString();
		const int32 LastSlashIndex = FullLevelName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);

		if (LastSlashIndex != INDEX_NONE)
		{
			// Trim the leading path before the level name to make it more readable
			// The level FName was taken directly from the Outermost UObject, otherwise we would do this operation on the game thread
			FullLevelName.MidInline(LastSlashIndex + 1, FullLevelName.Len() - (LastSlashIndex + 1), false);
		}

		LightNameWithLevel = FullLevelName + TEXT(".") + LightProxy->GetOwnerNameOrLabel();
	}
#endif
}

extern int32 GbEnableAsyncComputeTranslucencyLightingVolumeClear;

uint32 GetShadowQuality();

static bool LightRequiresDenosier(const FLightSceneInfo& LightSceneInfo)
{
	ELightComponentType LightType = ELightComponentType(LightSceneInfo.Proxy->GetLightType());
	if (LightType == LightType_Directional)
	{
		return LightSceneInfo.Proxy->GetLightSourceAngle() > 0;
	}
	else if (LightType == LightType_Point || LightType == LightType_Spot)
	{
		return LightSceneInfo.Proxy->GetSourceRadius() > 0;
	}
	else if (LightType == LightType_Rect)
	{
		return true;
	}
	else
	{
		check(0);
	}
	return false;
}



void FSceneRenderer::GatherAndSortLights(FSortedLightSetSceneInfo& OutSortedLights, bool bShadowedLightsInClustered)
{
	if (bAllowSimpleLights)
	{
		//都是级联粒子和Niagara粒子相关的代理，因此可推断，简单光源只用于粒子特效模块。
		GatherSimpleLights(ViewFamily, Views, OutSortedLights.SimpleLights);
	}
	FSimpleLightArray &SimpleLights = OutSortedLights.SimpleLights;
	TArray<FSortedLightSceneInfo, SceneRenderingAllocator> &SortedLights = OutSortedLights.SortedLights;

	// NOTE: we allocate space also for simple lights such that they can be referenced in the same sorted range
	SortedLights.Empty(Scene->Lights.Num() + SimpleLights.InstanceData.Num());

	bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows && GetShadowQuality() > 0;

#if ENABLE_DEBUG_DISCARD_PROP
	int Total = Scene->Lights.Num() + SimpleLights.InstanceData.Num();
	int NumToKeep = int(float(Total) * (1.0f - GDebugLightDiscardProp));
	const float DebugDiscardStride = float(NumToKeep) / float(Total);
	float DebugDiscardCounter = 0.0f;
#endif // ENABLE_DEBUG_DISCARD_PROP
	// Build a list of visible lights.
	for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
		const FLightSceneInfo* const LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;

#if ENABLE_DEBUG_DISCARD_PROP
		{
			int PrevCounter = int(DebugDiscardCounter);
			DebugDiscardCounter += DebugDiscardStride;
			if (PrevCounter >= int(DebugDiscardCounter))
			{
				continue;
			}
		}
#endif // ENABLE_DEBUG_DISCARD_PROP

		if (LightSceneInfo->ShouldRenderLightViewIndependent()
			// Reflection override skips direct specular because it tends to be blindingly bright with a perfectly smooth surface
			&& !ViewFamily.EngineShowFlags.ReflectionOverride)
		{
			// Check if the light is visible in any of the views.
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				if (LightSceneInfo->ShouldRenderLight(Views[ViewIndex]))
				{
					FSortedLightSceneInfo* SortedLightInfo = new(SortedLights) FSortedLightSceneInfo(LightSceneInfo);

					// Check for shadows and light functions.
					SortedLightInfo->SortKey.Fields.LightType = LightSceneInfoCompact.LightType;
					SortedLightInfo->SortKey.Fields.bTextureProfile = ViewFamily.EngineShowFlags.TexturedLightProfiles && LightSceneInfo->Proxy->GetIESTextureResource();
					SortedLightInfo->SortKey.Fields.bShadowed = bDynamicShadows && CheckForProjectedShadows(LightSceneInfo);
					SortedLightInfo->SortKey.Fields.bLightFunction = ViewFamily.EngineShowFlags.LightFunctions && CheckForLightFunction(LightSceneInfo);
					SortedLightInfo->SortKey.Fields.bUsesLightingChannels = Views[ViewIndex].bUsesLightingChannels && LightSceneInfo->Proxy->GetLightingChannelMask() != GetDefaultLightingChannelMask();

					// These are not simple lights.
					SortedLightInfo->SortKey.Fields.bIsNotSimpleLight = 1;

					// tiled and clustered deferred lighting only supported for certain lights that don't use any additional features
					// And also that are not directional (mostly because it does'nt make so much sense to insert them into every grid cell in the universe)
					// In the forward case one directional light gets put into its own variables, and in the deferred case it gets a full-screen pass.
					// Usually it'll have shadows and stuff anyway.
					// Rect lights are not supported as the performance impact is significant even if not used, for now, left for trad. deferred.
					// 单光源都可以被分块或分簇渲染，但对于非简单光源，只有满足以下条件的光源才可被分块或分簇渲染：
					// 1. 没有使用光源的附加特性（TextureProfile、LightFunction、LightingChannel）。
					// 2. 没有开启阴影。
					// 3. 非平行光或矩形光。
					const bool bClusteredDeferredSupported =
						!SortedLightInfo->SortKey.Fields.bTextureProfile &&
						(!SortedLightInfo->SortKey.Fields.bShadowed || bShadowedLightsInClustered) &&
						!SortedLightInfo->SortKey.Fields.bLightFunction &&
						!SortedLightInfo->SortKey.Fields.bUsesLightingChannels
						&& LightSceneInfoCompact.LightType != LightType_Directional
						&& LightSceneInfoCompact.LightType != LightType_Rect;

					// One pass projection is supported for lights with only virtual shadow maps
					// TODO: Exclude lights that also have non-virtual shadow maps
					bool bHasVirtualShadowMap = VisibleLightInfos[LightSceneInfo->Id].GetVirtualShadowMapId(&Views[ViewIndex]) != INDEX_NONE;
					SortedLightInfo->SortKey.Fields.bDoesNotWriteIntoPackedShadowMask = !bClusteredDeferredSupported || !bHasVirtualShadowMap;
					SortedLightInfo->SortKey.Fields.bClusteredDeferredNotSupported = !bClusteredDeferredSupported;
					break;
				}
			}
		}
	}
	// Add the simple lights also
	for (int32 SimpleLightIndex = 0; SimpleLightIndex < SimpleLights.InstanceData.Num(); SimpleLightIndex++)
	{
#if ENABLE_DEBUG_DISCARD_PROP
		{
			int PrevCounter = int(DebugDiscardCounter);
			DebugDiscardCounter += DebugDiscardStride;
			if (PrevCounter >= int(DebugDiscardCounter))
			{
				continue;
			}
		}
#endif // ENABLE_DEBUG_DISCARD_PROP

		FSortedLightSceneInfo* SortedLightInfo = new(SortedLights) FSortedLightSceneInfo(SimpleLightIndex);
		SortedLightInfo->SortKey.Fields.LightType = LightType_Point;
		SortedLightInfo->SortKey.Fields.bTextureProfile = 0;
		SortedLightInfo->SortKey.Fields.bShadowed = 0;
		SortedLightInfo->SortKey.Fields.bLightFunction = 0;
		SortedLightInfo->SortKey.Fields.bUsesLightingChannels = 0;

		// These are simple lights.
		SortedLightInfo->SortKey.Fields.bIsNotSimpleLight = 0;

		// Simple lights are ok to use with tiled and clustered deferred lighting
		SortedLightInfo->SortKey.Fields.bClusteredDeferredNotSupported = 0;
	}

	// Sort non-shadowed, non-light function lights first to avoid render target switches.
	struct FCompareFSortedLightSceneInfo
	{
		FORCEINLINE bool operator()( const FSortedLightSceneInfo& A, const FSortedLightSceneInfo& B ) const
		{
			return A.SortKey.Packed < B.SortKey.Packed;
		}
	};
	SortedLights.Sort( FCompareFSortedLightSceneInfo() );

	// Scan and find ranges.
	OutSortedLights.SimpleLightsEnd = SortedLights.Num();
	OutSortedLights.ClusteredSupportedEnd = SortedLights.Num();
	OutSortedLights.UnbatchedLightStart = SortedLights.Num();

	// Iterate over all lights to be rendered and build ranges for tiled deferred and unshadowed lights
	for (int32 LightIndex = 0; LightIndex < SortedLights.Num(); LightIndex++)
	{
		const FSortedLightSceneInfo& SortedLightInfo = SortedLights[LightIndex];
		const bool bDrawShadows = SortedLightInfo.SortKey.Fields.bShadowed;
		const bool bDrawLightFunction = SortedLightInfo.SortKey.Fields.bLightFunction;
		const bool bTextureLightProfile = SortedLightInfo.SortKey.Fields.bTextureProfile;
		const bool bLightingChannels = SortedLightInfo.SortKey.Fields.bUsesLightingChannels;

		if (SortedLightInfo.SortKey.Fields.bIsNotSimpleLight && OutSortedLights.SimpleLightsEnd == SortedLights.Num())
		{
			// Mark the first index to not be simple
			OutSortedLights.SimpleLightsEnd = LightIndex;
		}

		if (SortedLightInfo.SortKey.Fields.bClusteredDeferredNotSupported && OutSortedLights.ClusteredSupportedEnd == SortedLights.Num())
		{
			// Mark the first index to not support clustered deferred
			OutSortedLights.ClusteredSupportedEnd = LightIndex;
		}

		if( (bDrawShadows || bDrawLightFunction || bLightingChannels) && SortedLightInfo.SortKey.Fields.bClusteredDeferredNotSupported )
		{
			// Once we find an unbatched shadowed light, we can exit the loop
			check(SortedLightInfo.SortKey.Fields.bClusteredDeferredNotSupported);
			OutSortedLights.UnbatchedLightStart = LightIndex;
			break;
		}
	}

	// Make sure no obvious things went wrong!
	check(OutSortedLights.ClusteredSupportedEnd >= OutSortedLights.SimpleLightsEnd);
	check(OutSortedLights.UnbatchedLightStart >= OutSortedLights.ClusteredSupportedEnd);
}

FHairStrandsTransmittanceMaskData CreateDummyHairStrandsTransmittanceMaskData(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap);

void FDeferredShadingSceneRenderer::RenderLights(
	FRDGBuilder& GraphBuilder,
	FMinimalSceneTextures& SceneTextures,
	const FTranslucencyLightingVolumeTextures& TranslucencyLightingVolumeTextures,
	FRDGTextureRef LightingChannelsTexture,
	FSortedLightSetSceneInfo& SortedLightSet)
{
	const bool bUseHairLighting = HairStrands::HasViewHairStrandsData(Views);

	RDG_EVENT_SCOPE(GraphBuilder, "Lights");
	RDG_GPU_STAT_SCOPE(GraphBuilder, Lights);

	SCOPED_NAMED_EVENT(FDeferredShadingSceneRenderer_RenderLights, FColor::Emerald);
	SCOPE_CYCLE_COUNTER(STAT_LightingDrawTime);
	SCOPE_CYCLE_COUNTER(STAT_LightRendering);

	const FSimpleLightArray &SimpleLights = SortedLightSet.SimpleLights;
	const TArray<FSortedLightSceneInfo, SceneRenderingAllocator> &SortedLights = SortedLightSet.SortedLights;
	// 带阴影的光源起始索引.
	const int32 UnbatchedLightStart = SortedLightSet.UnbatchedLightStart;
	const int32 SimpleLightsEnd = SortedLightSet.SimpleLightsEnd;

	FHairStrandsTransmittanceMaskData DummyTransmittanceMaskData;
	if (bUseHairLighting && Views.Num() > 0)
	{
		DummyTransmittanceMaskData = CreateDummyHairStrandsTransmittanceMaskData(GraphBuilder, Views[0].ShaderMap);
	}

	// 直接光照
	{
		RDG_EVENT_SCOPE(GraphBuilder, "DirectLighting");

		// STRATA_TODO move right after stencil clear so that it is also common with EnvLight pass
		if (ViewFamily.EngineShowFlags.DirectLighting && Strata::IsStrataEnabled())
		{
			// Update the stencil buffer, marking simple/complex strata material only once for all the following passes.
			Strata::AddStrataStencilPass(GraphBuilder, Views, SceneTextures);
		}

		// 无阴影光照
		if(ViewFamily.EngineShowFlags.DirectLighting)
		{
			RDG_EVENT_SCOPE(GraphBuilder, "BatchedLights");
			INC_DWORD_STAT_BY(STAT_NumBatchedLights, UnbatchedLightStart);

			// 无阴影的光源起始索引.
			// Currently they have a special path anyway in case of standard deferred so always skip the simple lights
			int32 StandardDeferredStart = SortedLightSet.SimpleLightsEnd;

			bool bRenderSimpleLightsStandardDeferred = SortedLightSet.SimpleLights.InstanceData.Num() > 0;

			UE_CLOG(ShouldUseClusteredDeferredShading() && !AreLightsInLightGrid(), LogRenderer, Warning,
				TEXT("Clustered deferred shading is enabled, but lights were not injected in grid, falling back to other methods (hint 'r.LightCulling.Quality' may cause this)."));

			// 分簇延迟光照.
			// True if the clustered shading is enabled and the feature level is there, and that the light grid had lights injected.
			if (ShouldUseClusteredDeferredShading() && AreLightsInLightGrid())
			{
				FRDGTextureRef ShadowMaskBits = nullptr;
				FRDGTextureRef HairStrandsShadowMaskBits = nullptr;
				if( VirtualShadowMapArray.IsAllocated() && CVarVirtualShadowOnePassProjection.GetValueOnRenderThread() )
				{
					// TODO: This needs to move into the view loop in clustered deferred shading pass
					for (const FViewInfo& View : Views)
					{
						ShadowMaskBits = RenderVirtualShadowMapProjectionOnePass(
							GraphBuilder,
							SceneTextures,
							View,
							VirtualShadowMapArray,
							EVirtualShadowMapProjectionInputType::GBuffer);

						if (HairStrands::HasViewHairStrandsData(View))
						{
							HairStrandsShadowMaskBits = RenderVirtualShadowMapProjectionOnePass(
							GraphBuilder,
							SceneTextures,
							View,
							VirtualShadowMapArray,
							EVirtualShadowMapProjectionInputType::HairStrands);
						}
					}
				}
				else
				{
					ShadowMaskBits = GraphBuilder.RegisterExternalTexture( GSystemTextures.ZeroUIntDummy );
				}

				// Tell the trad. deferred that the clustered deferred capable lights are taken care of.
				// This includes the simple lights
				StandardDeferredStart = SortedLightSet.ClusteredSupportedEnd;
				// Tell the trad. deferred that the simple lights are spoken for.
				bRenderSimpleLightsStandardDeferred = false;

				// 增加分簇延迟渲染Pass.
				AddClusteredDeferredShadingPass(GraphBuilder, SceneTextures, SortedLightSet, ShadowMaskBits, HairStrandsShadowMaskBits);
			}

			// 简单光照.
			if (bRenderSimpleLightsStandardDeferred)
			{
				// 渲染简单光照.
				RenderSimpleLightsStandardDeferred(GraphBuilder, SceneTextures, SortedLightSet.SimpleLights);
			}

			// 标准延迟光照.
			// Draw non-shadowed non-light function lights without changing render targets between them
			for (int32 ViewIndex = 0, ViewCount = Views.Num(); ViewIndex < ViewCount; ++ViewIndex)
			{
				const FViewInfo& View = Views[ViewIndex];
				RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, ViewCount > 1, "View%d", ViewIndex);
				SCOPED_GPU_MASK(GraphBuilder.RHICmdList, View.GPUMask);

				for (int32 LightIndex = StandardDeferredStart; LightIndex < UnbatchedLightStart; LightIndex++)
				{
					// Render the light to the scene color buffer, using a 1x1 white texture as input
					// 渲染无阴影光照.
					const FLightSceneInfo* LightSceneInfo = SortedLights[LightIndex].LightSceneInfo;
					RenderLight(GraphBuilder, Scene, View, SceneTextures, LightSceneInfo, nullptr, LightingChannelsTexture, false /*bRenderOverlap*/, false /*bCloudShadow*/);
				}
			}

			// Add a special version when hair rendering is enabled for getting lighting on hair. 
			if (bUseHairLighting)
			{
				FRDGTextureRef NullScreenShadowMaskSubPixelTexture = nullptr;
				for (FViewInfo& View : Views)
				{
					if (HairStrands::HasViewHairStrandsData(View))
					{
						// Draw non-shadowed non-light function lights without changing render targets between them
						for (int32 LightIndex = StandardDeferredStart; LightIndex < UnbatchedLightStart; LightIndex++)
						{
							const FLightSceneInfo* LightSceneInfo = SortedLights[LightIndex].LightSceneInfo;
							RenderLightForHair(GraphBuilder, View, SceneTextures, LightSceneInfo, NullScreenShadowMaskSubPixelTexture, LightingChannelsTexture, DummyTransmittanceMaskData, false /*bForwardRendering*/);
						}
					}
				}
			}

			if (GUseTranslucentLightingVolumes && GSupportsVolumeTextureRendering)
			{
				if (UnbatchedLightStart)
				{
					// Inject non-shadowed, non-simple, non-light function lights in to the volume.
					InjectTranslucencyLightingVolumeArray(GraphBuilder, Views, Scene, *this, TranslucencyLightingVolumeTextures, VisibleLightInfos, SortedLights, TInterval<int32>(SimpleLightsEnd, UnbatchedLightStart));
				}

				if (SimpleLights.InstanceData.Num() > 0)
				{
					auto& SimpleLightsByView = *GraphBuilder.AllocObject<TArray<FSimpleLightArray, SceneRenderingAllocator>>();
					SimpleLightsByView.SetNum(Views.Num());

					SplitSimpleLightsByView(Views, SimpleLights, SimpleLightsByView);

					for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
					{
						FSimpleLightArray& SimpleLightArray = SimpleLightsByView[ViewIndex];

						if (SimpleLightArray.InstanceData.Num() > 0)
						{
							FViewInfo& View = Views[ViewIndex];
							RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
							RDG_EVENT_SCOPE(GraphBuilder, "InjectSimpleLightsTranslucentLighting");
							InjectSimpleTranslucencyLightingVolumeArray(GraphBuilder, View, ViewIndex, Views.Num(), TranslucencyLightingVolumeTextures, SimpleLightArray);
						}
					}
				}
			}
		}

		// 带阴影的光照
		{
			RDG_EVENT_SCOPE(GraphBuilder, "UnbatchedLights");

			const int32 DenoiserMode = CVarShadowUseDenoiser.GetValueOnRenderThread();

			const IScreenSpaceDenoiser* DefaultDenoiser = IScreenSpaceDenoiser::GetDefaultDenoiser();
			const IScreenSpaceDenoiser* DenoiserToUse = DenoiserMode == 1 ? DefaultDenoiser : GScreenSpaceDenoiser;

			TArray<FRDGTextureRef, SceneRenderingAllocator> PreprocessedShadowMaskTextures;
			TArray<FRDGTextureRef, SceneRenderingAllocator> PreprocessedShadowMaskSubPixelTextures;

			const int32 MaxDenoisingBatchSize = FMath::Clamp(CVarMaxShadowDenoisingBatchSize.GetValueOnRenderThread(), 1, IScreenSpaceDenoiser::kMaxBatchSize);
			const int32 MaxRTShadowBatchSize = CVarMaxShadowRayTracingBatchSize.GetValueOnRenderThread();
			const bool bDoShadowDenoisingBatching = DenoiserMode != 0 && MaxDenoisingBatchSize > 1;

			//#dxr_todo: support multiview for the batching case
			const bool bDoShadowBatching = (bDoShadowDenoisingBatching || MaxRTShadowBatchSize > 1) && Views.Num() == 1;

			// Optimisations: batches all shadow ray tracing denoising. Definitely could be smarter to avoid high VGPR pressure if this entire
			// function was converted to render graph, and want least intrusive change as possible. So right not it trades render target memory pressure
			// for denoising perf.
			if (RHI_RAYTRACING && bDoShadowBatching)
			{
				const uint32 ViewIndex = 0;
				FViewInfo& View = Views[ViewIndex];

				// Allocate PreprocessedShadowMaskTextures once so QueueTextureExtraction can deferred write.
				{
					if (!View.bStatePrevViewInfoIsReadOnly)
					{
						View.ViewState->PrevFrameViewInfo.ShadowHistories.Empty();
						View.ViewState->PrevFrameViewInfo.ShadowHistories.Reserve(SortedLights.Num());
					}

					PreprocessedShadowMaskTextures.SetNum(SortedLights.Num());
				}

				PreprocessedShadowMaskTextures.SetNum(SortedLights.Num());

				if (HairStrands::HasViewHairStrandsData(View))
				{ 
					PreprocessedShadowMaskSubPixelTextures.SetNum(SortedLights.Num());
				}
			} // if (RHI_RAYTRACING)

			const bool bDirectLighting = ViewFamily.EngineShowFlags.DirectLighting;

			FRDGTextureRef SharedScreenShadowMaskTexture = nullptr;
			FRDGTextureRef SharedScreenShadowMaskSubPixelTexture = nullptr;

			// 渲染带阴影的光源和光照函数光源.
			// Draw shadowed and light function lights
			for (int32 LightIndex = UnbatchedLightStart; LightIndex < SortedLights.Num(); LightIndex++)
			{
				const FSortedLightSceneInfo& SortedLightInfo = SortedLights[LightIndex];
				const FLightSceneInfo& LightSceneInfo = *SortedLightInfo.LightSceneInfo;
				const FLightSceneProxy& LightSceneProxy = *LightSceneInfo.Proxy;

				// Note: Skip shadow mask generation for rect light if direct illumination is computed
				//		 stochastically (rather than analytically + shadow mask)
				const bool bDrawShadows = SortedLightInfo.SortKey.Fields.bShadowed;
				const bool bDrawLightFunction = SortedLightInfo.SortKey.Fields.bLightFunction;
				const bool bDrawPreviewIndicator = ViewFamily.EngineShowFlags.PreviewShadowsIndicator && !LightSceneInfo.IsPrecomputedLightingValid() && LightSceneProxy.HasStaticShadowing();
				const bool bDrawHairShadow = bDrawShadows && bUseHairLighting;
				const bool bUseHairDeepShadow = bDrawShadows && bUseHairLighting && LightSceneProxy.CastsHairStrandsDeepShadow();
				bool bInjectedTranslucentVolume = false;
				bool bUsedShadowMaskTexture = false;

				FScopeCycleCounter Context(LightSceneProxy.GetStatId());

				FRDGTextureRef ScreenShadowMaskTexture = nullptr;
				FRDGTextureRef ScreenShadowMaskSubPixelTexture = nullptr;

				if (bDrawShadows || bDrawLightFunction || bDrawPreviewIndicator)
				{
					if (!SharedScreenShadowMaskTexture)
					{
						const FRDGTextureDesc SharedScreenShadowMaskTextureDesc(FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_B8G8R8A8, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | GFastVRamConfig.ScreenSpaceShadowMask));
						SharedScreenShadowMaskTexture = GraphBuilder.CreateTexture(SharedScreenShadowMaskTextureDesc, TEXT("ShadowMaskTexture"));

						if (bUseHairLighting)
						{
							SharedScreenShadowMaskSubPixelTexture = GraphBuilder.CreateTexture(SharedScreenShadowMaskTextureDesc, TEXT("ShadowMaskSubPixelTexture"));
						}
					}
					ScreenShadowMaskTexture = SharedScreenShadowMaskTexture;
					ScreenShadowMaskSubPixelTexture = SharedScreenShadowMaskSubPixelTexture;
				}

				FString LightNameWithLevel;
				GetLightNameForDrawEvent(&LightSceneProxy, LightNameWithLevel);
				RDG_EVENT_SCOPE(GraphBuilder, "%s", *LightNameWithLevel);

				if (bDrawShadows)
				{
					INC_DWORD_STAT(STAT_NumShadowedLights);

					const FLightOcclusionType OcclusionType = GetLightOcclusionType(LightSceneProxy);

					// Inline ray traced shadow batching, launches shadow batches when needed
					// reduces memory overhead while keeping shadows batched to optimize costs
					{
						const uint32 ViewIndex = 0;
						FViewInfo& View = Views[ViewIndex];

						IScreenSpaceDenoiser::FShadowRayTracingConfig RayTracingConfig;
						RayTracingConfig.RayCountPerPixel = GShadowRayTracingSamplesPerPixel > -1? GShadowRayTracingSamplesPerPixel : LightSceneProxy.GetSamplesPerPixel();

						const bool bDenoiserCompatible = !LightRequiresDenosier(LightSceneInfo) || IScreenSpaceDenoiser::EShadowRequirements::PenumbraAndClosestOccluder == DenoiserToUse->GetShadowRequirements(View, LightSceneInfo, RayTracingConfig);

						const bool bWantsBatchedShadow = OcclusionType == FLightOcclusionType::Raytraced && 
							bDoShadowBatching &&
							bDenoiserCompatible &&
							SortedLightInfo.SortKey.Fields.bShadowed;

						// determine if this light doesn't yet have a precomuted shadow and execute a batch to amortize costs if one is needed
						if (
							RHI_RAYTRACING &&
							bWantsBatchedShadow &&
							(PreprocessedShadowMaskTextures.Num() == 0 || !PreprocessedShadowMaskTextures[LightIndex - UnbatchedLightStart]))
						{
							RDG_EVENT_SCOPE(GraphBuilder, "ShadowBatch");
							TStaticArray<IScreenSpaceDenoiser::FShadowVisibilityParameters, IScreenSpaceDenoiser::kMaxBatchSize> DenoisingQueue;
							TStaticArray<int32, IScreenSpaceDenoiser::kMaxBatchSize> LightIndices;

							FSceneTextureParameters SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, SceneTextures.UniformBuffer);

							int32 ProcessShadows = 0;

							const auto QuickOffDenoisingBatch = [&]
							{
								int32 InputParameterCount = 0;
								for (int32 i = 0; i < IScreenSpaceDenoiser::kMaxBatchSize; i++)
								{
									InputParameterCount += DenoisingQueue[i].LightSceneInfo != nullptr ? 1 : 0;
								}

								check(InputParameterCount >= 1);

								TStaticArray<IScreenSpaceDenoiser::FShadowVisibilityOutputs, IScreenSpaceDenoiser::kMaxBatchSize> Outputs;

								RDG_EVENT_SCOPE(GraphBuilder, "%s%s(Shadow BatchSize=%d) %dx%d",
									DenoiserToUse != DefaultDenoiser ? TEXT("ThirdParty ") : TEXT(""),
									DenoiserToUse->GetDebugName(),
									InputParameterCount,
									View.ViewRect.Width(), View.ViewRect.Height());

								DenoiserToUse->DenoiseShadowVisibilityMasks(
									GraphBuilder,
									View,
									&View.PrevViewInfo,
									SceneTextureParameters,
									DenoisingQueue,
									InputParameterCount,
									Outputs);

								for (int32 i = 0; i < InputParameterCount; i++)
								{
									const FLightSceneInfo* LocalLightSceneInfo = DenoisingQueue[i].LightSceneInfo;

									int32 LocalLightIndex = LightIndices[i];
									FRDGTextureRef& RefDestination = PreprocessedShadowMaskTextures[LocalLightIndex - UnbatchedLightStart];
									check(RefDestination == nullptr);
									RefDestination = Outputs[i].Mask;
									DenoisingQueue[i].LightSceneInfo = nullptr;
								}
							}; // QuickOffDenoisingBatch

							// Ray trace shadows of light that needs, and quick off denoising batch.
							for (int32 LightBatchIndex = LightIndex; LightBatchIndex < SortedLights.Num(); LightBatchIndex++)
							{
								const FSortedLightSceneInfo& BatchSortedLightInfo = SortedLights[LightBatchIndex];
								const FLightSceneInfo& BatchLightSceneInfo = *BatchSortedLightInfo.LightSceneInfo;

								// Denoiser do not support texture rect light important sampling.
								const bool bBatchDrawShadows = BatchSortedLightInfo.SortKey.Fields.bShadowed;

								if (!bBatchDrawShadows)
									continue;

								const FLightOcclusionType BatchOcclusionType = GetLightOcclusionType(*BatchLightSceneInfo.Proxy);
								if (BatchOcclusionType != FLightOcclusionType::Raytraced)
									continue;

								const bool bRequiresDenoiser = LightRequiresDenosier(BatchLightSceneInfo) && DenoiserMode > 0;

								IScreenSpaceDenoiser::FShadowRayTracingConfig BatchRayTracingConfig;
								BatchRayTracingConfig.RayCountPerPixel = GShadowRayTracingSamplesPerPixel > -1 ? GShadowRayTracingSamplesPerPixel : BatchLightSceneInfo.Proxy->GetSamplesPerPixel();

								IScreenSpaceDenoiser::EShadowRequirements DenoiserRequirements = bRequiresDenoiser ?
									DenoiserToUse->GetShadowRequirements(View, BatchLightSceneInfo, BatchRayTracingConfig) :
									IScreenSpaceDenoiser::EShadowRequirements::Bailout;

								// Not worth batching and increase memory pressure if the denoiser do not support this ray tracing config.
								// TODO: add suport for batch with multiple SPP.
								if (bRequiresDenoiser && DenoiserRequirements != IScreenSpaceDenoiser::EShadowRequirements::PenumbraAndClosestOccluder)
								{
									continue;
								}

								// Ray trace the shadow.
								//#dxr_todo: support multiview for the batching case
								FRDGTextureRef RayTracingShadowMaskTexture;
								{
									FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
										SceneTextures.Config.Extent,
										PF_FloatRGBA,
										FClearValueBinding::Black,
										TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
									RayTracingShadowMaskTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingOcclusion"));
								}

								FRDGTextureRef RayDistanceTexture;
								{
									FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
										SceneTextures.Config.Extent,
										PF_R16F,
										FClearValueBinding::Black,
										TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
									RayDistanceTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingOcclusionDistance"));
								}

								FRDGTextureRef SubPixelRayTracingShadowMaskTexture = nullptr;
								FRDGTextureUAV* SubPixelRayTracingShadowMaskUAV = nullptr;
								if (bUseHairLighting)
								{
									FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
										SceneTextures.Config.Extent,
										PF_FloatRGBA,
										FClearValueBinding::Black,
										TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
									SubPixelRayTracingShadowMaskTexture = GraphBuilder.CreateTexture(Desc, TEXT("SubPixelRayTracingOcclusion"));
									SubPixelRayTracingShadowMaskUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SubPixelRayTracingShadowMaskTexture));
								}

								FString BatchLightNameWithLevel;
								GetLightNameForDrawEvent(BatchLightSceneInfo.Proxy, BatchLightNameWithLevel);

								FRDGTextureUAV* RayTracingShadowMaskUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RayTracingShadowMaskTexture));
								FRDGTextureUAV* RayHitDistanceUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RayDistanceTexture));
								{
									RDG_EVENT_SCOPE(GraphBuilder, "%s", *BatchLightNameWithLevel);

									// Ray trace the shadow cast by opaque geometries on to hair strands geometries
									// Note: No denoiser is required on this output, as the hair strands are geometrically noisy, which make it hard to denoise
									RenderRayTracingShadows(
										GraphBuilder,
										SceneTextureParameters,
										View,
										BatchLightSceneInfo,
										BatchRayTracingConfig,
										DenoiserRequirements,
										LightingChannelsTexture,
										RayTracingShadowMaskUAV,
										RayHitDistanceUAV,
										SubPixelRayTracingShadowMaskUAV);
									
									if (HairStrands::HasViewHairStrandsData(View))
									{
										FRDGTextureRef& RefDestination = PreprocessedShadowMaskSubPixelTextures[LightBatchIndex - UnbatchedLightStart];
										check(RefDestination == nullptr);
										RefDestination = SubPixelRayTracingShadowMaskTexture;
									}
								}

								bool bBatchFull = false;

								if (bRequiresDenoiser)
								{
									// Queue the ray tracing output for shadow denoising.
									for (int32 i = 0; i < IScreenSpaceDenoiser::kMaxBatchSize; i++)
									{
										if (DenoisingQueue[i].LightSceneInfo == nullptr)
										{
											DenoisingQueue[i].LightSceneInfo = &BatchLightSceneInfo;
											DenoisingQueue[i].RayTracingConfig = RayTracingConfig;
											DenoisingQueue[i].InputTextures.Mask = RayTracingShadowMaskTexture;
											DenoisingQueue[i].InputTextures.ClosestOccluder = RayDistanceTexture;
											LightIndices[i] = LightBatchIndex;

											// If queue for this light type is full, quick of the batch.
											if ((i + 1) == MaxDenoisingBatchSize)
											{
												QuickOffDenoisingBatch();
												bBatchFull = true;
											}
											break;
										}
										else
										{
											check((i - 1) < IScreenSpaceDenoiser::kMaxBatchSize);
										}
									}
								}
								else
								{
									PreprocessedShadowMaskTextures[LightBatchIndex - UnbatchedLightStart] = RayTracingShadowMaskTexture;
								}

								// terminate batch if we filled a denoiser batch or hit our max light batch
								ProcessShadows++;
								if (bBatchFull || ProcessShadows == MaxRTShadowBatchSize)
								{
									break;
								}
							}

							// Ensures all denoising queues are processed.
							if (DenoisingQueue[0].LightSceneInfo)
							{
								QuickOffDenoisingBatch();
							}
						}
					} // end inline batched raytraced shadow

					if (RHI_RAYTRACING && PreprocessedShadowMaskTextures.Num() > 0 && PreprocessedShadowMaskTextures[LightIndex - UnbatchedLightStart])
					{
						const uint32 ShadowMaskIndex = LightIndex - UnbatchedLightStart;
						ScreenShadowMaskTexture = PreprocessedShadowMaskTextures[ShadowMaskIndex];
						PreprocessedShadowMaskTextures[ShadowMaskIndex] = nullptr;

						// Subp-ixel shadow for hair strands geometries
						if (bUseHairLighting && ShadowMaskIndex < uint32(PreprocessedShadowMaskSubPixelTextures.Num()))
						{
							ScreenShadowMaskSubPixelTexture = PreprocessedShadowMaskSubPixelTextures[ShadowMaskIndex];
							PreprocessedShadowMaskSubPixelTextures[ShadowMaskIndex] = nullptr;
						}

						// Inject deep shadow mask if the light supports it
						if (bUseHairDeepShadow)
						{
							RenderHairStrandsDeepShadowMask(GraphBuilder, Views, &LightSceneInfo, ScreenShadowMaskTexture);
						}
					}
					else if (OcclusionType == FLightOcclusionType::Raytraced)
					{
						FSceneTextureParameters SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, SceneTextures.UniformBuffer);

						FRDGTextureRef RayTracingShadowMaskTexture;
						{
							FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
								SceneTextures.Config.Extent,
								PF_FloatRGBA,
								FClearValueBinding::Black,
								TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
							RayTracingShadowMaskTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingOcclusion"));
						}

						FRDGTextureRef RayDistanceTexture;
						{
							FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
								SceneTextures.Config.Extent,
								PF_R16F,
								FClearValueBinding::Black,
								TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
							RayDistanceTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingOcclusionDistance"));
						}

						FRDGTextureUAV* RayTracingShadowMaskUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RayTracingShadowMaskTexture));
						FRDGTextureUAV* RayHitDistanceUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RayDistanceTexture));

						FRDGTextureRef SubPixelRayTracingShadowMaskTexture = nullptr;
						FRDGTextureUAV* SubPixelRayTracingShadowMaskUAV = nullptr;
						if (bUseHairLighting)
						{
							FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
								SceneTextures.Config.Extent,
								PF_FloatRGBA,
								FClearValueBinding::Black,
								TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
							SubPixelRayTracingShadowMaskTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingOcclusion"));
							SubPixelRayTracingShadowMaskUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SubPixelRayTracingShadowMaskTexture));
						}

						FRDGTextureRef RayTracingShadowMaskTileTexture;
						{
							FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
								SceneTextures.Config.Extent,
								PF_FloatRGBA,
								FClearValueBinding::Black,
								TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
							RayTracingShadowMaskTileTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingOcclusionTile"));
						}

						bool bIsMultiview = Views.Num() > 0;

						for (FViewInfo& View : Views)
						{
							RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

							IScreenSpaceDenoiser::FShadowRayTracingConfig RayTracingConfig;
							RayTracingConfig.RayCountPerPixel = GShadowRayTracingSamplesPerPixel > -1 ? GShadowRayTracingSamplesPerPixel : LightSceneProxy.GetSamplesPerPixel();

							IScreenSpaceDenoiser::EShadowRequirements DenoiserRequirements = IScreenSpaceDenoiser::EShadowRequirements::Bailout;
							if (DenoiserMode != 0 && LightRequiresDenosier(LightSceneInfo))
							{
								DenoiserRequirements = DenoiserToUse->GetShadowRequirements(View, LightSceneInfo, RayTracingConfig);
							}

							RenderRayTracingShadows(
								GraphBuilder,
								SceneTextureParameters,
								View,
								LightSceneInfo,
								RayTracingConfig,
								DenoiserRequirements,
								LightingChannelsTexture,
								RayTracingShadowMaskUAV,
								RayHitDistanceUAV,
								SubPixelRayTracingShadowMaskUAV);

							if (DenoiserRequirements != IScreenSpaceDenoiser::EShadowRequirements::Bailout)
							{
								TStaticArray<IScreenSpaceDenoiser::FShadowVisibilityParameters, IScreenSpaceDenoiser::kMaxBatchSize> InputParameters;
								TStaticArray<IScreenSpaceDenoiser::FShadowVisibilityOutputs, IScreenSpaceDenoiser::kMaxBatchSize> Outputs;

								InputParameters[0].InputTextures.Mask = RayTracingShadowMaskTexture;
								InputParameters[0].InputTextures.ClosestOccluder = RayDistanceTexture;
								InputParameters[0].LightSceneInfo = &LightSceneInfo;
								InputParameters[0].RayTracingConfig = RayTracingConfig;

								int32 InputParameterCount = 1;

								RDG_EVENT_SCOPE(GraphBuilder, "%s%s(Shadow BatchSize=%d) %dx%d",
									DenoiserToUse != DefaultDenoiser ? TEXT("ThirdParty ") : TEXT(""),
									DenoiserToUse->GetDebugName(),
									InputParameterCount,
									View.ViewRect.Width(), View.ViewRect.Height());

								DenoiserToUse->DenoiseShadowVisibilityMasks(
									GraphBuilder,
									View,
									&View.PrevViewInfo,
									SceneTextureParameters,
									InputParameters,
									InputParameterCount,
									Outputs);

								if (bIsMultiview)
								{
									AddDrawTexturePass(GraphBuilder, View, Outputs[0].Mask, RayTracingShadowMaskTileTexture, View.ViewRect.Min, View.ViewRect.Min, View.ViewRect.Size());
									ScreenShadowMaskTexture = RayTracingShadowMaskTileTexture;
								}
								else
								{
									ScreenShadowMaskTexture = Outputs[0].Mask;
								}
							}
							else
							{
								ScreenShadowMaskTexture = RayTracingShadowMaskTexture;
							}

							if (HairStrands::HasViewHairStrandsData(View))
							{
								ScreenShadowMaskSubPixelTexture = SubPixelRayTracingShadowMaskTexture;
							}
						}

						// Inject deep shadow mask if the light supports it
						if (bUseHairDeepShadow)
						{
							RenderHairStrandsShadowMask(GraphBuilder, Views, &LightSceneInfo, false /*bForwardShading*/, ScreenShadowMaskTexture);
						}
					}
					// 处理阴影图.
					else // (OcclusionType == FOcclusionType::Shadowmap)
					{					
						const auto ClearShadowMask = [&](FRDGTextureRef InScreenShadowMaskTexture)
						{
							// Clear light attenuation for local lights with a quad covering their extents
							const bool bClearLightScreenExtentsOnly = CVarAllowClearLightSceneExtentsOnly.GetValueOnRenderThread() && SortedLightInfo.SortKey.Fields.LightType != LightType_Directional;

							if (bClearLightScreenExtentsOnly)
							{
								FRenderTargetParameters* PassParameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
								PassParameters->RenderTargets[0] = FRenderTargetBinding(InScreenShadowMaskTexture, ERenderTargetLoadAction::ENoAction);
								
								// 清理阴影遮蔽图.
								GraphBuilder.AddPass(
									RDG_EVENT_NAME("ClearQuad"),
									PassParameters,
									ERDGPassFlags::Raster,
									[this, &LightSceneProxy](FRHICommandList& RHICmdList)
								{
									for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
									{
										const FViewInfo& View = Views[ViewIndex];
										SCOPED_GPU_MASK(RHICmdList, View.GPUMask);

										FIntRect ScissorRect;
										if (!LightSceneProxy.GetScissorRect(ScissorRect, View, View.ViewRect))
										{
											ScissorRect = View.ViewRect;
										}

										if (ScissorRect.Min.X < ScissorRect.Max.X && ScissorRect.Min.Y < ScissorRect.Max.Y)
										{
											RHICmdList.SetViewport(ScissorRect.Min.X, ScissorRect.Min.Y, 0.0f, ScissorRect.Max.X, ScissorRect.Max.Y, 1.0f);
											DrawClearQuad(RHICmdList, true, FLinearColor(1, 1, 1, 1), false, 0, false, 0);
										}
										else
										{
											LightSceneProxy.GetScissorRect(ScissorRect, View, View.ViewRect);
										}
									}
								});
							}
							else
							{
								AddClearRenderTargetPass(GraphBuilder, InScreenShadowMaskTexture);
							}
						};

						ClearShadowMask(ScreenShadowMaskTexture);
						if (ScreenShadowMaskSubPixelTexture)
						{
							ClearShadowMask(ScreenShadowMaskSubPixelTexture);
						}

						RenderDeferredShadowProjections(GraphBuilder, SceneTextures, TranslucencyLightingVolumeTextures, &LightSceneInfo, ScreenShadowMaskTexture, ScreenShadowMaskSubPixelTexture, bInjectedTranslucentVolume);
					}

					bUsedShadowMaskTexture = true;
				}

				// 处理光照函数(light function).
				// Render light function to the attenuation buffer.
				if (bDirectLighting)
				{
					if (bDrawLightFunction)
					{
						const bool bLightFunctionRendered = RenderLightFunction(GraphBuilder, SceneTextures, &LightSceneInfo, ScreenShadowMaskTexture, bDrawShadows, false, false);
						bUsedShadowMaskTexture |= bLightFunctionRendered;

						if (CVarAppliedLightFunctionOnHair.GetValueOnRenderThread() > 0 && bLightFunctionRendered && ScreenShadowMaskSubPixelTexture)
						{
							RenderLightFunction(GraphBuilder, SceneTextures, &LightSceneInfo, ScreenShadowMaskSubPixelTexture, bDrawShadows, false, true);
						}
					}

					if (bDrawPreviewIndicator)
					{
						RenderPreviewShadowsIndicator(GraphBuilder, SceneTextures, &LightSceneInfo, ScreenShadowMaskTexture, bUsedShadowMaskTexture, false);
					}

					if (!bDrawShadows)
					{
						INC_DWORD_STAT(STAT_NumLightFunctionOnlyLights);
					}
				}

				if(bDirectLighting && !bInjectedTranslucentVolume)
				{
					for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
					{
						FViewInfo& View = Views[ViewIndex];
						RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

						// Accumulate this light's unshadowed contribution to the translucency lighting volume
						InjectTranslucencyLightingVolume(GraphBuilder, View, ViewIndex, Scene, *this, TranslucencyLightingVolumeTextures, VisibleLightInfos, LightSceneInfo, nullptr);
					}
				}
			
				// If we never rendered into the mask, don't attempt to read from it.
				if (!bUsedShadowMaskTexture)
				{
					ScreenShadowMaskTexture = nullptr;
					ScreenShadowMaskSubPixelTexture = nullptr;
				}

				// 渲染标准延迟光照.
				// 渲染带阴影的光源.
				// Render the light to the scene color buffer, conditionally using the attenuation buffer or a 1x1 white texture as input 
				if (bDirectLighting)
				{
					for (int32 ViewIndex = 0, ViewCount = Views.Num(); ViewIndex < ViewCount; ++ViewIndex)
					{
						const FViewInfo& View = Views[ViewIndex];

						RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, ViewCount > 1, "View%d", ViewIndex);
						SCOPED_GPU_MASK(GraphBuilder.RHICmdList, View.GPUMask);
						RenderLight(GraphBuilder, Scene, View, SceneTextures, &LightSceneInfo, ScreenShadowMaskTexture, LightingChannelsTexture, false /*bRenderOverlap*/, true /*bCloudShadow*/);
					}
				}

				if (bUseHairLighting)
				{
					for (FViewInfo& View : Views)
					{
						if (bDrawHairShadow && HairStrands::HasViewHairStrandsData(View))
						{
							FHairStrandsTransmittanceMaskData TransmittanceMaskData = RenderHairStrandsTransmittanceMask(GraphBuilder, View, &LightSceneInfo, false, ScreenShadowMaskSubPixelTexture);
							if (TransmittanceMaskData.TransmittanceMask == nullptr)
							{
								TransmittanceMaskData = DummyTransmittanceMaskData;
							}

							// Note: ideally the light should still be evaluated for hair when not casting shadow, but for preserving the old behavior, and not adding 
							// any perf. regression, we disable this light for hair rendering 
							RenderLightForHair(GraphBuilder, View, SceneTextures, &LightSceneInfo, ScreenShadowMaskSubPixelTexture, LightingChannelsTexture, TransmittanceMaskData, false /*bForwardRendering*/);
						}
					}
				}
			}
		}
	}
}

static void RenderLightArrayForOverlapViewmode(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const TArray<FViewInfo>& Views,
	const FMinimalSceneTextures& SceneTextures,
	FRDGTextureRef LightingChannelsTexture,
	const TSparseArray<FLightSceneInfoCompact, TAlignedSparseArrayAllocator<alignof(FLightSceneInfoCompact)>>& LightArray)
{
	for (auto LightIt = LightArray.CreateConstIterator(); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
		const FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;

		// Nothing to do for black lights.
		if (LightSceneInfoCompact.Color.IsAlmostBlack())
		{
			continue;
		}

		// Only render shadow casting stationary lights
		if (!LightSceneInfo->Proxy->HasStaticShadowing() ||
			 LightSceneInfo->Proxy->HasStaticLighting()  ||
			!LightSceneInfo->Proxy->CastsStaticShadow())
		{
			continue;
		}

		// Check if the light is visible in any of the views.
		for (const FViewInfo& View : Views)
		{
			SCOPED_GPU_MASK(GraphBuilder.RHICmdList, View.GPUMask);
			RenderLight(GraphBuilder, Scene, View, SceneTextures, LightSceneInfo, nullptr, LightingChannelsTexture, true /*bRenderOverlap*/, false /*bCloudShadow*/);
		}
	}
}

void FDeferredShadingSceneRenderer::RenderStationaryLightOverlap(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	FRDGTextureRef LightingChannelsTexture)
{
	if (Scene->bIsEditorScene)
	{
		// Clear to discard base pass values in scene color since we didn't skip that, to have valid scene depths
		AddClearRenderTargetPass(GraphBuilder, SceneTextures.Color.Target, FLinearColor::Black);

		RenderLightArrayForOverlapViewmode(GraphBuilder, Scene, Views, SceneTextures, LightingChannelsTexture, Scene->Lights);

		//Note: making use of FScene::InvisibleLights, which contains lights that haven't been added to the scene in the same way as visible lights
		// So code called by RenderLightArrayForOverlapViewmode must be careful what it accesses
		RenderLightArrayForOverlapViewmode(GraphBuilder, Scene, Views, SceneTextures, LightingChannelsTexture, Scene->InvisibleLights);
	}
}

static void InternalSetBoundingGeometryRasterizerState(FGraphicsPipelineStateInitializer& GraphicsPSOInit, const FViewInfo& View, bool bCameraInsideLightGeometry)
{
	if (bCameraInsideLightGeometry)
	{
		// Render backfaces with depth tests disabled since the camera is inside (or close to inside) the light geometry
		GraphicsPSOInit.RasterizerState = View.bReverseCulling ? TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI() : TStaticRasterizerState<FM_Solid, CM_CCW>::GetRHI();
	}
	else
	{
		// Render frontfaces with depth tests on to get the speedup from HiZ since the camera is outside the light geometry
		GraphicsPSOInit.RasterizerState = View.bReverseCulling ? TStaticRasterizerState<FM_Solid, CM_CCW>::GetRHI() : TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI();
	}
}

template<ECompareFunction CompareFunction>
static uint32 InternalSetBoundingGeometryDepthState(FGraphicsPipelineStateInitializer& GraphicsPSOInit, EStrataTileMaterialType TileType)
{
	// bCameraInsideLightGeometry = true  -> CompareFunction = Always
	// bCameraInsideLightGeometry = false -> CompareFunction = CF_DepthNearOrEqual
	uint32 StencilRef = 0u;
	if (TileType != EStrataTileMaterialType::ECount)
	{
		check(Strata::IsStrataEnabled());
		switch (TileType)
		{
		case EStrataTileMaterialType::ESimple : StencilRef = Strata::StencilBit_Fast;    GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CompareFunction, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, Strata::StencilBit_Fast, 0x0>::GetRHI(); break;
		case EStrataTileMaterialType::ESingle : StencilRef = Strata::StencilBit_Single;  GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CompareFunction, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, Strata::StencilBit_Single, 0x0>::GetRHI(); break;
		case EStrataTileMaterialType::EComplex: StencilRef = Strata::StencilBit_Complex; GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CompareFunction, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, Strata::StencilBit_Fast | Strata::StencilBit_Single, 0x0>::GetRHI(); break;
		default: check(false);
		}
	}
	else
	{
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CompareFunction>::GetRHI();
	}
	return StencilRef;
}

/** Sets up rasterizer and depth state for rendering bounding geometry in a deferred pass. */
static uint32 SetBoundingGeometryRasterizerAndDepthState(FGraphicsPipelineStateInitializer& GraphicsPSOInit, const FViewInfo& View, bool bCameraInsideLightGeometry, EStrataTileMaterialType TileType)
{
	uint32 StencilRef = 0u;
	InternalSetBoundingGeometryRasterizerState(GraphicsPSOInit, View, bCameraInsideLightGeometry);
	if (bCameraInsideLightGeometry)
	{
		StencilRef = InternalSetBoundingGeometryDepthState<CF_Always>(GraphicsPSOInit, TileType);
	}
	else
	{
		StencilRef = InternalSetBoundingGeometryDepthState<CF_DepthNearOrEqual>(GraphicsPSOInit, TileType);
	}

	return StencilRef;
}

// Use DBT to allow work culling on shadow lights
static void CalculateLightNearFarDepthFromBounds(const FViewInfo& View, const FSphere &LightBounds, float &NearDepth, float &FarDepth)
{
	const FMatrix ViewProjection = View.ViewMatrices.GetViewProjectionMatrix();
	const FVector ViewDirection = View.GetViewDirection();

	// push camera relative bounds center along view vec by its radius
	const FVector FarPoint = LightBounds.Center + LightBounds.W * ViewDirection;
	const FVector4 FarPoint4 = FVector4(FarPoint, 1.f);
	const FVector4 FarPoint4Clip = ViewProjection.TransformFVector4(FarPoint4);
	FarDepth = FarPoint4Clip.Z / FarPoint4Clip.W;

	// pull camera relative bounds center along -view vec by its radius
	const FVector NearPoint = LightBounds.Center - LightBounds.W * ViewDirection;
	const FVector4 NearPoint4 = FVector4(NearPoint, 1.f);
	const FVector4 NearPoint4Clip = ViewProjection.TransformFVector4(NearPoint4);
	NearDepth = NearPoint4Clip.Z / NearPoint4Clip.W;

	// negative means behind view, but we use a NearClipPlane==1.f depth

	if (NearPoint4Clip.W < 0)
		NearDepth = 1;

	if (FarPoint4Clip.W < 0)
		FarDepth = 1;

	NearDepth = FMath::Clamp(NearDepth, 0.0f, 1.0f);
	FarDepth = FMath::Clamp(FarDepth, 0.0f, 1.0f);

}

static TRDGUniformBufferRef<FDeferredLightUniformStruct> CreateDeferredLightUniformBuffer(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FLightSceneInfo& LightSceneInfo)
{
	auto* DeferredLightStruct = GraphBuilder.AllocParameters<FDeferredLightUniformStruct>();
	*DeferredLightStruct = GetDeferredLightParameters(View, LightSceneInfo);
	return GraphBuilder.CreateUniformBuffer(DeferredLightStruct);
}

static TRDGUniformBufferRef<FDeferredLightUniformStruct> CreateDeferredLightUniformBuffer(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FSimpleLightEntry& SimpleLight, const FVector& SimpleLightPosition)
{
	auto* DeferredLightStruct = GraphBuilder.AllocParameters<FDeferredLightUniformStruct>();
	*DeferredLightStruct = GetSimpleDeferredLightParameters(View, SimpleLight, SimpleLightPosition);
	return GraphBuilder.CreateUniformBuffer(DeferredLightStruct);
}

static FDeferredLightPS::FParameters GetDeferredLightPSParameters(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef SceneDepthTexture,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer,
	TRDGUniformBufferRef<FHairStrandsViewUniformParameters> HairStrandsUniformBuffer,
	FRDGTextureRef ShadowMaskTexture,
	FRDGTextureRef LightingChannelsTexture,
	bool bCloudShadow)
{
	FDeferredLightPS::FParameters Out;

	const ELightComponentType LightType = (ELightComponentType)LightSceneInfo->Proxy->GetLightType();
	const bool bIsDirectional = LightType == LightType_Directional;

	FRDGTextureRef WhiteDummy = GSystemTextures.GetWhiteDummy(GraphBuilder);
	FRDGTextureRef DepthDummy = GSystemTextures.GetDepthDummy(GraphBuilder);
	FRDGBufferRef BufferDummy = GSystemTextures.GetDefaultBuffer(GraphBuilder, 4, 0u);
	FRDGBufferSRVRef BufferDummySRV = GraphBuilder.CreateSRV(BufferDummy, PF_R32_UINT);

	// PS - General parameters
	const FVolumetricCloudRenderSceneInfo* CloudInfo = bCloudShadow ? Scene->GetVolumetricCloudSceneInfo() : nullptr;
	Out.SceneTextures = SceneTexturesUniformBuffer;
	Out.HairStrands = HairStrandsUniformBuffer;
	Out.Strata = Strata::BindStrataGlobalUniformParameters(View.StrataSceneData);
	Out.LightingChannelsTexture = LightingChannelsTexture ? LightingChannelsTexture : WhiteDummy;
	Out.LightingChannelsSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Out.CloudShadowAO = GetCloudShadowAOParameters(GraphBuilder, View, CloudInfo);
	Out.CloudShadowEnabled = SetupLightCloudTransmittanceParameters(GraphBuilder, Scene, View, LightSceneInfo, Out.CloudShadow) ? 1 : 0;
	Out.LightAttenuationTexture = ShadowMaskTexture ? ShadowMaskTexture : WhiteDummy;
	Out.LightAttenuationTextureSampler = TStaticSamplerState<SF_Point, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	Out.DummyRectLightTextureForCapsuleCompilerWarning = DepthDummy;
	Out.DummyRectLightSamplerForCapsuleCompilerWarning = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Out.IESTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Out.IESTexture = GSystemTextures.WhiteDummy->GetRHI();
	if (LightSceneInfo->Proxy->GetIESTextureResource())
	{
		Out.IESTexture = LightSceneInfo->Proxy->GetIESTextureResource()->TextureRHI;
	}
	Out.View = View.ViewUniformBuffer;
	Out.DeferredLight = CreateDeferredLightUniformBuffer(GraphBuilder, View, *LightSceneInfo);
	// PS - Hair (default value)
	Out.ScreenShadowMaskSubPixelTexture = WhiteDummy;
	Out.HairTransmittanceBuffer = BufferDummySRV;
	Out.HairTransmittanceBufferMaxCount = 0;
	Out.HairShadowMaskValid = false;
	Out.ShadowChannelMask = FVector4f(1, 1, 1, 1);
	// PS - Render Targets
	Out.RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
	if (SceneDepthTexture)
	{
		Out.RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite);
	}

	return Out;
}

// Used by RenderLights to render a light to the scene color buffer.
template<typename TShaderType, typename TParametersType>
static void InternalRenderLight(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	TShaderType& PixelShader,
	TParametersType* PassParameters,
	EStrataTileMaterialType StrataTileMaterialType,
	const TCHAR* ShaderName)
{
	const FLightSceneProxy* RESTRICT LightProxy = LightSceneInfo->Proxy;
	const bool bTransmission = LightProxy->Transmission();
	const FSphere LightBounds = LightProxy->GetBoundingSphere();
	const ELightComponentType LightType = (ELightComponentType)LightProxy->GetLightType();

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("%s", ShaderName),
		PassParameters,
		ERDGPassFlags::Raster,
		[Scene, &View, PixelShader, LightSceneInfo, PassParameters, LightBounds, LightType, StrataTileMaterialType](FRHICommandList& RHICmdList)
	{

		const bool bIsRadial = LightType != LightType_Directional;
		const bool bEnableStrataTiledPass   = StrataTileMaterialType != EStrataTileMaterialType::ECount;
		const bool bEnableStrataStencilTest = StrataTileMaterialType != EStrataTileMaterialType::ECount && bIsRadial;

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		// Set the device viewport for the view.
		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		if (LightType == LightType_Directional)
		{
			FDeferredLightVS::FPermutationDomain PermutationVectorVS;
			PermutationVectorVS.Set<FDeferredLightVS::FRadialLight>(false);
			TShaderMapRef<FDeferredLightVS> VertexShader(View.ShaderMap, PermutationVectorVS);

			Strata::FStrataTilePassVS::FPermutationDomain VSPermutationVector;
			VSPermutationVector.Set< Strata::FStrataTilePassVS::FEnableDebug >(false);
			VSPermutationVector.Set< Strata::FStrataTilePassVS::FEnableTexCoordScreenVector >(true);
			TShaderMapRef<Strata::FStrataTilePassVS> TileVertexShader(View.ShaderMap, VSPermutationVector);

			Strata::FStrataTilePassVS::FParameters VSParameters;
			if (Strata::IsStrataEnabled())
			{
				Strata::FillUpTiledPassData(StrataTileMaterialType, View, VSParameters, GraphicsPSOInit.PrimitiveType);
			}

			// Turn DBT back off
			GraphicsPSOInit.bDepthBounds = false;
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = bEnableStrataTiledPass ? TileVertexShader.GetVertexShader() : VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();		
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0x0);

			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

			if (StrataTileMaterialType != ECount)
			{
				check(Strata::IsStrataEnabled());
				SetShaderParameters(RHICmdList, TileVertexShader, TileVertexShader.GetVertexShader(), VSParameters);
				RHICmdList.DrawPrimitiveIndirect(VSParameters.TileIndirectBuffer->GetIndirectRHICallBuffer(), 0);
			}
			else
			{
				FDeferredLightVS::FParameters VSParameters2 = FDeferredLightVS::GetParameters(View);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters2);

				// Apply the directional light as a full screen quad
				DrawRectangle(
					RHICmdList,
					0, 0,
					View.ViewRect.Width(), View.ViewRect.Height(),
					View.ViewRect.Min.X, View.ViewRect.Min.Y,	
					View.ViewRect.Width(), View.ViewRect.Height(),
					View.ViewRect.Size(),
					GetSceneTextureExtent(),
					VertexShader,
					EDRF_UseTriangleOptimization);
			}
		}
		else // Radial light (LightType_Point, LightType_Spot, LightType_Rect)
		{
			// Use DBT to allow work culling on shadow lights
			// Disable depth bound when hair rendering is enabled as this rejects partially covered pixel write (with opaque background)
			GraphicsPSOInit.bDepthBounds = GSupportsDepthBoundsTest && GAllowDepthBoundsTest != 0;

			FDeferredLightVS::FPermutationDomain PermutationVectorVS;
			PermutationVectorVS.Set<FDeferredLightVS::FRadialLight>(true);
			TShaderMapRef<FDeferredLightVS> VertexShader(View.ShaderMap, PermutationVectorVS);

			const bool bCameraInsideLightGeometry = ((FVector)View.ViewMatrices.GetViewOrigin() - LightBounds.Center).SizeSquared() < FMath::Square(LightBounds.W * 1.05f + View.NearClippingDistance * 2.0f)
			//const bool bCameraInsideLightGeometry = LightProxy->AffectsBounds( FSphere( View.ViewMatrices.GetViewOrigin(), View.NearClippingDistance * 2.0f ) )
				// Always draw backfaces in ortho
				//@todo - accurate ortho camera / light intersection
				|| !View.IsPerspectiveProjection();

			const uint32 StencilRef = SetBoundingGeometryRasterizerAndDepthState(GraphicsPSOInit, View, bCameraInsideLightGeometry, StrataTileMaterialType);
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, StencilRef);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

			FDeferredLightVS::FParameters VSParameters2 = FDeferredLightVS::GetParameters(View, LightSceneInfo);
			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters2);

			// Use DBT to allow work culling on shadow lights
			if (GraphicsPSOInit.bDepthBounds)
			{
				// Can use the depth bounds test to skip work for pixels which won't be touched by the light (i.e outside the depth range)
				float NearDepth = 1.f;
				float FarDepth = 0.f;
				CalculateLightNearFarDepthFromBounds(View,LightBounds,NearDepth,FarDepth);
				if (NearDepth <= FarDepth)
				{
					NearDepth = 1.0f;
					FarDepth = 0.0f;
				}

				// UE uses reversed depth, so far < near
				RHICmdList.SetDepthBounds(FarDepth, NearDepth);
			}

			if( LightType == LightType_Point || LightType == LightType_Rect )
			{
				// Apply the point or spot light with some approximate bounding geometry,
				// So we can get speedups from depth testing and not processing pixels outside of the light's influence.
				StencilingGeometry::DrawSphere(RHICmdList);
			}
			else if (LightType == LightType_Spot)
			{
				StencilingGeometry::DrawCone(RHICmdList);
			}
		}	
	}); // RenderPass
}


/** Shader parameters for Standard Deferred Light Overlap Debug pass. */
BEGIN_SHADER_PARAMETER_STRUCT(FRenderLightParameters, )
	// PS/VS parameter structs
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightPS::FParameters, PS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightVS::FParameters, VS)
	// Strata tiles
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileListBufferSimple)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileListBufferSingle)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileListBufferComplex)
	RDG_BUFFER_ACCESS(TileIndirectBufferSimple, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(TileIndirectBufferSingle, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(TileIndirectBufferComplex, ERHIAccess::IndirectArgs)
END_SHADER_PARAMETER_STRUCT()

/** Shader parameters for Standard Deferred Light pass. */
BEGIN_SHADER_PARAMETER_STRUCT(FRenderLightOverlapParameters, )
	// PS/VS parameter structs
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightOverlapPS::FParameters, PS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightVS::FParameters, VS)
END_SHADER_PARAMETER_STRUCT()

static void RenderLight(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	const FLightSceneInfo* LightSceneInfo,
	FRDGTextureRef ScreenShadowMaskTexture,
	FRDGTextureRef LightingChannelsTexture,
	bool bRenderOverlap, 
	bool bCloudShadow)
{
	// Ensure the light is valid for this view
	if (!LightSceneInfo->ShouldRenderLight(View))
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_DirectLightRenderingTime);
	INC_DWORD_STAT(STAT_NumLightsUsingStandardDeferred);

	const FLightSceneProxy* RESTRICT LightProxy = LightSceneInfo->Proxy;
	const bool bUseIESTexture = View.Family->EngineShowFlags.TexturedLightProfiles && (LightSceneInfo->Proxy->GetIESTextureResource() != 0);
	const bool bTransmission = LightProxy->Transmission();
	const FSphere LightBounds = LightProxy->GetBoundingSphere();
	const ELightComponentType LightType = (ELightComponentType)LightProxy->GetLightType();
	const bool bIsRadial = LightType != LightType_Directional;
	const bool bSupportAnisotropyPermutation = ShouldRenderAnisotropyPass(View) && !Strata::IsStrataEnabled(); // Strata managed anisotropy differently than legacy path. No need for special permutation.

	// Debug Overlap shader
	if (bRenderOverlap)
	{
		FRenderLightOverlapParameters* PassParameters = GraphBuilder.AllocParameters<FRenderLightOverlapParameters>();
		// PS - General parameters
		PassParameters->PS.bHasValidChannel = LightSceneInfo->Proxy->GetPreviewShadowMapChannel() == INDEX_NONE ? 0.0f : 1.0f;
		PassParameters->PS.View = View.ViewUniformBuffer;
		PassParameters->PS.DeferredLight = CreateDeferredLightUniformBuffer(GraphBuilder, View, *LightSceneInfo);
		PassParameters->PS.SceneTextures = SceneTextures.UniformBuffer;
		PassParameters->PS.RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);
		if (SceneTextures.Depth.Target)
		{
			PassParameters->PS.RenderTargets.DepthStencil = FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite);
		}
		// VS - General parameters
		if (bIsRadial)
		{
			PassParameters->VS = FDeferredLightVS::GetParameters(View, LightSceneInfo, false);
		}
		else
		{
			PassParameters->VS = FDeferredLightVS::GetParameters(View, false);
		}

		FDeferredLightOverlapPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FDeferredLightOverlapPS::FRadialAttenuation>(bIsRadial);
		TShaderMapRef<FDeferredLightOverlapPS> PixelShader(View.ShaderMap, PermutationVector);		
		InternalRenderLight(GraphBuilder, Scene, View, LightSceneInfo, PixelShader, PassParameters, EStrataTileMaterialType::ECount, TEXT("Light::StandardDeferred(Overlap)"));
	}
	// Lighting shader
	else
	{
		FRenderLightParameters* PassParameters = GraphBuilder.AllocParameters<FRenderLightParameters>();
		// PS - Generatl parameters
		PassParameters->PS = GetDeferredLightPSParameters(GraphBuilder, Scene, View, LightSceneInfo, SceneTextures.Color.Target, SceneTextures.Depth.Target, SceneTextures.UniformBuffer, View.HairStrandsViewData.UniformBuffer, ScreenShadowMaskTexture, LightingChannelsTexture, bCloudShadow);
		// VS - General parameters
		if (bIsRadial)
		{
			PassParameters->VS = FDeferredLightVS::GetParameters(View, LightSceneInfo, false);
		}
		else // Directional
		{
			PassParameters->VS = FDeferredLightVS::GetParameters(View, false);
		}
		// VS - Strata tile parameters
		if (Strata::IsStrataEnabled())
		{
			PassParameters->TileListBufferSimple = View.StrataSceneData->ClassificationTileListBufferSRV[EStrataTileMaterialType::ESimple];
			PassParameters->TileListBufferSingle = View.StrataSceneData->ClassificationTileListBufferSRV[EStrataTileMaterialType::ESingle];
			PassParameters->TileListBufferComplex = View.StrataSceneData->ClassificationTileListBufferSRV[EStrataTileMaterialType::EComplex];
			PassParameters->TileIndirectBufferSimple = View.StrataSceneData->ClassificationTileIndirectBuffer[EStrataTileMaterialType::ESimple];
			PassParameters->TileIndirectBufferSingle = View.StrataSceneData->ClassificationTileIndirectBuffer[EStrataTileMaterialType::ESingle];
			PassParameters->TileIndirectBufferComplex = View.StrataSceneData->ClassificationTileIndirectBuffer[EStrataTileMaterialType::EComplex];
		}
		else
		{
			FRDGBufferRef BufferDummy = GSystemTextures.GetDefaultBuffer(GraphBuilder, 4, 0u);
			FRDGBufferSRVRef BufferDummySRV = GraphBuilder.CreateSRV(BufferDummy, PF_R32_UINT);
			PassParameters->TileListBufferSimple = BufferDummySRV;
			PassParameters->TileListBufferComplex = BufferDummySRV;
			PassParameters->TileIndirectBufferSimple = BufferDummy;
			PassParameters->TileIndirectBufferComplex = BufferDummy;
		}

		FDeferredLightPS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FDeferredLightPS::FTransmissionDim >(bTransmission);
		PermutationVector.Set< FDeferredLightPS::FHairLighting>(0);
		PermutationVector.Set< FDeferredLightPS::FLightingChannelsDim >(View.bUsesLightingChannels);
		PermutationVector.Set< FDeferredLightPS::FVisualizeCullingDim >(View.Family->EngineShowFlags.VisualizeLightCulling);
		PermutationVector.Set< FDeferredLightPS::FStrataTileType >(0);
		if (bIsRadial)
		{
			PermutationVector.Set< FDeferredLightPS::FSourceShapeDim >(LightProxy->IsRectLight() ? ELightSourceShape::Rect : ELightSourceShape::Capsule);
			PermutationVector.Set< FDeferredLightPS::FSourceTextureDim >(LightProxy->IsRectLight() && LightProxy->HasSourceTexture());
			PermutationVector.Set< FDeferredLightPS::FIESProfileDim >(bUseIESTexture);
			PermutationVector.Set< FDeferredLightPS::FAnistropicMaterials >(bSupportAnisotropyPermutation && !LightSceneInfo->Proxy->IsRectLight());
			PermutationVector.Set < FDeferredLightPS::FAtmosphereTransmittance >(false);
			PermutationVector.Set< FDeferredLightPS::FCloudTransmittance >(false);
		}
		else // Directional
		{
			PermutationVector.Set< FDeferredLightPS::FSourceShapeDim >(ELightSourceShape::Directional);
			PermutationVector.Set< FDeferredLightPS::FSourceTextureDim >(false);
			PermutationVector.Set< FDeferredLightPS::FIESProfileDim >(false);
			PermutationVector.Set< FDeferredLightPS::FAnistropicMaterials >(bSupportAnisotropyPermutation);
			// Only directional lights are rendered in this path, so we only need to check if it is use to light the atmosphere
			PermutationVector.Set< FDeferredLightPS::FAtmosphereTransmittance >(IsLightAtmospherePerPixelTransmittanceEnabled(Scene, View, LightSceneInfo));
			PermutationVector.Set< FDeferredLightPS::FCloudTransmittance >(PassParameters->PS.CloudShadowEnabled > 0);
		}
		PermutationVector = FDeferredLightPS::RemapPermutation(PermutationVector);

		// Strata tile rendering: 
		// * if the light is directional, then dispatch a set of rect tiles
		// * if the light is radial/local, then dispatch a light geometry with stencil test. The stencil buffer has been prefilled with the tile result (simple/complex) 
		//   so that the geometry get correctly stencil culled on complex/simple part of the screen
		if (Strata::IsStrataEnabled())
		{
			// Simple tiles
			{
				const EStrataTileMaterialType TileType = EStrataTileMaterialType::ESimple;
				PermutationVector.Set<FDeferredLightPS::FStrataTileType>(TileType);
				TShaderMapRef< FDeferredLightPS > PixelShader(View.ShaderMap, PermutationVector);
				InternalRenderLight(GraphBuilder, Scene, View, LightSceneInfo, PixelShader, PassParameters, TileType, TEXT("Light::StandardDeferred(Simple)"));
			}
			// Single tiles
			{
				const EStrataTileMaterialType TileType = EStrataTileMaterialType::ESingle;
				PermutationVector.Set<FDeferredLightPS::FStrataTileType>(TileType);
				TShaderMapRef< FDeferredLightPS > PixelShader(View.ShaderMap, PermutationVector);
				InternalRenderLight(GraphBuilder, Scene, View, LightSceneInfo, PixelShader, PassParameters, TileType, TEXT("Light::StandardDeferred(Single)"));
			}
			// Complex tiles
			{
				const EStrataTileMaterialType TileType = EStrataTileMaterialType::EComplex;
				PermutationVector.Set<FDeferredLightPS::FStrataTileType>(TileType);
				TShaderMapRef< FDeferredLightPS > PixelShader(View.ShaderMap, PermutationVector);
				InternalRenderLight(GraphBuilder, Scene, View, LightSceneInfo, PixelShader, PassParameters, TileType, TEXT("Light::StandardDeferred(Complex)"));
			}
		}
		else
		{
			PermutationVector.Set< FDeferredLightPS::FStrataTileType>(0);
			TShaderMapRef< FDeferredLightPS > PixelShader(View.ShaderMap, PermutationVector);
			InternalRenderLight(GraphBuilder, Scene, View, LightSceneInfo, PixelShader, PassParameters, EStrataTileMaterialType::ECount, TEXT("Light::StandardDeferred"));
		}
	}
}

/** Shader parameters for Standard Deferred Light for HairStrands pass. */
BEGIN_SHADER_PARAMETER_STRUCT(FRenderLightForHairParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightHairVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()

void FDeferredShadingSceneRenderer::RenderLightForHair(
	FRDGBuilder& GraphBuilder,
	FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	const FLightSceneInfo* LightSceneInfo,
	FRDGTextureRef HairShadowMaskTexture,
	FRDGTextureRef LightingChannelsTexture,
	const FHairStrandsTransmittanceMaskData& InTransmittanceMaskData,
	const bool bForwardRendering)
{
	// Ensure the light is valid for this view
	const bool bHairRenderingEnabled = HairStrands::HasViewHairStrandsData(View);
	if (!bHairRenderingEnabled || !LightSceneInfo->ShouldRenderLight(View) || View.HairStrandsViewData.VisibilityData.SampleLightingTexture == nullptr)
	{
		return;
	}
	
	// Sanity check
	check(InTransmittanceMaskData.TransmittanceMask);

	SCOPE_CYCLE_COUNTER(STAT_DirectLightRenderingTime);
	INC_DWORD_STAT(STAT_NumLightsUsingStandardDeferred);
	RDG_EVENT_SCOPE(GraphBuilder, "StandardDeferredLighting_Hair");
	RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

	const bool bIsDirectional = LightSceneInfo->Proxy->GetLightType() == LightType_Directional;
	const bool bCloudShadow   = bIsDirectional;

	FRenderLightForHairParameters* PassParameters = GraphBuilder.AllocParameters<FRenderLightForHairParameters>();
	// VS - General parameters
	PassParameters->VS.HairStrands = HairStrands::BindHairStrandsViewUniformParameters(View);
	// PS - General parameters
	PassParameters->PS = GetDeferredLightPSParameters(
		GraphBuilder,
		Scene,
		View,
		LightSceneInfo,
		SceneTextures.Color.Target,
		SceneTextures.Depth.Target,
		SceneTextures.UniformBuffer,
		HairStrands::BindHairStrandsViewUniformParameters(View),
		HairShadowMaskTexture,
		LightingChannelsTexture,
		bCloudShadow);

	// PS - Hair parameters
	const FIntPoint SampleLightingViewportResolution = View.HairStrandsViewData.VisibilityData.SampleLightingViewportResolution;
	PassParameters->PS.HairTransmittanceBuffer = GraphBuilder.CreateSRV(InTransmittanceMaskData.TransmittanceMask, FHairStrandsTransmittanceMaskData::Format);
	PassParameters->PS.HairTransmittanceBufferMaxCount = InTransmittanceMaskData.TransmittanceMask ? InTransmittanceMaskData.TransmittanceMask->Desc.NumElements : 0;
	PassParameters->PS.ShadowChannelMask = FVector4f(1, 1, 1, 1);
	if (HairShadowMaskTexture)
	{
		PassParameters->PS.ScreenShadowMaskSubPixelTexture = HairShadowMaskTexture;
		PassParameters->PS.HairShadowMaskValid = true;
	}
	if (bForwardRendering)
	{
		PassParameters->PS.ShadowChannelMask = FVector4f(0, 0, 0, 0);
		PassParameters->PS.ShadowChannelMask[FMath::Clamp(LightSceneInfo->GetDynamicShadowMapChannel(), 0, 3)] = 1.f;
	}
	PassParameters->PS.RenderTargets[0] = FRenderTargetBinding(View.HairStrandsViewData.VisibilityData.SampleLightingTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->PS.RenderTargets.DepthStencil = FDepthStencilBinding(nullptr, ERenderTargetLoadAction::ENoAction, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthNop_StencilNop);

	FDeferredLightPS::FPermutationDomain PermutationVector;
	PermutationVector.Set< FDeferredLightPS::FLightingChannelsDim >(View.bUsesLightingChannels);
	PermutationVector.Set< FDeferredLightPS::FVisualizeCullingDim >(false);
	PermutationVector.Set< FDeferredLightPS::FTransmissionDim >(false);
	PermutationVector.Set< FDeferredLightPS::FHairLighting>(1);
	if (bIsDirectional)
	{
		PermutationVector.Set< FDeferredLightPS::FSourceShapeDim >(ELightSourceShape::Directional);
		PermutationVector.Set< FDeferredLightPS::FSourceTextureDim >(false);
		PermutationVector.Set< FDeferredLightPS::FIESProfileDim >(false);
		PermutationVector.Set< FDeferredLightPS::FAtmosphereTransmittance >(IsLightAtmospherePerPixelTransmittanceEnabled(Scene, View, LightSceneInfo));
		PermutationVector.Set< FDeferredLightPS::FCloudTransmittance >(PassParameters->PS.CloudShadowEnabled > 0.f);
	}
	else
	{
		const bool bUseIESTexture = View.Family->EngineShowFlags.TexturedLightProfiles && LightSceneInfo->Proxy->GetIESTextureResource() != 0;
		PermutationVector.Set< FDeferredLightPS::FSourceShapeDim >(LightSceneInfo->Proxy->IsRectLight() ? ELightSourceShape::Rect : ELightSourceShape::Capsule);
		PermutationVector.Set< FDeferredLightPS::FSourceTextureDim >(LightSceneInfo->Proxy->IsRectLight() && LightSceneInfo->Proxy->HasSourceTexture());
		PermutationVector.Set< FDeferredLightPS::FIESProfileDim >(bUseIESTexture);
		PermutationVector.Set< FDeferredLightPS::FAtmosphereTransmittance >(false);
		PermutationVector.Set< FDeferredLightPS::FCloudTransmittance >(false);
	}

	TShaderMapRef<FDeferredLightHairVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FDeferredLightPS> PixelShader(View.ShaderMap, PermutationVector);

	GraphBuilder.AddPass(
		{},
		PassParameters,
		ERDGPassFlags::Raster,
		[this, VertexShader, PixelShader, PassParameters, SampleLightingViewportResolution](FRHICommandList& RHICmdList)
	{
		RHICmdList.SetViewport(0, 0, 0.0f, SampleLightingViewportResolution.X, SampleLightingViewportResolution.Y, 1.0f);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Max, BF_One, BF_One>::GetRHI();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
		GraphicsPSOInit.bDepthBounds = false;
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);
		
		RHICmdList.SetStreamSource(0, nullptr, 0);
		RHICmdList.DrawPrimitive(0, 1, 1);
	});
}

// Forward lighting version for hair
void FDeferredShadingSceneRenderer::RenderLightsForHair(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	FSortedLightSetSceneInfo &SortedLightSet,
	FRDGTextureRef ScreenShadowMaskSubPixelTexture,
	FRDGTextureRef LightingChannelsTexture)
{
	const TArray<FSortedLightSceneInfo, SceneRenderingAllocator> &SortedLights = SortedLightSet.SortedLights;
	const int32 UnbatchedLightStart = SortedLightSet.UnbatchedLightStart;
	const int32 SimpleLightsEnd = SortedLightSet.SimpleLightsEnd;

	if (ViewFamily.EngineShowFlags.DirectLighting)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "DirectLighting");

		for (FViewInfo& View : Views)
		{
			if (!HairStrands::HasViewHairStrandsData(View))
			{
				continue;
			}

			FHairStrandsTransmittanceMaskData DummyTransmittanceMaskData = CreateDummyHairStrandsTransmittanceMaskData(GraphBuilder, View.ShaderMap);
			for (int32 LightIndex = UnbatchedLightStart; LightIndex < SortedLights.Num(); LightIndex++)
			{
				const FSortedLightSceneInfo& SortedLightInfo = SortedLights[LightIndex];
				const FLightSceneInfo& LightSceneInfo = *SortedLightInfo.LightSceneInfo;
				if (LightSceneInfo.Proxy)
				{
					const bool bDrawHairShadow = SortedLightInfo.SortKey.Fields.bShadowed;
					FHairStrandsTransmittanceMaskData TransmittanceMaskData = DummyTransmittanceMaskData;
					if (bDrawHairShadow)
					{
						TransmittanceMaskData = RenderHairStrandsTransmittanceMask(GraphBuilder, View, &LightSceneInfo, true, ScreenShadowMaskSubPixelTexture);
					}

					RenderLightForHair(
						GraphBuilder,
						View,
						SceneTextures,
						&LightSceneInfo,
						ScreenShadowMaskSubPixelTexture,
						LightingChannelsTexture,
						TransmittanceMaskData,
						true /*bForwardRendering*/);
				}
			}
		}
	}
}

BEGIN_SHADER_PARAMETER_STRUCT(FSimpleLightsStandardDeferredParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightPS::FParameters, PS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDeferredLightVS::FParameters, VS)
END_SHADER_PARAMETER_STRUCT()

static FSimpleLightsStandardDeferredParameters GetRenderLightSimpleParameters(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	const FSimpleLightEntry& SimpleLight,
	const FVector& SimpleLightPosition)
{
	FSimpleLightsStandardDeferredParameters Out;

	FRDGTextureRef WhiteDummy = GSystemTextures.GetWhiteDummy(GraphBuilder);
	FRDGTextureRef DepthDummy = GSystemTextures.GetDepthDummy(GraphBuilder);
	FRDGBufferRef BufferDummy = GSystemTextures.GetDefaultBuffer(GraphBuilder, 4, 0u);
	FRDGBufferSRVRef BufferDummySRV = GraphBuilder.CreateSRV(BufferDummy, PF_R32_UINT);
	
	// PS - General parmaeters
	Out.PS.SceneTextures = SceneTextures.UniformBuffer;
	Out.PS.HairStrands = View.HairStrandsViewData.UniformBuffer;
	Out.PS.Strata = Strata::BindStrataGlobalUniformParameters(View.StrataSceneData);
	Out.PS.LightingChannelsTexture = WhiteDummy;
	Out.PS.LightingChannelsSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Out.PS.CloudShadowAO = GetCloudShadowAOParameters(GraphBuilder, View, nullptr);
	Out.PS.CloudShadowEnabled = 0;
	SetupLightCloudTransmittanceParameters(GraphBuilder, nullptr, View, nullptr, Out.PS.CloudShadow);
	Out.PS.LightAttenuationTexture = WhiteDummy;
	Out.PS.LightAttenuationTextureSampler = TStaticSamplerState<SF_Point, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	Out.PS.DummyRectLightTextureForCapsuleCompilerWarning = DepthDummy;
	Out.PS.DummyRectLightSamplerForCapsuleCompilerWarning = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Out.PS.IESTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Out.PS.IESTexture = GSystemTextures.WhiteDummy->GetRHI();
	Out.PS.View = View.ViewUniformBuffer;
	Out.PS.DeferredLight = CreateDeferredLightUniformBuffer(GraphBuilder, View, SimpleLight, SimpleLightPosition);
	// PS - Hair (default)
	Out.PS.ScreenShadowMaskSubPixelTexture = WhiteDummy;
	Out.PS.HairTransmittanceBuffer = BufferDummySRV;
	Out.PS.HairTransmittanceBufferMaxCount = 0;
	Out.PS.HairShadowMaskValid = false;
	Out.PS.ShadowChannelMask = FVector4f(1, 1, 1, 1);
	// PS - RT/Depth
	Out.PS.RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);
	if (SceneTextures.Depth.Target)
	{
		Out.PS.RenderTargets.DepthStencil = FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite);
	}

	// VS - General paramters (dummy geometry, as the geometry is setup within the pass light loop)
	FSphere SphereLight;
	SphereLight.Center = SimpleLightPosition; // Should we account for LWC Position+Tile here?
	SphereLight.W = SimpleLight.Radius;
	Out.VS = FDeferredLightVS::GetParameters(View, SphereLight, false);

	return Out;
}

void FDeferredShadingSceneRenderer::RenderSimpleLightsStandardDeferred(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	const FSimpleLightArray& SimpleLights)
{
	if (SimpleLights.InstanceData.Num() == 0)
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_DirectLightRenderingTime);
	INC_DWORD_STAT_BY(STAT_NumLightsUsingStandardDeferred, SimpleLights.InstanceData.Num());

	for (int32 ViewIndex = 0, NumViews = Views.Num(); ViewIndex < NumViews; ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];

		FSimpleLightsStandardDeferredParameters* PassParameters = GraphBuilder.AllocParameters<FSimpleLightsStandardDeferredParameters>();
		*PassParameters = GetRenderLightSimpleParameters(
			GraphBuilder,
			Scene,
			View,
			SceneTextures,
			SimpleLights.InstanceData[0], // Use a dummy light to create the PassParameter buffer. The light data will be
			FVector(0, 0, 0));		  // update dynamically with the pass light loop for efficiency purpose

		FDeferredLightPS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FDeferredLightPS::FSourceShapeDim >(ELightSourceShape::Capsule);
		PermutationVector.Set< FDeferredLightPS::FIESProfileDim >(false);
		PermutationVector.Set< FDeferredLightPS::FVisualizeCullingDim >(View.Family->EngineShowFlags.VisualizeLightCulling);
		PermutationVector.Set< FDeferredLightPS::FLightingChannelsDim >(false);
		PermutationVector.Set< FDeferredLightPS::FAnistropicMaterials >(false);
		PermutationVector.Set< FDeferredLightPS::FTransmissionDim >(false);
		PermutationVector.Set< FDeferredLightPS::FHairLighting>(0);
		PermutationVector.Set< FDeferredLightPS::FAtmosphereTransmittance >(false);
		PermutationVector.Set< FDeferredLightPS::FCloudTransmittance >(false);
		PermutationVector.Set< FDeferredLightPS::FStrataTileType>(0);
		TShaderMapRef<FDeferredLightPS> PixelShader(View.ShaderMap, PermutationVector);

		FDeferredLightVS::FPermutationDomain PermutationVectorVS;
		PermutationVectorVS.Set<FDeferredLightVS::FRadialLight>(true);
		TShaderMapRef<FDeferredLightVS> VertexShader(View.ShaderMap, PermutationVectorVS);

		// STRATA_TODO: add simple/complex tile support for simple lights
		const EStrataTileMaterialType TileType = Strata::IsStrataEnabled() ? EStrataTileMaterialType::EComplex : EStrataTileMaterialType::ECount;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("StandardDeferredSimpleLights"),
			PassParameters,
			ERDGPassFlags::Raster,
			[this, &View, &SimpleLights, ViewIndex, NumViews, PassParameters, PixelShader, VertexShader, TileType](FRHICommandList& RHICmdList)
		{
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

			// Use additive blending for color
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			for (int32 LightIndex = 0; LightIndex < SimpleLights.InstanceData.Num(); LightIndex++)
			{
				const FSimpleLightEntry& SimpleLight = SimpleLights.InstanceData[LightIndex];

				const FSimpleLightPerViewEntry& SimpleLightPerViewData = SimpleLights.GetViewDependentData(LightIndex, ViewIndex, NumViews);
				const FSphere LightBounds(SimpleLightPerViewData.Position, SimpleLight.Radius);


				// Set the device viewport for the view.
				RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

				const bool bCameraInsideLightGeometry = ((FVector)View.ViewMatrices.GetViewOrigin() - LightBounds.Center).SizeSquared() < FMath::Square(LightBounds.W * 1.05f + View.NearClippingDistance * 2.0f)
								// Always draw backfaces in ortho
								//@todo - accurate ortho camera / light intersection
								|| !View.IsPerspectiveProjection();

				SetBoundingGeometryRasterizerAndDepthState(GraphicsPSOInit, View, bCameraInsideLightGeometry, TileType);
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);


				// Update the light parameters with a custom uniform buffer
				FDeferredLightUniformStruct DeferredLightUniformsValue = GetSimpleDeferredLightParameters(View, SimpleLight, SimpleLightPerViewData);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);
				SetUniformBufferParameterImmediate(RHICmdList, RHICmdList.GetBoundPixelShader(), PixelShader->GetUniformBufferParameter<FDeferredLightUniformStruct>(), DeferredLightUniformsValue);

				// Update vertex shader parameters with custom parameters/uniform buffer
				FDeferredLightVS::FParameters ParametersVS = FDeferredLightVS::GetParameters(View, LightBounds);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), ParametersVS /*PassParameters->VS*/);

				// Apply the point or spot light with some approximately bounding geometry,
				// So we can get speedups from depth testing and not processing pixels outside of the light's influence.
				StencilingGeometry::DrawSphere(RHICmdList);
			}
		});
	}
}

class FCopyStencilToLightingChannelsPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCopyStencilToLightingChannelsPS);
	SHADER_USE_PARAMETER_STRUCT(FCopyStencilToLightingChannelsPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneStencilTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("STENCIL_LIGHTING_CHANNELS_SHIFT"), STENCIL_LIGHTING_CHANNELS_BIT_ID);
		OutEnvironment.SetRenderTargetOutputFormat(0, PF_R16_UINT);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCopyStencilToLightingChannelsPS, "/Engine/Private/DownsampleDepthPixelShader.usf", "CopyStencilToLightingChannelsPS", SF_Pixel);

FRDGTextureRef FDeferredShadingSceneRenderer::CopyStencilToLightingChannelTexture(FRDGBuilder& GraphBuilder, FRDGTextureSRVRef SceneStencilTexture)
{
	bool bNeedToCopyStencilToTexture = false;

	for (int32 ViewIndex = 0, ViewCount = Views.Num(); ViewIndex < ViewCount; ++ViewIndex)
	{
		bNeedToCopyStencilToTexture = bNeedToCopyStencilToTexture 
			|| Views[ViewIndex].bUsesLightingChannels
			// Lumen uses a bit in stencil
			|| GetViewPipelineState(Views[ViewIndex]).DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen
			|| GetViewPipelineState(Views[ViewIndex]).ReflectionsMethod == EReflectionsMethod::Lumen;
	}

	FRDGTextureRef LightingChannelsTexture = nullptr;

	if (bNeedToCopyStencilToTexture)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "CopyStencilToLightingChannels");

		{
			check(SceneStencilTexture && SceneStencilTexture->Desc.Texture);
			const FIntPoint TextureExtent = SceneStencilTexture->Desc.Texture->Desc.Extent;
			const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(TextureExtent, PF_R8_UINT, FClearValueBinding::None, TexCreate_RenderTargetable | TexCreate_ShaderResource);
			LightingChannelsTexture = GraphBuilder.CreateTexture(Desc, TEXT("LightingChannels"));
		}

		const ERenderTargetLoadAction LoadAction = ERenderTargetLoadAction::ENoAction;

		for (int32 ViewIndex = 0, ViewCount = Views.Num(); ViewIndex < ViewCount; ++ViewIndex)
		{
			const FViewInfo& View = Views[ViewIndex];
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);
			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

			auto* PassParameters = GraphBuilder.AllocParameters<FCopyStencilToLightingChannelsPS::FParameters>();
			PassParameters->RenderTargets[0] = FRenderTargetBinding(LightingChannelsTexture, View.DecayLoadAction(LoadAction));
			PassParameters->SceneStencilTexture = SceneStencilTexture;
			PassParameters->View = View.ViewUniformBuffer;

			const FScreenPassTextureViewport Viewport(LightingChannelsTexture, View.ViewRect);

			TShaderMapRef<FCopyStencilToLightingChannelsPS> PixelShader(View.ShaderMap);
			AddDrawScreenPass(GraphBuilder, {}, View, Viewport, Viewport, PixelShader, PassParameters);
		}
	}

	return LightingChannelsTexture;
}
