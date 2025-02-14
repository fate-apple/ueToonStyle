// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenSceneData.h: Private scene manager definitions.
=============================================================================*/

#pragma once

// Dependencies.

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "RHI.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "SceneTypes.h"
#include "UniformBuffer.h"
#include "LumenSparseSpanArray.h"
#include "LumenUniqueList.h"
#include "LumenSurfaceCacheFeedback.h"
#include "Containers/BinaryHeap.h"
#include "Lumen.h"
#include "MeshCardRepresentation.h"
#include "LumenHeightfields.h"

class FMeshCardsBuildData;
class FLumenCardBuildData;
class FLumenCardPassUniformParameters;
class FPrimitiveSceneInfo;
class FDistanceFieldSceneData;
class FLumenCardRenderer;
struct FLumenPageTableEntry;

static constexpr uint32 MaxDistantCards = 8;

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FLumenCardScene, )
	SHADER_PARAMETER(uint32, NumCards)
	SHADER_PARAMETER(uint32, NumMeshCards)
	SHADER_PARAMETER(uint32, NumCardPages)
	SHADER_PARAMETER(uint32, NumHeightfields)
	SHADER_PARAMETER(uint32, MaxConeSteps)
	SHADER_PARAMETER(FVector2f, PhysicalAtlasSize)
	SHADER_PARAMETER(FVector2f, InvPhysicalAtlasSize)
	SHADER_PARAMETER(float, IndirectLightingAtlasDownsampleFactor)
	SHADER_PARAMETER(uint32, NumDistantCards)
	SHADER_PARAMETER(float, DistantSceneMaxTraceDistance)
	SHADER_PARAMETER(FVector3f, DistantSceneDirection)
	SHADER_PARAMETER_SCALAR_ARRAY(uint32, DistantCardIndices, [MaxDistantCards])
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, CardData)
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, CardPageData)
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, MeshCardsData)
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, HeightfieldData)
	SHADER_PARAMETER_SRV(ByteAddressBuffer, PageTableBuffer)
	SHADER_PARAMETER_SRV(ByteAddressBuffer, SceneInstanceIndexToMeshCardsIndexBuffer)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OpacityAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AlbedoAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EmissiveAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthAtlas)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

//Surface Mipmap，其 PageTableSpanOffset 和 PageTableSpanSize 成员可以直接索引到 PageTable 中
//surface Mipmap 是Surface Cache 上对同一个 Mesh 同时存在多个分辨率不同的 Material Attribute，(m某一层级的内容)
//Capture Atlas and  Card Atlas 是Surface Cache 资源管理方式，
struct FLumenSurfaceMipMap
{
	uint8 SizeInPagesX = 0;
	uint8 SizeInPagesY = 0;
	uint8 ResLevelX = 0;
	uint8 ResLevelY = 0;

	int32 PageTableSpanOffset = -1;
	uint16 PageTableSpanSize = 0;
	bool bLocked = false;

	bool IsAllocated() const
	{
		return PageTableSpanSize > 0;
	}

	FIntPoint GetSizeInPages() const
	{
		return FIntPoint(SizeInPagesX, SizeInPagesY);
	}

	int32 GetPageTableIndex(int32 LocalPageIndex) const
	{
		return PageTableSpanOffset + LocalPageIndex;
	}
};

struct FLumenMipMapDesc
{
	FIntPoint Resolution;
	FIntPoint SizeInPages;
	FIntPoint PageResolution;
	uint16 ResLevelX;
	uint16 ResLevelY;
	bool bSubAllocation;
};

//Card 的数据结构，用于捕获 Mesh 材质属性，包括生成 Surface Mipmap 以及对应的方向索引等信息。
//注意这里的 Mipmap 与传统意义上的 Mipmap 不同，分辨率从小到大排序，Level 越小分辨率越低。
class FLumenCard
{
public:
	FLumenCard();
	~FLumenCard();

	FLumenCardOBB LocalOBB;
	FLumenCardOBB WorldOBB;
	FLumenCardOBB MeshCardsOBB;

	bool bVisible = false;
	bool bHeightfield = false;
	bool bDistantScene = false;

