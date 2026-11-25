// Copyright (c) 2026 Vorsel.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

namespace FishSchool::V2
{
	struct FGridSettings
	{
		uint8 NumCellsX = 50;
		uint8 NumCellsY = 50;
		uint8 NumCellsZ = 50;
		float CellSize = 30.0f;
		int32 NumTraversalTasks = 8;
	};

	/** 为一个位置快照构建均匀网格，按 Cell 分组保存鱼 ID。 */
	class FGrid
	{
	public:
		/**
		 * 设置固定网格并调整数组尺寸。构建完成后，Cell 列表与鱼 ID 区间才可用于查询。
		 * @param Settings 网格尺寸、正的 Cell 边长和任务数。
		 * @param Destination 旧版用来确定网格原点的鱼群目标点（世界坐标）。
		 * @param InNumFishes 此网格应接收的鱼数量。数量改变后重新初始化。
		 */
		void Initialize(const FGridSettings& Settings, const FVector3f& Destination, int32 InNumFishes);

		/**
		 * 执行并行归类与计数、前缀和、并行 Scatter。返回时所有任务均已完成。
		 * @param Positions 按鱼 ID 索引的世界位置，调用期间须保持稳定。
		 * @param MinInvisibleFishCount 构建密集格列表的鱼数阈值，可见性读取同一个阈值。
		 * @param bUsePrefixSumParallel 启用旧版从两端扫描的并行前缀和。
		 */
		void Build(TConstArrayView<FVector3f> Positions, int32 MinInvisibleFishCount, bool bUsePrefixSumParallel);

		int32 GetGridIndex(int32 X, int32 Y, int32 Z) const
		{
			return X + Y * NumCellsX + Z * NumCellsPerLayer;
		}

		bool IsAtGridEdge(int32 X, int32 Y, int32 Z) const
		{
			return X == 0 || Y == 0 || Z == 0 || X == NumCellsX - 1 || Y == NumCellsY - 1 || Z == NumCellsZ - 1;
		}

		/** 获取网格内的 Cell 坐标。网格外桶没有坐标。 */
		FIntVector GetCellCoordinates(int32 CellIndex) const
		{
			return FIntVector(CellX[CellIndex], CellY[CellIndex], CellZ[CellIndex]);
		}

		FIntVector GetDimensions() const { return FIntVector(NumCellsX, NumCellsY, NumCellsZ); }
		int32 GetNumCells() const { return TotalNumCells; }
		const FVector3f& GetOrigin() const { return GridOrigin; }
		float GetCellSize() const { return StaticCellSize; }
		int32 GetDenseFishThreshold() const { return DenseFishThreshold; }
		int32 GetOutsideBucketIndex() const { return OutsideBucketIndex; }
		int32 GetNumFishesInGrid() const { return NumFishesInGrid; }

		// 以下视图在网格重新构建或初始化前有效。每条鱼的 Cell ID 为 INDEX_NONE 时，鱼在网格外。
		TConstArrayView<int32> GetFishCellIds() const { return CellIds; }
		// Cell 的鱼 ID 区间为 [CellStarts[Cell], CellStarts[Cell] + CellCounts[Cell])。
		// 网格外桶同样有区间，排在 SortedFishIds 的最后。
		TConstArrayView<int32> GetCellStarts() const { return CellStarts; }
		TConstArrayView<int32> GetCellCounts() const { return CellCounts; }
		TConstArrayView<int32> GetSortedFishIds() const { return SortedFishIds; }

		TConstArrayView<int32> GetActivatedCells() const
		{
			return TConstArrayView<int32>(ActivatedCellIndex.GetData(), NumActivatedCells);
		}

		TConstArrayView<int32> GetDenseCells() const
		{
			return TConstArrayView<int32>(DenseCellIndex.GetData(), NumDenseCells);
		}

	private:
		void ResetGrid();
		void UpdateFishInGrid(TConstArrayView<FVector3f> Positions, int32 TaskIndex, int32 BeginIndex, int32 EndIndex);
		void UpdatePrefixSumAndGetActivatedCells();
		void UpdatePrefixSumParallelAndGetActivatedCells();
		void UpdateFishIds(int32 TaskIndex, int32 BeginIndex, int32 EndIndex);

		uint8 NumCellsX = 0;
		uint8 NumCellsY = 0;
		uint8 NumCellsZ = 0;
		int32 NumCellsPerLayer = 0;
		int32 TotalNumCells = 0;
		int32 TotalNumBuckets = 0;
		int32 OutsideBucketIndex = INDEX_NONE;
		FVector3f GridOrigin = FVector3f::ZeroVector;
		float StaticCellSize = 0.0f;
		float InverseCellSize = 0.0f;
		int32 DenseFishThreshold = 0;
		int32 NumFishSchoolTraversalTasks = 0;
		int32 NumFishes = 0;
		int32 NumFishesInGrid = 0;

		TArray<int32> CellIds;
		TArray<int32> CellStarts;
		TArray<int32> CellCounts;
		TArray<int32> SortedFishIds;
		TArray<uint8> CellX;
		TArray<uint8> CellY;
		TArray<uint8> CellZ;

		TArray<int32> ActivatedCellIndex;
		TArray<int32> DenseCellIndex;
		int32 NumActivatedCells = 0;
		int32 NumDenseCells = 0;

		// 构建时依次存放每个 Cell/任务的计数、前缀和偏移和 Scatter 写入游标。
		TArray<int32> CellFishCountsPerTask;
		TArray<int32> LocalActivatedCells[2];
		TArray<int32> LocalDenseCells[2];
	};
} // namespace FishSchool::V2
