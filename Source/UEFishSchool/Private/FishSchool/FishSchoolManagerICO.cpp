// Fill out your copyright notice in the Description page of Project Settings.


#include "FishSchool/FishSchoolManagerICO.h"

#include "Async/ParallelFor.h"
#include "DrawDebugHelpers.h"
#include "EnhancedInputComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "Algo/BinarySearch.h"
#include "SceneView.h"
#include "TimerManager.h"

#define FishSchoolEpsilon 1e-5

//Fish School Manager ISM CPU Optimization
AFishSchoolManagerICO::AFishSchoolManagerICO()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	FishISMComponent = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("FishISM"));
	RootComponent = FishISMComponent;

	FishISMComponent->SetCastShadow(false);
	FishISMComponent->SetMobility(EComponentMobility::Movable);
	FishISMComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FishISMComponent->SetGenerateOverlapEvents(false);

	InitDistanceAdaptiveParameters();
}

// Called when the game starts or when spawned
void AFishSchoolManagerICO::BeginPlay()
{
	Super::BeginPlay();

	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PC->InputComponent))
		{
			if (SaveFishSchoolAction)
			{
				EnhancedInputComponent->BindAction(SaveFishSchoolAction, ETriggerEvent::Started, this,
				                                   &AFishSchoolManagerICO::HandleSaveFishSchool);
			}
			if (StartFishSchoolAction)
			{
				EnhancedInputComponent->BindAction(StartFishSchoolAction, ETriggerEvent::Started, this,
				                                   &AFishSchoolManagerICO::HandleStartFishSchool);
			}
			if (SaveGridDataAction)
			{
				EnhancedInputComponent->BindAction(SaveGridDataAction, ETriggerEvent::Started, this,
				                                   &AFishSchoolManagerICO::HandleExportOccupiedGridCounts);
			}
			if (SaveChunkDataAction)
			{
				EnhancedInputComponent->BindAction(SaveChunkDataAction, ETriggerEvent::Started, this,
				                                   &AFishSchoolManagerICO::HandleExportRenderChunkStats);
			}
		}
	}
}

// Called every frame
void AFishSchoolManagerICO::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bIsPlaying)
	{
		UpdatePlayerPosition();
		UpdateDistanceAdaptiveParams(false);
		UpdatePlayerFlowPrecompute();
		UpdatePredatorPosition();
		ActorLocation = FVector3f(GetActorLocation());
		TraversalGridFishGroup(DeltaTime);

		if (bUseChunkedISM)
		{
			RenderChunkedInstances();
		}
		else
		{
			RenderAllInstances();
		}
		
		//debug
		CachedFishSchoolStats.FishCount=MaxNumFishes;
		CachedFishSchoolStats.ActiveCellCount=GridData.ActivatedCellIndex.Num();
		CachedFishSchoolStats.DenseCellPercent=
			static_cast<float>(GridData.DenseCellIndex.Num())/static_cast<float>(CachedFishSchoolStats.ActiveCellCount);
		CachedFishSchoolStats.AvgFishCountInActiveCells=
			static_cast<float>(MaxNumFishes)/static_cast<float>(CachedFishSchoolStats.ActiveCellCount);
		CachedFishSchoolStats.VisibleHighFishCount = 0;
		for (FRenderChunk& Chunk : RenderChunks)
		{
			CachedFishSchoolStats.VisibleHighFishCount+=Chunk.NumVisibleHigh;
		}
	}
}

void AFishSchoolManagerICO::InitFishSchool()
{
	ActorLocation = FVector3f(GetActorLocation());
	// Parameters Init
	NumFishes = MaxNumFishes;
	FishSchoolDestination = ActorLocation;
	TargetToOriginalLengthRatio = FVector(FishTargetLength / FishOriginalLength);
	MinFishSeparationDistance = MinFishSeparationDistanceInBodyLengths * FishTargetLength;
	ReferenceFishSpeed = ReferenceFishSpeedInBodyLengths * FishTargetLength;
	RandomSpeedWeight = RandomSpeedWeightInBodyLengths * FishTargetLength;
	CurrentAvgVectorUpdateTickInterval = 0;
	NumFishSchoolTraversalTasks = 1 << NumFishTraversalTasksPowerOfTwo;
	NumUpdateKNNBlock = 1 << MaxUpdateKNNBlockPowerOfTwo;
	CurrentUpdateKNNBlock = 0;
	CurrentPredatorAvoidSpeed = ReferenceFishSpeed * PredatorAvoidSpeedMultiplier;
	MinDistanceToDestinationSquare = FMath::Square(MinDistanceToDestination);
	MinDistanceToPlayerSquare = FMath::Square(MinDistanceToPlayer);
	// 距离切换
	CurrentDistanceAdaptiveMode = EDistanceAdaptiveMode::Far;
	DestinationToPlayerDirection = FVector3f::ZeroVector;
	bDepthBufferWarmupFinished = false;
	// TArray Init
	FishSchool.Positions[PreviousBuffer].SetNum(NumFishes);
	FishSchool.Positions[NextBuffer].SetNum(NumFishes);
	FishSchool.Directions[PreviousBuffer].SetNum(NumFishes);
	FishSchool.Directions[NextBuffer].SetNum(NumFishes);
	FishSchool.CellIds.SetNum(NumFishes);
	FishSchool.FishInstanceIds.SetNum(NumFishes);
	FishSchool.CachedNeighbourIds.Init(INDEX_NONE, NumFishes * NumCachedNeighbours);
	PredatorPositions.SetNum(NumPredators);
	MinDistanceToPredator.SetNum(NumPredators);
	RandomVectors.SetNum(RandomArraySize);

	for (int32 i = 0; i < RandomArraySize; ++i)
	{
		RandomVectors[i] = FVector3f(FMath::VRand());
	}
	KMinDistances.SetNum(NumFishSchoolTraversalTasks * NumCachedNeighbours);
	NeighbourOffsets.SetNum(27);
	int32 CurrentOffsetIndex = 0;
	for (int32 x = -1; x < 2; x++)
	{
		for (int32 y = -1; y < 2; y++)
		{
			for (int32 z = -1; z < 2; z++)
			{
				NeighbourOffsets[CurrentOffsetIndex] = GetGridIndex(x, y, z);
				++CurrentOffsetIndex;
			}
		}
	}
}

void AFishSchoolManagerICO::RandomInitPositionsAndDirections()
{
	float HalfBoxLength = FishSpawnBoxSideLength * 0.5f;
	for (int32 i = 0; i < NumFishes; i++)
	{
		FVector3f RandPos = ActorLocation + FVector3f(FMath::VRand()) * FMath::FRandRange(200.f, HalfBoxLength);
		FVector3f RandDir = FVector3f(FMath::VRand());
		FishSchool.Positions[NextBuffer][i] = RandPos;
		FishSchool.Directions[NextBuffer][i] = RandDir;
		FishSchool.Positions[PreviousBuffer][i] = RandPos;
		FishSchool.Directions[PreviousBuffer][i] = RandDir;
	}
}

void AFishSchoolManagerICO::InitRenderInstances()
{
	FishISMComponent->ClearInstances();
	FishISMComponent->NumCustomDataFloats = 2;

	AllTransforms.SetNum(NumFishes);

	const float Duration = 40.0f / 32.0f; // 1.25f
	const float AnimPlayrate = 1.0f;

	for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
	{
		const int32 InstanceId = FishISMComponent->AddInstance(RenderInstance(FishIndex), true);
		FishSchool.FishInstanceIds[FishIndex] = InstanceId;

		const float AnimOffset = FMath::FRand() * Duration;
		const bool bMarkRenderStateDirty = (FishIndex == NumFishes - 1);

		FishISMComponent->SetCustomDataValue(InstanceId, 0, AnimOffset, false);
		FishISMComponent->SetCustomDataValue(InstanceId, 1, AnimPlayrate, bMarkRenderStateDirty);
	}
}

void AFishSchoolManagerICO::RenderAllInstances()
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(RenderAllInstances);
	// int32 NumVisibleHigh = 0, NumOccludedLowRender = 0, NumOccludedLowHidden = 0;
	if (bUseDepthBuffer && bHasValidVisibilityData)
	{
		for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
		{
			int32 CellIndex = FishSchool.CellIds[FishIndex];
			ECellStatus Status = (CellIndex >= 0) ? GridData.CellStatuses[CellIndex] : ECellStatus::OffscreenLow;

			// if (Status == ECellStatus::VisibleHigh)++NumVisibleHigh;
			// if (Status == ECellStatus::OccludedLowRender)++NumOccludedLowRender;
			// if (Status == ECellStatus::OccludedLowHidden)++NumOccludedLowHidden;

			if (Status == ECellStatus::VisibleHigh || Status == ECellStatus::OccludedLowRender)
			{
				// 真实位置
				AllTransforms[FishIndex] = RenderInstance(FishIndex);
			}
			else
			{
				// 隐藏：缩放为 0
				AllTransforms[FishIndex] = HiddenTransform;
			}
		}
	}
	else
	{
		for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
		{
			AllTransforms[FishIndex] = RenderInstance(FishIndex);
		}
	}
	FishISMComponent->BatchUpdateInstancesTransforms(0, AllTransforms, true, true, true);
	// UE_LOG(LogTemp, Display, TEXT("NumVisibleHigh=%d, NumOccludedLowRender=%d, NumOccludedLowHidden=%d"),
	//        NumVisibleHigh, NumOccludedLowRender, NumOccludedLowHidden);
}

FTransform AFishSchoolManagerICO::RenderInstance(int32 FishIndex)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(RenderInstance);

	const FVector3f Pos = FishSchool.Positions[PreviousBuffer][FishIndex];
	const FRotator Rot = FRotationMatrix::MakeFromX(FVector(FishSchool.Directions[PreviousBuffer][FishIndex])).
		Rotator();
	return FTransform(Rot, FVector(Pos), TargetToOriginalLengthRatio);
}

void AFishSchoolManagerICO::UpdatePlayerPosition()
{
	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0);
	if (PlayerChar)
	{
		UCapsuleComponent* Capsule = PlayerChar->GetCapsuleComponent();
		if (Capsule)
		{
			PlayerPosition = FVector3f(Capsule->GetComponentLocation());
			PlayerPositionToDestination = (FishSchoolDestination - PlayerPosition).GetSafeNormal();
			FVector3f Offset = PlayerPosition - GridOrigin;
			const int32 X = FMath::FloorToInt(Offset.X * InverseCellSize);
			const int32 Y = FMath::FloorToInt(Offset.Y * InverseCellSize);
			const int32 Z = FMath::FloorToInt(Offset.Z * InverseCellSize);
			PlayerGridIndex = FIntVector(X, Y, Z);
		}
	}
}

