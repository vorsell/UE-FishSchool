// Copyright (c) 2026 Vorsel.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

namespace FishSchool::V2
{
	class FGrid;
	class FPlayerFlow;
	class FVisibility;

	struct FSimulationSettings
	{
		int32 NumTraversalTasks = 8;
		int32 NumCachedNeighbours = 5;
		int32 NumUpdateKNNBlock = 2;
		int32 RandomArraySize = 1024;
		int32 NumPredators = 0;
		float MinFishSeparationDistance = 40.0f;
		float ReferenceFishSpeed = 60.0f;
		float RandomSpeedWeight = 10.0f;
		float CurrentPredatorAvoidSpeed = 180.0f;
		float MinDistanceToDestinationSquare = 90000.0f;
	};

	struct FBehaviorSettings
	{
		FVector3f FishSchoolDestination = FVector3f::ZeroVector;
		FVector3f OrbitAxis = FVector3f::UpVector;
		float ReferenceFishRotationSpeed = 90.0f;
		float DestinationWeight = 1.0f;
		float SchoolCenterWeight = 1.0f;
		float AverageForwardWeight = 1.0f;
		float SeparationWeight = 1.0f;
		float OrbitWeight = 0.5f;
		float PlayerFlowWeight = 2.0f;
		float RandomDirectionWeight = 1.0f;
		float RandomRotationWeight = 10.0f;
	};

	/** 管理鱼的双缓冲、近邻缓存和每帧并行行为更新。 */
	class FSimulation
	{
	public:
		/**
		 * 分配仿真缓冲并准备随机向量和邻居偏移。
		 * @param Grid 已初始化的网格几何。
		 * @param InNumFishes 固定的仿真鱼数。
		 * @param InSettings 任务数、缓存大小和初始化时确定的行为参数。
		 */
		void Initialize(const FGrid& Grid, int32 InNumFishes, const FSimulationSettings& InSettings);

		/** 用旧版分布初始化两份位置和朝向缓冲。 */
		void RandomInitialize(const FVector3f& Origin, float SpawnBoxSideLength);

		void UpdatePredatorPosition(const FVector3f& PlayerPosition);
		void SwapBuffers() { Swap(PreviousBuffer, NextBuffer); }

		/**
		 * 每个任务只写自己负责鱼的下一缓冲和近邻缓存。函数返回时所有任务都已完成。
		 * @param Grid 用 ReadPositions 构建的本帧网格。
		 * @param Visibility 本帧可见性，无效时内部 Cell 回退到 KNN。
		 * @param PlayerFlow 本帧预计算的玩家通道。
		 * @param Behavior 当前距离档位生效的行为参数。
		 * @param DeltaTime 仿真时间步，单位秒。
		 * @param RandomSeed 本帧方向扰动使用的随机种子。
		 */
		void Step(const FGrid& Grid, const FVisibility& Visibility, const FPlayerFlow& PlayerFlow,
		          const FBehaviorSettings& Behavior, float DeltaTime, uint32 RandomSeed);

		TConstArrayView<FVector3f> ReadPositions() const { return Positions[PreviousBuffer]; }
		TConstArrayView<FVector3f> ReadDirections() const { return Directions[PreviousBuffer]; }

		// CSV 恢复时使用，按实际的前后缓冲下标读取。
		TArrayView<FVector3f> EditPreviousPositions() { return Positions[PreviousBuffer]; }
		TArrayView<FVector3f> EditPreviousDirections() { return Directions[PreviousBuffer]; }
		TArrayView<FVector3f> EditNextPositions() { return Positions[NextBuffer]; }
		TArrayView<FVector3f> EditNextDirections() { return Directions[NextBuffer]; }

	private:
		struct FFrame;

		void UpdateFishOrbitCross(const FFrame& Frame, int32 CurrentFishIndex, float DeltaTime);
		void UpdateFishDestination(const FBehaviorSettings& Behavior, int32 CurrentFishIndex, float DeltaTime);
		void UpdateFishKNN(const FFrame& Frame, int32 TaskIndex, int32 CurrentFishIndex, float DeltaTime,
		                   bool bUpdateKNN);
		bool GetSeparationAvgPositionAndDirection(const FFrame& Frame, int32 TaskIndex, int32 CurrentFishIndex,
		                                          bool bUpdateKNN, FVector3f& SeparationVector,
		                                          FVector3f& AvgPositionVector, FVector3f& AvgDirectionVector,
		                                          bool& bHitPredatorAvoidance);
		bool GridFindKNearestNeighbour(const FFrame& Frame, int32 TaskIndex, int32 CurrentFishIndex,
		                               FVector3f& NearestNeighbourToCurrent, float& NearestSquaredDistance);
		FVector3f GetHashRandomVector(int32 FishIndex, uint32 RandomSeed) const;

		FSimulationSettings Settings;
		TArray<FVector3f> Positions[2];
		TArray<FVector3f> Directions[2];
		TArray<int32> CachedNeighbourIds;
		TArray<FVector3f> PredatorPositions;
		TArray<float> MinDistanceToPredator;
		TArray<FVector3f> RandomVectors;
		TArray<float> KMinDistances;
		TArray<int32> NeighbourOffsets;
		int32 PreviousBuffer = 0;
		int32 NextBuffer = 1;
		int32 NumFishes = 0;
		int32 CurrentUpdateKNNBlock = 0;
	};
} // namespace FishSchool::V2
