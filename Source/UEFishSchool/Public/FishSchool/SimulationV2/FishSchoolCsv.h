// Copyright (c) 2026 Vorsel.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

namespace FishSchool::V2
{
	class FGrid;
	class FRenderer;
	class FVisibility;

	namespace Csv
	{
		FString GetFishStateFilePath(const FString& DataFolder, int32 NumFishes);

		/**
		 * 按鱼 ID 顺序保存旧版六列鱼状态。
		 * @param FilePath 输出 CSV 的完整路径。
		 * @param Positions 要保存的位置快照。
		 * @param Directions 与 Positions 同一快照的朝向，两组数组长度须相同。
		 * @return 文件写入成功时返回 true。
		 */
		bool SaveFishState(const FString& FilePath, TConstArrayView<FVector3f> Positions,
		                   TConstArrayView<FVector3f> Directions);

		/**
		 * 从旧版 CSV 加载位置和朝向，写入仿真的两份缓冲区。
		 * @param FilePath 输入 CSV 的完整路径。
		 * @param PreviousPositions 第一份位置缓冲，长度决定期望鱼数。
		 * @param PreviousDirections 第一份朝向缓冲。
		 * @param NextPositions 第二份位置缓冲。
		 * @param NextDirections 第二份朝向缓冲。
		 * @return 填满两份缓冲时返回 true。失败时可能已写入部分行，调用方应重新初始化两份缓冲。
		 */
		bool LoadFishState(const FString& FilePath,
		                   TArrayView<FVector3f> PreviousPositions, TArrayView<FVector3f> PreviousDirections,
		                   TArrayView<FVector3f> NextPositions, TArrayView<FVector3f> NextDirections);

		/** 导出激活格。无有效可见性时，CellStatus 列填 Empty。 */
		bool ExportOccupiedGridCounts(const FString& FilePath, const FGrid& Grid, const FVisibility& Visibility);

		/** 按 Chunk ID 导出提交器保存的状态。 */
		bool ExportRenderChunkStats(const FString& FilePath, const FRenderer& Renderer);
	}
} // namespace FishSchool::V2