void AFishSchoolManagerICO::UpdatePredatorPosition()
{
	// TODO:用tag获取位置和具体避让半径
	for (int32 PredatorIndex = 0; PredatorIndex < NumPredators; ++PredatorIndex)
	{
		PredatorPositions[PredatorIndex] = PlayerPosition;
		MinDistanceToPredator[PredatorIndex] = 300.0f;
	}
}

void AFishSchoolManagerICO::TraversalGridFishGroup(float DeltaTime)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(TraversalGridFishGroup);

	SwapBuffer();
	UpdateGrid();
	//分块多线程位置计算
	ParallelFor(NumFishSchoolTraversalTasks, [&](int32 TaskId)
	{
		const int32 Begin = NumFishes * TaskId / NumFishSchoolTraversalTasks;
		const int32 End = (TaskId == NumFishSchoolTraversalTasks - 1)
			                  ? NumFishes
			                  : NumFishes * (TaskId + 1) / NumFishSchoolTraversalTasks;
		for (int32 SortedFishIndex = Begin; SortedFishIndex < End; ++SortedFishIndex)
		{
			int32 FishIndex = GridData.SortedFishIds[SortedFishIndex];
			int32 GridIndex = FishSchool.CellIds[FishIndex];
			if (SortedFishIndex >= NumFishesInGrid || IsAtGridEdge(GridIndex)) //TODO: 在网格外只往目标点走，后续若鱼群移动可能需要修改
			{
				//当前分支覆盖了在网格外，以及在网格边界处的情况
				UpdateFishDestination(FishIndex, DeltaTime);
			}
			else if (bUseDepthBuffer && bHasValidVisibilityData) //使用本帧有效的深度缓存
			{
				if (GridData.CellStatuses[GridIndex] == ECellStatus::VisibleHigh) //需要高精度计算
				{
					UpdateFishKNN(TaskId, FishIndex, DeltaTime,
					              (FishIndex & (NumUpdateKNNBlock - 1)) == CurrentUpdateKNNBlock);
				}
				else
				{
					//2为被遮挡但是在屏幕内，3为不在屏幕内，都需要简单圆周计算，
					// UpdateFishOrbitRodrigues(FishIndex, DeltaTime);
					UpdateFishOrbitCross(FishIndex, DeltaTime);
				}
			}
			else
			{
				UpdateFishKNN(TaskId, FishIndex, DeltaTime,
				              (FishIndex & (NumUpdateKNNBlock - 1)) == CurrentUpdateKNNBlock);
			}
		}
	});
	RandomSeed++;
	CurrentUpdateKNNBlock = (CurrentUpdateKNNBlock + 1) % NumUpdateKNNBlock;
}

void AFishSchoolManagerICO::UpdateGrid()
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateGrid);
	bHasValidVisibilityData = false;

	bool bUpdateAvgPositionsAndDirections = CurrentAvgVectorUpdateTickInterval == MinAvgVectorUpdateTickInterval;
	if (bUpdateAvgPositionsAndDirections)
	{
		CurrentAvgVectorUpdateTickInterval = 0;
	}
	// 网格加速
	ResetGrid();
	// 1.鱼在哪个格子以及每个任务下格子里有几条鱼
	ParallelFor(NumFishSchoolTraversalTasks, [&](int32 TaskId)
	{
		const int32 Begin = NumFishes * TaskId / NumFishSchoolTraversalTasks;
		const int32 End = (TaskId == NumFishSchoolTraversalTasks - 1)
			                  ? NumFishes
			                  : NumFishes * (TaskId + 1) / NumFishSchoolTraversalTasks;
		UpdateFishInGrid(TaskId, Begin, End);
	});
	// 2.网格里准备存鱼，每个任务分别应该存在哪里
	if (bUsePrefixSumParallel)
	{
		UpdatePrefixSumParallelAndGetActivatedCells();
	}
	else
	{
		UpdatePrefixSumAndGetActivatedCells();
	}
	// 3.网格内存鱼
	ParallelFor(NumFishSchoolTraversalTasks, [&](int32 TaskId)
	{
		const int32 Begin = NumFishes * TaskId / NumFishSchoolTraversalTasks;
		const int32 End = (TaskId == NumFishSchoolTraversalTasks - 1)
			                  ? NumFishes
			                  : NumFishes * (TaskId + 1) / NumFishSchoolTraversalTasks;
		UpdateFishIds(TaskId, Begin, End);
	});
	// 4.计算周围的网格位置与朝向和
	// ParallelFor(NumActivatedGridTraversalTasks, [&](int32 TaskId)
	// {
	// 	const int32 Begin = NumActivatedCells * TaskId / NumActivatedGridTraversalTasks;
	// 	const int32 End = (TaskId == NumActivatedGridTraversalTasks - 1)
	// 		                  ? NumActivatedCells
	// 		                  : NumActivatedCells * (TaskId + 1) / NumActivatedGridTraversalTasks;
	// });
	// 深度缓冲加速
	if (bUseDepthBuffer)
	{
		if (UpdateViewProjectionMatrix())
		{
			ResetDepthBuffer();
			// 6.遮挡与预计算
			ParallelFor(NumDenseGridTraversalTasks, [&](int32 TaskId)
			{
				const int32 Begin = NumDenseCells * TaskId / NumDenseGridTraversalTasks;
				const int32 End = (TaskId == NumDenseGridTraversalTasks - 1)
					                  ? NumDenseCells
					                  : NumDenseCells * (TaskId + 1) / NumDenseGridTraversalTasks;
				UpdateDepthBufferInHighDensityAreas(TaskId, Begin, End);
			});
			// 7.合并高密度遮挡
			if (NumDenseGridTraversalTasks > 1) { MergeDepthBuffer(NumDenseGridTraversalTasks); }
			// 8.计算低密度遮挡
			ParallelFor(NumActivatedGridTraversalTasks, [&](int32 TaskId)
			{
				const int32 Begin = NumActivatedCells * TaskId / NumActivatedGridTraversalTasks;
				const int32 End = (TaskId == NumActivatedGridTraversalTasks - 1)
					                  ? NumActivatedCells
					                  : NumActivatedCells * (TaskId + 1) / NumActivatedGridTraversalTasks;
				UpdateDepthBufferInLowDensityAreas(TaskId, Begin, End);
			});
			// 9.计算扩充
			if (DilatedGridSize > 1)
			{
				DilateGrid();
			}
			bHasValidVisibilityData = true;
		}
	}
	++CurrentAvgVectorUpdateTickInterval;
}

void AFishSchoolManagerICO::UpdateFishOrbitRodrigues(int32 CurrentFishIndex, float DeltaTime)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishOrbitRodrigues);

	FVector3f CurrentPos = FishSchool.Positions[PreviousBuffer][CurrentFishIndex];
	FVector3f CurrentDir = FishSchool.Directions[PreviousBuffer][CurrentFishIndex];
	float Radius = FMath::Max(FVector3f::Distance(FishSchoolDestination, CurrentPos), 1.0f);
	float SinTheta, CosTheta;
	FMath::SinCos(&SinTheta, &CosTheta, (ReferenceFishSpeed * DeltaTime) / Radius);

	FishSchool.Directions[NextBuffer][CurrentFishIndex] = CurrentDir * CosTheta +
		OrbitAxis.Cross(CurrentDir) * SinTheta +
		OrbitAxis * FVector3f::DotProduct(OrbitAxis, CurrentDir) * (1.0f - CosTheta);
	FishSchool.Positions[NextBuffer][CurrentFishIndex] = CurrentPos * CosTheta +
		OrbitAxis.Cross(CurrentPos) * SinTheta +
		OrbitAxis * FVector3f::DotProduct(OrbitAxis, CurrentPos) * (1.0f - CosTheta);
}

void AFishSchoolManagerICO::UpdateFishOrbitCross(int32 CurrentFishIndex, float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishOrbitCross);

	const FVector3f CurrentPosition = FishSchool.Positions[PreviousBuffer][CurrentFishIndex];

	const FVector3f BaseFlowVector =
		OrbitAxis.Cross(FishSchoolDestination - CurrentPosition).GetSafeNormal();

	FVector3f PlayerFlowCorrection = FVector3f::ZeroVector;
	GetPlayerFlowVector(CurrentFishIndex, BaseFlowVector, PlayerFlowCorrection);

	const FVector3f TargetVector = (BaseFlowVector + PlayerFlowCorrection * PlayerFlowWeight).GetSafeNormal();

	FishSchool.Directions[NextBuffer][CurrentFishIndex] = VInterpNormalRotationTo3f(
		FishSchool.Directions[PreviousBuffer][CurrentFishIndex], TargetVector, DeltaTime, ReferenceFishRotationSpeed
	);

	FishSchool.Positions[NextBuffer][CurrentFishIndex] =
		CurrentPosition + FishSchool.Directions[NextBuffer][CurrentFishIndex] * ReferenceFishSpeed * DeltaTime;
}

void AFishSchoolManagerICO::UpdateFishDestination(int32 CurrentFishIndex, float DeltaTime)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishDestination);

	FVector3f CurrentPosition = FishSchool.Positions[PreviousBuffer][CurrentFishIndex];
	FVector3f TargetVector = (FishSchoolDestination - CurrentPosition).
		GetSafeNormal();
	FishSchool.Directions[NextBuffer][CurrentFishIndex] = VInterpNormalRotationTo3f(
		FishSchool.Directions[PreviousBuffer][CurrentFishIndex], TargetVector, DeltaTime, ReferenceFishRotationSpeed);
	FishSchool.Positions[NextBuffer][CurrentFishIndex] = CurrentPosition +
		FishSchool.Directions[NextBuffer][CurrentFishIndex] * ReferenceFishSpeed * DeltaTime;
}