	// First and last allocated mip map
	uint8 MinAllocatedResLevel = UINT8_MAX;
	uint8 MaxAllocatedResLevel = 0;

	// Requested res level based on distance. Actual allocated res level may be lower if atlas is out of space.
	uint8 DesiredLockedResLevel = 0;

	// Surface cache allocations per mip map, indexed by [ResLevel - Lumen::MinResLevel]
	FLumenSurfaceMipMap SurfaceMipMaps[Lumen::NumResLevels];

	int32 MeshCardsIndex = -1;
	int32 IndexInMeshCards = -1;
	uint8 IndexInBuildData = UINT8_MAX;
	uint8 AxisAlignedDirectionIndex = UINT8_MAX;
	float ResolutionScale = 1.0f;

	void Initialize(
		float InResolutionScale,
		const FMatrix& LocalToWorld,
		const FLumenMeshCards& InMeshCardsInstance,
		const FLumenCardBuildData& CardBuildData,
		int32 InIndexInMeshCards,
		int32 InMeshCardsIndex,
		uint8 InIndexInBuildData);

	void SetTransform(const FMatrix44f& LocalToWorld, const FLumenMeshCards& MeshCards);

	void UpdateMinMaxAllocatedLevel();

	bool IsAllocated() const
	{
		return MinAllocatedResLevel <= MaxAllocatedResLevel;
	}

	struct FSurfaceStats
	{
		uint32 NumVirtualTexels = 0;
		uint32 NumLockedVirtualTexels = 0;
		uint32 NumPhysicalTexels = 0;
		uint32 NumLockedPhysicalTexels = 0;
		uint32 DroppedResLevels = 0;
	};

	void GetSurfaceStats(const TSparseSpanArray<FLumenPageTableEntry>& PageTable, FSurfaceStats& Stats) const;

	FLumenSurfaceMipMap& GetMipMap(int32 ResLevel)
	{
		const int32 MipIndex = ResLevel - Lumen::MinResLevel;
		check(MipIndex >= 0 && MipIndex < UE_ARRAY_COUNT(SurfaceMipMaps));
		return SurfaceMipMaps[MipIndex]; 
	}

	FIntPoint ResLevelToResLevelXYBias() const;
	void GetMipMapDesc(int32 ResLevel, FLumenMipMapDesc& Desc) const;

	const FLumenSurfaceMipMap& GetMipMap(int32 ResLevel) const
	{
		const int32 MipIndex = ResLevel - Lumen::MinResLevel;
		check(MipIndex >= 0 && MipIndex < UE_ARRAY_COUNT(SurfaceMipMaps));
		return SurfaceMipMaps[MipIndex];
	}
};

class FLumenPrimitiveGroupRemoveInfo
{
public:
	FLumenPrimitiveGroupRemoveInfo(const FPrimitiveSceneInfo* InPrimitive, int32 InPrimitiveIndex)
		: Primitive(InPrimitive)
		, PrimitiveIndex(InPrimitiveIndex)
		, LumenPrimitiveGroupIndices(InPrimitive->LumenPrimitiveGroupIndices)
	{}

	// Must not be dereferenced after creation, the primitive was removed from the scene and deleted
	// Value of the pointer is still useful for map lookups
	const FPrimitiveSceneInfo* Primitive;

	// Need to copy by value as this is a deferred remove and Primitive may be already destroyed
	int32 PrimitiveIndex;
	TArray<int32, TInlineAllocator<1>> LumenPrimitiveGroupIndices;
};

// Defines a group of scene primitives for a given LOD level
//成 Lumen Scene 的基本图元集合，对应一个到多个 FPrimitiveSceneInfo，
//为了快速索引，在 FPrimitiveSceneInfo 中也保存了对应的 FLumenPrimitiveGroup 的索引列表。
class FLumenPrimitiveGroup
{
public:
	TArray<FPrimitiveSceneInfo*, TInlineAllocator<1>> Primitives;
	int32 PrimitiveInstanceIndex = -1;
	//指向 FLumenMeshCards 的索引
	int32 MeshCardsIndex = -1;
	int32 HeightfieldIndex = -1;

