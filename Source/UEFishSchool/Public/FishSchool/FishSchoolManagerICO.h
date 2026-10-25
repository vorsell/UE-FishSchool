// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "Stats/FishSchoolStats.h"

#include "FishSchoolManagerICO.generated.h"

class UInstancedStaticMeshComponent;
class UInputAction;

UENUM(BlueprintType)
enum class EDistanceAdaptiveMode : uint8
{
	VeryNear = 0, // 网格内
	Near = 1, // 网格外，但在外接球内
	Mid = 2, // 在 2 倍外接球内
	Far = 3, // 更远
};

UENUM(BlueprintType)
enum class ECellStatus : uint8
{
	Empty = 0 UMETA(DisplayName = "Empty"),
	OffscreenLow = 1 UMETA(DisplayName = "OffscreenLow"),
	VisibleHigh = 2 UMETA(DisplayName = "VisibleHigh"),
	OccludedLowRender = 3 UMETA(DisplayName = "OccludedLowRender"),
	OccludedLowHidden = 4 UMETA(DisplayName = "OccludedLowHidden"),
};

UENUM(BlueprintType)
enum class ERenderChunkState : uint8
{
	Hidden = 0,
	LowRender = 1,
	Visible = 2,
};

USTRUCT(BlueprintType)
struct FFishSchool
{
	GENERATED_BODY()

	// 位置
	TArray<FVector3f> Positions[2];
	// 朝向
	TArray<FVector3f> Directions[2];
	// 鱼当前所在的网格 ID
	TArray<int32> CellIds;
	// 鱼在 ISM 中的实例下标
	TArray<int32> FishInstanceIds;
	// 缓存的近邻鱼 ID，用于减少 KNN 次数
	TArray<int32> CachedNeighbourIds;

	FFishSchool() = default;
};

USTRUCT(BlueprintType)
struct FGrid
{
	GENERATED_BODY()

	TArray<int32> CellCounts;
	TArray<int32> CellStarts;
	TArray<int32> SortedFishIds;
	TArray<int32> ActivatedCellIndex;
	TArray<int32> DenseCellIndex;
	TArray<FVector3f> CellPositionSums;
	TArray<FVector3f> CellDirectionSums;
	TArray<ECellStatus> CellStatuses;
	TArray<uint8> CellX;
	TArray<uint8> CellY;
	TArray<uint8> CellZ;

	FGrid() = default;
};

USTRUCT(BlueprintType)
struct FVisibilityBuffer
{
	GENERATED_BODY()
	// 网格中最近元素下标
	TArray<int32> CellIndexBufferX;
	TArray<int32> CellIndexBufferY;
	TArray<int32> CellIndexBufferZ;
	TArray<int32> CellIndexBufferW;
	// 网格中最近元素距离
	TArray<float> CellDepthBuffer;

	FVisibilityBuffer() = default;
};

USTRUCT()
struct FRenderChunk
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	UInstancedStaticMeshComponent* ISM = nullptr;

	// 这一帧需要显示/提交的鱼（只存 VisibleHigh + OccludedLowRender）
	TArray<int32> FishIds;

	// 这次真正要提交给 BatchUpdate 的 transform 数组
	TArray<FTransform> SubmitTransforms;

	int32 NumVisibleHigh = 0;
	int32 NumLowRender = 0;

	// 上一帧实际提交了多少个实例（用于决定是否要补一段隐藏尾巴）
	int32 LastSubmittedCount = 0;

	// 距离上次真正提交已经过了多少 tick
	int32 TicksSinceSubmit = 0;

	ERenderChunkState State = ERenderChunkState::Hidden;
	ERenderChunkState PrevState = ERenderChunkState::Hidden;
};

USTRUCT(BlueprintType)
struct FDistanceAdaptiveParamSet
{
	GENERATED_BODY()

	// 深度缓冲
	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	bool bEnableDepthBuffer = true;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	int32 DepthBufferSizeX = 64;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	int32 DepthBufferSizeY = 64;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	float ViewBorderExpandPercent = 0.05f;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	int32 MinInvisibleFishCount = 4;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	int32 GridVisibilityDilationRadius = 1;