void AFishSchoolManagerICO::UpdateFishKNN(int32 TaskIndex, int32 CurrentFishIndex, float DeltaTime, bool bUpdateKNN)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishKNN);

	FVector3f CurrentPosition = FishSchool.Positions[PreviousBuffer][CurrentFishIndex];

	//1.计算目的地向量
	FVector3f DestinationVector = (FishSchoolDestination - CurrentPosition).GetSafeNormal();
	//2.回避向量（避障、捕食者、鱼间）
	//3.计算平均位置
	//4.计算平均朝向
	FVector3f AvgPositionVector = FVector3f::ZeroVector;
	FVector3f AvgDirectionVector = FVector3f::ZeroVector;
	FVector3f SeparationVector = FVector3f::ZeroVector;
	bool bHitPredatorAvoidance = false;
	GetSeparationAvgPositionAndDirection(TaskIndex, CurrentFishIndex, bUpdateKNN, SeparationVector, AvgPositionVector,
	                                     AvgDirectionVector, bHitPredatorAvoidance);
	//5.环绕向量
	FVector3f OrbitVector = OrbitAxis.Cross(DestinationVector).GetSafeNormal();
	//根据旋转速度计算朝向
	FVector3f BaseFlowVector =
		DestinationVector * DestinationWeight +
		AvgPositionVector * SchoolCenterWeight +
		AvgDirectionVector * AverageForwardWeight +
		SeparationVector * SeparationWeight +
		OrbitVector * OrbitWeight;

	// 3. 玩家通道返回的是“对基础流向的修正”，不是额外独立风场
	FVector3f PlayerFlowCorrection = FVector3f::ZeroVector;
	GetPlayerFlowVector(CurrentFishIndex, BaseFlowVector, PlayerFlowCorrection);

	FVector3f TargetVector =
		BaseFlowVector +
		PlayerFlowCorrection * PlayerFlowWeight +
		GetHashRandomVector(CurrentFishIndex) * RandomDirectionWeight; //6.随机方向

	if (!TargetVector.IsNearlyZero())
	{
		float FinalRotationSpeed = ReferenceFishRotationSpeed + RandomRotationWeight * FMath::FRandRange(-1.0f, 1.0f);
		FishSchool.Directions[NextBuffer][CurrentFishIndex] = VInterpNormalRotationTo3f(
			FishSchool.Directions[PreviousBuffer][CurrentFishIndex], TargetVector.GetUnsafeNormal(), DeltaTime,
			FinalRotationSpeed);
	}
	float FinalMovingSpeed = ReferenceFishSpeed;
	// 捕食者后覆盖，优先级更高
	if (bHitPredatorAvoidance)
	{
		FinalMovingSpeed = CurrentPredatorAvoidSpeed;
	}
	// 随机扰动最后再加，但建议不要太大
	FinalMovingSpeed += RandomSpeedWeight * FMath::FRandRange(-1.0f, 1.0f);
	FinalMovingSpeed = FMath::Max(FinalMovingSpeed, 0.0f);
	FishSchool.Positions[NextBuffer][CurrentFishIndex] = FishSchool.Positions[PreviousBuffer][CurrentFishIndex] +
		FishSchool.Directions[NextBuffer][CurrentFishIndex] * FinalMovingSpeed * DeltaTime;
}