	// 由 Primitives 内所有FPrimitiveSceneInfo的BBOX合并而来
	FRenderBounds WorldSpaceBoundingBox;
	Experimental::FHashElementId RayTracingGroupMapElementId;
	float CardResolutionScale = 1.0f;

	bool bValidMeshCards = false;
	bool bFarField = false;
	bool bHeightfield = false;
	bool bEmissiveLightSource = false;

	bool HasMergedInstances() const;

	bool HasMergedPrimitives() const
	{
		return RayTracingGroupMapElementId.IsValid();
	}
};

//PageTable 中的每一项数据，存储了 Surface Mipmap 与 Capture Atlas\Card Atlas 寻址的相关信息，
//包括物理页地址和物理存储区域，同时还包括对应的 Card 索引、Mipmap Level 等数据。
struct FLumenPageTableEntry
{
	// Allocated physical page data
	FIntPoint PhysicalPageCoord = FIntPoint(-1, -1);

	// Allows to point to a sub-allocation inside a shared physical page
	FIntRect PhysicalAtlasRect;

	// Sampling data, can point to a coarser page
	uint32 SamplePageIndex = 0;
	uint16 SampleAtlasBiasX = 0;
	uint16 SampleAtlasBiasY = 0;
	uint16 SampleCardResLevelX = 0;
	uint16 SampleCardResLevelY = 0;

	// CardPage for atlas operations
	int32 CardIndex = -1;
	uint8 ResLevel = 0;
	FVector4f CardUVRect;

	FIntPoint SubAllocationSize = FIntPoint(-1, -1);

	bool IsSubAllocation() const
	{
		return SubAllocationSize.X >= 0 || SubAllocationSize.Y >= 0;
	}

	bool IsMapped() const 
	{ 
		return PhysicalPageCoord.X >= 0 && PhysicalPageCoord.Y >= 0;
	}

	uint32 GetNumVirtualTexels() const
	{
		return IsSubAllocation() ? SubAllocationSize.X * SubAllocationSize.Y : Lumen::VirtualPageSize * Lumen::VirtualPageSize;
	}

	uint32 GetNumPhysicalTexels() const
	{
		return IsMapped() ? PhysicalAtlasRect.Area() : 0;
	}
};

class FSurfaceCacheRequest
{
public:
	int32 CardIndex = -1;
	uint16 ResLevel = 0;
	uint16 LocalPageIndex = UINT16_MAX;
	float Distance = 0.0f;

	bool IsLockedMip() const { return LocalPageIndex == UINT16_MAX; }
};

union FVirtualPageIndex
{
	FVirtualPageIndex() {}
	FVirtualPageIndex(int32 InCardIndex, uint16 InResLevel, uint16 InLocalPageIndex)
		: CardIndex(InCardIndex), ResLevel(InResLevel), LocalPageIndex(InLocalPageIndex)
	{}

	uint64 PackedValue;
	struct
	{
		int32 CardIndex;
		uint16 ResLevel;
		uint16 LocalPageIndex;
	};
};

// Physical page allocator, which routes sub page sized allocations to a bin allocator
class FLumenSurfaceCacheAllocator
{
public:
	struct FAllocation
	{
		// Allocated physical page data
		FIntPoint PhysicalPageCoord = FIntPoint(-1, -1);

		// Allows to point to a sub-allocation inside a shared physical page
		FIntRect PhysicalAtlasRect;
	};

	struct FBinStats
	{
		FIntPoint ElementSize;
		int32 NumAllocations;
		int32 NumPages;
	};

	struct FStats
	{
		uint32 NumFreePages = 0;

		uint32 BinNumPages = 0;
		uint32 BinNumWastedPages = 0;
		uint32 BinPageFreeTexels = 0;

		TArray<FBinStats> Bins;
	};

	void Init(FIntPoint PageAtlasSizeInPages);
	void Allocate(const FLumenPageTableEntry& Page, FAllocation& Allocation);
	void Free(const FLumenPageTableEntry& Page);
	bool IsSpaceAvailable(const FLumenCard& Card, int32 ResLevel, bool bSinglePage) const;
	void GetStats(FStats& Stats) const;

private:

	struct FPageBinAllocation
	{
	public:
		FIntPoint PageCoord;
		TArray<FIntPoint> FreeList;
	};

	struct FPageBin
	{
		FPageBin(FIntPoint InElementSize);

		int32 GetNumElements() const
		{
			return PageSizeInElements.X * PageSizeInElements.Y;
		}

		FIntPoint ElementSize = FIntPoint(0, 0);
		FIntPoint PageSizeInElements = FIntPoint(0, 0);

		TArray<FPageBinAllocation, TInlineAllocator<16>> BinAllocations;
	};

	FIntPoint AllocatePhysicalAtlasPage();
	void FreePhysicalAtlasPage(FIntPoint PageCoord);

	TArray<FIntPoint> PhysicalPageFreeList;
	TArray<FPageBin> PageBins;
};

enum class ESurfaceCacheCompression : uint8
{
	Disabled,
	UAVAliasing,
	CopyTextureRegion
};

// Temporaries valid only in a single frame
struct FLumenSceneFrameTemporaries
{
	// Current frame's buffers for writing feedback
	FLumenSurfaceCacheFeedback::FFeedbackResources SurfaceCacheFeedbackResources;

	FRDGBufferRef CardPageLastUsedBuffer = nullptr;
	FRDGBufferRef CardPageHighResLastUsedBuffer = nullptr;
};

// Tracks scene-wide lighting state whose changes we should propagate quickly by flushing various lighting caches
class FLumenGlobalLightingState
{
public:
	FLinearColor DirectionalLightColor;
	FLinearColor SkyLightColor;
	bool bDirectionalLightValid;
	bool bSkyLightValid;

	FLumenGlobalLightingState()
	{
		DirectionalLightColor = FLinearColor::Black;
		SkyLightColor = FLinearColor::Black;
		bDirectionalLightValid = false;
		bSkyLightValid = false;
	}
};

class FLumenSceneData
{
public:
	// Clear all cached state like surface cache atlas. Including extra state like final lighting. Used only for debugging.
	bool bDebugClearAllCachedState = false;

	FScatterUploadBuffer UploadBuffer;
	FScatterUploadBuffer ByteBufferUploadBuffer;

	TSparseSpanArray<FLumenCard> Cards;
	FUniqueIndexList CardIndicesToUpdateInBuffer;
	FRWBufferStructured CardBuffer;

	// Modified bounds for caching voxel lighting
	TArray<FRenderBounds> PrimitiveModifiedBounds;

	// Primitive groups
	TSparseSpanArray<FLumenPrimitiveGroup> PrimitiveGroups;
	// Maps RayTracingGroupId to a specific Primitive Group Index
	Experimental::TRobinHoodHashMap<int32, int32> RayTracingGroups;

	// List of landscape primitive added to the Lumen scene
	TArray<const FPrimitiveSceneInfo*> LandscapePrimitives;

	// Mesh Cards
	FUniqueIndexList MeshCardsIndicesToUpdateInBuffer;
	TSparseSpanArray<FLumenMeshCards> MeshCards;
	FRWBufferStructured MeshCardsBuffer;

	// Heightfields
	FUniqueIndexList HeightfieldIndicesToUpdateInBuffer;
	TSparseSpanArray<FLumenHeightfield> Heightfields;
	FRWBufferStructured HeightfieldBuffer;

	// GPUScene instance index to MeshCards mapping
	FUniqueIndexList PrimitivesToUpdateMeshCards;
	FRWByteAddressBuffer SceneInstanceIndexToMeshCardsIndexBuffer;

	TArray<int32, TInlineAllocator<8>> DistantCardIndices;

	// Single card tile per FLumenPageTableEntry. Used for various atlas update operations
	FRWBufferStructured CardPageBuffer;

	// Last frame index when this page was sampled from. Used to controlling page update rate
	TRefCountPtr<FRDGPooledBuffer> CardPageLastUsedBuffer;
	TRefCountPtr<FRDGPooledBuffer> CardPageHighResLastUsedBuffer;