	// chunk
	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	int32 ChunkVisibleThreshold = 120;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	int32 ChunkLowRenderThreshold = 200;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	int32 LowRenderChunkSubmitInterval = 4;


	// 旋转和行为权重
	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	float RotationSpeed = 90.0f;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	float DestinationWeight = 1.0f;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	float SeparationWeight = 1.0f;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	float OrbitWeight = 0.5f;

	UPROPERTY(EditAnywhere, Category = "DistanceAdaptive")
	float RandomDirectionWeight = 1.0f;
};

//Fish School Manager ISM CPU Optimization
UCLASS()
class UEFISHSCHOOL_API AFishSchoolManagerICO : public AActor
{
	GENERATED_BODY()

public:
	AFishSchoolManagerICO();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;
	
	const FFishSchoolStats& GetFishSchoolStats() const { return CachedFishSchoolStats; }

	// 鱼群基础设置
	UPROPERTY(VisibleAnywhere, Category = "FishSchoolManager|Base")
	UInstancedStaticMeshComponent* FishISMComponent;
	// 随机生成区域的立方体边长
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base",
		meta = (EditCondition = "!bIsPlaying", EditConditionHides))
	float FishSpawnBoxSideLength = 3000.0f;
	// 鱼的最大生成数量
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base",
		meta = (EditCondition = "!bIsPlaying", EditConditionHides))
	int32 MaxNumFishes = 100000;
	// 当前鱼的数量
	UPROPERTY(VisibleAnywhere, Category = "FishSchoolManager|Base")
	int32 NumFishes = 0;
	// 当前鱼的数量
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	int32 NumPredators = 0;
	// 鱼群目的地
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	FVector3f FishSchoolDestination;
	// 鱼原始长度
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	float FishOriginalLength = 199.0f;
	// 鱼缩放后的目标长度
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	float FishTargetLength = 20.0f;
	// 随机数组大小
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	int32 RandomArraySize = 1024;
	// 随机数组大小
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	uint32 RandomSeed = 114514;

	// chunk渲染
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|RenderChunk")
	bool bUseChunkedISM = true;
	// chunk尺寸
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|RenderChunk")
	int32 NumRenderChunksX = 5;
	// chunk尺寸
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|RenderChunk")
	int32 NumRenderChunksY = 5;
	// chunk尺寸
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|RenderChunk")
	int32 NumRenderChunksZ = 5;
	// 低渲染频率的间隔帧
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|RenderChunk")
	int32 LowRenderChunkSubmitInterval = 4;
	// 高频率渲染鱼数阈值
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|RenderChunk")
	int32 ChunkVisibleThreshold = 120;
	// 低频率渲染鱼数阈值
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|RenderChunk")
	int32 ChunkLowRenderThreshold = 200;

	// 鱼群计算加速参数
	// 缓存的近邻数量
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters",
		meta = (EditCondition = "!bIsPlaying", EditConditionHides))
	int32 NumCachedNeighbours = 5;
	// 经过多少 tick 后重新计算平均位置和平均朝向
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters",
		meta = (EditCondition = "!bIsPlaying", EditConditionHides))
	uint8 MinAvgVectorUpdateTickInterval = 1;
	// 经过多少 tick 后重新更新有鱼的网格数量
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters",
		meta = (EditCondition = "!bIsPlaying", EditConditionHides))
	uint8 MineNumActivatedCellsUpdateTickInterval = 20;
	// 以多少为单位向上取值有鱼的网格数量，预留空间
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters",
		meta = (EditCondition = "!bIsPlaying", EditConditionHides))
	int32 NumReservedActivatedCells = 1000;
	// 2 的次幂
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters")
	int32 MaxUpdateKNNBlockPowerOfTwo = 1;
	// 鱼群遍历任务数量的 2 次幂，例如 3 表示 2^3 = 8 个任务
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters")
	int32 NumFishTraversalTasksPowerOfTwo = 3;
	// 网格的遍历任务数量的 2 次幂，例如 3 表示 2^3 = 8 个任务
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters")
	int32 NumDenseGridTraversalTasksPowerOfTwo = 2;
	// 激活网格的遍历任务数量的 2 次幂，例如 3 表示 2^3 = 8 个任务
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters")
	int32 NumActivatedGridTraversalTasksPowerOfTwo = 2;
	// 双路并行前缀和
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OptimizeParameters")
	bool bUsePrefixSumParallel = false;

	// 间距
	// 最小鱼间距（单位：体长）
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	float MinFishSeparationDistanceInBodyLengths = 2.0f;
	// 与目标点的最小距离，留出一片空间
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	float MinDistanceToDestination = 300.0f;
	// 与玩家的最小距离，留出一片空间
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	float MinDistanceToPlayer = 100.0f;

	// 速度
	// 基准速度，冲刺为 5 倍体长/s，平均速度为 3 倍体长/s（单位：体长/s）
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Speed")
	float ReferenceFishSpeedInBodyLengths = 3.0f;
	// 基准旋转速度（单位：角度）
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Speed")
	float ReferenceFishRotationSpeed = 90.0f;
	// 速度（倍率，应用时再乘 ReferenceFishSpeed）
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Speed|DistanceAdaptive")
	float PredatorAvoidSpeedMultiplier = 3.0f;

	// 权重
	// 鱼群中心权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float SchoolCenterWeight = 1.0f;
	// 鱼群目标权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float DestinationWeight = 1.0f;
	// 平均朝向权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float AverageForwardWeight = 1.0f;
	// 回避权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float SeparationWeight = 1.0f;
	// 通道权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float PlayerFlowWeight  = 2.0f;
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight|PlayerFlow")
	float PlayerFlowLengthScale = 1.5f;   // 通道长度放大到玩家距离的 1.5 倍
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight|PlayerFlow")
	float PlayerFlowRadiusScale = 1.25f;  // 通道整体半径放大
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight|PlayerFlow")
	float PlayerFlowSideStrength = 1.25f;  // 侧向分流强度
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight|PlayerFlow")
	float PlayerFlowRadialStrength = 3.0f; // 轻微外推，别太大
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight|PlayerFlow")
	float PlayerFlowSurfaceRadiusScale = 1.0f; // 目标“表面半径”，相对影响半径
	
	// 绕点运动权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float OrbitWeight = 0.5f;
	// 随机方向权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float RandomDirectionWeight = 1.0f;
	// 随机速度权重，单位：体长
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float RandomSpeedWeightInBodyLengths = 0.5f;
	// 随机旋转速度权重
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Weight")
	float RandomRotationWeight = 10.0f;

	// 其他鱼群设置
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|OtherFishSetting")
	FVector3f OrbitAxis = FVector3f::UpVector;

	// 网格优化，固定网格大小
	// 在鱼群到达目标点且目标点长时间不动时的立方体网格尺寸
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization")
	float StaticCellSize = 30.0f;
	// 网格数量，最外层视作不在网格内
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization")
	uint8 NumCellsX = 50;
	// 网格数量，最外层视作不在网格内
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization")
	uint8 NumCellsY = 50;
	// 网格数量，最外层视作不在网格内
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization")
	uint8 NumCellsZ = 50;

	// 是否启用深度缓冲
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization|DepthBuffer")
	bool bUseDepthBuffer = false;
	// 深度缓冲存储数组大小
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization|DepthBuffer")
	int32 GridDepthBufferSizeX = 64;
	// 深度缓冲存储数组大小
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization|DepthBuffer")
	int32 GridDepthBufferSizeY = 64;
	// 按百分比扩展有效屏幕面积
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization|DepthBuffer")
	float ViewBorderExpandPercent = 0.0f;
	// 网格可视范围扩展，值为以当前网格为中心的扩展半径
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization|DepthBuffer")
	int32 GridVisibilityDilationRadius = 0;
	// 单个网格启用深度缓冲判定的最小鱼数量
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|GridOptimization|DepthBuffer")
	int32 MinInvisibleFishCount = 3;

	//距离切换
	// 距离驱动参数切换
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	bool bUseDistanceAdaptiveParams = true;
	
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	float DepthBufferWarmupDelay = 25.0f;

	// 四档参数（你主要用前三档，Far 是兜底）
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FDistanceAdaptiveParamSet VeryNearAdaptiveParams;

	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FDistanceAdaptiveParamSet NearAdaptiveParams;

	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FDistanceAdaptiveParamSet MidAdaptiveParams;

	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FDistanceAdaptiveParamSet FarAdaptiveParams;

	// 文件保存位置
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|SavedData")
	FString DataFilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FishSchoolState"));

	// Debug: Input
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	UInputAction* SaveFishSchoolAction;
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	UInputAction* SaveGridDataAction;
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	UInputAction* SaveChunkDataAction;
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	UInputAction* StartFishSchoolAction;