void AFishSchoolManagerICO::UpdatePlayerFlowPrecompute()
{
	// Near 和 VeryNear 都开，不要靠近后突然关掉
	bPlayerFlowActive =
		(CurrentDistanceAdaptiveMode == EDistanceAdaptiveMode::Near) ||
		(CurrentDistanceAdaptiveMode == EDistanceAdaptiveMode::VeryNear);

	if (!bPlayerFlowActive)
	{
		PlayerFlowCandidateCells.Reset();
		return;
	}

	const FVector3f Diff = PlayerPosition - FishSchoolDestination;
	const float DistSq = Diff.SizeSquared();

	PlayerFlowAxis = DestinationToPlayerDirection;
	PlayerFlowChannelOrigin = FishSchoolDestination;

	const float PlayerDist = FMath::Sqrt(DistSq);
	PlayerFlowChannelLength = FMath::Max(PlayerDist * PlayerFlowLengthScale, 0.0f);
	PlayerFlowInvChannelLength = (PlayerFlowChannelLength > KINDA_SMALL_NUMBER)
		                             ? (1.0f / PlayerFlowChannelLength)
		                             : 0.0f;

	// 影响半径：决定“多宽范围内开始参与绕流”
	PlayerFlowChannelRadius = MinDistanceToPlayer * PlayerFlowRadiusScale;
	PlayerFlowChannelRadiusSq = FMath::Square(PlayerFlowChannelRadius);
	PlayerFlowInvChannelRadiusSq = 1.0f / FMath::Max(PlayerFlowChannelRadiusSq, FishSchoolEpsilon);

	// 候选 cell 的局部坐标轴：只是为了 OBB 粗筛，不是最终绕流方向
	PlayerFlowSideAxis = FVector3f::CrossProduct(OrbitAxis, PlayerFlowAxis);
	if (PlayerFlowSideAxis.SizeSquared() < FishSchoolEpsilon)
	{
		PlayerFlowSideAxis = FVector3f::CrossProduct(PlayerFlowAxis, FVector3f::UpVector);
		if (PlayerFlowSideAxis.SizeSquared() < FishSchoolEpsilon)
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

	UpdatePlayerFlowCellMask();
}

void AFishSchoolManagerICO::UpdatePlayerFlowCellMask()
{
	PlayerFlowCandidateCells.Reset();

	if (!bPlayerFlowActive || TotalNumCells <= 0)
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

	const FVector3f MinOffset = MinP - GridOrigin;
	const FVector3f MaxOffset = MaxP - GridOrigin;

	const int32 MinX = FMath::Clamp(FMath::FloorToInt(MinOffset.X * InverseCellSize), 0,
	                                static_cast<int32>(NumCellsX) - 1);
	const int32 MinY = FMath::Clamp(FMath::FloorToInt(MinOffset.Y * InverseCellSize), 0,
	                                static_cast<int32>(NumCellsY) - 1);
	const int32 MinZ = FMath::Clamp(FMath::FloorToInt(MinOffset.Z * InverseCellSize), 0,
	                                static_cast<int32>(NumCellsZ) - 1);

	const int32 MaxX = FMath::Clamp(FMath::FloorToInt(MaxOffset.X * InverseCellSize), 0,
	                                static_cast<int32>(NumCellsX) - 1);
	const int32 MaxY = FMath::Clamp(FMath::FloorToInt(MaxOffset.Y * InverseCellSize), 0,
	                                static_cast<int32>(NumCellsY) - 1);
	const int32 MaxZ = FMath::Clamp(FMath::FloorToInt(MaxOffset.Z * InverseCellSize), 0,
	                                static_cast<int32>(NumCellsZ) - 1);

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
				const int32 CellIndex = GetGridIndex(X, Y, Z);

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

bool AFishSchoolManagerICO::IsPlayerFlowCandidateCell(int32 CellIndex) const
{
	return Algo::BinarySearch(PlayerFlowCandidateCells, CellIndex) != INDEX_NONE;
}

bool AFishSchoolManagerICO::GetSeparationAvgPositionAndDirection(int32 TaskIndex, int32 CurrentFishIndex,
                                                                 bool bUpdateKNN, FVector3f& SeparationVector,
                                                                 FVector3f& AvgPositionVector,
                                                                 FVector3f& AvgDirectionVector,
                                                                 bool& bHitPredatorAvoidance)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(GetSeparationAvgPositionAndDirection);

	bHitPredatorAvoidance = false;

	bool bHasFoundNearest = false;
	FVector3f CurrentPosition = FishSchool.Positions[PreviousBuffer][CurrentFishIndex];

	// 躲避障碍物
	if (FVector3f DiffToDest = CurrentPosition - FishSchoolDestination;
		DiffToDest.SizeSquared() < MinDistanceToDestinationSquare)
	{
		SeparationVector += DiffToDest.GetSafeNormal() * (MinDistanceToDestinationSquare / (DiffToDest.
			SizeSquared() + FishSchoolEpsilon));
	}
	// 躲避最近捕食者
	for (int32 PredatorIndex = 0; PredatorIndex < NumPredators; ++PredatorIndex)
	{
		float MinDistSquare = FMath::Square(MinDistanceToPredator[PredatorIndex]);
		if (FVector3f DiffToPredator = CurrentPosition - PredatorPositions[PredatorIndex];
			DiffToPredator.SizeSquared() < MinDistSquare)
		{
			bHitPredatorAvoidance = true;
			SeparationVector += DiffToPredator.GetSafeNormal() *
				(MinDistSquare / (DiffToPredator.SizeSquared() + FishSchoolEpsilon));
		}
	}

	AvgPositionVector = FVector3f::ZeroVector;
	AvgDirectionVector = FVector3f::ZeroVector;
	int32 NumNeighbours = 0;
	int32 StartIndex = CurrentFishIndex * NumCachedNeighbours;
	int32 EndIndex = (CurrentFishIndex + 1) * NumCachedNeighbours;
	FVector3f NearestNeighbourToCurrentVector = FVector3f::ZeroVector;
	//鱼群内躲避
	if (bUpdateKNN) //需要更新knn
	{
		float NearestSquaredDistance = 0.0f;
		bHasFoundNearest = GridFindKNearestNeighbour(TaskIndex, CurrentFishIndex, NearestNeighbourToCurrentVector,
		                                             NearestSquaredDistance);
		if (bHasFoundNearest)
		{
			//如果找到了最近邻，那么计算回避向量以及平均位置与平均向量
			SeparationVector += NearestNeighbourToCurrentVector.GetSafeNormal() * (MinFishSeparationDistance *
				MinFishSeparationDistance / (NearestSquaredDistance + FishSchoolEpsilon));
			for (int32 NeighbourIndex = StartIndex; NeighbourIndex < EndIndex; ++NeighbourIndex)
			{
				int32 NeighbourFishIndex = FishSchool.CachedNeighbourIds[NeighbourIndex];
				if (NeighbourFishIndex != -1)
				{
					++NumNeighbours;
					AvgPositionVector += FishSchool.Positions[PreviousBuffer][NeighbourFishIndex];
					AvgDirectionVector += FishSchool.Directions[PreviousBuffer][NeighbourFishIndex];
				}
			}
			if (NumNeighbours > 0)
			{
				AvgPositionVector = (AvgPositionVector / NumNeighbours - CurrentPosition).GetSafeNormal();
				AvgDirectionVector = AvgDirectionVector.GetSafeNormal();
			}
		}
	}
	else //不需要更新knn
	{
		//如果在网格内但不更新最近邻信息，那么使用上一个时刻的平均位置和平均朝向加权和
		float NearestSquaredDistance = FLT_MAX;
		for (int32 NeighbourIndex = StartIndex; NeighbourIndex < EndIndex; NeighbourIndex++)
		{
			int32 Neighbour = FishSchool.CachedNeighbourIds[NeighbourIndex];
			if (Neighbour == -1)
			{
				continue;
			}
			FVector3f NeighbourPosition = FishSchool.Positions[PreviousBuffer][Neighbour];
			AvgPositionVector += NeighbourPosition;
			AvgDirectionVector += FishSchool.Directions[PreviousBuffer][Neighbour];
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
			SeparationVector += NearestNeighbourToCurrentVector.GetSafeNormal() * (MinFishSeparationDistance *
				MinFishSeparationDistance / (NearestSquaredDistance + FishSchoolEpsilon));
			AvgPositionVector = (AvgPositionVector / NumNeighbours - CurrentPosition).GetSafeNormal();
			AvgDirectionVector = AvgDirectionVector.GetSafeNormal();
		}
	}
	return bHasFoundNearest;
}

bool AFishSchoolManagerICO::GetPlayerFlowVector(int32 CurrentFishIndex, const FVector3f& BaseFlowVector,
                                                FVector3f& PlayerFlowVector)
{
	if (!bPlayerFlowActive)
	{
		return false;
	}

	const int32 CellIndex = FishSchool.CellIds[CurrentFishIndex];
	if (CellIndex < 0 || !IsPlayerFlowCandidateCell(CellIndex))
	{
		return false;
	}

	const FVector3f& Axis = PlayerFlowAxis;
	const FVector3f CurrentPosition = FishSchool.Positions[PreviousBuffer][CurrentFishIndex];

	const FVector3f ToFish = CurrentPosition - PlayerFlowChannelOrigin;
	const float Axial = FVector3f::DotProduct(ToFish, Axis);

	if (Axial <= 0.0f || Axial >= PlayerFlowChannelLength)
	{
		return false;
	}

	const FVector3f ClosestPointOnAxis = PlayerFlowChannelOrigin + Axis * Axial;
	const FVector3f Radial = CurrentPosition - ClosestPointOnAxis;
	const float RadialSq = Radial.SizeSquared();

	if (RadialSq >= PlayerFlowChannelRadiusSq || RadialSq <= FishSchoolEpsilon)
	{
		return false;
	}

	const float RadialLen = FMath::Sqrt(RadialSq);
	const FVector3f RadialNormal = Radial / RadialLen;

	// 基础流向投影到截面平面
	FVector3f PlaneFlow = BaseFlowVector - Axis * FVector3f::DotProduct(BaseFlowVector, Axis);

	if (PlaneFlow.SizeSquared() < FishSchoolEpsilon)
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

		if (PlaneFlow.SizeSquared() < FishSchoolEpsilon)
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
		FMath::Clamp((CoreRadiusSq - RadialSq) / FMath::Max(CoreRadiusSq, FishSchoolEpsilon), 0.0f, 1.0f);

	const float RadialEnvelope =
		EnterEnvelope * (1.0f - FMath::Clamp((T - 0.82f) / 0.10f, 0.0f, 1.0f));

	// 3) 尾部合流：只对外圈鱼，在后半段慢慢往中间收
	const float MergeEnvelope = FMath::Clamp((T - 0.62f) / 0.18f, 0.0f, 1.0f);
	const float OuterRing = FMath::Clamp((Radius01 - 0.58f) / 0.42f, 0.0f, 1.0f);

	const FVector3f SlideCorrection =
		(DesiredPlaneFlow - CurrentPlaneFlow) * (PlayerFlowSideStrength * SideEnvelope);

	const FVector3f SurfaceCorrection =
		RadialNormal * (PlayerFlowRadialStrength * 1.6f * CorePush * RadialEnvelope);

	const FVector3f MergeCorrection =
		(-RadialNormal) * (PlayerFlowRadialStrength * 0.55f * MergeEnvelope * OuterRing);

	PlayerFlowVector += SlideCorrection + SurfaceCorrection + MergeCorrection;
	return true;
}

bool AFishSchoolManagerICO::GridFindKNearestNeighbour(int32 TaskIndex, const int32 CurrentFishIndex,
                                                      FVector3f& NearestNeighbourToCurrent,
                                                      float& NearestSquaredDistance)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(GridFindKNearestNeighbour);

	int32 KMinDistanceIndex = TaskIndex * NumCachedNeighbours;
	int32 NeighbourArrayStartIndex = CurrentFishIndex * NumCachedNeighbours;
	for (int32 NeighbourIndex = 0; NeighbourIndex < NumCachedNeighbours; ++NeighbourIndex)
	{
		FishSchool.CachedNeighbourIds[NeighbourArrayStartIndex + NeighbourIndex] = -1;
		KMinDistances[KMinDistanceIndex + NeighbourIndex] = FLT_MAX;
	}
	FVector3f CurrentLocation = FishSchool.Positions[PreviousBuffer][CurrentFishIndex];
	for (const int32 Offset : NeighbourOffsets)
	{
		const int32 GridIndex = FishSchool.CellIds[CurrentFishIndex] + Offset;
		const int32 BeginIndex = GridData.CellStarts[GridIndex];
		const int32 EndIndex = BeginIndex + GridData.CellCounts[GridIndex];
		for (int32 SortedFishIndex = BeginIndex; SortedFishIndex < EndIndex; ++SortedFishIndex)
		{
			int32 CurrentNeighbourIndex = GridData.SortedFishIds[SortedFishIndex];
			if (CurrentNeighbourIndex == CurrentFishIndex)
			{
				continue;
			}
			FVector3f OffsetVector = CurrentLocation - FishSchool.Positions[PreviousBuffer][CurrentNeighbourIndex];
			float SquaredDistance = OffsetVector.SizeSquared();
			for (int32 i = 0; i < NumCachedNeighbours; i++)
			{
				if (SquaredDistance < KMinDistances[KMinDistanceIndex + i])
				{
					for (int32 j = NumCachedNeighbours - 1; j > i; j--) //K近邻
					{
						FishSchool.CachedNeighbourIds[NeighbourArrayStartIndex + j] =
							FishSchool.CachedNeighbourIds[NeighbourArrayStartIndex + j - 1];
						KMinDistances[KMinDistanceIndex + j] = KMinDistances[KMinDistanceIndex + j - 1];
					}
					FishSchool.CachedNeighbourIds[NeighbourArrayStartIndex + i] = CurrentNeighbourIndex;
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

void AFishSchoolManagerICO::InitRenderChunks()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(InitRenderChunks);

	ClearRenderChunks();

	// 单大 ISM 先不用了
	FishISMComponent->ClearInstances();
	FishISMComponent->SetVisibility(false);
	FishISMComponent->SetHiddenInGame(true);

	TotalNumRenderChunks = NumRenderChunksX * NumRenderChunksY * NumRenderChunksZ;

	RenderChunkCellsX = FMath::DivideAndRoundUp((int32)NumCellsX, NumRenderChunksX);
	RenderChunkCellsY = FMath::DivideAndRoundUp((int32)NumCellsY, NumRenderChunksY);
	RenderChunkCellsZ = FMath::DivideAndRoundUp((int32)NumCellsZ, NumRenderChunksZ);

	RenderChunks.SetNum(TotalNumRenderChunks);

	for (int32 ChunkId = 0; ChunkId < TotalNumRenderChunks; ++ChunkId)
	{
		FRenderChunk& Chunk = RenderChunks[ChunkId];

		FString Name = FString::Printf(TEXT("FishChunkISM_%d"), ChunkId);
		Chunk.ISM = NewObject<UInstancedStaticMeshComponent>(this, *Name);
		Chunk.ISM->SetupAttachment(RootComponent);
		Chunk.ISM->RegisterComponent();

		// 复制旧 FishISMComponent 的设置
		Chunk.ISM->SetStaticMesh(FishISMComponent->GetStaticMesh());
		Chunk.ISM->SetCastShadow(false);
		Chunk.ISM->SetMobility(EComponentMobility::Movable);
		Chunk.ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Chunk.ISM->SetGenerateOverlapEvents(false);

		// 先别搞 per-fish VAT offset，快速版先不用 custom data
		Chunk.ISM->NumCustomDataFloats = 0;

		Chunk.FishIds.Reserve(FMath::Max(NumFishes / TotalNumRenderChunks, 64));
		Chunk.SubmitTransforms.Reserve(FMath::Max(NumFishes / TotalNumRenderChunks, 64));
		Chunk.LastSubmittedCount = 0;
		Chunk.TicksSinceSubmit = 0;
		Chunk.State = ERenderChunkState::Hidden;
		Chunk.PrevState = ERenderChunkState::Hidden;
	}
}

void AFishSchoolManagerICO::ClearRenderChunks()
{
	for (FRenderChunk& Chunk : RenderChunks)
	{
		if (Chunk.ISM)
		{
			Chunk.ISM->DestroyComponent();
			Chunk.ISM = nullptr;
		}
	}
	RenderChunks.Reset();
}

void AFishSchoolManagerICO::ResetRenderChunkFrameData()
{
	for (FRenderChunk& Chunk : RenderChunks)
	{
		Chunk.FishIds.Reset();
		Chunk.NumVisibleHigh = 0;
		Chunk.NumLowRender = 0;
		Chunk.PrevState = Chunk.State;
	}
}

void AFishSchoolManagerICO::BuildRenderChunkLists()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(BuildRenderChunkLists);
	// TODO:遍历activated cells，而非遍历所有的鱼
	for (int32 FishIndex = 0; FishIndex < NumFishes; ++FishIndex)
	{
		const int32 CellIndex = FishSchool.CellIds[FishIndex];
		const bool bUseVisibility = bUseDepthBuffer && bHasValidVisibilityData;
		const ECellStatus Status = !bUseVisibility
			? ECellStatus::VisibleHigh
			: ((CellIndex >= 0) ? GridData.CellStatuses[CellIndex] : ECellStatus::OffscreenLow);

		if (Status != ECellStatus::VisibleHigh && Status != ECellStatus::OccludedLowRender)
		{
			continue;
		}

		int32 RenderCellIndex = CellIndex;
		if (RenderCellIndex < 0 && !bUseVisibility)
		{
			// Outside fish still render while culling is unavailable. Assign them
			// to the nearest boundary chunk without changing their simulation cell.
			const FVector3f Offset = FishSchool.Positions[PreviousBuffer][FishIndex] - GridOrigin;
			const int32 X = FMath::Clamp(FMath::FloorToInt(Offset.X * InverseCellSize), 0, int32(NumCellsX) - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt(Offset.Y * InverseCellSize), 0, int32(NumCellsY) - 1);
			const int32 Z = FMath::Clamp(FMath::FloorToInt(Offset.Z * InverseCellSize), 0, int32(NumCellsZ) - 1);
			RenderCellIndex = GetGridIndex(X, Y, Z);
		}
		const int32 ChunkId = GetRenderChunkIdFromCell(RenderCellIndex);
		if (ChunkId == INDEX_NONE)
		{
			continue;
		}

		FRenderChunk& Chunk = RenderChunks[ChunkId];
		Chunk.FishIds.Add(FishIndex);

		if (Status == ECellStatus::VisibleHigh)
		{
			++Chunk.NumVisibleHigh;
		}
		else
		{
			++Chunk.NumLowRender;
		}
	}
}

void AFishSchoolManagerICO::UpdateRenderChunkStates()
{
	for (FRenderChunk& Chunk : RenderChunks)
	{
		if (!bUseDepthBuffer || !bHasValidVisibilityData)
		{
			// No visibility decision means no threshold-based hiding either.
			Chunk.State = Chunk.FishIds.IsEmpty() ? ERenderChunkState::Hidden : ERenderChunkState::Visible;
		}
		else if (Chunk.NumVisibleHigh >= ChunkVisibleThreshold)
		{
			Chunk.State = ERenderChunkState::Visible;
		}
		else if (Chunk.NumLowRender >= ChunkLowRenderThreshold)
		{
			Chunk.State = ERenderChunkState::LowRender;
		}
		else
		{
			Chunk.State = ERenderChunkState::Hidden;
		}
	}
}

void AFishSchoolManagerICO::RenderChunkedInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RenderChunkedInstances);

	ResetRenderChunkFrameData();
	BuildRenderChunkLists();
	UpdateRenderChunkStates();

	for (int32 ChunkId = 0; ChunkId < RenderChunks.Num(); ++ChunkId)
	{
		FRenderChunk& Chunk = RenderChunks[ChunkId];

		if (!ShouldSubmitChunk(Chunk))
		{
			++Chunk.TicksSinceSubmit;
			continue;
		}

		const int32 CurrentVisibleCount =
			(Chunk.State == ERenderChunkState::Hidden) ? 0 : Chunk.FishIds.Num();

		const int32 SubmitCount = FMath::Max(CurrentVisibleCount, Chunk.LastSubmittedCount);
		if (SubmitCount <= 0)
		{
			Chunk.TicksSinceSubmit = 0;
			continue;
		}

		EnsureRenderChunkCapacity(ChunkId, SubmitCount);
		Chunk.SubmitTransforms.SetNumUninitialized(SubmitCount);

		// 前半段：真实鱼
		for (int32 i = 0; i < CurrentVisibleCount; ++i)
		{
			Chunk.SubmitTransforms[i] = RenderInstance(Chunk.FishIds[i]);
		}

		// 后半段：隐藏尾巴，避免旧实例残留
		for (int32 i = CurrentVisibleCount; i < SubmitCount; ++i)
		{
			Chunk.SubmitTransforms[i] = HiddenTransform;
		}

		Chunk.ISM->BatchUpdateInstancesTransforms(
			0,
			Chunk.SubmitTransforms,
			true,
			true,
			true
		);

		Chunk.LastSubmittedCount = CurrentVisibleCount;
		Chunk.TicksSinceSubmit = 0;
	}
}

void AFishSchoolManagerICO::EnsureRenderChunkCapacity(int32 ChunkId, int32 RequiredCount)
{
	FRenderChunk& Chunk = RenderChunks[ChunkId];

	const int32 CurrentInstanceCount = Chunk.ISM->GetInstanceCount();
	if (CurrentInstanceCount >= RequiredCount)
	{
		return;
	}

	for (int32 i = CurrentInstanceCount; i < RequiredCount; ++i)
	{
		Chunk.ISM->AddInstance(HiddenTransform, true);
	}
}

int32 AFishSchoolManagerICO::GetRenderChunkIdFromCell(int32 CellIndex) const
{
	if (CellIndex < 0)
	{
		return INDEX_NONE;
	}

	const int32 ChunkX = FMath::Min((int32)GridData.CellX[CellIndex] / RenderChunkCellsX, NumRenderChunksX - 1);
	const int32 ChunkY = FMath::Min((int32)GridData.CellY[CellIndex] / RenderChunkCellsY, NumRenderChunksY - 1);
	const int32 ChunkZ = FMath::Min((int32)GridData.CellZ[CellIndex] / RenderChunkCellsZ, NumRenderChunksZ - 1);

	return ChunkX + ChunkY * NumRenderChunksX + ChunkZ * NumRenderChunksX * NumRenderChunksY;
}

bool AFishSchoolManagerICO::ShouldSubmitChunk(const FRenderChunk& Chunk) const
{
	switch (Chunk.State)
	{
	case ERenderChunkState::Visible:
		// Visible chunk 每帧都提交
		return true;

	case ERenderChunkState::LowRender:
		// LowRender chunk 低频提交
		return Chunk.TicksSinceSubmit >= LowRenderChunkSubmitInterval
			|| Chunk.PrevState != Chunk.State
			|| Chunk.LastSubmittedCount != Chunk.FishIds.Num();

	case ERenderChunkState::Hidden:
	default:
		// 只有在上一帧还显示过东西时，才需要补一次隐藏提交
		return Chunk.LastSubmittedCount > 0;
	}
}

void AFishSchoolManagerICO::InitGrid()
{
	//Grid Parameters
	CurrentNumActivatedCellsUpdateTickInterval = 0;
	MaxNumActivatedCells = 0;
	MaxNumDenseCells = 0;
	// 包含最后一个网格外的网格来统计网格外的鱼
	TotalNumCells = NumCellsX * NumCellsY * NumCellsZ;
	OutsideBucketIndex = TotalNumCells;
	TotalNumBuckets = TotalNumCells + 1;
	InverseCellSize = 1.0f / StaticCellSize;
	NumCellsPerLayer = NumCellsX * NumCellsY;
	GridOrigin = FishSchoolDestination - FVector3f(NumCellsX / 2, NumCellsY / 2, NumCellsZ / 2) * StaticCellSize;
	NumActivatedGridTraversalTasks = 1 << NumActivatedGridTraversalTasksPowerOfTwo;
	NumDenseGridTraversalTasks = 1 << NumDenseGridTraversalTasksPowerOfTwo;
	if (MinAvgVectorUpdateTickInterval <= 0)
	{
		MinAvgVectorUpdateTickInterval = 1;
	}
	// TArray Init
	GridData.SortedFishIds.SetNum(NumFishes);
	GridData.CellCounts.SetNum(TotalNumBuckets);
	GridData.CellStarts.SetNum(TotalNumBuckets);
	GridData.CellStatuses.SetNum(TotalNumCells);
	CellFishCountsPerTask.SetNum(NumFishSchoolTraversalTasks * TotalNumBuckets);
	// 暂时没用
	// GridData.CellPositionSums.SetNum(TotalNumCells);
	// GridData.CellDirectionSums.SetNum(TotalNumCells);
	GridData.ActivatedCellIndex.SetNum(FMath::Min(NumFishes, TotalNumCells));
	GridData.DenseCellIndex.SetNum(FMath::Min(NumFishes, TotalNumCells));
	// 只考虑网格内的下标处理
	GridData.CellX.SetNum(TotalNumCells);
	GridData.CellY.SetNum(TotalNumCells);
	GridData.CellZ.SetNum(TotalNumCells);

	PlayerFlowCandidateCells.Reset();

	for (uint8 x = 0; x < NumCellsX; ++x)
	{
		for (uint8 y = 0; y < NumCellsY; ++y)
		{
			for (uint8 z = 0; z < NumCellsZ; ++z)
			{
				const int32 FlatIndex = GetGridIndex(x, y, z);
				GridData.CellX[FlatIndex] = x;
				GridData.CellY[FlatIndex] = y;
				GridData.CellZ[FlatIndex] = z;
			}
		}
	}


	//debug
	DrawDebugSphere(GetWorld(), FVector(ActorLocation), 20, 20, FColor::Red, true);
	DrawDebugBox(
		GetWorld(),
		FVector(GridOrigin) + StaticCellSize * FVector(NumCellsX / 2, NumCellsY / 2, NumCellsZ / 2),
		FVector(NumCellsX / 2, NumCellsY / 2, NumCellsZ / 2) * StaticCellSize, FColor::Red, true);
}

void AFishSchoolManagerICO::ResetGrid()
{
	//0表示网格内鱼的数量为0
	FMemory::Memset(CellFishCountsPerTask.GetData(), 0,
	                NumFishSchoolTraversalTasks * TotalNumBuckets * sizeof(int32));
	GridData.ActivatedCellIndex.Reset();
	GridData.DenseCellIndex.Reset();
}

bool AFishSchoolManagerICO::IsAtGridEdge(int32 CellIndex)
{
	uint8 X = GridData.CellX[CellIndex];
	uint8 Y = GridData.CellY[CellIndex];
	uint8 Z = GridData.CellZ[CellIndex];
	//跳过边界计算
	return IsAtGridEdge(X, Y, Z);
}

FORCEINLINE bool AFishSchoolManagerICO::IsAtGridEdge(uint8 X, uint8 Y, uint8 Z)
{
	//跳过边界计算
	return X == 0 || Y == 0 || Z == 0 || X == NumCellsX - 1 || Y == NumCellsY - 1 || Z == NumCellsZ - 1;
}

void AFishSchoolManagerICO::UpdatePrefixSumAndGetActivatedCells()
{
	//正序
	int32 Count = 0;
	for (int32 CellIndex = 0; CellIndex < TotalNumCells; ++CellIndex)
	{
		const int32 CurrentCellIndex = CellIndex * NumFishSchoolTraversalTasks;
		GridData.CellStarts[CellIndex] = Count;
		for (int32 TaskId = 0; TaskId < NumFishSchoolTraversalTasks; ++TaskId)
		{
			const int32 Current = CurrentCellIndex + TaskId;
			const int32 LocalCount = CellFishCountsPerTask[Current];
			CellFishCountsPerTask[Current] = Count;
			Count += LocalCount;
		}
		//遍历完了当前cell的内容，确认当前cell起点
		int32 CellCount = Count - GridData.CellStarts[CellIndex];
		GridData.CellCounts[CellIndex] = CellCount;
		//网格遍历用
		if (CellCount > 0)
		{
			GridData.ActivatedCellIndex.Add(CellIndex);
			if (CellCount >= MinInvisibleFishCount)
			{
				GridData.DenseCellIndex.Add(CellIndex);
			}
		}
	}
	// 正序遍历本来就已经是有序的了，不需要sort。
	NumActivatedCells = GridData.ActivatedCellIndex.Num();
	NumDenseCells = GridData.DenseCellIndex.Num();
	NumFishesInGrid = Count;
	GridData.CellStarts[OutsideBucketIndex] = Count;
	GridData.CellCounts[OutsideBucketIndex] = NumFishes - GridData.CellStarts[OutsideBucketIndex];
	const int32 OutsideBase = OutsideBucketIndex * NumFishSchoolTraversalTasks;
	for (int32 TaskId = 0; TaskId < NumFishSchoolTraversalTasks; ++TaskId)
	{
		const int32 Current = OutsideBase + TaskId;
		const int32 LocalCount = CellFishCountsPerTask[Current];
		CellFishCountsPerTask[Current] = Count;
		Count += LocalCount;
	}
}

void AFishSchoolManagerICO::UpdatePrefixSumParallelAndGetActivatedCells()
{
	LocalActivatedCells[0].Reset();
	LocalActivatedCells[1].Reset();
	LocalDenseCells[0].Reset();
	LocalDenseCells[1].Reset();
	//遍历所有的
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
				GridData.CellStarts[CellIndex] = Count;
				for (int32 j = 0; j < NumFishSchoolTraversalTasks; ++j)
				{
					const int32 Current = CurrentCellIndex + j;
					const int32 LocalCount = CellFishCountsPerTask[Current];
					CellFishCountsPerTask[Current] = Count;
					Count += LocalCount;
				}
				//遍历完了当前cell的内容，确认上一个格子的起点
				int32 CellCount = Count - GridData.CellStarts[CellIndex];
				GridData.CellCounts[CellIndex] = CellCount;
				//网格遍历用
				if (CellCount > 0)
				{
					LocalActivatedCells[0].Add(CellIndex);
					if (CellCount >= MinInvisibleFishCount)
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
			GridData.CellStarts[OutsideBucketIndex] = Count;
			GridData.CellCounts[OutsideBucketIndex] = NumFishes - Count;
			NumFishesInGrid = Count;
			for (int32 CellIndex = TotalNumCells - 1; CellIndex >= HalfTotalCells; --CellIndex)
			{
				const int32 CurrentCellIndex = CellIndex * NumFishSchoolTraversalTasks;
				for (int32 TaskIndex = NumFishSchoolTraversalTasks - 1; TaskIndex >= 0; --TaskIndex)
				{
					const int32 Current = CurrentCellIndex + TaskIndex;
					Count -= CellFishCountsPerTask[Current];
					CellFishCountsPerTask[Current] = Count;
				}
				GridData.CellStarts[CellIndex] = Count;
				//遍历完了一个cell的内容，确认当前格子的起点
				int32 CellCount = GridData.CellStarts[CellIndex + 1] - Count;
				GridData.CellCounts[CellIndex] = CellCount;
				// 网格遍历用
				if (CellCount > 0)
				{
					LocalActivatedCells[1].Add(CellIndex);
					if (CellCount >= MinInvisibleFishCount)
					{
						LocalDenseCells[1].Add(CellIndex);
					}
				}
			}
		}
	});
	NumActivatedCells = LocalActivatedCells[0].Num() + LocalActivatedCells[1].Num();
	if (MaxNumActivatedCells < NumActivatedCells)
	{
		GridData.ActivatedCellIndex.SetNumUninitialized(NumActivatedCells);
	}

	//正序倒序遍历本来内部就是有序的，不需要sort
	int32 WriteIndex = 0;
	for (int32 CellId : LocalActivatedCells[0])
	{
		GridData.ActivatedCellIndex[WriteIndex++] = CellId;
	}
	WriteIndex = NumActivatedCells;
	for (int32 CellId : LocalActivatedCells[1])
	{
		GridData.ActivatedCellIndex[--WriteIndex] = CellId;
	}

	WriteIndex = 0;
	NumDenseCells = LocalDenseCells[0].Num() + LocalDenseCells[1].Num();
	if (MaxNumDenseCells < NumDenseCells)
	{
		GridData.DenseCellIndex.SetNumUninitialized(NumDenseCells);
	}
	for (int32 CellId : LocalDenseCells[0])
	{
		GridData.DenseCellIndex[WriteIndex++] = CellId;
	}
	WriteIndex = NumDenseCells;
	for (int32 CellId : LocalDenseCells[1])
	{
		GridData.DenseCellIndex[--WriteIndex] = CellId;
	}
}