	// Captured from the triangle scene
	TRefCountPtr<IPooledRenderTarget> AlbedoAtlas;
	TRefCountPtr<IPooledRenderTarget> OpacityAtlas;
	TRefCountPtr<IPooledRenderTarget> NormalAtlas;
	TRefCountPtr<IPooledRenderTarget> EmissiveAtlas;
	TRefCountPtr<IPooledRenderTarget> DepthAtlas;

	// Generated
	TRefCountPtr<IPooledRenderTarget> DirectLightingAtlas;
	TRefCountPtr<IPooledRenderTarget> IndirectLightingAtlas;
	TRefCountPtr<IPooledRenderTarget> RadiosityNumFramesAccumulatedAtlas;
	TRefCountPtr<IPooledRenderTarget> FinalLightingAtlas;

	// Radiosity probes
	TRefCountPtr<IPooledRenderTarget> RadiosityTraceRadianceAtlas;
	TRefCountPtr<IPooledRenderTarget> RadiosityTraceHitDistanceAtlas;
	TRefCountPtr<IPooledRenderTarget> RadiosityProbeSHRedAtlas;
	TRefCountPtr<IPooledRenderTarget> RadiosityProbeSHGreenAtlas;
	TRefCountPtr<IPooledRenderTarget> RadiosityProbeSHBlueAtlas;

	// Virtual surface cache feedback
	FLumenSurfaceCacheFeedback SurfaceCacheFeedback;

	FLumenGlobalLightingState GlobalLightingState;

	bool bFinalLightingAtlasContentsValid;
	int32 NumMeshCardsToAdd = 0;
	int32 NumLockedCardsToUpdate = 0;
	int32 NumHiResPagesToAdd = 0;

	bool bTrackAllPrimitives;
	TSet<FPrimitiveSceneInfo*> PendingAddOperations;
	TSet<FPrimitiveSceneInfo*> PendingUpdateOperations;
	TArray<FLumenPrimitiveGroupRemoveInfo> PendingRemoveOperations;

	FLumenSceneData(EShaderPlatform ShaderPlatform, EWorldType::Type WorldType);
	~FLumenSceneData();

	void AddPrimitive(FPrimitiveSceneInfo* InPrimitive);
	void UpdatePrimitive(FPrimitiveSceneInfo* InPrimitive);
	void UpdatePrimitiveInstanceOffset(int32 PrimitiveIndex);
	void RemovePrimitive(FPrimitiveSceneInfo* InPrimitive, int32 PrimitiveIndex);
	void ResetAndConsolidate();

	void AddMeshCards(int32 PrimitiveGroupIndex);
	void UpdateMeshCards(const FMatrix& LocalToWorld, int32 MeshCardsIndex, const FMeshCardsBuildData& MeshCardsBuildData);
	void RemoveMeshCards(FLumenPrimitiveGroup& PrimitiveGroup);

	void RemoveCardFromAtlas(int32 CardIndex);

	bool HasPendingOperations() const
	{
		return PendingAddOperations.Num() > 0 || PendingUpdateOperations.Num() > 0 || PendingRemoveOperations.Num() > 0;
	}

	void DumpStats(const FDistanceFieldSceneData& DistanceFieldSceneData, bool bDumpMeshDistanceFields, bool bDumpPrimitiveGroups);
	bool UpdateAtlasSize();
	void RemoveAllMeshCards();
	void UploadPageTable(FRDGBuilder& GraphBuilder);

	void AllocateCardAtlases(FRDGBuilder& GraphBuilder, const FViewInfo& View);
	void ReallocVirtualSurface(FLumenCard& Card, int32 CardIndex, int32 ResLevel, bool bLockPages);
	void FreeVirtualSurface(FLumenCard& Card, uint8 FromResLevel, uint8 ToResLevel);

	void UpdateCardMipMapHierarchy(FLumenCard& Card);

	bool IsPhysicalSpaceAvailable(const FLumenCard& Card, int32 ResLevel, bool bSinglePage) const
	{
		return SurfaceCacheAllocator.IsSpaceAvailable(Card, ResLevel, bSinglePage);
	}

	void ForceEvictEntireCache();
	bool EvictOldestAllocation(uint32 MaxFramesSinceLastUsed, TSparseUniqueList<int32, SceneRenderingAllocator>& DirtyCards);

