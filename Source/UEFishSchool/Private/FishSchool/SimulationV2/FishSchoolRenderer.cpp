// Copyright (c) 2026 Vorsel.

#include "FishSchool/SimulationV2/FishSchoolRenderer.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "FishSchool/SimulationV2/FishSchoolGrid.h"
#include "FishSchool/SimulationV2/FishSchoolVisibility.h"
#include "GameFramework/Actor.h"
#include "Math/RotationMatrix.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace FishSchool::V2
{
	void FRenderer::Initialize(UInstancedStaticMeshComponent& RootComponent, const FGrid& Grid,
	                           TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions,
	                           const FRenderSettings& Settings)
	{
		check(IsInGameThread());
		check(RootComponent.GetOwner() && RootComponent.IsRegistered());
		check(Positions.Num() == Directions.Num() && Positions.Num() == Grid.GetFishCellIds().Num());
		check(!Settings.bUseChunkedISM || (Settings.ChunkCounts.X > 0 && Settings.ChunkCounts.Y > 0 &&
			Settings.ChunkCounts.Z > 0 && Grid.GetNumCells() > 0));

		Shutdown();
		RootISM = &RootComponent;
		NumFishes = Positions.Num();
		bUseChunkedISM = Settings.bUseChunkedISM;
		GridDimensions = Grid.GetDimensions();
		ChunkCounts = Settings.ChunkCounts;
		TargetToOriginalLengthRatio = FVector(Settings.FishScale);

		if (bUseChunkedISM)
		{
			bRootWasVisible = RootComponent.GetVisibleFlag();
			bRootWasHiddenInGame = RootComponent.bHiddenInGame;
			InitRenderChunks(RootComponent, GridDimensions);
		}
		else
		{
			InitRenderInstances(RootComponent, Positions, Directions);
		}
	}

	void FRenderer::Shutdown()
	{
		check(IsInGameThread());
		for (FRenderChunk& Chunk : RenderChunks)
		{
			if (UInstancedStaticMeshComponent* Component = Chunk.ISM.Get())
			{
				Component->DestroyComponent();
			}
		}
		RenderChunks.Reset();

		if (UInstancedStaticMeshComponent* Component = RootISM.Get())
		{
			Component->ClearInstances();
			if (bUseChunkedISM)
			{
				Component->SetVisibility(bRootWasVisible);
				Component->SetHiddenInGame(bRootWasHiddenInGame);
			}
		}
		RootISM.Reset();
		AllTransforms.Reset();
		NumFishes = 0;
		bUseChunkedISM = false;
	}

	void FRenderer::Render(const FGrid& Grid, const FVisibility& Visibility,
	                       TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions,
	                       const FChunkSubmitSettings& SubmitSettings)
	{
		check(IsInGameThread());
		UInstancedStaticMeshComponent* Component = RootISM.Get();
		if (Component == nullptr)
		{
			return;
		}
		check(Positions.Num() == NumFishes && Directions.Num() == NumFishes);
		check(Grid.GetFishCellIds().Num() == NumFishes && Grid.GetDimensions() == GridDimensions);
		check(!Visibility.HasValidData() || Visibility.GetCellStatuses().Num() == Grid.GetNumCells());

		if (bUseChunkedISM)
		{
			RenderChunkedInstances(Grid, Visibility, Positions, Directions, SubmitSettings);
		}
		else
		{
			RenderAllInstances(*Component, Grid, Visibility, Positions, Directions);
		}
	}

	void FRenderer::InitRenderInstances(UInstancedStaticMeshComponent& Component,
	                                    TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions)
	{
		Component.ClearInstances();
		Component.NumCustomDataFloats = 2;
		AllTransforms.SetNum(NumFishes);

		const float Duration = 40.0f / 32.0f;
		const float AnimPlayrate = 1.0f;
		// 按鱼 ID 创建固定实例槽，并为每条鱼写入摆尾时间偏移和播放速度。
		for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
		{
			const int32 InstanceId = Component.AddInstance(RenderInstance(FishIndex, Positions, Directions), true);
			const float AnimOffset = FMath::FRand() * Duration;
			const bool bMarkRenderStateDirty = (FishIndex == NumFishes - 1);
			Component.SetCustomDataValue(InstanceId, 0, AnimOffset, false);
			Component.SetCustomDataValue(InstanceId, 1, AnimPlayrate, bMarkRenderStateDirty);
		}
	}

	void FRenderer::RenderAllInstances(UInstancedStaticMeshComponent& Component, const FGrid& Grid,
	                                   const FVisibility& Visibility, TConstArrayView<FVector3f> Positions,
	                                   TConstArrayView<FVector3f> Directions)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RenderAllInstances);
		// 单 ISM 每次都提交整张变换数组。可见性有效时，用零缩放覆盖被裁剪鱼的槽位。
		if (Visibility.HasValidData())
		{
			const TConstArrayView<int32> CellIds = Grid.GetFishCellIds();
			const TConstArrayView<ECellStatus> CellStatuses = Visibility.GetCellStatuses();
			for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
			{
				const int32 CellIndex = CellIds[FishIndex];
				const ECellStatus Status = (CellIndex >= 0) ? CellStatuses[CellIndex] : ECellStatus::OffscreenLow;
				if (Status == ECellStatus::VisibleHigh || Status == ECellStatus::OccludedLowRender)
				{
					AllTransforms[FishIndex] = RenderInstance(FishIndex, Positions, Directions);
				}
				else
				{
					AllTransforms[FishIndex] = HiddenTransform;
				}
			}
		}
		else
		{
			for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
			{
				AllTransforms[FishIndex] = RenderInstance(FishIndex, Positions, Directions);
			}
		}

		Component.BatchUpdateInstancesTransforms(0, AllTransforms, true, true, true);
	}

	FTransform FRenderer::RenderInstance(int32 FishIndex, TConstArrayView<FVector3f> Positions,
	                                     TConstArrayView<FVector3f> Directions) const
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RenderInstance);
		const FVector3f Pos = Positions[FishIndex];
		const FRotator Rot = FRotationMatrix::MakeFromX(FVector(Directions[FishIndex])).Rotator();
		return FTransform(Rot, FVector(Pos), TargetToOriginalLengthRatio);
	}

	void FRenderer::InitRenderChunks(UInstancedStaticMeshComponent& Component, const FIntVector& InGridDimensions)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(InitRenderChunks);

		Component.ClearInstances();
		Component.SetVisibility(false);
		Component.SetHiddenInGame(true);

		const int32 TotalNumRenderChunks = ChunkCounts.X * ChunkCounts.Y * ChunkCounts.Z;
		ChunkCells.X = FMath::DivideAndRoundUp(InGridDimensions.X, ChunkCounts.X);
		ChunkCells.Y = FMath::DivideAndRoundUp(InGridDimensions.Y, ChunkCounts.Y);
		ChunkCells.Z = FMath::DivideAndRoundUp(InGridDimensions.Z, ChunkCounts.Z);
		RenderChunks.SetNum(TotalNumRenderChunks);

		AActor* Owner = Component.GetOwner();
		// 为每个空间块创建一个 ISM，挂到根 ISM 下，并复用根组件的网格资源。
		for (int32 ChunkId = 0; ChunkId < TotalNumRenderChunks; ++ChunkId)
		{
			FRenderChunk& Chunk = RenderChunks[ChunkId];

			const FString Name = FString::Printf(TEXT("FishChunkISM_%d"), ChunkId);
			UInstancedStaticMeshComponent* ChunkISM =
				NewObject<UInstancedStaticMeshComponent>(Owner, *Name, RF_Transient);
			ChunkISM->SetupAttachment(&Component);
			// 由 Actor 持有运行时组件，提交器只保存弱引用。
			Owner->AddInstanceComponent(ChunkISM);
			ChunkISM->RegisterComponent();
			Chunk.ISM = ChunkISM;

			ChunkISM->SetStaticMesh(Component.GetStaticMesh());
			ChunkISM->SetCastShadow(false);
			ChunkISM->SetMobility(EComponentMobility::Movable);
			ChunkISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			ChunkISM->SetGenerateOverlapEvents(false);
			// 沿用旧版 Chunk 路径的实例数据布局。
			ChunkISM->NumCustomDataFloats = 0;

			Chunk.FishIds.Reserve(FMath::Max(NumFishes / TotalNumRenderChunks, 64));
			Chunk.SubmitTransforms.Reserve(FMath::Max(NumFishes / TotalNumRenderChunks, 64));
		}
	}

	void FRenderer::ResetRenderChunkFrameData()
	{
		for (FRenderChunk& Chunk : RenderChunks)
		{
			Chunk.FishIds.Reset();
			Chunk.Stats.NumVisibleHigh = 0;
			Chunk.Stats.NumLowRender = 0;
			Chunk.PrevState = Chunk.Stats.State;
		}
	}

	void FRenderer::BuildRenderChunkLists(const FGrid& Grid, const FVisibility& Visibility,
	                                      TConstArrayView<FVector3f> Positions)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(BuildRenderChunkLists);
		const bool bUseVisibility = Visibility.HasValidData();
		const TConstArrayView<int32> CellIds = Grid.GetFishCellIds();
		const TConstArrayView<ECellStatus> CellStatuses = bUseVisibility
			                                                  ? Visibility.GetCellStatuses()
			                                                  : TConstArrayView<ECellStatus>();
		const float InverseCellSize = 1.0f / Grid.GetCellSize();

		// 按鱼 ID 顺序筛出本帧的渲染候选，再根据 Cell 归入对应的 Chunk。
		for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
		{
			const int32 CellIndex = CellIds[FishIndex];
			const ECellStatus Status = !bUseVisibility
				                           ? ECellStatus::VisibleHigh
				                           : ((CellIndex >= 0) ? CellStatuses[CellIndex] : ECellStatus::OffscreenLow);

			if (Status != ECellStatus::VisibleHigh && Status != ECellStatus::OccludedLowRender)
			{
				continue;
			}

			int32 RenderCellIndex = CellIndex;
			if (RenderCellIndex < 0 && !bUseVisibility)
			{
				// 可见性回退时，网格外的鱼仍显示在最近的边界 Chunk，仿真归属保持原值。
				const FVector3f Offset = Positions[FishIndex] - Grid.GetOrigin();
				const int32 X = FMath::Clamp(FMath::FloorToInt(Offset.X * InverseCellSize), 0, GridDimensions.X - 1);
				const int32 Y = FMath::Clamp(FMath::FloorToInt(Offset.Y * InverseCellSize), 0, GridDimensions.Y - 1);
				const int32 Z = FMath::Clamp(FMath::FloorToInt(Offset.Z * InverseCellSize), 0, GridDimensions.Z - 1);
				RenderCellIndex = Grid.GetGridIndex(X, Y, Z);
			}
			const int32 ChunkId = GetRenderChunkIdFromCell(Grid, RenderCellIndex);
			if (ChunkId == INDEX_NONE)
			{
				continue;
			}

			FRenderChunk& Chunk = RenderChunks[ChunkId];
			Chunk.FishIds.Add(FishIndex);
			if (Status == ECellStatus::VisibleHigh)
			{
				++Chunk.Stats.NumVisibleHigh;
			}
			else
			{
				++Chunk.Stats.NumLowRender;
			}
		}
	}

	void FRenderer::UpdateRenderChunkStates(bool bUseVisibility, const FChunkSubmitSettings& SubmitSettings)
	{
		for (FRenderChunk& Chunk : RenderChunks)
		{
			if (!bUseVisibility)
			{
				// 可见性回退时，所有非空 Chunk 按可见处理。
				Chunk.Stats.State = Chunk.FishIds.IsEmpty() ? ERenderChunkState::Hidden : ERenderChunkState::Visible;
			}
			else if (Chunk.Stats.NumVisibleHigh >= SubmitSettings.VisibleThreshold)
			{
				Chunk.Stats.State = ERenderChunkState::Visible;
			}
			else if (Chunk.Stats.NumLowRender >= SubmitSettings.LowRenderThreshold)
			{
				Chunk.Stats.State = ERenderChunkState::LowRender;
			}
			else
			{
				Chunk.Stats.State = ERenderChunkState::Hidden;
			}
		}
	}

	void FRenderer::RenderChunkedInstances(const FGrid& Grid, const FVisibility& Visibility,
	                                       TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Directions,
	                                       const FChunkSubmitSettings& SubmitSettings)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RenderChunkedInstances);

		// 先清理上一帧的候选列表，再按本帧鱼数和可见性确定各块状态。
		ResetRenderChunkFrameData();
		BuildRenderChunkLists(Grid, Visibility, Positions);
		UpdateRenderChunkStates(Visibility.HasValidData(), SubmitSettings);

		// 逐块判断本帧是否提交。跳过低频块时，GPU 继续使用上次提交的变换。
		for (FRenderChunk& Chunk : RenderChunks)
		{
			if (!ShouldSubmitChunk(Chunk, SubmitSettings.LowRenderSubmitInterval))
			{
				++Chunk.Stats.TicksSinceSubmit;
				continue;
			}
			UInstancedStaticMeshComponent* Component = Chunk.ISM.Get();
			if (Component == nullptr)
			{
				continue;
			}

			const int32 CurrentVisibleCount =
				(Chunk.Stats.State == ERenderChunkState::Hidden) ? 0 : Chunk.FishIds.Num();
			const int32 SubmitCount = FMath::Max(CurrentVisibleCount, Chunk.Stats.LastSubmittedCount);
			if (SubmitCount <= 0)
			{
				Chunk.Stats.TicksSinceSubmit = 0;
				continue;
			}

			// 先确保实例容量，再按列表顺序为本帧可见鱼生成变换。
			EnsureRenderChunkCapacity(*Component, SubmitCount);
			Chunk.SubmitTransforms.SetNumUninitialized(SubmitCount);
			for (int32 i = 0; i < CurrentVisibleCount; ++i)
			{
				Chunk.SubmitTransforms[i] = RenderInstance(Chunk.FishIds[i], Positions, Directions);
			}
			// 鱼数减少时补写隐藏尾槽，避免上一帧的实例留在屏幕上。
			for (int32 i = CurrentVisibleCount; i < SubmitCount; ++i)
			{
				Chunk.SubmitTransforms[i] = HiddenTransform;
			}

			Component->BatchUpdateInstancesTransforms(0, Chunk.SubmitTransforms, true, true, true);
			Chunk.Stats.LastSubmittedCount = CurrentVisibleCount;
			Chunk.Stats.TicksSinceSubmit = 0;
		}
	}

	void FRenderer::EnsureRenderChunkCapacity(UInstancedStaticMeshComponent& Component, int32 RequiredCount)
	{
		const int32 CurrentInstanceCount = Component.GetInstanceCount();
		if (CurrentInstanceCount >= RequiredCount)
		{
			return;
		}
		for (int32 i = CurrentInstanceCount; i < RequiredCount; ++i)
		{
			Component.AddInstance(HiddenTransform, true);
		}
	}

	int32 FRenderer::GetRenderChunkIdFromCell(const FGrid& Grid, int32 CellIndex) const
	{
		if (CellIndex < 0)
		{
			return INDEX_NONE;
		}

		const FIntVector Cell = Grid.GetCellCoordinates(CellIndex);
		const int32 ChunkX = FMath::Min(Cell.X / ChunkCells.X, ChunkCounts.X - 1);
		const int32 ChunkY = FMath::Min(Cell.Y / ChunkCells.Y, ChunkCounts.Y - 1);
		const int32 ChunkZ = FMath::Min(Cell.Z / ChunkCells.Z, ChunkCounts.Z - 1);
		return ChunkX + ChunkY * ChunkCounts.X + ChunkZ * ChunkCounts.X * ChunkCounts.Y;
	}

	bool FRenderer::ShouldSubmitChunk(const FRenderChunk& Chunk, int32 LowRenderChunkSubmitInterval)
	{
		switch (Chunk.Stats.State)
		{
		case ERenderChunkState::Visible:
			return true;
		case ERenderChunkState::LowRender:
			return Chunk.Stats.TicksSinceSubmit >= LowRenderChunkSubmitInterval
				|| Chunk.PrevState != Chunk.Stats.State
				|| Chunk.Stats.LastSubmittedCount != Chunk.FishIds.Num();
		case ERenderChunkState::Hidden:
		default:
			return Chunk.Stats.LastSubmittedCount > 0;
		}
	}
} // namespace FishSchool::V2