void AFishSchoolManagerICO::UpdateNumActivatedCellsByMaxValueIntervalTicks()
{
	if (MaxNumActivatedCells < NumActivatedCells)
	{
		MaxNumActivatedCells = NumActivatedCells;
	}
	if (MaxNumDenseCells < NumDenseCells)
	{
		MaxNumDenseCells = NumDenseCells;
	}
	if (CurrentNumActivatedCellsUpdateTickInterval >= MinAvgVectorUpdateTickInterval)
	{
		CurrentNumActivatedCellsUpdateTickInterval = 0;
		int32 NumCells = MaxNumActivatedCells / NumReservedActivatedCells + 1 * NumReservedActivatedCells; // 正整数上取整
		GridData.ActivatedCellIndex.SetNum(NumCells);
		NumCells = MaxNumDenseCells / NumReservedActivatedCells + 1 * NumReservedActivatedCells; // 正整数上取整
		GridData.DenseCellIndex.SetNum(NumCells);
	}
	++CurrentNumActivatedCellsUpdateTickInterval;
}

void AFishSchoolManagerICO::UpdateFishInGrid(int32 TaskIndex, int32 BeginIndex, int32 EndIndex)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateFishInGrid);

	//并行：找到有哪些鱼
	for (int32 FishIndex = BeginIndex; FishIndex < EndIndex; ++FishIndex)
	{
		const FVector3f Offset = FishSchool.Positions[PreviousBuffer][FishIndex] - GridOrigin;

		const int32 X = FMath::FloorToInt(Offset.X * InverseCellSize);
		const int32 Y = FMath::FloorToInt(Offset.Y * InverseCellSize);
		const int32 Z = FMath::FloorToInt(Offset.Z * InverseCellSize);
		if (X >= 0 && X < NumCellsX && Y >= 0 && Y < NumCellsY && Z >= 0 && Z < NumCellsZ)
		{
			//如果鱼在网格内
			const int32 FlatIndex = GetGridIndex(X, Y, Z);
			FishSchool.CellIds[FishIndex] = FlatIndex;
			++CellFishCountsPerTask[FlatIndex * NumFishSchoolTraversalTasks + TaskIndex]; //让相邻的cell连续排列，便于后续的计算
		}
		else
		{
			FishSchool.CellIds[FishIndex] = -1;
			++CellFishCountsPerTask[OutsideBucketIndex * NumFishSchoolTraversalTasks + TaskIndex];
		}
	}
}

