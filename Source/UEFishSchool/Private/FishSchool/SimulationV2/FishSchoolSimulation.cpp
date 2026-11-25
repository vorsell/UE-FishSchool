// Copyright (c) 2026 Vorsel.

#include "FishSchool/SimulationV2/FishSchoolSimulation.h"

#include "Async/ParallelFor.h"
#include "FishSchool/SimulationV2/FishSchoolGrid.h"
#include "FishSchool/SimulationV2/FishSchoolPlayerFlow.h"
#include "FishSchool/SimulationV2/FishSchoolVisibility.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include <cfloat>

namespace FishSchool::V2
{
	namespace
	{
		constexpr double SimulationEpsilon = 1e-5;

		FORCEINLINE static FVector3f VInterpNormalRotationTo3f(const FVector3f& Current, const FVector3f& Target,
		                                                       float DeltaTime, float RotationSpeedDegrees)
		{
			const FQuat4f DeltaQuat = FQuat4f::FindBetween(Current, Target);

			FVector3f DeltaAxis(0.f, 0.f, 0.f);
			float DeltaAngle = 0.f;
			DeltaQuat.ToAxisAndAngle(DeltaAxis, DeltaAngle);

			const float RotationStepRadians = FMath::DegreesToRadians(RotationSpeedDegrees) * DeltaTime;

			if (FMath::Abs(DeltaAngle) > RotationStepRadians)
			{
				DeltaAngle = FMath::Clamp(DeltaAngle, -RotationStepRadians, RotationStepRadians);
				return FQuat4f(DeltaAxis, DeltaAngle).RotateVector(Current);
			}

			return Target;
		}
	}

	struct FSimulation::FFrame
	{
		const FPlayerFlow& PlayerFlow;
		const FBehaviorSettings& Behavior;
		TConstArrayView<int32> CellIds;
		TConstArrayView<int32> CellStarts;
		TConstArrayView<int32> CellCounts;
		TConstArrayView<int32> SortedFishIds;
		uint32 RandomSeed;
	};

	void FSimulation::Initialize(const FGrid& Grid, int32 InNumFishes, const FSimulationSettings& InSettings)
	{
		check(InNumFishes >= 0 && InSettings.NumCachedNeighbours > 0 && InSettings.NumTraversalTasks > 0);
		check(InSettings.RandomArraySize > 0 && InSettings.NumUpdateKNNBlock > 0 && InSettings.NumPredators >= 0);
		Settings = InSettings;
		NumFishes = InNumFishes;
		CurrentUpdateKNNBlock = 0;
		Positions[PreviousBuffer].SetNum(NumFishes);
		Positions[NextBuffer].SetNum(NumFishes);
		Directions[PreviousBuffer].SetNum(NumFishes);
		Directions[NextBuffer].SetNum(NumFishes);
		CachedNeighbourIds.Init(INDEX_NONE, NumFishes * Settings.NumCachedNeighbours);
		PredatorPositions.SetNum(Settings.NumPredators);
		MinDistanceToPredator.SetNum(Settings.NumPredators);
		RandomVectors.SetNum(Settings.RandomArraySize);

		for (int32 i = 0; i < Settings.RandomArraySize; ++i)
		{
			RandomVectors[i] = FVector3f(FMath::VRand());
		}
		KMinDistances.SetNum(Settings.NumTraversalTasks * Settings.NumCachedNeighbours);
		NeighbourOffsets.SetNum(27);

		// 按旧版 X、Y、Z 次序预存 27 个相对 Cell 偏移，保持等距候选的处理顺序。
		int32 CurrentOffsetIndex = 0;
		for (int32 x = -1; x < 2; ++x)
		{
			for (int32 y = -1; y < 2; ++y)
			{
				for (int32 z = -1; z < 2; ++z)
				{
					NeighbourOffsets[CurrentOffsetIndex] = Grid.GetGridIndex(x, y, z);
					++CurrentOffsetIndex;
				}
			}
		}
	}

	void FSimulation::RandomInitialize(const FVector3f& Origin, float SpawnBoxSideLength)
	{
		float HalfBoxLength = SpawnBoxSideLength * 0.5f;
		for (int32 i = 0; i < NumFishes; i++)
		{
			FVector3f RandPos = Origin + FVector3f(FMath::VRand()) * FMath::FRandRange(200.f, HalfBoxLength);
			FVector3f RandDir = FVector3f(FMath::VRand());
			Positions[NextBuffer][i] = RandPos;
			Directions[NextBuffer][i] = RandDir;
			Positions[PreviousBuffer][i] = RandPos;
			Directions[PreviousBuffer][i] = RandDir;
		}
	}