private:
	UPROPERTY(Transient)
	FFishSchoolStats CachedFishSchoolStats;
	
	UPROPERTY()
	bool bIsPlaying = false;

	FFishSchool FishSchool;
	FGrid GridData;
	int32 PreviousBuffer = 0;
	int32 NextBuffer = 1;
	FVector TargetToOriginalLengthRatio;
	float MinDistanceToDestinationSquare;
	float MinDistanceToPlayerSquare;
	float MinFishSeparationDistance;
	float ReferenceFishSpeed;
	float RandomSpeedWeight;
	TArray<FTransform> AllTransforms;
	int32 CurrentAvgVectorUpdateTickInterval;
	int32 CurrentNumActivatedCellsUpdateTickInterval;
	TArray<FVector3f> RandomVectors;
	// NumCachedNeighbours * 任务数量
	TArray<float> KMinDistances;
	TArray<int32> NeighbourOffsets;
	FVector3f ActorLocation;
	int32 NumUpdateKNNBlock;
	int32 CurrentUpdateKNNBlock;
	FTimerHandle DepthBufferTimerHandle;
	FIntVector PlayerGridIndex;
	FVector3f PlayerPosition;
	FVector3f PlayerPositionToDestination;
	TArray<FVector3f> PredatorPositions;
	TArray<float> MinDistanceToPredator;
	//胶囊通道
	bool bPlayerFlowActive = false;
	FVector3f PlayerFlowAxis = FVector3f::ZeroVector;
	FVector3f PlayerFlowChannelOrigin = FVector3f::ZeroVector;
	float PlayerFlowChannelLength = 0.0f;
	float PlayerFlowInvChannelLength = 0.0f;
	float PlayerFlowChannelRadius = 0.0f;
	float PlayerFlowChannelRadiusSq = 0.0f;
	float PlayerFlowInvChannelRadiusSq = 0.0f;
	TArray<int32> PlayerFlowCandidateCells;
	FVector3f PlayerFlowSideAxis = FVector3f::ZeroVector;
	FVector3f PlayerFlowCrossAxis = FVector3f::ZeroVector;

	// 给粗筛用，略大于真正椭圆柱
	float PlayerFlowMaskSideRadius = 0.0f;
	float PlayerFlowMaskCrossRadius = 0.0f;


	void InitFishSchool();
	void RandomInitPositionsAndDirections();
	void InitRenderInstances();
	void RenderAllInstances();
	FTransform RenderInstance(int32 FishIndex);
	void UpdatePlayerPosition();
	void UpdatePredatorPosition();
	void TraversalGridFishGroup(float DeltaTime);
	void UpdateGrid();
	void UpdateFishOrbitRodrigues(int32 CurrentFishIndex, float DeltaTime);
	void UpdateFishOrbitCross(int32 CurrentFishIndex, float DeltaTime);
	void UpdateFishDestination(int32 CurrentFishIndex, float DeltaTime);
	void UpdateFishKNN(int32 TaskIndex, int32 CurrentFishIndex, float DeltaTime, bool bUpdateKNN);
	void UpdatePlayerFlowPrecompute();
	void UpdatePlayerFlowCellMask();
	bool IsPlayerFlowCandidateCell(int32 CellIndex) const;
	bool GetSeparationAvgPositionAndDirection(int32 TaskIndex, int32 CurrentFishIndex, bool bUpdateKNN,
	                                          FVector3f& SeparationVector, FVector3f& AvgPositionVector,
	                                          FVector3f& AvgDirectionVector, bool& bHitPredatorAvoidance);
	bool GetPlayerFlowVector(int32 CurrentFishIndex, const FVector3f& BaseFlowVector, FVector3f& PlayerFlowVector);
	
	/**
	 * 找至多K个近邻，若鱼数量不够或者网格内鱼数量不够返回false。如果找到了任意近邻则返回true。
	 * @param TaskIndex 任务编号
	 * @param CurrentFishIndex 计算目标鱼的K近邻
	 * @param NearestNeighbourToCurrent 目标鱼的当前网格内位置
	 * @param NearestNeighbourToCurrent 返回值，返回未归一化的向量（最近邻指向当前鱼）
	 * @param NearestSquaredDistance 返回值，最近邻距离平方，减少计算次数
	 * @return 是否找到任意一个近邻
	 */
	bool GridFindKNearestNeighbour(int32 TaskIndex, const int32 CurrentFishIndex, FVector3f& NearestNeighbourToCurrent,
	                               float& NearestSquaredDistance);
	FORCEINLINE void SwapBuffer()
	{
		Swap(PreviousBuffer, NextBuffer);
	}

	// Use the UE API; the exported copy does not vendor the engine implementation.
	FORCEINLINE static FVector3f VInterpNormalRotationTo3f(const FVector3f& Current, const FVector3f& Target,
	                                                       float DeltaTime, float RotationSpeedDegrees)
	{
		return FVector3f(FMath::VInterpNormalRotationTo(
			FVector(Current), FVector(Target), DeltaTime, RotationSpeedDegrees));
	}

	FORCEINLINE FVector3f GetHashRandomVector(int32 FishIndex)
	{
		static constexpr uint32 Hash = 2654435761u;
		return RandomVectors[(FishIndex ^ (RandomSeed * 2246822519u) * Hash) & (RandomArraySize - 1)];
	}
	

	// chunk ISM
	TArray<FRenderChunk> RenderChunks;

	int32 TotalNumRenderChunks = 0;
	int32 RenderChunkCellsX = 1;
	int32 RenderChunkCellsY = 1;
	int32 RenderChunkCellsZ = 1;
	FTransform HiddenTransform = FTransform(FRotator::ZeroRotator, FVector(0, 0, -100000), FVector::ZeroVector);

	// ===== Render Chunk Methods =====
	void InitRenderChunks();
	void ClearRenderChunks();
	void ResetRenderChunkFrameData();
	void BuildRenderChunkLists();
	void UpdateRenderChunkStates();
	void RenderChunkedInstances();
	void EnsureRenderChunkCapacity(int32 ChunkId, int32 RequiredCount);
	int32 GetRenderChunkIdFromCell(int32 CellIndex) const;
	bool ShouldSubmitChunk(const FRenderChunk& Chunk) const;

	// 网格更新缓存
	int32 TotalNumCells;
	int32 OutsideBucketIndex;
	// 额外处理网格外的鱼
	int32 TotalNumBuckets;
	int32 NumFishesInGrid;
	float InverseCellSize;
	int32 NumCellsPerLayer;
	int32 NumFishSchoolTraversalTasks;
	int32 NumDenseGridTraversalTasks;
	int32 NumActivatedGridTraversalTasks;
	FVector3f GridOrigin;
	// 每个任务对每个网格统计到的鱼数量,长度：NumGridUpdateTasks * TotalNumCells,由于鱼的位置是随机生成的，因此单个网格中鱼的数量通常不太可能超过 256,TODO: 若该假设稳定成立，可考虑改为 uint8
	TArray<int32> CellFishCountsPerTask;
	TArray<int32> DilateOffsets;
	TArray<int32> LocalActivatedCells[2];
	TArray<int32> LocalDenseCells[2];
	int32 NumActivatedCells;
	int32 MaxNumActivatedCells;
	int32 NumDenseCells;
	int32 MaxNumDenseCells;

	void InitGrid();
	void ResetGrid();
	bool IsAtGridEdge(int32 CellIndex);
	FORCEINLINE bool IsAtGridEdge(uint8 X, uint8 Y, uint8 Z);
	void UpdatePrefixSumAndGetActivatedCells();
	void UpdatePrefixSumParallelAndGetActivatedCells();
	void UpdateNumActivatedCellsByMaxValueIntervalTicks();
	void UpdateFishInGrid(int32 TaskIndex, int32 BeginIndex, int32 EndIndex);
	void UpdateFishIds(int32 TaskIndex, int32 BeginIndex, int32 EndIndex);

	// 深度缓存
	FVisibilityBuffer DepthBuffer;
	// False during warmup or when no camera projection is available this frame.
	bool bHasValidVisibilityData = false;
	int32 TotalDepthBufferSize;
	FMatrix ViewProjectionMatrix;
	int32 DilatedGridSize;
	float NdcMarginX;
	float NdcMarginY;
	float InvNdcMarginX;
	float InvNdcMarginY;

	void InitDepthBuffer();
	void ResetDepthBuffer();
	bool UpdateViewProjectionMatrix();
	void UpdateDepthBufferInHighDensityAreas(int32 TaskIndex, int32 BeginIndex, int32 EndIndex);
	void UpdateDepthBufferInLowDensityAreas(int32 TaskIndex, int32 BeginIndex, int32 EndIndex);
	void MergeDepthBuffer(int32 TotalTasksSize);
	void DilateGrid();
	void UpdateCellDepthBufferInHighDensityAreas(int32 TaskIndex, int32 CurrentCellX, int32 CurrentCellY,
	                                             int32 CurrentCellZ, int32 CurrentCellIndex, int32 TotalTasksSize);
	void UpdateCellDepthBufferInLowDensityAreas(int32 TaskIndex, int32 CurrentCellX, int32 CurrentCellY,
	                                            int32 CurrentCellZ, int32 CurrentCellIndex);

	FORCEINLINE int32 GetGridIndex(int32 X, int32 Y, int32 Z) const
	{
		return X + Y * NumCellsX + Z * NumCellsPerLayer;
	}

	// 不可视区域剔除
	bool bCullInvisibleGridsByDepthBuffer = false;


	// ===== Distance Adaptive Runtime =====
	EDistanceAdaptiveMode CurrentDistanceAdaptiveMode = EDistanceAdaptiveMode::Far;
	// depth buffer 是否已经允许启用（用于替代你现在直接改 bUseDepthBuffer 的 timer 逻辑）
	bool bDepthBufferWarmupFinished = false;
	// 从 destination 指向 player，用于提前开道球串
	FVector3f DestinationToPlayerDirection = FVector3f::ZeroVector;
	// 当前生效的绝对速度
	float CurrentPlayerAvoidSpeed = 0.0f;
	float CurrentPredatorAvoidSpeed = 0.0f;
	// ===== Distance Adaptive Precompute =====
	// 网格半长（半边长）
	FVector3f GridHalfExtent = FVector3f::ZeroVector;
	// 外接球半径及平方
	float GridOuterSphereRadius = 0.0f;
	float GridOuterSphereRadiusSq = 0.0f;
	// 2倍外接球半径及平方
	float GridDoubleOuterSphereRadius = 0.0f;
	float GridDoubleOuterSphereRadiusSq = 0.0f;
	// ===== Distance Adaptive Methods =====
	void InitDistanceAdaptiveParameters();
	void InitDistanceAdaptivePrecompute();
	const FDistanceAdaptiveParamSet& GetDistanceAdaptiveParamSet(EDistanceAdaptiveMode Mode) const;
	void ApplyDistanceAdaptiveParamSet(const FDistanceAdaptiveParamSet& Params, bool bForceReinitDepthBuffer = false);
	void UpdateDistanceAdaptiveParams(bool bForceReinitDepthBuffer = false);

	// Debug
	FString GetFishStateFilePath(int32 InNumFishes) const;

	bool SaveFishStateCsvByCurrentCount(const FString FilePath) const;
	bool ExportOccupiedGridCountsCsv(const FString& FilePath) const;
	bool TryLoadFishStateCsvByCount(int32 InNumFishes);
	bool ExportRenderChunkStatsCsv(const FString& FilePath) const;
	void HandleExportRenderChunkStats();
	void HandleSaveFishSchool();
	void HandleExportOccupiedGridCounts();
	void HandleStartFishSchool();
};