void AFishSchoolManagerICO::UpdateFishIds(int32 TaskIndex, int32 BeginIndex, int32 EndIndex)
{
	for (int32 FishIndex = BeginIndex; FishIndex < EndIndex; ++FishIndex)
	{
		const int32 CellIndex = FishSchool.CellIds[FishIndex];
		const int32 CellFishCountsPerTaskIndex = CellIndex < 0
			                                         ? OutsideBucketIndex * NumFishSchoolTraversalTasks + TaskIndex
			                                         : CellIndex * NumFishSchoolTraversalTasks + TaskIndex;
		const int32 Dst = CellFishCountsPerTask[CellFishCountsPerTaskIndex]++;
		GridData.SortedFishIds[Dst] = FishIndex;
	}
}

void AFishSchoolManagerICO::InitDepthBuffer()
{
	//Depth Buffer Parameters
	NumActivatedCells = 0;
	DilatedGridSize = 2 * GridVisibilityDilationRadius + 1;
	TotalDepthBufferSize = GridDepthBufferSizeX * GridDepthBufferSizeY;
	NdcMarginX = 2.0f * ViewBorderExpandPercent + 1.0f;
	InvNdcMarginX = 1.0f / 2.0f / NdcMarginX;
	NdcMarginY = 2.0f * ViewBorderExpandPercent + 1.0f;
	InvNdcMarginY = 1.0f / 2.0f / NdcMarginY;
	//TArray Init
	DepthBuffer.CellIndexBufferX.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
	DepthBuffer.CellIndexBufferY.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
	DepthBuffer.CellIndexBufferZ.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
	DepthBuffer.CellIndexBufferW.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
	DepthBuffer.CellDepthBuffer.SetNum(NumDenseGridTraversalTasks * TotalDepthBufferSize);
	if (bUsePrefixSumParallel)
	{
		LocalActivatedCells[0].Reserve(TotalNumCells / 2 + 1);
		LocalActivatedCells[1].Reserve(TotalNumCells / 2 + 1);
		LocalDenseCells[0].Reserve(TotalNumCells / 2 + 1);
		LocalDenseCells[1].Reserve(TotalNumCells / 2 + 1);
	}
	DilateOffsets.SetNum(DilatedGridSize * DilatedGridSize * DilatedGridSize);
	int32 DilateOffsetsIndex = 0;
	for (int8 x = -GridVisibilityDilationRadius; x < GridVisibilityDilationRadius + 1; ++x)
	{
		for (int8 y = -GridVisibilityDilationRadius; y < GridVisibilityDilationRadius + 1; ++y)
		{
			for (int8 z = -GridVisibilityDilationRadius; z < GridVisibilityDilationRadius + 1; ++z)
			{
				DilateOffsets[DilateOffsetsIndex] = GetGridIndex(x, y, z);
				DilateOffsetsIndex++;
			}
		}
	}
}

void AFishSchoolManagerICO::ResetDepthBuffer()
{
	//-1表示网格内没有存储任何最近网格
	FMemory::Memset(DepthBuffer.CellIndexBufferW.GetData(), 0xFF,
	                NumDenseGridTraversalTasks * TotalDepthBufferSize * sizeof(int32));
	for (int32 DepthBufferIndex = 0; DepthBufferIndex < NumDenseGridTraversalTasks * TotalDepthBufferSize; ++
	     DepthBufferIndex)
	{
		DepthBuffer.CellDepthBuffer[DepthBufferIndex] = FLT_MAX;
	}
}

bool AFishSchoolManagerICO::UpdateViewProjectionMatrix()
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateViewProjectionMatrix);

	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && PC->GetLocalPlayer() && PC->GetLocalPlayer()->ViewportClient && PC->GetLocalPlayer()->ViewportClient->Viewport)
	{
		FSceneViewProjectionData ProjectionData;
		if (PC->GetLocalPlayer()->GetProjectionData(
			PC->GetLocalPlayer()->ViewportClient->Viewport, ProjectionData, INDEX_NONE))
		{
			// ViewProjectionMatrix = ViewMatrix * ProjectionMatrix
			ViewProjectionMatrix = ProjectionData.ComputeViewProjectionMatrix();
			return true;
		}
	}
	return false;
}

void AFishSchoolManagerICO::UpdateDepthBufferInHighDensityAreas(int32 TaskIndex, int32 BeginIndex, int32 EndIndex)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDepthBufferInHighDensityAreas);

	uint8 X, Y, Z;
	int32 CellIndex;
	for (int32 ActivatedCellIndex = BeginIndex; ActivatedCellIndex < EndIndex; ++ActivatedCellIndex)
	{
		CellIndex = GridData.DenseCellIndex[ActivatedCellIndex];
		X = GridData.CellX[CellIndex];
		Y = GridData.CellY[CellIndex];
		Z = GridData.CellZ[CellIndex];
		if (IsAtGridEdge(X, Y, Z))
		{
			GridData.CellStatuses[CellIndex] = ECellStatus::Empty;
			continue;
		}
		UpdateCellDepthBufferInHighDensityAreas(TaskIndex, X, Y, Z, CellIndex, NumDenseGridTraversalTasks);
	}
}