	void FSimulation::UpdatePredatorPosition(const FVector3f& PlayerPosition)
	{
		for (int32 PredatorIndex = 0; PredatorIndex < PredatorPositions.Num(); ++PredatorIndex)
		{
			PredatorPositions[PredatorIndex] = PlayerPosition;
			MinDistanceToPredator[PredatorIndex] = 300.0f;
		}
	}

	void FSimulation::Step(const FGrid& Grid, const FVisibility& Visibility, const FPlayerFlow& PlayerFlow,
	                       const FBehaviorSettings& Behavior, float DeltaTime, uint32 RandomSeed)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(TraversalGridFishGroup);
		check(Grid.GetSortedFishIds().Num() == NumFishes);
		const bool bUseVisibility = Visibility.HasValidData();
		const TConstArrayView<ECellStatus> CellStatuses = bUseVisibility
			? Visibility.GetCellStatuses() : TConstArrayView<ECellStatus>();
		const FFrame Frame{PlayerFlow, Behavior, Grid.GetFishCellIds(), Grid.GetCellStarts(), Grid.GetCellCounts(),
		                   Grid.GetSortedFishIds(), RandomSeed};
		const int32 NumFishesInGrid = Grid.GetNumFishesInGrid();
		const int32 NumTasks = Settings.NumTraversalTasks;

