// Copyright (c) 2026 Vorsel.

#include "FishSchool/SimulationV2/FishSchoolVisibility.h"

#include "FishSchool/SimulationV2/FishSchoolGrid.h"

#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include <cfloat>

namespace FishSchool::V2
{
	void FVisibility::EnsureInitialized(const FGrid& Grid, const FVisibilitySettings& Settings)
	{
		const FIntVector Dimensions = Grid.GetDimensions();
		if (AllocatedSettings.IsSet() && AllocatedSettings.GetValue() == Settings && GridDimensions == Dimensions)
		{
			return;
		}

		check(Grid.GetNumCells() > 0);
		check(Settings.DepthBufferSizeX > 0 && Settings.DepthBufferSizeY > 0);
		check(Settings.NumDenseTraversalTasks > 0 && Settings.NumActivatedTraversalTasks > 0);
		check(Settings.DilationRadius >= 0 && Settings.DilationRadius < 127);

		GridDimensions = Dimensions;
		CellStatuses.SetNum(Grid.GetNumCells());
		bHasValidVisibilityData = false;
		GridDepthBufferSizeX = Settings.DepthBufferSizeX;
		GridDepthBufferSizeY = Settings.DepthBufferSizeY;
		GridVisibilityDilationRadius = Settings.DilationRadius;
		NumDenseGridTraversalTasks = Settings.NumDenseTraversalTasks;
		NumActivatedGridTraversalTasks = Settings.NumActivatedTraversalTasks;

		DilatedGridSize = 2 * GridVisibilityDilationRadius + 1;
		TotalDepthBufferSize = GridDepthBufferSizeX * GridDepthBufferSizeY;
		NdcMarginX = 2.0f * Settings.ViewBorderExpandPercent + 1.0f;
		InvNdcMarginX = 1.0f / 2.0f / NdcMarginX;
		NdcMarginY = 2.0f * Settings.ViewBorderExpandPercent + 1.0f;
		InvNdcMarginY = 1.0f / 2.0f / NdcMarginY;
		DepthBuffer.CellIndexBufferX.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
		DepthBuffer.CellIndexBufferY.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
		DepthBuffer.CellIndexBufferZ.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
		DepthBuffer.CellIndexBufferW.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
		DepthBuffer.CellDepthBuffer.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
		DilateOffsets.SetNum(DilatedGridSize * DilatedGridSize * DilatedGridSize);
		int32 DilateOffsetsIndex = 0;
		// 把膨胀范围内的相对坐标换成一维 Cell 偏移，逐帧扩张时可以直接使用。
		for (int8 x = -GridVisibilityDilationRadius; x < GridVisibilityDilationRadius + 1; ++x)
		{
			for (int8 y = -GridVisibilityDilationRadius; y < GridVisibilityDilationRadius + 1; ++y)
			{
				for (int8 z = -GridVisibilityDilationRadius; z < GridVisibilityDilationRadius + 1; ++z)
				{
					DilateOffsets[DilateOffsetsIndex] = Grid.GetGridIndex(x, y, z);
					DilateOffsetsIndex++;
				}
			}
		}
		// 记录完成分配的配置，关闭期间的档位切换不会改写它。
		AllocatedSettings = Settings;
	}