void AFishSchoolManagerICO::UpdateDepthBufferInLowDensityAreas(int32 TaskIndex, int32 BeginIndex, int32 EndIndex)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDepthBufferInLowDensityAreas);

	uint8 X, Y, Z;
	int32 CellIndex;
	for (int32 ActivatedCellIndex = BeginIndex; ActivatedCellIndex < EndIndex; ++ActivatedCellIndex)
	{
		CellIndex = GridData.ActivatedCellIndex[ActivatedCellIndex];
		X = GridData.CellX[CellIndex];
		Y = GridData.CellY[CellIndex];
		Z = GridData.CellZ[CellIndex];
		//鱼在阈值内，并且不可能没有鱼
		if (GridData.CellCounts[CellIndex] < MinInvisibleFishCount)
		{
			UpdateCellDepthBufferInLowDensityAreas(TaskIndex, X, Y, Z, CellIndex);
		}
	}
}

void AFishSchoolManagerICO::MergeDepthBuffer(int32 TotalTasksSize)
{
	int32 CurrentBuffer, CurrentIndex;
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
					GridData.CellStatuses[PreviousWinner] = ECellStatus::OccludedLowHidden;
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
					GridData.CellStatuses[LosingCell] = ECellStatus::OccludedLowHidden;
				}
			}
		}
	}
}

void AFishSchoolManagerICO::DilateGrid()
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(DilateGrid);

	// 扩张表面高性能计算网格，减少空洞
	if (GridVisibilityDilationRadius == 0)
	{
		return;
	}
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

		for (dx = -GridVisibilityDilationRadius; dx < GridVisibilityDilationRadius + 1; dx++)
		{
			x = X + dx;
			if (0 >= x || x >= NumCellsX - 1)
			{
				CurrentOffsetNum += DilatedGridSizeSquare;
				continue;
			}
			for (dy = -GridVisibilityDilationRadius; dy < GridVisibilityDilationRadius + 1; dy++)
			{
				y = Y + dy;
				if (0 >= y || y >= NumCellsY - 1)
				{
					CurrentOffsetNum += DilatedGridSize;
					continue;
				}
				for (dz = -GridVisibilityDilationRadius; dz < GridVisibilityDilationRadius + 1; dz++)
				{
					z = Z + dz;
					if (0 < z && z < NumCellsZ - 1)
					{
						GridData.CellStatuses[DepthBuffer.CellIndexBufferW[CurrentBufferIndex] +
							DilateOffsets[CurrentOffsetNum]] = ECellStatus::VisibleHigh;
					}
					CurrentOffsetNum++;
				}
			}
		}
	}
}

void AFishSchoolManagerICO::UpdateCellDepthBufferInHighDensityAreas(int32 TaskIndex, int32 CurrentCellX,
                                                                    int32 CurrentCellY, int32 CurrentCellZ,
                                                                    int32 CurrentCellIndex, int32 TotalTasksSize)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateCellDepthBufferInHighDensityAreas);

	const FVector3f GridCenter = GridOrigin + FVector3f((CurrentCellX + 0.5) * StaticCellSize,
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
			int32 BufferIdx = TaskIndex + X * TotalTasksSize + Y * GridDepthBufferSizeX * TotalTasksSize;

			if (Distance < DepthBuffer.CellDepthBuffer[BufferIdx])
			{
				//如果是最近的网格
				//将当前的网格设置为需要高精度计算
				GridData.CellStatuses[CurrentCellIndex] = ECellStatus::VisibleHigh;
				const int32 PreviousBufferCellIndex = DepthBuffer.CellIndexBufferW[BufferIdx];
				if (PreviousBufferCellIndex != -1)
				{
					//将之前检测到的设置为只需要简单圆周计算
					GridData.CellStatuses[PreviousBufferCellIndex] = ECellStatus::OccludedLowHidden;
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
				GridData.CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
			}
		}
		else
		{
			//如果不在视野范围内不进行渲染
			GridData.CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
		}
	}
	else
	{
		//在相机背后不进行渲染
		GridData.CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
	}
}

void AFishSchoolManagerICO::UpdateCellDepthBufferInLowDensityAreas(int32 TaskIndex, int32 CurrentCellX,
                                                                   int32 CurrentCellY, int32 CurrentCellZ,
                                                                   int32 CurrentCellIndex)
{
	//debug
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateCellDepthBufferInLowDensityAreas);

	const FVector3f GridCenter = GridOrigin + FVector3f((CurrentCellX + 0.5) * StaticCellSize,
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
				GridData.CellStatuses[CurrentCellIndex] = ECellStatus::VisibleHigh;
			}
			else
			{
				//如果不是最近的网格将当前的网格设置为只需要简单圆周计算
				GridData.CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowRender;
			}
		}
		else
		{
			//如果不在视野范围内不进行渲染
			GridData.CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
		}
	}
	else
	{
		//在相机背后不进行渲染
		GridData.CellStatuses[CurrentCellIndex] = ECellStatus::OccludedLowHidden;
	}
}

void AFishSchoolManagerICO::InitDistanceAdaptiveParameters()
{
	// VeryNear：玩家在网格内
	VeryNearAdaptiveParams.bEnableDepthBuffer = true;
	VeryNearAdaptiveParams.DepthBufferSizeX = 32;
	VeryNearAdaptiveParams.DepthBufferSizeY = 32;
	VeryNearAdaptiveParams.ViewBorderExpandPercent = 0.2f;
	VeryNearAdaptiveParams.MinInvisibleFishCount = 3;
	VeryNearAdaptiveParams.GridVisibilityDilationRadius = 1;
	VeryNearAdaptiveParams.ChunkVisibleThreshold = 0;
	VeryNearAdaptiveParams.ChunkLowRenderThreshold = 40;
	VeryNearAdaptiveParams.LowRenderChunkSubmitInterval = 1;
	VeryNearAdaptiveParams.DestinationWeight = 1.0f;
	VeryNearAdaptiveParams.SeparationWeight = 1.0f;
	VeryNearAdaptiveParams.OrbitWeight = 0.5f;
	VeryNearAdaptiveParams.RandomDirectionWeight = 0.2f;

	// Near：外接球内
	NearAdaptiveParams.bEnableDepthBuffer = true;
	NearAdaptiveParams.DepthBufferSizeX = 64;
	NearAdaptiveParams.DepthBufferSizeY = 64;
	NearAdaptiveParams.ViewBorderExpandPercent = 0.1f;
	NearAdaptiveParams.MinInvisibleFishCount = 3;
	NearAdaptiveParams.GridVisibilityDilationRadius = 1;
	NearAdaptiveParams.ChunkVisibleThreshold = 80;
	NearAdaptiveParams.ChunkLowRenderThreshold = 140;
	NearAdaptiveParams.LowRenderChunkSubmitInterval = 2;
	NearAdaptiveParams.DestinationWeight = 1.0f;
	NearAdaptiveParams.SeparationWeight = 1.0f;
	NearAdaptiveParams.OrbitWeight = 0.5f;
	NearAdaptiveParams.RandomDirectionWeight = 0.4f;

	// Mid：2倍外接球内
	MidAdaptiveParams.bEnableDepthBuffer = true;
	MidAdaptiveParams.DepthBufferSizeX = 128;
	MidAdaptiveParams.DepthBufferSizeY = 128;
	MidAdaptiveParams.ViewBorderExpandPercent = 0.05f;
	MidAdaptiveParams.MinInvisibleFishCount = 2;
	MidAdaptiveParams.GridVisibilityDilationRadius = 1;
	MidAdaptiveParams.ChunkVisibleThreshold = 140;
	MidAdaptiveParams.ChunkLowRenderThreshold = 220;
	MidAdaptiveParams.LowRenderChunkSubmitInterval = 4;
	MidAdaptiveParams.DestinationWeight = 1.0f;
	MidAdaptiveParams.SeparationWeight = 1.0f;
	MidAdaptiveParams.OrbitWeight = 0.5f;
	MidAdaptiveParams.RandomDirectionWeight = 1.0f;

	// Far：更远
	FarAdaptiveParams.bEnableDepthBuffer = true;
	FarAdaptiveParams.DepthBufferSizeX = 256;
	FarAdaptiveParams.DepthBufferSizeY = 256;
	FarAdaptiveParams.ViewBorderExpandPercent = 0.05f;
	FarAdaptiveParams.MinInvisibleFishCount = 2;
	FarAdaptiveParams.GridVisibilityDilationRadius = 1;
	FarAdaptiveParams.ChunkVisibleThreshold = 180;
	FarAdaptiveParams.ChunkLowRenderThreshold = 260;
	FarAdaptiveParams.LowRenderChunkSubmitInterval = 5;
	FarAdaptiveParams.DestinationWeight = 0.0f;
	FarAdaptiveParams.SeparationWeight = 0.0f;
	FarAdaptiveParams.OrbitWeight = 1.0f;
	FarAdaptiveParams.RandomDirectionWeight = 0.0f;
}

void AFishSchoolManagerICO::InitDistanceAdaptivePrecompute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(InitDistanceAdaptivePrecompute);

	// 1. 预计算网格半长
	GridHalfExtent = FVector3f(
		NumCellsX * StaticCellSize * 0.5f,
		NumCellsY * StaticCellSize * 0.5f,
		NumCellsZ * StaticCellSize * 0.5f
	);

	// 2. 预计算外接球半径
	GridOuterSphereRadius = GridHalfExtent.Length();
	GridOuterSphereRadiusSq = FMath::Square(GridOuterSphereRadius);

	GridDoubleOuterSphereRadius = GridOuterSphereRadius * 2.0f;
	GridDoubleOuterSphereRadiusSq = FMath::Square(GridDoubleOuterSphereRadius);
}

const FDistanceAdaptiveParamSet& AFishSchoolManagerICO::GetDistanceAdaptiveParamSet(
	EDistanceAdaptiveMode Mode) const
{
	switch (Mode)
	{
	case EDistanceAdaptiveMode::VeryNear:
		return VeryNearAdaptiveParams;
	case EDistanceAdaptiveMode::Near:
		return NearAdaptiveParams;
	case EDistanceAdaptiveMode::Mid:
		return MidAdaptiveParams;
	case EDistanceAdaptiveMode::Far:
	default:
		return FarAdaptiveParams;
	}
}

