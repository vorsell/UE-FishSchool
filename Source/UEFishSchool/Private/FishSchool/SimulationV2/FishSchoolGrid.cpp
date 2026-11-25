// Copyright (c) 2026 Vorsel.

#include "FishSchool/SimulationV2/FishSchoolGrid.h"

#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace FishSchool::V2
{
	void FGrid::Initialize(const FGridSettings& Settings, const FVector3f& Destination, int32 InNumFishes)
	{
		check(Settings.NumCellsX > 0 && Settings.NumCellsY > 0 && Settings.NumCellsZ > 0);
		check(Settings.CellSize > 0.0f && Settings.NumTraversalTasks > 0 && InNumFishes >= 0);

		NumCellsX = Settings.NumCellsX;
		NumCellsY = Settings.NumCellsY;
		NumCellsZ = Settings.NumCellsZ;
		StaticCellSize = Settings.CellSize;
		NumFishSchoolTraversalTasks = Settings.NumTraversalTasks;
		NumFishes = InNumFishes;
		NumFishesInGrid = 0;
		NumActivatedCells = 0;
		NumDenseCells = 0;
		DenseFishThreshold = 0;

		// 包含最后一个网格外的网格来统计网格外的鱼
		TotalNumCells = NumCellsX * NumCellsY * NumCellsZ;
		OutsideBucketIndex = TotalNumCells;
		TotalNumBuckets = TotalNumCells + 1;
		InverseCellSize = 1.0f / StaticCellSize;
		NumCellsPerLayer = NumCellsX * NumCellsY;
		GridOrigin = Destination - FVector3f(NumCellsX / 2, NumCellsY / 2, NumCellsZ / 2) * StaticCellSize;

		CellIds.SetNum(NumFishes);
		SortedFishIds.SetNum(NumFishes);
		CellCounts.SetNum(TotalNumBuckets);
		CellStarts.SetNum(TotalNumBuckets);
		CellFishCountsPerTask.SetNum(NumFishSchoolTraversalTasks * TotalNumBuckets);
		ActivatedCellIndex.SetNum(FMath::Min(NumFishes, TotalNumCells));
		DenseCellIndex.SetNum(FMath::Min(NumFishes, TotalNumCells));
		// 只考虑网格内的下标处理
		CellX.SetNum(TotalNumCells);
		CellY.SetNum(TotalNumCells);
		CellZ.SetNum(TotalNumCells);

		// 预存 Cell ID 对应的三轴坐标，后续查询时可以直接读取。
		for (uint8 x = 0; x < NumCellsX; ++x)
		{
			for (uint8 y = 0; y < NumCellsY; ++y)
			{
				for (uint8 z = 0; z < NumCellsZ; ++z)
				{
					const int32 FlatIndex = GetGridIndex(x, y, z);
					CellX[FlatIndex] = x;
					CellY[FlatIndex] = y;
					CellZ[FlatIndex] = z;
				}
			}
		}
	}

	void FGrid::Build(TConstArrayView<FVector3f> Positions, int32 MinInvisibleFishCount, bool bUsePrefixSumParallel)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FishSchoolV2_BuildGrid);
		check(NumFishSchoolTraversalTasks > 0 && Positions.Num() == NumFishes);

		DenseFishThreshold = MinInvisibleFishCount;
		ResetGrid();
		// 每个任务给自己负责的鱼确定 Cell，并写入独立的计数槽。
		ParallelFor(NumFishSchoolTraversalTasks, [&](int32 TaskId)
		{
			const int32 Begin = NumFishes * TaskId / NumFishSchoolTraversalTasks;
			const int32 End = (TaskId == NumFishSchoolTraversalTasks - 1)
				                  ? NumFishes
				                  : NumFishes * (TaskId + 1) / NumFishSchoolTraversalTasks;
			UpdateFishInGrid(Positions, TaskId, Begin, End);
		});
		// 将计数转换为每个 Cell、每个任务的写入起点。
		if (bUsePrefixSumParallel)
		{
			UpdatePrefixSumParallelAndGetActivatedCells();
		}
		else
		{
			UpdatePrefixSumAndGetActivatedCells();
		}
		// 沿用计数阶段的鱼 ID 区间，将 ID 写入对应位置。
		ParallelFor(NumFishSchoolTraversalTasks, [&](int32 TaskId)
		{
			const int32 Begin = NumFishes * TaskId / NumFishSchoolTraversalTasks;
			const int32 End = (TaskId == NumFishSchoolTraversalTasks - 1)
				                  ? NumFishes
				                  : NumFishes * (TaskId + 1) / NumFishSchoolTraversalTasks;
			UpdateFishIds(TaskId, Begin, End);
		});
	}

	void FGrid::ResetGrid()
	{
		FMemory::Memset(CellFishCountsPerTask.GetData(), 0,
		                NumFishSchoolTraversalTasks * TotalNumBuckets * sizeof(int32));
		ActivatedCellIndex.Reset();
		DenseCellIndex.Reset();
	}

	void FGrid::UpdatePrefixSumAndGetActivatedCells()
	{
		//正序
		int32 Count = 0;
		for (int32 CellIndex = 0; CellIndex < TotalNumCells; ++CellIndex)
		{
			const int32 CurrentCellIndex = CellIndex * NumFishSchoolTraversalTasks;
			CellStarts[CellIndex] = Count;
			for (int32 TaskId = 0; TaskId < NumFishSchoolTraversalTasks; ++TaskId)
			{
				const int32 Current = CurrentCellIndex + TaskId;
				const int32 LocalCount = CellFishCountsPerTask[Current];
				CellFishCountsPerTask[Current] = Count;
				Count += LocalCount;
			}
			//遍历完了当前cell的内容，确认当前cell起点
			int32 CellCount = Count - CellStarts[CellIndex];
			CellCounts[CellIndex] = CellCount;
			//网格遍历用
			if (CellCount > 0)
			{
				ActivatedCellIndex.Add(CellIndex);
				if (CellCount >= DenseFishThreshold)
				{
					DenseCellIndex.Add(CellIndex);
				}
			}
		}
		// 正序遍历本来就已经是有序的了，不需要sort。
		NumActivatedCells = ActivatedCellIndex.Num();
		NumDenseCells = DenseCellIndex.Num();
		NumFishesInGrid = Count;
		CellStarts[OutsideBucketIndex] = Count;
		CellCounts[OutsideBucketIndex] = NumFishes - CellStarts[OutsideBucketIndex];
		// 网格外的鱼排在所有有效格之后，也按任务分配写入区间。
		const int32 OutsideBase = OutsideBucketIndex * NumFishSchoolTraversalTasks;
		for (int32 TaskId = 0; TaskId < NumFishSchoolTraversalTasks; ++TaskId)
		{
			const int32 Current = OutsideBase + TaskId;
			const int32 LocalCount = CellFishCountsPerTask[Current];
			CellFishCountsPerTask[Current] = Count;
			Count += LocalCount;
		}
	}

	void FGrid::UpdatePrefixSumParallelAndGetActivatedCells()
	{
		// 清空两路临时列表并保留容量。首次使用并行前缀和时按本任务可能遇到的最多激活格数预留。
		const int32 ScratchCapacity = FMath::Min(NumFishes, TotalNumCells / 2 + 1);
		LocalActivatedCells[0].Reset(ScratchCapacity);
		LocalActivatedCells[1].Reset(ScratchCapacity);
		LocalDenseCells[0].Reset(ScratchCapacity);
		LocalDenseCells[1].Reset(ScratchCapacity);
		// 前半段从 0 起向后累加，后半段从鱼总数向前扣减。两路各写自己的 Cell 区间。
		ParallelFor(2, [&](int32 TaskId)
		{
			//按网格数量截断
			const int32 HalfTotalCells = TotalNumCells / 2;
			int32 Count;
			if (!TaskId)
			{
				//正序
				Count = 0;
				for (int32 CellIndex = 0; CellIndex < HalfTotalCells; ++CellIndex)
				{
					const int32 CurrentCellIndex = CellIndex * NumFishSchoolTraversalTasks;
					CellStarts[CellIndex] = Count;
					for (int32 j = 0; j < NumFishSchoolTraversalTasks; ++j)
					{
						const int32 Current = CurrentCellIndex + j;
						const int32 LocalCount = CellFishCountsPerTask[Current];
						CellFishCountsPerTask[Current] = Count;
						Count += LocalCount;
					}
					//遍历完了当前cell的内容，确认上一个格子的起点
					int32 CellCount = Count - CellStarts[CellIndex];
					CellCounts[CellIndex] = CellCount;
					//网格遍历用
					if (CellCount > 0)
					{
						LocalActivatedCells[0].Add(CellIndex);
						if (CellCount >= DenseFishThreshold)
						{
							LocalDenseCells[0].Add(CellIndex);
						}
					}
				}
			}
			else
			{
				//逆序
				Count = NumFishes;
				//先检查无效网格
				for (int32 TaskIndex = NumFishSchoolTraversalTasks - 1; TaskIndex >= 0; --TaskIndex)
				{
					const int32 Current = OutsideBucketIndex * NumFishSchoolTraversalTasks + TaskIndex;
					Count -= CellFishCountsPerTask[Current];
					CellFishCountsPerTask[Current] = Count;
				}
				CellStarts[OutsideBucketIndex] = Count;
				CellCounts[OutsideBucketIndex] = NumFishes - Count;
				NumFishesInGrid = Count;
				// 倒序已知后一格的起点，用相邻起点之差得到当前格的鱼数。
				for (int32 CellIndex = TotalNumCells - 1; CellIndex >= HalfTotalCells; --CellIndex)
				{
					const int32 CurrentCellIndex = CellIndex * NumFishSchoolTraversalTasks;
					for (int32 TaskIndex = NumFishSchoolTraversalTasks - 1; TaskIndex >= 0; --TaskIndex)
					{
						const int32 Current = CurrentCellIndex + TaskIndex;
						Count -= CellFishCountsPerTask[Current];
						CellFishCountsPerTask[Current] = Count;
					}
					CellStarts[CellIndex] = Count;
					//遍历完了一个cell的内容，确认当前格子的起点
					int32 CellCount = CellStarts[CellIndex + 1] - Count;
					CellCounts[CellIndex] = CellCount;
					// 网格遍历用
					if (CellCount > 0)
					{
						LocalActivatedCells[1].Add(CellIndex);
						if (CellCount >= DenseFishThreshold)
						{
							LocalDenseCells[1].Add(CellIndex);
						}
					}
				}
			}
		});
		NumActivatedCells = LocalActivatedCells[0].Num() + LocalActivatedCells[1].Num();
		if (NumActivatedCells > 0)
		{
			ActivatedCellIndex.SetNumUninitialized(NumActivatedCells);
		}

		//正序倒序遍历本来内部就是有序的，不需要sort
		int32 WriteIndex = 0;
		for (int32 CellId : LocalActivatedCells[0])
		{
			ActivatedCellIndex[WriteIndex++] = CellId;
		}
		WriteIndex = NumActivatedCells;
		for (int32 CellId : LocalActivatedCells[1])
		{
			ActivatedCellIndex[--WriteIndex] = CellId;
		}

		WriteIndex = 0;
		NumDenseCells = LocalDenseCells[0].Num() + LocalDenseCells[1].Num();
		if (NumDenseCells > 0)
		{
			DenseCellIndex.SetNumUninitialized(NumDenseCells);
		}
		for (int32 CellId : LocalDenseCells[0])
		{
			DenseCellIndex[WriteIndex++] = CellId;
		}
		WriteIndex = NumDenseCells;
		for (int32 CellId : LocalDenseCells[1])
		{
			DenseCellIndex[--WriteIndex] = CellId;
		}
	}

	void FGrid::UpdateFishInGrid(TConstArrayView<FVector3f> Positions, int32 TaskIndex, int32 BeginIndex,
	                             int32 EndIndex)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishInGrid);

		for (int32 FishIndex = BeginIndex; FishIndex < EndIndex; ++FishIndex)
		{
			const FVector3f Offset = Positions[FishIndex] - GridOrigin;

			const int32 X = FMath::FloorToInt(Offset.X * InverseCellSize);
			const int32 Y = FMath::FloorToInt(Offset.Y * InverseCellSize);
			const int32 Z = FMath::FloorToInt(Offset.Z * InverseCellSize);
			if (X >= 0 && X < NumCellsX && Y >= 0 && Y < NumCellsY && Z >= 0 && Z < NumCellsZ)
			{
				const int32 FlatIndex = GetGridIndex(X, Y, Z);
				CellIds[FishIndex] = FlatIndex;
				++CellFishCountsPerTask[FlatIndex * NumFishSchoolTraversalTasks + TaskIndex];
			}
			else
			{
				CellIds[FishIndex] = INDEX_NONE;
				++CellFishCountsPerTask[OutsideBucketIndex * NumFishSchoolTraversalTasks + TaskIndex];
			}
		}
	}

	void FGrid::UpdateFishIds(int32 TaskIndex, int32 BeginIndex, int32 EndIndex)
	{
		for (int32 FishIndex = BeginIndex; FishIndex < EndIndex; ++FishIndex)
		{
			const int32 CellIndex = CellIds[FishIndex];
			const int32 CellFishCountsPerTaskIndex = CellIndex < 0
				                                         ? OutsideBucketIndex * NumFishSchoolTraversalTasks + TaskIndex
				                                         : CellIndex * NumFishSchoolTraversalTasks + TaskIndex;
			// 同一 Cell 的不同任务有各自的游标，前缀和已为它们分配互不重叠的区间。
			const int32 Dst = CellFishCountsPerTask[CellFishCountsPerTaskIndex]++;
			SortedFishIds[Dst] = FishIndex;
		}
	}
} // namespace FishSchool::V2
