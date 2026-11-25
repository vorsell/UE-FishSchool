// Copyright (c) 2026 Vorsel.

#include "FishSchool/SimulationV2/FishSchoolPlayerFlow.h"

#include "Algo/BinarySearch.h"
#include "FishSchool/SimulationV2/FishSchoolGrid.h"

namespace FishSchool::V2
{
	namespace
	{
		constexpr double PlayerFlowEpsilon = 1e-5;
	}

	void FPlayerFlow::Prepare(const FGrid& Grid, const FPlayerFlowSettings& InSettings,
	                          const FVector3f& Destination, const FVector3f& PlayerPosition,
	                          const FVector3f& DestinationToPlayerDirection, const FVector3f& OrbitAxis,
	                          float MinDistanceToPlayer, bool bEnabled)
	{
		bPlayerFlowActive = bEnabled;
		Settings = InSettings;

		if (!bPlayerFlowActive)
		{
			PlayerFlowCandidateCells.Reset();
			return;
		}

		const FVector3f Diff = PlayerPosition - Destination;
		const float DistSq = Diff.SizeSquared();

		PlayerFlowAxis = DestinationToPlayerDirection;
		PlayerFlowChannelOrigin = Destination;

		const float PlayerDist = FMath::Sqrt(DistSq);
		PlayerFlowChannelLength = FMath::Max(PlayerDist * Settings.LengthScale, 0.0f);
		PlayerFlowInvChannelLength = (PlayerFlowChannelLength > KINDA_SMALL_NUMBER)
			                             ? (1.0f / PlayerFlowChannelLength)
			                             : 0.0f;

		// 影响半径：决定“多宽范围内开始参与绕流”
		PlayerFlowChannelRadius = MinDistanceToPlayer * Settings.RadiusScale;
		PlayerFlowChannelRadiusSq = FMath::Square(PlayerFlowChannelRadius);

		// 候选 cell 的局部坐标轴：只是为了 OBB 粗筛，不是最终绕流方向
		PlayerFlowSideAxis = FVector3f::CrossProduct(OrbitAxis, PlayerFlowAxis);
		if (PlayerFlowSideAxis.SizeSquared() < PlayerFlowEpsilon)
		{
			PlayerFlowSideAxis = FVector3f::CrossProduct(PlayerFlowAxis, FVector3f::UpVector);
			if (PlayerFlowSideAxis.SizeSquared() < PlayerFlowEpsilon)
			{
				PlayerFlowSideAxis = FVector3f::CrossProduct(PlayerFlowAxis, FVector3f::RightVector);
			}
		}
		PlayerFlowSideAxis = PlayerFlowSideAxis.GetSafeNormal();

		PlayerFlowCrossAxis = FVector3f::CrossProduct(PlayerFlowAxis, PlayerFlowSideAxis).GetSafeNormal();

		// 候选 cell 粗筛范围：用圆柱外再放大一点
		const float MaskRadius = PlayerFlowChannelRadius * 1.15f;
		PlayerFlowMaskSideRadius = MaskRadius;
		PlayerFlowMaskCrossRadius = MaskRadius;

		UpdatePlayerFlowCellMask(Grid);
	}

