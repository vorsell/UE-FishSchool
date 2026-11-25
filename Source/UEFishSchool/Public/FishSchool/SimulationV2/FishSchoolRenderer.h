// Copyright (c) 2026 Vorsel.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UInstancedStaticMeshComponent;

namespace FishSchool::V2
{
	class FGrid;
	class FVisibility;

	enum class ERenderChunkState : uint8
	{
		Hidden = 0,
		LowRender = 1,
		Visible = 2
	};

	struct FRenderSettings
	{
		bool bUseChunkedISM = true;
		FIntVector ChunkCounts = FIntVector(5, 5, 5);
		float FishScale = 1.0f;
	};

	struct FChunkSubmitSettings
	{
		int32 VisibleThreshold = 120;
		int32 LowRenderThreshold = 200;
		int32 LowRenderSubmitInterval = 4;
	};

	struct FRenderChunkStats
	{
		int32 NumVisibleHigh = 0;
		int32 NumLowRender = 0;
		// 上次提交的非隐藏鱼数，不含补写的零缩放尾槽。
		int32 LastSubmittedCount = 0;
		int32 TicksSinceSubmit = 0;
		ERenderChunkState State = ERenderChunkState::Hidden;
	};

	/** 游戏线程上的 ISM 提交器。Actor 持有根组件和运行时 Chunk 组件。 */
	class FRenderer
	{
	public:
		FRenderer() = default;
		FRenderer(const FRenderer&) = delete;
		FRenderer& operator=(const FRenderer&) = delete;

		/**
		 * 初始化单 ISM 或为每个 Chunk 创建一个 ISM。使用场景中已有的根 ISM 配置。
		 * @param RootComponent Actor 已注册的根 ISM，提供网格资源和 Chunk 的挂接点。
		 * @param Grid 已初始化的网格。网格维度变化后需要重新初始化。
		 * @param Positions 按鱼 ID 索引的初始世界位置。
		 * @param Directions 按鱼 ID 索引的初始朝向。
		 * @param Settings 渲染模式、Chunk 划分和鱼模型缩放比例。
		 */
		void Initialize(UInstancedStaticMeshComponent& RootComponent, const FGrid& Grid,
		                TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions,
		                const FRenderSettings& Settings);

		/**
		 * 更新本帧的实例变换。初始化、提交和清理均在游戏线程调用。
		 * @param Grid 用 Positions 构建的当前网格。
		 * @param Visibility 当前网格的可见性。结果无效时走可见性回退。
		 * @param Positions 构建网格时使用的位置快照，即旧版 PreviousBuffer。
		 * @param Directions 与 Positions 同一缓冲区的朝向。
		 * @param SubmitSettings 当前距离档位的 Chunk 阈值和提交间隔。
		 */
		void Render(const FGrid& Grid, const FVisibility& Visibility,
		            TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions,
		            const FChunkSubmitSettings& SubmitSettings);

		/** 释放运行时 Chunk 和单 ISM 实例，由调用方在结束时执行。 */
		void Shutdown();

		int32 GetNumChunks() const { return RenderChunks.Num(); }
		const FRenderChunkStats& GetChunkStats(int32 ChunkId) const { return RenderChunks[ChunkId].Stats; }

	private:
		struct FRenderChunk
		{
			TWeakObjectPtr<UInstancedStaticMeshComponent> ISM;
			TArray<int32> FishIds;
			TArray<FTransform> SubmitTransforms;
			FRenderChunkStats Stats;
			ERenderChunkState PrevState = ERenderChunkState::Hidden;
		};

		void InitRenderInstances(UInstancedStaticMeshComponent& Component,
		                         TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions);
		void InitRenderChunks(UInstancedStaticMeshComponent& Component, const FIntVector& GridDimensions);
		void RenderAllInstances(UInstancedStaticMeshComponent& Component, const FGrid& Grid,
		                        const FVisibility& Visibility, TConstArrayView<FVector3f> Positions,
		                        TConstArrayView<FVector3f> Directions);
		void RenderChunkedInstances(const FGrid& Grid, const FVisibility& Visibility,
		                            TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions,
		                            const FChunkSubmitSettings& SubmitSettings);
		void ResetRenderChunkFrameData();
		void BuildRenderChunkLists(const FGrid& Grid, const FVisibility& Visibility,
		                           TConstArrayView<FVector3f> Positions);
		void UpdateRenderChunkStates(bool bUseVisibility, const FChunkSubmitSettings& SubmitSettings);
		static void EnsureRenderChunkCapacity(UInstancedStaticMeshComponent& Component, int32 RequiredCount);
		int32 GetRenderChunkIdFromCell(const FGrid& Grid, int32 CellIndex) const;
		static bool ShouldSubmitChunk(const FRenderChunk& Chunk, int32 LowRenderChunkSubmitInterval);
		FTransform RenderInstance(int32 FishIndex, TConstArrayView<FVector3f> Positions,
		                          TConstArrayView<FVector3f> Directions) const;

		TWeakObjectPtr<UInstancedStaticMeshComponent> RootISM;
		TArray<FTransform> AllTransforms;
		TArray<FRenderChunk> RenderChunks;
		FIntVector GridDimensions = FIntVector::ZeroValue;
		FIntVector ChunkCounts = FIntVector::ZeroValue;
		FIntVector ChunkCells = FIntVector::ZeroValue;
		FVector TargetToOriginalLengthRatio = FVector::OneVector;
		int32 NumFishes = 0;
		bool bUseChunkedISM = false;
		bool bRootWasVisible = true;
		bool bRootWasHiddenInGame = false;

		inline static const FTransform HiddenTransform =
			FTransform(FRotator::ZeroRotator, FVector(0, 0, -100000), FVector::ZeroVector);
	};
} // namespace FishSchool::V2