void AFishSchoolManagerICO::ApplyDistanceAdaptiveParamSet(const FDistanceAdaptiveParamSet& Params,
                                                          bool bForceReinitDepthBuffer)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ApplyDistanceAdaptiveParamSet);

	// 注意：这里不是直接让 Param 控制 bUseDepthBuffer，
	// 而是还要过一层 warmup 判断
	const bool bEffectiveDepthBufferEnabled =
		bDepthBufferWarmupFinished && Params.bEnableDepthBuffer;

	const bool bNeedReinitDepthBuffer =
		bForceReinitDepthBuffer ||
		(GridDepthBufferSizeX != Params.DepthBufferSizeX) ||
		(GridDepthBufferSizeY != Params.DepthBufferSizeY) ||
		(ViewBorderExpandPercent != Params.ViewBorderExpandPercent) ||
		(GridVisibilityDilationRadius != Params.GridVisibilityDilationRadius);

	// 写入现有系统变量
	bUseDepthBuffer = bEffectiveDepthBufferEnabled;
	GridDepthBufferSizeX = Params.DepthBufferSizeX;
	GridDepthBufferSizeY = Params.DepthBufferSizeY;
	ViewBorderExpandPercent = Params.ViewBorderExpandPercent;
	MinInvisibleFishCount = Params.MinInvisibleFishCount;
	GridVisibilityDilationRadius = Params.GridVisibilityDilationRadius;

	ChunkVisibleThreshold = Params.ChunkVisibleThreshold;
	ChunkLowRenderThreshold = Params.ChunkLowRenderThreshold;
	LowRenderChunkSubmitInterval = Params.LowRenderChunkSubmitInterval;

	ReferenceFishRotationSpeed = Params.RotationSpeed;
	DestinationWeight = Params.DestinationWeight;
	SeparationWeight = Params.SeparationWeight;
	OrbitWeight = Params.OrbitWeight;
	RandomDirectionWeight = Params.RandomDirectionWeight;

	// depth buffer 相关缓存只有在启用时才需要重建
	if (bUseDepthBuffer && bNeedReinitDepthBuffer)
	{
		InitDepthBuffer();
	}
}

void AFishSchoolManagerICO::UpdateDistanceAdaptiveParams(bool bForceReinitDepthBuffer)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDistanceAdaptiveParams);

	if (!bUseDistanceAdaptiveParams)
	{
		return;
	}

	// 玩家相对鱼群中心（也就是 destination）的向量
	const FVector3f Diff = PlayerPosition - FishSchoolDestination;
	const FVector3f AbsDiff(FMath::Abs(Diff.X), FMath::Abs(Diff.Y), FMath::Abs(Diff.Z));
	const float DistSq = Diff.SizeSquared();

	EDistanceAdaptiveMode NewMode = EDistanceAdaptiveMode::Far;

	// 第一档：玩家在鱼群的内部
	if (DistSq <= MinDistanceToDestinationSquare)
	{
		NewMode = EDistanceAdaptiveMode::VeryNear;
	}
	// 第二档：玩家在外接球内
	else if (DistSq <= GridOuterSphereRadiusSq)
	{
		NewMode = EDistanceAdaptiveMode::Near;
	}
	// 第三档：玩家在 2 倍外接球内
	else if (DistSq <= GridDoubleOuterSphereRadiusSq)
	{
		NewMode = EDistanceAdaptiveMode::Mid;
	}
	else
	{
		NewMode = EDistanceAdaptiveMode::Far;
	}

	CurrentDistanceAdaptiveMode = NewMode;

	// destination 指向 player，用来串球
	DestinationToPlayerDirection = Diff.GetSafeNormal();

	const FDistanceAdaptiveParamSet& Params = GetDistanceAdaptiveParamSet(NewMode);
	ApplyDistanceAdaptiveParamSet(Params, bForceReinitDepthBuffer);
}

FString AFishSchoolManagerICO::GetFishStateFilePath(int32 InNumFishes) const
{
	return DataFilePath / FString::Printf(TEXT("FishState_%d.csv"), InNumFishes);
}

bool AFishSchoolManagerICO::SaveFishStateCsvByCurrentCount(const FString FilePath) const
{
	FString FileContent = TEXT("PosX,PosY,PosZ,FwdX,FwdY,FwdZ\n");
	for (int32 i = 0; i < NumFishes; ++i)
	{
		const FVector3f& Loc = FishSchool.Positions[PreviousBuffer][i];
		const FVector3f& Fwd = FishSchool.Directions[PreviousBuffer][i];

		FileContent += FString::Printf(
			TEXT("%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
			Loc.X, Loc.Y, Loc.Z, Fwd.X, Fwd.Y, Fwd.Z
		);
	}

	IFileManager::Get().MakeDirectory(*DataFilePath, true);
	return FFileHelper::SaveStringToFile(FileContent, *FilePath);
}

bool AFishSchoolManagerICO::ExportOccupiedGridCountsCsv(const FString& FilePath) const
{
	FString Content;
	Content.Reserve(1024 * 1024);

	Content += TEXT("CellIndex,X,Y,Z,FishCount,CellStatus\n");

	for (int32 ActivatedCellIndex = 0; ActivatedCellIndex < NumActivatedCells; ++ActivatedCellIndex)
	{
		int32 CellIndex = GridData.ActivatedCellIndex[ActivatedCellIndex];
		const int32 FishCount = GridData.CellCounts[CellIndex];
		if (FishCount <= 0)
		{
			continue;
		}

		Content += FString::Printf(
			TEXT("%d,%d,%d,%d,%d,%d\n"),
			CellIndex,
			static_cast<int32>(GridData.CellX[CellIndex]),
			static_cast<int32>(GridData.CellY[CellIndex]),
			static_cast<int32>(GridData.CellZ[CellIndex]),
			FishCount,
			static_cast<int32>(GridData.CellStatuses[CellIndex])
		);
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	return FFileHelper::SaveStringToFile(Content, *FilePath);
}

bool AFishSchoolManagerICO::TryLoadFishStateCsvByCount(int32 InNumFishes)
{
	const FString FilePath = GetFishStateFilePath(InNumFishes);

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return false;
	}

	int32 FishIndex = 0;
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("PosX,")) || Line.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}

		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 6)
		{
			continue;
		}

		if (FishIndex >= NumFishes)
		{
			break;
		}

		const FVector3f Pos(
			FCString::Atof(*Parts[0]),
			FCString::Atof(*Parts[1]),
			FCString::Atof(*Parts[2])
		);

		const FVector3f Dir = FVector3f(
			FCString::Atof(*Parts[3]),
			FCString::Atof(*Parts[4]),
			FCString::Atof(*Parts[5])
		).GetSafeNormal();

		FishSchool.Positions[PreviousBuffer][FishIndex] = Pos;
		FishSchool.Positions[NextBuffer][FishIndex] = Pos;
		FishSchool.Directions[PreviousBuffer][FishIndex] = Dir;
		FishSchool.Directions[NextBuffer][FishIndex] = Dir;

		++FishIndex;
	}

	if (FishIndex != NumFishes)
	{
		UE_LOG(LogTemp, Warning, TEXT("Fish count mismatch in file: expected %d, got %d"), NumFishes, FishIndex);
		return false;
	}

	return true;
}

bool AFishSchoolManagerICO::ExportRenderChunkStatsCsv(const FString& FilePath) const
{
	FString Content;
	Content.Reserve(1024 * 1024);

	Content += TEXT("ChunkId,NumVisibleHigh,NumLowRender,LastSubmittedCount,TicksSinceSubmit,State\n");

	for (int32 ChunkId = 0; ChunkId < RenderChunks.Num(); ++ChunkId)
	{
		const FRenderChunk& Chunk = RenderChunks[ChunkId];

		Content += FString::Printf(
			TEXT("%d,%d,%d,%d,%d,%d\n"),
			ChunkId,
			Chunk.NumVisibleHigh,
			Chunk.NumLowRender,
			Chunk.LastSubmittedCount,
			Chunk.TicksSinceSubmit,
			static_cast<int32>(Chunk.State)
		);
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	return FFileHelper::SaveStringToFile(Content, *FilePath);
}

void AFishSchoolManagerICO::HandleExportRenderChunkStats()
{
	const FString FilePath = DataFilePath / TEXT("RenderChunkStats.csv");
	if (ExportRenderChunkStatsCsv(FilePath))
	{
		UE_LOG(LogTemp, Display, TEXT("Render chunk stats exported: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to export render chunk stats: %s"), *FilePath);
	}
}

void AFishSchoolManagerICO::HandleSaveFishSchool()
{
	UE_LOG(LogTemp, Display, TEXT("Saving Fish School!"));
	bIsPlaying = false;

	const FString FilePath = GetFishStateFilePath(NumFishes);

	if (SaveFishStateCsvByCurrentCount(FilePath))
	{
		UE_LOG(LogTemp, Display, TEXT("Fish state saved to: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save fish state: %s"), *FilePath);
	}
}

void AFishSchoolManagerICO::HandleExportOccupiedGridCounts()
{
	const FString FilePath = DataFilePath / TEXT("OccupiedGridCounts.csv");
	if (ExportOccupiedGridCountsCsv(FilePath))
	{
		UE_LOG(LogTemp, Display, TEXT("Occupied grid counts exported: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to export occupied grid counts: %s"), *FilePath);
	}
}

void AFishSchoolManagerICO::HandleStartFishSchool()
{
	UE_LOG(LogTemp, Display, TEXT("Start Fish School Calculating!"));
	bIsPlaying = true;

	InitFishSchool();

	const bool bLoadedSavedState = TryLoadFishStateCsvByCount(NumFishes);
	if (!bLoadedSavedState)
	{
		UE_LOG(LogTemp, Error, TEXT("Random Init Positions And Directions."));
		RandomInitPositionsAndDirections();

		bDepthBufferWarmupFinished = !bUseDepthBuffer;
		if (bUseDepthBuffer)
		{
			GetWorld()->GetTimerManager().SetTimer(
				DepthBufferTimerHandle,
				[this]()
				{
					bDepthBufferWarmupFinished = true;
					InitDepthBuffer();
					UE_LOG(LogTemp, Display, TEXT("DepthBuffer Warmup Finished!"));
				},
				DepthBufferWarmupDelay,
				false
			);
		}
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Loaded saved fish state."));
		bDepthBufferWarmupFinished = true;
	}

	InitGrid();

	// 新增：预计算距离分档相关数据
	InitDistanceAdaptivePrecompute();

	// 先拿到一次玩家位置
	UpdatePlayerPosition();

	// 新增：首次应用当前档位参数，并强制重建 depth buffer 缓存
	UpdateDistanceAdaptiveParams(true);

	if (bUseChunkedISM)
	{
		InitRenderChunks();
	}
	else
	{
		InitRenderInstances();
	}
}