	uint32 GetSurfaceCacheUpdateFrameIndex() const;
	void IncrementSurfaceCacheUpdateFrameIndex();

	const FLumenPageTableEntry& GetPageTableEntry(int32 PageTableIndex) const { return PageTable[PageTableIndex]; }
	FLumenPageTableEntry& GetPageTableEntry(int32 PageTableIndex) { return PageTable[PageTableIndex]; }
	void MapSurfaceCachePage(const FLumenSurfaceMipMap& MipMap, int32 PageTableIndex, FRHIGPUMask GPUMask);
	int32 GetNumCardPages() const { return PageTable.Num(); }
	FIntPoint GetPhysicalAtlasSize() const { return PhysicalAtlasSize; }
	FIntPoint GetRadiosityAtlasSize() const;
	FIntPoint GetCardCaptureAtlasSizeInPages() const;
	FIntPoint GetCardCaptureAtlasSize() const;
	uint32 GetCardCaptureRefreshNumTexels() const;
	uint32 GetCardCaptureRefreshNumPages() const;
	ESurfaceCacheCompression GetPhysicalAtlasCompression() const { return PhysicalAtlasCompression; }

	void UpdateSurfaceCacheFeedback(const TArray<FVector, TInlineAllocator<2>>& LumenSceneCameraOrigins, TArray<FSurfaceCacheRequest, SceneRenderingAllocator>& MeshCardsUpdate);

	void ProcessLumenSurfaceCacheRequests(
		const FViewInfo& MainView,
		float MaxCardUpdateDistanceFromCamera,
		int32 MaxTileCapturesPerFrame,
		FLumenCardRenderer& LumenCardRenderer,
		FRHIGPUMask GPUMask,
		const TArray<FSurfaceCacheRequest, SceneRenderingAllocator>& SurfaceCacheRequests);

	FShaderResourceViewRHIRef GetPageTableBufferSRV() const { return PageTableBuffer.SRV;  };

	int32 GetMeshCardsIndex(const FPrimitiveSceneInfo* PrimitiveSceneInfo, int32 InstanceIndex) const;

	void CopyBuffersForResample(FRDGBuilder& GraphBuilder, FShaderResourceViewRHIRef& LastCardBufferForResampleSRV, FShaderResourceViewRHIRef& LastPageTableBufferForResampleSRV);

private:

	void AddMeshCardsFromBuildData(int32 PrimitiveGroupIndex, const FMatrix& LocalToWorld, const FMeshCardsBuildData& MeshCardsBuildData, FLumenPrimitiveGroup& PrimitiveGroup);

	void UnmapSurfaceCachePage(bool bLocked, FLumenPageTableEntry& Page, int32 PageIndex);

	// Frame index used to time-splice various surface cache update operations
	// 0 is a special value, and means that surface contains default data
	uint32 SurfaceCacheUpdateFrameIndex = 1;

	// Virtual surface cache page table
	FIntPoint PhysicalAtlasSize = FIntPoint(0, 0);
	ESurfaceCacheCompression PhysicalAtlasCompression;
	FLumenSurfaceCacheAllocator SurfaceCacheAllocator;

	TSparseSpanArray<FLumenPageTableEntry> PageTable;
	TArray<int32> PageTableIndicesToUpdateInBuffer;
	FRWByteAddressBuffer PageTableBuffer;

	FRWBufferStructured LastCardBufferForResample;
	FRWByteAddressBuffer LastPageTableBufferForResample;

	// List of high res allocated physical pages which can be deallocated on demand, ordered by last used frame
	// FeedbackFrameIndex, PageTableIndex
	FBinaryHeap<uint32, uint32> UnlockedAllocationHeap;

	// List of pages ordered by last captured frame used to periodically recapture pages, or for multi-GPU scenarios,
	// to track that a page is uninitialized on a particular GPU, and needs to be captured for the first time (indicated
	// by a CapturedSurfaceCacheFrameIndex value of zero).
	// CapturedSurfaceCacheFrameIndex, PageTableIndex
	FBinaryHeap<uint32, uint32> LastCapturedPageHeap[MAX_NUM_GPUS];
};