		// 按排序后的 ID 区间分派任务；每条鱼只出现在一个任务里。
		ParallelFor(NumTasks, [&](int32 TaskId)
		{
			const int32 Begin = NumFishes * TaskId / NumTasks;
			const int32 End = (TaskId == NumTasks - 1) ? NumFishes : NumFishes * (TaskId + 1) / NumTasks;
			for (int32 SortedFishIndex = Begin; SortedFishIndex < End; ++SortedFishIndex)
			{
				const int32 FishIndex = Frame.SortedFishIds[SortedFishIndex];
				const int32 CellIndex = Frame.CellIds[FishIndex];

				// 网格外的桶排在尾部，先处理它，避免用无效 Cell ID 查询坐标。
				if (SortedFishIndex >= NumFishesInGrid)
				{
					UpdateFishDestination(Behavior, FishIndex, DeltaTime);
					continue;
				}
				const FIntVector Cell = Grid.GetCellCoordinates(CellIndex);
				if (Grid.IsAtGridEdge(Cell.X, Cell.Y, Cell.Z))
				{
					UpdateFishDestination(Behavior, FishIndex, DeltaTime);
				}
				else if (bUseVisibility && CellStatuses[CellIndex] != ECellStatus::VisibleHigh)
				{
					UpdateFishOrbitCross(Frame, FishIndex, DeltaTime);
				}
				else
				{
					const bool bUpdateKNN = (FishIndex & (Settings.NumUpdateKNNBlock - 1)) == CurrentUpdateKNNBlock;
					UpdateFishKNN(Frame, TaskId, FishIndex, DeltaTime, bUpdateKNN);
				}
			}
		});
		CurrentUpdateKNNBlock = (CurrentUpdateKNNBlock + 1) % Settings.NumUpdateKNNBlock;
	}

	void FSimulation::UpdateFishOrbitCross(const FFrame& Frame, int32 CurrentFishIndex, float DeltaTime)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishOrbitCross);

		const FVector3f CurrentPosition = Positions[PreviousBuffer][CurrentFishIndex];

		const FVector3f BaseFlowVector =
			Frame.Behavior.OrbitAxis.Cross(Frame.Behavior.FishSchoolDestination - CurrentPosition).GetSafeNormal();

		FVector3f PlayerFlowCorrection = FVector3f::ZeroVector;
		Frame.PlayerFlow.GetCorrection(Frame.CellIds[CurrentFishIndex], CurrentPosition, BaseFlowVector,
		                               PlayerFlowCorrection);

		const FVector3f TargetVector = (BaseFlowVector + PlayerFlowCorrection * Frame.Behavior.PlayerFlowWeight).GetSafeNormal();

		Directions[NextBuffer][CurrentFishIndex] = VInterpNormalRotationTo3f(
			Directions[PreviousBuffer][CurrentFishIndex], TargetVector, DeltaTime, Frame.Behavior.ReferenceFishRotationSpeed
		);

		Positions[NextBuffer][CurrentFishIndex] =
			CurrentPosition + Directions[NextBuffer][CurrentFishIndex] * Settings.ReferenceFishSpeed * DeltaTime;
	}

	void FSimulation::UpdateFishDestination(const FBehaviorSettings& Behavior, int32 CurrentFishIndex, float DeltaTime)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishDestination);

		FVector3f CurrentPosition = Positions[PreviousBuffer][CurrentFishIndex];
		FVector3f TargetVector = (Behavior.FishSchoolDestination - CurrentPosition).
			GetSafeNormal();
		Directions[NextBuffer][CurrentFishIndex] = VInterpNormalRotationTo3f(
			Directions[PreviousBuffer][CurrentFishIndex], TargetVector, DeltaTime, Behavior.ReferenceFishRotationSpeed);
		Positions[NextBuffer][CurrentFishIndex] = CurrentPosition +
			Directions[NextBuffer][CurrentFishIndex] * Settings.ReferenceFishSpeed * DeltaTime;
	}

	void FSimulation::UpdateFishKNN(const FFrame& Frame, int32 TaskIndex, int32 CurrentFishIndex, float DeltaTime, bool bUpdateKNN)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishKNN);

		FVector3f CurrentPosition = Positions[PreviousBuffer][CurrentFishIndex];

		// 先合成目标、分离、聚合、对齐和环绕，再叠加绕流与随机扰动。
		FVector3f DestinationVector = (Frame.Behavior.FishSchoolDestination - CurrentPosition).GetSafeNormal();
		FVector3f AvgPositionVector = FVector3f::ZeroVector;
		FVector3f AvgDirectionVector = FVector3f::ZeroVector;
		FVector3f SeparationVector = FVector3f::ZeroVector;
		bool bHitPredatorAvoidance = false;
		GetSeparationAvgPositionAndDirection(Frame, TaskIndex, CurrentFishIndex, bUpdateKNN, SeparationVector, AvgPositionVector,
		                                     AvgDirectionVector, bHitPredatorAvoidance);
		FVector3f OrbitVector = Frame.Behavior.OrbitAxis.Cross(DestinationVector).GetSafeNormal();
		FVector3f BaseFlowVector =
			DestinationVector * Frame.Behavior.DestinationWeight +
			AvgPositionVector * Frame.Behavior.SchoolCenterWeight +
			AvgDirectionVector * Frame.Behavior.AverageForwardWeight +
			SeparationVector * Frame.Behavior.SeparationWeight +
			OrbitVector * Frame.Behavior.OrbitWeight;

		// 绕流以当前基础流向为输入，输出加到基础流向上。
		FVector3f PlayerFlowCorrection = FVector3f::ZeroVector;
		Frame.PlayerFlow.GetCorrection(Frame.CellIds[CurrentFishIndex], CurrentPosition, BaseFlowVector,
		                               PlayerFlowCorrection);

		FVector3f TargetVector =
			BaseFlowVector +
			PlayerFlowCorrection * Frame.Behavior.PlayerFlowWeight +
			GetHashRandomVector(CurrentFishIndex, Frame.RandomSeed) * Frame.Behavior.RandomDirectionWeight;

		// 先更新目标朝向，再按本帧游速写入下一缓冲的位置。
		if (!TargetVector.IsNearlyZero())
		{
			float FinalRotationSpeed = Frame.Behavior.ReferenceFishRotationSpeed + Frame.Behavior.RandomRotationWeight * FMath::FRandRange(-1.0f, 1.0f);
			Directions[NextBuffer][CurrentFishIndex] = VInterpNormalRotationTo3f(
				Directions[PreviousBuffer][CurrentFishIndex], TargetVector.GetUnsafeNormal(), DeltaTime,
				FinalRotationSpeed);
		}
		float FinalMovingSpeed = Settings.ReferenceFishSpeed;
		// 捕食者后覆盖，优先级更高
		if (bHitPredatorAvoidance)
		{
			FinalMovingSpeed = Settings.CurrentPredatorAvoidSpeed;
		}
		// 随机扰动最后再加，但建议不要太大
		FinalMovingSpeed += Settings.RandomSpeedWeight * FMath::FRandRange(-1.0f, 1.0f);
		FinalMovingSpeed = FMath::Max(FinalMovingSpeed, 0.0f);
		Positions[NextBuffer][CurrentFishIndex] = Positions[PreviousBuffer][CurrentFishIndex] +
			Directions[NextBuffer][CurrentFishIndex] * FinalMovingSpeed * DeltaTime;
	}

	bool FSimulation::GetSeparationAvgPositionAndDirection(const FFrame& Frame, int32 TaskIndex, int32 CurrentFishIndex,
	                                                                 bool bUpdateKNN, FVector3f& SeparationVector,
	                                                                 FVector3f& AvgPositionVector,
	                                                                 FVector3f& AvgDirectionVector,
	                                                                 bool& bHitPredatorAvoidance)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GetSeparationAvgPositionAndDirection);

		bHitPredatorAvoidance = false;

		bool bHasFoundNearest = false;
		FVector3f CurrentPosition = Positions[PreviousBuffer][CurrentFishIndex];

		// 目标点附近保留空间，进入最小距离时加入径向排斥。
		if (FVector3f DiffToDest = CurrentPosition - Frame.Behavior.FishSchoolDestination;
			DiffToDest.SizeSquared() < Settings.MinDistanceToDestinationSquare)
		{
			SeparationVector += DiffToDest.GetSafeNormal() * (Settings.MinDistanceToDestinationSquare / (DiffToDest.
				SizeSquared() + SimulationEpsilon));
		}
		// 累加进入作用半径的捕食者排斥，并标记是否需要逃离速度。
		for (int32 PredatorIndex = 0; PredatorIndex < PredatorPositions.Num(); ++PredatorIndex)
		{
			float MinDistSquare = FMath::Square(MinDistanceToPredator[PredatorIndex]);
			if (FVector3f DiffToPredator = CurrentPosition - PredatorPositions[PredatorIndex];
				DiffToPredator.SizeSquared() < MinDistSquare)
			{
				bHitPredatorAvoidance = true;
				SeparationVector += DiffToPredator.GetSafeNormal() *
					(MinDistSquare / (DiffToPredator.SizeSquared() + SimulationEpsilon));
			}
		}

		AvgPositionVector = FVector3f::ZeroVector;
		AvgDirectionVector = FVector3f::ZeroVector;
		int32 NumNeighbours = 0;
		int32 StartIndex = CurrentFishIndex * Settings.NumCachedNeighbours;
		int32 EndIndex = (CurrentFishIndex + 1) * Settings.NumCachedNeighbours;
		FVector3f NearestNeighbourToCurrentVector = FVector3f::ZeroVector;
		// 刷新组遍历邻格，找到最近邻后再汇总有效邻居的位置和朝向。
		if (bUpdateKNN)
		{
			float NearestSquaredDistance = 0.0f;
			bHasFoundNearest = GridFindKNearestNeighbour(Frame, TaskIndex, CurrentFishIndex, NearestNeighbourToCurrentVector,
			                                             NearestSquaredDistance);
			if (bHasFoundNearest)
			{
				//如果找到了最近邻，那么计算回避向量以及平均位置与平均向量
				SeparationVector += NearestNeighbourToCurrentVector.GetSafeNormal() * (Settings.MinFishSeparationDistance *
					Settings.MinFishSeparationDistance / (NearestSquaredDistance + SimulationEpsilon));
				for (int32 NeighbourIndex = StartIndex; NeighbourIndex < EndIndex; ++NeighbourIndex)
				{
					int32 NeighbourFishIndex = CachedNeighbourIds[NeighbourIndex];
					if (NeighbourFishIndex != -1)
					{
						++NumNeighbours;
						AvgPositionVector += Positions[PreviousBuffer][NeighbourFishIndex];
						AvgDirectionVector += Directions[PreviousBuffer][NeighbourFishIndex];
					}
				}
				if (NumNeighbours > 0)
				{
					AvgPositionVector = (AvgPositionVector / NumNeighbours - CurrentPosition).GetSafeNormal();
					AvgDirectionVector = AvgDirectionVector.GetSafeNormal();
				}
			}
		}
		else
		{
			// 其余组复用邻居 ID，仍从当前读取缓冲重新计算邻居位置、朝向和距离。
			float NearestSquaredDistance = FLT_MAX;
			for (int32 NeighbourIndex = StartIndex; NeighbourIndex < EndIndex; NeighbourIndex++)
			{
				int32 Neighbour = CachedNeighbourIds[NeighbourIndex];
				if (Neighbour == -1)
				{
					continue;
				}
				FVector3f NeighbourPosition = Positions[PreviousBuffer][Neighbour];
				AvgPositionVector += NeighbourPosition;
				AvgDirectionVector += Directions[PreviousBuffer][Neighbour];
				++NumNeighbours;
				FVector3f BiasVector = CurrentPosition - NeighbourPosition;
				if (float SquaredDistance = BiasVector.SizeSquared(); SquaredDistance < NearestSquaredDistance)
				{
					NearestSquaredDistance = SquaredDistance;
					NearestNeighbourToCurrentVector = BiasVector;
				}
			}
			if (NearestSquaredDistance != FLT_MAX)
			{
				bHasFoundNearest = true;
				SeparationVector += NearestNeighbourToCurrentVector.GetSafeNormal() * (Settings.MinFishSeparationDistance *
					Settings.MinFishSeparationDistance / (NearestSquaredDistance + SimulationEpsilon));
				AvgPositionVector = (AvgPositionVector / NumNeighbours - CurrentPosition).GetSafeNormal();
				AvgDirectionVector = AvgDirectionVector.GetSafeNormal();
			}
		}
		return bHasFoundNearest;
	}

	bool FSimulation::GridFindKNearestNeighbour(const FFrame& Frame, int32 TaskIndex, const int32 CurrentFishIndex,
	                                                      FVector3f& NearestNeighbourToCurrent,
	                                                      float& NearestSquaredDistance)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GridFindKNearestNeighbour);

		int32 KMinDistanceIndex = TaskIndex * Settings.NumCachedNeighbours;
		int32 NeighbourArrayStartIndex = CurrentFishIndex * Settings.NumCachedNeighbours;
		// 近邻 ID 按鱼存储；临时距离按任务划分，任务间的写入互不重叠。
		for (int32 NeighbourIndex = 0; NeighbourIndex < Settings.NumCachedNeighbours; ++NeighbourIndex)
		{
			CachedNeighbourIds[NeighbourArrayStartIndex + NeighbourIndex] = -1;
			KMinDistances[KMinDistanceIndex + NeighbourIndex] = FLT_MAX;
		}
		FVector3f CurrentLocation = Positions[PreviousBuffer][CurrentFishIndex];
		// 保留固定的 27 格遍历次序。严格小于才能插入，等距候选沿用旧顺序。
		for (const int32 Offset : NeighbourOffsets)
		{
			const int32 GridIndex = Frame.CellIds[CurrentFishIndex] + Offset;
			const int32 BeginIndex = Frame.CellStarts[GridIndex];
			const int32 EndIndex = BeginIndex + Frame.CellCounts[GridIndex];
			for (int32 SortedFishIndex = BeginIndex; SortedFishIndex < EndIndex; ++SortedFishIndex)
			{
				int32 CurrentNeighbourIndex = Frame.SortedFishIds[SortedFishIndex];
				if (CurrentNeighbourIndex == CurrentFishIndex)
				{
					continue;
				}
				FVector3f OffsetVector = CurrentLocation - Positions[PreviousBuffer][CurrentNeighbourIndex];
				float SquaredDistance = OffsetVector.SizeSquared();
				// 维护按距离排列的前 K 个候选，多余的候选不会写入缓存。
				for (int32 i = 0; i < Settings.NumCachedNeighbours; i++)
				{
					if (SquaredDistance < KMinDistances[KMinDistanceIndex + i])
					{
						for (int32 j = Settings.NumCachedNeighbours - 1; j > i; j--) //K近邻
						{
							CachedNeighbourIds[NeighbourArrayStartIndex + j] =
								CachedNeighbourIds[NeighbourArrayStartIndex + j - 1];
							KMinDistances[KMinDistanceIndex + j] = KMinDistances[KMinDistanceIndex + j - 1];
						}
						CachedNeighbourIds[NeighbourArrayStartIndex + i] = CurrentNeighbourIndex;
						KMinDistances[KMinDistanceIndex + i] = SquaredDistance;
						if (i == 0) //最近邻
						{
							NearestNeighbourToCurrent = OffsetVector;
							NearestSquaredDistance = SquaredDistance;
						}
						break;
					}
				}
			}
		}
		return KMinDistances[KMinDistanceIndex] != FLT_MAX;
	}

	FVector3f FSimulation::GetHashRandomVector(int32 FishIndex, uint32 RandomSeed) const
	{
		static constexpr uint32 Hash = 2654435761u;
		return RandomVectors[(FishIndex ^ (RandomSeed * 2246822519u) * Hash) & (Settings.RandomArraySize - 1)];
	}
} // namespace FishSchool::V2