	void FVisibility::Update(const FGrid& Grid, const FMatrix* InViewProjection)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FishSchoolV2_UpdateVisibility);
		bHasValidVisibilityData = false;
		if (InViewProjection == nullptr)
		{
			return;
		}

		check(AllocatedSettings.IsSet() && GridDimensions == Grid.GetDimensions());
		ViewProjectionMatrix = *InViewProjection;
		ResetDepthBuffer();
		const int32 NumDenseCells = Grid.GetDenseCells().Num();
		const int32 NumActivatedCells = Grid.GetActivatedCells().Num();
		// 密集格先写入各任务自己的深度槽。
		ParallelFor(NumDenseGridTraversalTasks, [&](int32 TaskId)
		{
			const int32 Begin = NumDenseCells * TaskId / NumDenseGridTraversalTasks;
			const int32 End = (TaskId == NumDenseGridTraversalTasks - 1)
				                  ? NumDenseCells
				                  : NumDenseCells * (TaskId + 1) / NumDenseGridTraversalTasks;
			UpdateDepthBufferInHighDensityAreas(Grid, TaskId, Begin, End);
		});
		if (NumDenseGridTraversalTasks > 1)
		{
			MergeDepthBuffer(NumDenseGridTraversalTasks);
		}
		// 稀疏格读取全局最近深度，结果不写回深度表。
		ParallelFor(NumActivatedGridTraversalTasks, [&](int32 TaskId)
		{
			const int32 Begin = NumActivatedCells * TaskId / NumActivatedGridTraversalTasks;
			const int32 End = (TaskId == NumActivatedGridTraversalTasks - 1)
				                  ? NumActivatedCells
				                  : NumActivatedCells * (TaskId + 1) / NumActivatedGridTraversalTasks;
			UpdateDepthBufferInLowDensityAreas(Grid, Begin, End);
		});
		if (DilatedGridSize > 1)
		{
			DilateGrid(Grid);
		}
		bHasValidVisibilityData = true;
	}

	void FVisibility::ResetDepthBuffer()
	{
		// -1 表示该深度桶还没有写入 Cell。
		FMemory::Memset(DepthBuffer.CellIndexBufferW.GetData(), 0xFF,
		                NumDenseGridTraversalTasks * TotalDepthBufferSize * sizeof(int32));
		for (int32 DepthBufferIndex = 0; DepthBufferIndex < NumDenseGridTraversalTasks * TotalDepthBufferSize; ++
		     DepthBufferIndex)
		{
			DepthBuffer.CellDepthBuffer[DepthBufferIndex] = FLT_MAX;
		}
	}

	void FVisibility::UpdateDepthBufferInHighDensityAreas(const FGrid& Grid, int32 TaskIndex, int32 BeginIndex,
	                                                      int32 EndIndex)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDepthBufferInHighDensityAreas);

		const TConstArrayView<int32> DenseCells = Grid.GetDenseCells();
		for (int32 ActivatedCellIndex = BeginIndex; ActivatedCellIndex < EndIndex; ++ActivatedCellIndex)
		{
			const int32 CellIndex = DenseCells[ActivatedCellIndex];
			const FIntVector Cell = Grid.GetCellCoordinates(CellIndex);
			if (Grid.IsAtGridEdge(Cell.X, Cell.Y, Cell.Z))
			{
				CellStatuses[CellIndex] = ECellStatus::Empty;
				continue;
			}
			UpdateCellDepthBufferInHighDensityAreas(Grid, TaskIndex, Cell.X, Cell.Y, Cell.Z, CellIndex,
			                                        NumDenseGridTraversalTasks);
		}
	}

	void FVisibility::UpdateDepthBufferInLowDensityAreas(const FGrid& Grid, int32 BeginIndex, int32 EndIndex)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDepthBufferInLowDensityAreas);

		const TConstArrayView<int32> ActivatedCells = Grid.GetActivatedCells();
		const TConstArrayView<int32> CellCounts = Grid.GetCellCounts();
		const int32 MinInvisibleFishCount = Grid.GetDenseFishThreshold();
		for (int32 ActivatedCellIndex = BeginIndex; ActivatedCellIndex < EndIndex; ++ActivatedCellIndex)
		{
			const int32 CellIndex = ActivatedCells[ActivatedCellIndex];
			if (CellCounts[CellIndex] < MinInvisibleFishCount)
			{
				const FIntVector Cell = Grid.GetCellCoordinates(CellIndex);
				UpdateCellDepthBufferInLowDensityAreas(Grid, Cell.X, Cell.Y, Cell.Z, CellIndex);
			}
		}
	}

	void FVisibility::MergeDepthBuffer(int32 TotalTasksSize)
	{
		int32 CurrentBuffer, CurrentIndex;
		// 逐个屏幕桶比较各任务的最近格，把全局赢家留在任务 0 的槽位，并更新落选格状态。
		for (int32 DepthBufferIndex = 0; DepthBufferIndex < TotalDepthBufferSize; ++DepthBufferIndex)
		{
			CurrentBuffer = DepthBufferIndex * TotalTasksSize;
			for (int32 TaskIndex = 1; TaskIndex < TotalTasksSize; ++TaskIndex)
			{
				CurrentIndex = CurrentBuffer + TaskIndex;
				//如果后几个task的值有比第一个小，就替换第一个task的值
				if (DepthBuffer.CellDepthBuffer[CurrentIndex] < DepthBuffer.CellDepthBuffer[CurrentBuffer])
				{
					const int32 PreviousWinner = DepthBuffer.CellIndexBufferW[CurrentBuffer];
					if (PreviousWinner != INDEX_NONE)
					{
						CellStatuses[PreviousWinner] = ECellStatus::OccludedLowHidden;
					}
					DepthBuffer.CellDepthBuffer[CurrentBuffer] = DepthBuffer.CellDepthBuffer[CurrentIndex];
					DepthBuffer.CellIndexBufferX[CurrentBuffer] = DepthBuffer.CellIndexBufferX[CurrentIndex];
					DepthBuffer.CellIndexBufferY[CurrentBuffer] = DepthBuffer.CellIndexBufferY[CurrentIndex];
					DepthBuffer.CellIndexBufferZ[CurrentBuffer] = DepthBuffer.CellIndexBufferZ[CurrentIndex];
					DepthBuffer.CellIndexBufferW[CurrentBuffer] = DepthBuffer.CellIndexBufferW[CurrentIndex];
				}
				else
				{
					const int32 LosingCell = DepthBuffer.CellIndexBufferW[CurrentIndex];
					if (LosingCell != INDEX_NONE)
					{
						CellStatuses[LosingCell] = ECellStatus::OccludedLowHidden;
					}
				}
			}
		}
	}

	void FVisibility::DilateGrid(const FGrid& Grid)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DilateGrid);

		// 扩张表面高性能计算网格，减少空洞
		if (GridVisibilityDilationRadius == 0)
		{
			return;
		}
		const FIntVector Dimensions = Grid.GetDimensions();
		int32 DilatedGridSizeSquare = DilatedGridSize * DilatedGridSize;
		int32 X, Y, Z, dx, dy, dz, x, y, z;
		for (int32 PixelIndex = 0; PixelIndex < TotalDepthBufferSize; PixelIndex++)
		{
			const int32 CurrentBufferIndex = PixelIndex * NumDenseGridTraversalTasks;
			if (DepthBuffer.CellIndexBufferW[CurrentBufferIndex] == -1)
			{
				// 没有网格直接跳过
				continue;
			}
			X = DepthBuffer.CellIndexBufferX[CurrentBufferIndex];
			Y = DepthBuffer.CellIndexBufferY[CurrentBufferIndex];
			Z = DepthBuffer.CellIndexBufferZ[CurrentBufferIndex];
			int32 CurrentOffsetNum = 0;

			// 扩张只覆盖网格内部。跳过一行或一层时，也要推进预计算偏移表的下标。
			for (dx = -GridVisibilityDilationRadius; dx < GridVisibilityDilationRadius + 1; dx++)
			{
				x = X + dx;
				if (0 >= x || x >= Dimensions.X - 1)
				{
					CurrentOffsetNum += DilatedGridSizeSquare;
					continue;
				}
				for (dy = -GridVisibilityDilationRadius; dy < GridVisibilityDilationRadius + 1; dy++)
				{
					y = Y + dy;
					if (0 >= y || y >= Dimensions.Y - 1)
					{
						CurrentOffsetNum += DilatedGridSize;
						continue;
					}
					for (dz = -GridVisibilityDilationRadius; dz < GridVisibilityDilationRadius + 1; dz++)
					{
						z = Z + dz;
						if (0 < z && z < Dimensions.Z - 1)
						{
							CellStatuses[DepthBuffer.CellIndexBufferW[CurrentBufferIndex] +
								DilateOffsets[CurrentOffsetNum]] = ECellStatus::VisibleHigh;
						}
						CurrentOffsetNum++;
					}
				}
			}
		}
	}

	void FVisibility::UpdateCellDepthBufferInHighDensityAreas(const FGrid& Grid, int32 TaskIndex,
	                                                          int32 CurrentCellX, int32 CurrentCellY,
	                                                          int32 CurrentCellZ,
	                                                          int32 CurrentCellIndex, int32 TotalTasksSize)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateCellDepthBufferInHighDensityAreas);

		const float StaticCellSize = Grid.GetCellSize();
		const FVector3f GridCenter = Grid.GetOrigin() + FVector3f((CurrentCellX + 0.5) * StaticCellSize,
		                                                          (CurrentCellY + 0.5) * StaticCellSize,
		                                                          (CurrentCellZ + 0.5) * StaticCellSize);
		const FVector4 ClipPos = ViewProjectionMatrix.TransformFVector4(FVector4(GridCenter, 1.0f));
		const float Distance = ClipPos.W;
		// 在相机前方
		if (Distance > 0.0f)
		{
			const float InvW = 1.0f / Distance;
			const float ndcX = ClipPos.X * InvW;
			const float ndcY = ClipPos.Y * InvW;

			if (ndcX >= -NdcMarginX && ndcX <= NdcMarginX && ndcY >= -NdcMarginY && ndcY <= NdcMarginY)
			{
				int32 X = FMath::FloorToInt((ndcX + NdcMarginX) * InvNdcMarginX * (GridDepthBufferSizeX - 1));
				int32 Y = FMath::FloorToInt((NdcMarginY - ndcY) * InvNdcMarginY * (GridDepthBufferSizeY - 1));
				X = FMath::Clamp(X, 0, GridDepthBufferSizeX - 1);
				Y = FMath::Clamp(Y, 0, GridDepthBufferSizeY - 1);
				// 同一个屏幕桶为每个任务保留一个槽，密集格先和自己任务的最近格比较。
				int32 BufferIdx = TaskIndex + X * TotalTasksSize + Y * GridDepthBufferSizeX * TotalTasksSize;

				if (Distance < DepthBuffer.CellDepthBuffer[BufferIdx])
				{
					//如果是最近的网格
					//将当前的网格设置为需要高精度计算
					CellStatuses[CurrentCellIndex] = ECellStatus::VisibleHigh;
					const int32 PreviousBufferCellIndex = DepthBuffer.CellIndexBufferW[BufferIdx];
					if (PreviousBufferCellIndex != -1)
					{
						//将之前检测到的设置为只需要简单圆周计算
						CellStatuses[PreviousBufferCellIndex] = ECellStatus::OccludedLowHidden;
					}
					DepthBuffer.CellDepthBuffer[BufferIdx] = Distance;
					DepthBuffer.CellIndexBufferX[BufferIdx] = CurrentCellX;
					DepthBuffer.CellIndexBufferY[BufferIdx] = CurrentCellY;
					DepthBuffer.CellIndexBufferZ[BufferIdx] = CurrentCellZ;
					DepthBuffer.CellIndexBufferW[BufferIdx] = CurrentCellIndex;
				}
				else
				{
					//如果不是最近的网格将当前的网格设置为只需要简单圆周计算
					CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
				}
			}
			else
			{
				//如果不在视野范围内不进行渲染
				CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
			}
		}
		else
		{
			//在相机背后不进行渲染
			CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
		}
	}

	void FVisibility::UpdateCellDepthBufferInLowDensityAreas(const FGrid& Grid, int32 CurrentCellX, int32 CurrentCellY,
	                                                         int32 CurrentCellZ, int32 CurrentCellIndex)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateCellDepthBufferInLowDensityAreas);

		const float StaticCellSize = Grid.GetCellSize();
		const FVector3f GridCenter = Grid.GetOrigin() + FVector3f((CurrentCellX + 0.5) * StaticCellSize,
		                                                          (CurrentCellY + 0.5) * StaticCellSize,
		                                                          (CurrentCellZ + 0.5) * StaticCellSize);
		const FVector4 ClipPos = ViewProjectionMatrix.TransformFVector4(FVector4(GridCenter, 1.0f));
		const float Distance = ClipPos.W;
		// 在相机前方
		if (Distance > 0.0f)
		{
			const float InvW = 1.0f / Distance;
			const float ndcX = ClipPos.X * InvW;
			const float ndcY = ClipPos.Y * InvW;

			if (ndcX >= -NdcMarginX && ndcX <= NdcMarginX && ndcY >= -NdcMarginY && ndcY <= NdcMarginY)
			{
				int32 X = FMath::FloorToInt((ndcX + NdcMarginX) * InvNdcMarginX * (GridDepthBufferSizeX - 1));
				int32 Y = FMath::FloorToInt((NdcMarginY - ndcY) * InvNdcMarginY * (GridDepthBufferSizeY - 1));
				X = FMath::Clamp(X, 0, GridDepthBufferSizeX - 1);
				Y = FMath::Clamp(Y, 0, GridDepthBufferSizeY - 1);
				const int32 PixelIndex = X + Y * GridDepthBufferSizeX;
				const int32 BufferIdx = PixelIndex * NumDenseGridTraversalTasks;
				if (Distance < DepthBuffer.CellDepthBuffer[BufferIdx])
				{
					//如果是最近的网格，将当前的网格设置为需要高精度计算，不需要将之前检测到的设置为只需要简单圆周计算
					CellStatuses[CurrentCellIndex] = ECellStatus::VisibleHigh;
				}
				else
				{
					//如果不是最近的网格将当前的网格设置为只需要简单圆周计算
					CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowRender;
				}
			}
			else
			{
				//如果不在视野范围内不进行渲染
				CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
			}
		}
		else
		{
			//在相机背后不进行渲染
			CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
		}
	}
} // namespace FishSchool::V2
