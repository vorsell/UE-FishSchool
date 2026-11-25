// Copyright (c) 2026 Vorsel.

#pragma once

#include "CoreMinimal.h"

namespace FishSchool::V2
{
	class FGrid;

	struct FPlayerFlowSettings
	{
		float LengthScale = 1.5f;
		float RadiusScale = 1.25f;
		float SideStrength = 1.25f;
		float RadialStrength = 3.0f;
	};

	/** 按球心到玩家的方向建立绕流通道，为仿真提供每条鱼的方向修正。 */
	class FPlayerFlow
	{
	public:
		/**
		 * 在并行仿真开始前重建通道及候选 Cell。
		 * @param Grid 已初始化的网格几何。此处只读坐标和尺寸，无需本帧鱼列表。
		 * @param Settings 通道的尺寸倍率与修正强度。
		 * @param Destination 鱼群目标点，也是通道起点。
		 * @param PlayerPosition 玩家位置，用来计算通道长度。
		 * @param DestinationToPlayerDirection 从目标点指向玩家的单位向量。
		 * @param OrbitAxis 鱼群环绕轴，用来确定通道的侧向。
		 * @param MinDistanceToPlayer 配置的影响距离，用来计算通道半径。
		 * @param bEnabled 当前距离档位是否启用绕流。
		 */
		void Prepare(const FGrid& Grid, const FPlayerFlowSettings& Settings, const FVector3f& Destination,
		             const FVector3f& PlayerPosition, const FVector3f& DestinationToPlayerDirection,
		             const FVector3f& OrbitAxis, float MinDistanceToPlayer, bool bEnabled);

		/**
		 * 判断鱼是否在通道内，并将绕流修正累加到输出向量。并行期间只读预计算数据。
		 * @param CellIndex 鱼在当前网格的 Cell ID，网格外为 INDEX_NONE。
		 * @param CurrentPosition 鱼的世界位置。
		 * @param BaseFlowVector 仿真已得到的基础流向。
		 * @param PlayerFlowVector 累加修正的输出向量，调用方通常传零向量。
		 * @return 应用通道修正时返回 true。
		 */
		bool GetCorrection(int32 CellIndex, const FVector3f& CurrentPosition, const FVector3f& BaseFlowVector,
		                   FVector3f& PlayerFlowVector) const;

	private:
		void UpdatePlayerFlowCellMask(const FGrid& Grid);
		bool IsPlayerFlowCandidateCell(int32 CellIndex) const;

		FPlayerFlowSettings Settings;
		bool bPlayerFlowActive = false;
		FVector3f PlayerFlowAxis = FVector3f::ZeroVector;
		FVector3f PlayerFlowChannelOrigin = FVector3f::ZeroVector;
		FVector3f PlayerFlowSideAxis = FVector3f::ZeroVector;
		FVector3f PlayerFlowCrossAxis = FVector3f::ZeroVector;
		float PlayerFlowChannelLength = 0.0f;
		float PlayerFlowInvChannelLength = 0.0f;
		float PlayerFlowChannelRadius = 0.0f;
		float PlayerFlowChannelRadiusSq = 0.0f;
		float PlayerFlowMaskSideRadius = 0.0f;
		float PlayerFlowMaskCrossRadius = 0.0f;
		TArray<int32> PlayerFlowCandidateCells;
	};
} // namespace FishSchool::V2
