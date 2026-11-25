// Copyright (c) 2026 Vorsel.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Misc/Optional.h"

namespace FishSchool::V2
{
	class FGrid;

	enum class ECellStatus : uint8
	{
		Empty = 0,
		OffscreenLow = 1,
		VisibleHigh = 2,
		OccludedLowRender = 3,
		OccludedLowHidden = 4
	};

	struct FVisibilitySettings
	{
		int32 DepthBufferSizeX = 64;
		int32 DepthBufferSizeY = 64;
		float ViewBorderExpandPercent = 0.0f;
		int32 DilationRadius = 0;
		int32 NumDenseTraversalTasks = 4;
		int32 NumActivatedTraversalTasks = 4;

		bool operator==(const FVisibilitySettings& Other) const = default;
	};

	/** 用格子中心和低分辨率深度表，计算鱼群 Cell 的近似可见性。 */
	class FVisibility
	{
	public:
		/**
		 * 确保深度表可用于当前网格。只有设置或网格维度与已分配的版本不同时才重建。
		 * @param Grid 已初始化的空间网格，用来建立 Cell 状态表和膨胀偏移。
		 * @param Settings 本次需要的深度表与任务配置。
		 */
		void EnsureInitialized(const FGrid& Grid, const FVisibilitySettings& Settings);

		/**
		 * 更新本帧可见性。返回后通过 HasValidData 判断是否可以读取本帧结果。
		 * @param Grid 用本帧位置快照 Build 完成的网格。
		 * @param InViewProjection 本帧相机的视图投影矩阵。深度关闭、预热中或取不到矩阵时传 nullptr。
		 */
		void Update(const FGrid& Grid, const FMatrix* InViewProjection);

		bool HasValidData() const { return bHasValidVisibilityData; }

		/** 仅当 HasValidData 为 true 时读取。有效状态对应本帧有鱼的 Cell。 */
		TConstArrayView<ECellStatus> GetCellStatuses() const { return CellStatuses; }

	private:
		void ResetDepthBuffer();
		void UpdateDepthBufferInHighDensityAreas(const FGrid& Grid, int32 TaskIndex, int32 BeginIndex, int32 EndIndex);
		void UpdateDepthBufferInLowDensityAreas(const FGrid& Grid, int32 BeginIndex, int32 EndIndex);
		void MergeDepthBuffer(int32 TotalTasksSize);
		void DilateGrid(const FGrid& Grid);
		void UpdateCellDepthBufferInHighDensityAreas(const FGrid& Grid, int32 TaskIndex, int32 CurrentCellX,
		                                             int32 CurrentCellY, int32 CurrentCellZ,
		                                             int32 CurrentCellIndex, int32 TotalTasksSize);
		void UpdateCellDepthBufferInLowDensityAreas(const FGrid& Grid, int32 CurrentCellX, int32 CurrentCellY,
		                                            int32 CurrentCellZ, int32 CurrentCellIndex);

		struct FDepthBuffer
		{
			TArray<int32> CellIndexBufferX;
			TArray<int32> CellIndexBufferY;
			TArray<int32> CellIndexBufferZ;
			TArray<int32> CellIndexBufferW;
			TArray<float> CellDepthBuffer;
		};

		FDepthBuffer DepthBuffer;
		TOptional<FVisibilitySettings> AllocatedSettings;
		TArray<ECellStatus> CellStatuses;
		TArray<int32> DilateOffsets;
		FIntVector GridDimensions = FIntVector::ZeroValue;
		FMatrix ViewProjectionMatrix = FMatrix::Identity;
		bool bHasValidVisibilityData = false;
		int32 GridDepthBufferSizeX = 0;
		int32 GridDepthBufferSizeY = 0;
		int32 TotalDepthBufferSize = 0;
		int32 NumDenseGridTraversalTasks = 0;
		int32 NumActivatedGridTraversalTasks = 0;
		int32 GridVisibilityDilationRadius = 0;
		int32 DilatedGridSize = 0;
		float NdcMarginX = 0.0f;
		float NdcMarginY = 0.0f;
		float InvNdcMarginX = 0.0f;
		float InvNdcMarginY = 0.0f;
	};
} // namespace FishSchool::V2