	void FPlayerFlow::UpdatePlayerFlowCellMask(const FGrid& Grid)
	{
		PlayerFlowCandidateCells.Reset();

		if (!bPlayerFlowActive || Grid.GetNumCells() <= 0)
		{
			return;
		}

		const float HalfLen = PlayerFlowChannelLength * 0.5f;
		const FVector3f BoxCenter = PlayerFlowChannelOrigin + PlayerFlowAxis * HalfLen;

		// 局部 OBB 的三个半轴
		const FVector3f Ex = PlayerFlowAxis * HalfLen;
		const FVector3f Ey = PlayerFlowSideAxis * PlayerFlowMaskSideRadius;
		const FVector3f Ez = PlayerFlowCrossAxis * PlayerFlowMaskCrossRadius;

		// 8 个角点，用世界 AABB 缩小扫描范围
		FVector3f Corners[8];
		int32 Idx = 0;
		for (int32 sx = -1; sx <= 1; sx += 2)
		{
			for (int32 sy = -1; sy <= 1; sy += 2)
			{
				for (int32 sz = -1; sz <= 1; sz += 2)
				{
					Corners[Idx++] = BoxCenter +
						Ex * static_cast<float>(sx) +
						Ey * static_cast<float>(sy) +
						Ez * static_cast<float>(sz);
				}
			}
		}

		FVector3f MinP = Corners[0];
		FVector3f MaxP = Corners[0];
		for (int32 i = 1; i < 8; ++i)
		{
			MinP.X = FMath::Min(MinP.X, Corners[i].X);
			MinP.Y = FMath::Min(MinP.Y, Corners[i].Y);
			MinP.Z = FMath::Min(MinP.Z, Corners[i].Z);

			MaxP.X = FMath::Max(MaxP.X, Corners[i].X);
			MaxP.Y = FMath::Max(MaxP.Y, Corners[i].Y);
			MaxP.Z = FMath::Max(MaxP.Z, Corners[i].Z);
		}

		// 将包围盒限制在网格内，再对这一段 Cell 做局部坐标粗筛。
		const FIntVector GridDimensions = Grid.GetDimensions();
		const FVector3f& GridOrigin = Grid.GetOrigin();
		const float StaticCellSize = Grid.GetCellSize();
		const float InverseCellSize = 1.0f / StaticCellSize;
		const FVector3f MinOffset = MinP - GridOrigin;
		const FVector3f MaxOffset = MaxP - GridOrigin;

		const int32 MinX = FMath::Clamp(FMath::FloorToInt(MinOffset.X * InverseCellSize), 0,
		                                GridDimensions.X - 1);
		const int32 MinY = FMath::Clamp(FMath::FloorToInt(MinOffset.Y * InverseCellSize), 0,
		                                GridDimensions.Y - 1);
		const int32 MinZ = FMath::Clamp(FMath::FloorToInt(MinOffset.Z * InverseCellSize), 0,
		                                GridDimensions.Z - 1);

		const int32 MaxX = FMath::Clamp(FMath::FloorToInt(MaxOffset.X * InverseCellSize), 0,
		                                GridDimensions.X - 1);
		const int32 MaxY = FMath::Clamp(FMath::FloorToInt(MaxOffset.Y * InverseCellSize), 0,
		                                GridDimensions.Y - 1);
		const int32 MaxZ = FMath::Clamp(FMath::FloorToInt(MaxOffset.Z * InverseCellSize), 0,
		                                GridDimensions.Z - 1);

		// cell center 到最远顶点的距离，做保守扩张，防止漏 cell
		const float CellHalfDiag = StaticCellSize * 0.8660254f;

		const float LocalXLength = HalfLen + CellHalfDiag;
		const float LocalYLength = PlayerFlowMaskSideRadius + CellHalfDiag;
		const float LocalZLength = PlayerFlowMaskCrossRadius + CellHalfDiag;
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 Z = MinZ; Z <= MaxZ; ++Z)
				{
					const int32 CellIndex = Grid.GetGridIndex(X, Y, Z);

					const FVector3f CellCenter = GridOrigin + FVector3f(
						(X + 0.5f) * StaticCellSize,
						(Y + 0.5f) * StaticCellSize,
						(Z + 0.5f) * StaticCellSize
					);

					const FVector3f Local = CellCenter - BoxCenter;

					const float Lx = FVector3f::DotProduct(Local, PlayerFlowAxis);
					const float Ly = FVector3f::DotProduct(Local, PlayerFlowSideAxis);
					const float Lz = FVector3f::DotProduct(Local, PlayerFlowCrossAxis);

					// 在局部 OBB 下做粗筛，半径略放大 + cell 半对角线兜底
					if (FMath::Abs(Lx) <= LocalXLength && FMath::Abs(Ly) <= LocalYLength && FMath::Abs(Lz) <= LocalZLength)
					{
						PlayerFlowCandidateCells.Add(CellIndex);
					}
				}
			}
		}

		PlayerFlowCandidateCells.Sort();
	}

	bool FPlayerFlow::IsPlayerFlowCandidateCell(int32 CellIndex) const
	{
		return Algo::BinarySearch(PlayerFlowCandidateCells, CellIndex) != INDEX_NONE;
	}

	bool FPlayerFlow::GetCorrection(int32 CellIndex, const FVector3f& CurrentPosition,
	                                const FVector3f& BaseFlowVector, FVector3f& PlayerFlowVector) const
	{
		if (!bPlayerFlowActive)
		{
			return false;
		}

		if (CellIndex < 0 || !IsPlayerFlowCandidateCell(CellIndex))
		{
			return false;
		}

		const FVector3f& Axis = PlayerFlowAxis;

		// 用沿轴距离和离轴距离确认鱼是否在通道里，再计算方向修正。
		const FVector3f ToFish = CurrentPosition - PlayerFlowChannelOrigin;
		const float Axial = FVector3f::DotProduct(ToFish, Axis);

		if (Axial <= 0.0f || Axial >= PlayerFlowChannelLength)
		{
			return false;
		}

		const FVector3f ClosestPointOnAxis = PlayerFlowChannelOrigin + Axis * Axial;
		const FVector3f Radial = CurrentPosition - ClosestPointOnAxis;
		const float RadialSq = Radial.SizeSquared();

		if (RadialSq >= PlayerFlowChannelRadiusSq || RadialSq <= PlayerFlowEpsilon)
		{
			return false;
		}

		const float RadialLen = FMath::Sqrt(RadialSq);
		const FVector3f RadialNormal = Radial / RadialLen;

		// 基础流向投影到截面平面
		FVector3f PlaneFlow = BaseFlowVector - Axis * FVector3f::DotProduct(BaseFlowVector, Axis);

		// 截面上没有有效流向时，沿通道轴与径向的叉积选一个切线方向。
		if (PlaneFlow.SizeSquared() < PlayerFlowEpsilon)
		{
			PlaneFlow = FVector3f::CrossProduct(Axis, RadialNormal);
			if (FVector3f::DotProduct(PlaneFlow, PlayerFlowSideAxis) < 0.0f)
			{
				PlaneFlow *= -1.0f;
			}
		}
		else
		{
			// 去掉朝内分量，只保留切向/离场分量
			const float InwardDot = FVector3f::DotProduct(PlaneFlow, RadialNormal);
			if (InwardDot < 0.0f)
			{
				PlaneFlow -= RadialNormal * InwardDot;
			}

			if (PlaneFlow.SizeSquared() < PlayerFlowEpsilon)
			{
				PlaneFlow = FVector3f::CrossProduct(Axis, RadialNormal);
				if (FVector3f::DotProduct(PlaneFlow, PlayerFlowSideAxis) < 0.0f)
				{
					PlaneFlow *= -1.0f;
				}
			}
		}

		const FVector3f DesiredPlaneFlow = PlaneFlow.GetSafeNormal();
		const FVector3f CurrentPlaneFlow =
			(BaseFlowVector - Axis * FVector3f::DotProduct(BaseFlowVector, Axis)).GetSafeNormal();

		const float T = Axial * PlayerFlowInvChannelLength;

		const float Radius01 = FMath::Clamp(RadialLen / PlayerFlowChannelRadius, 0.0f, 1.0f);
		const float Inner01 = 1.0f - Radius01;

		// 1) 前半段分流：只让它在前半段强，避免尾部继续挂两边
		const float EnterEnvelope = FMath::Clamp(T / 0.16f, 0.0f, 1.0f);
		const float SideFadeEnvelope = 1.0f - FMath::Clamp((T - 0.50f) / 0.18f, 0.0f, 1.0f);
		const float SideEnvelope = EnterEnvelope * SideFadeEnvelope * FMath::Square(Inner01);

		// 2) 中段清空：越靠近轴线越强，把通道内部鱼往外清
		const float CoreRadius = PlayerFlowChannelRadius * 0.95f;
		const float CoreRadiusSq = FMath::Square(CoreRadius);
		const float CorePush =
			FMath::Clamp((CoreRadiusSq - RadialSq) / FMath::Max(CoreRadiusSq, PlayerFlowEpsilon), 0.0f, 1.0f);

		const float RadialEnvelope =
			EnterEnvelope * (1.0f - FMath::Clamp((T - 0.82f) / 0.10f, 0.0f, 1.0f));

		// 3) 尾部合流：只对外圈鱼，在后半段慢慢往中间收
		const float MergeEnvelope = FMath::Clamp((T - 0.62f) / 0.18f, 0.0f, 1.0f);
		const float OuterRing = FMath::Clamp((Radius01 - 0.58f) / 0.42f, 0.0f, 1.0f);

		const FVector3f SlideCorrection =
			(DesiredPlaneFlow - CurrentPlaneFlow) * (Settings.SideStrength * SideEnvelope);

		const FVector3f SurfaceCorrection =
			RadialNormal * (Settings.RadialStrength * 1.6f * CorePush * RadialEnvelope);

		const FVector3f MergeCorrection =
			(-RadialNormal) * (Settings.RadialStrength * 0.55f * MergeEnvelope * OuterRing);

		PlayerFlowVector += SlideCorrection + SurfaceCorrection + MergeCorrection;
		return true;
	}
} // namespace FishSchool::V2
