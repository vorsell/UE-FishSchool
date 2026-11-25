// Copyright (c) 2026 Vorsel.

#pragma once

#include "CoreMinimal.h"
#include "FishSchool/SimulationV2/FishSchoolGrid.h"
#include "FishSchool/SimulationV2/FishSchoolPlayerFlow.h"
#include "FishSchool/SimulationV2/FishSchoolRenderer.h"
#include "FishSchool/SimulationV2/FishSchoolSimulation.h"
#include "FishSchool/SimulationV2/FishSchoolVisibility.h"
#include "GameFramework/Actor.h"
#include "Stats/FishSchoolStats.h"
#include "TimerManager.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "FishSchoolManagerV2.generated.h"

class UEnhancedInputComponent;
class UInputAction;
class UInstancedStaticMeshComponent;

USTRUCT(BlueprintType)
struct FFishSchoolDistanceParamsV2
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

/** 关卡入口，负责输入绑定、距离分档和每帧模块调度。 */
UCLASS()
class UEFISHSCHOOL_API AFishSchoolManagerV2 : public AActor
{
	GENERATED_BODY()

public:
	AFishSchoolManagerV2();
	virtual void Tick(float DeltaTime) override;

	const FFishSchoolStats& GetFishSchoolStats() const { return CachedFishSchoolStats; }

	// 鱼群基础设置
	UPROPERTY(VisibleAnywhere, Category = "FishSchoolManager|Base")
	TObjectPtr<UInstancedStaticMeshComponent> FishISMComponent;
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
	// 捕食者槽位数量，旧版回避数据取自玩家位置
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	int32 NumPredators = 0;
	// 鱼群目的地
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Base")
	FVector3f FishSchoolDestination = FVector3f::ZeroVector;
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

	// 基础深度开关。关闭距离分档时直接生效，启动时用于决定是否等待预热。
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

	// 四档距离配置
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FFishSchoolDistanceParamsV2 VeryNearAdaptiveParams;

	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FFishSchoolDistanceParamsV2 NearAdaptiveParams;

	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FFishSchoolDistanceParamsV2 MidAdaptiveParams;

	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|DistanceAdaptive")
	FFishSchoolDistanceParamsV2 FarAdaptiveParams;

	// 文件保存位置
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|SavedData")
	FString DataFilePath;

	// Debug: Input
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	TObjectPtr<UInputAction> SaveFishSchoolAction;
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	TObjectPtr<UInputAction> SaveGridDataAction;
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	TObjectPtr<UInputAction> SaveChunkDataAction;
	UPROPERTY(EditAnywhere, Category = "FishSchoolManager|Input")
	TObjectPtr<UInputAction> StartFishSchoolAction;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	enum class EDistanceMode : uint8
	{
		VeryNear,
		Near,
		Mid,
		Far
	};

	void HandleStartFishSchool();
	void HandleSaveFishSchool();
	void HandleExportOccupiedGridCounts();
	void HandleExportRenderChunkStats();
	void UpdatePlayerPosition();
	void PrecomputeDistanceRanges();
	void InitDistanceAdaptiveParameters();
	void UpdateDistanceAdaptiveMode();
	const FFishSchoolDistanceParamsV2& GetDistanceAdaptiveParamSet(EDistanceMode Mode) const;
	FishSchool::V2::FVisibilitySettings MakeVisibilitySettings(const FFishSchoolDistanceParamsV2* ActiveParams) const;
	void FinishDepthWarmup();
	bool TryGetViewProjectionMatrix(FMatrix& OutMatrix) const;
	void UpdateStats();
	FishSchool::V2::FBehaviorSettings MakeBehaviorSettings(const FFishSchoolDistanceParamsV2* ActiveParams) const;

	UPROPERTY()
	bool bIsPlaying = false;

	FishSchool::V2::FGrid Grid;
	FishSchool::V2::FVisibility Visibility;
	FishSchool::V2::FSimulation Simulation;
	FishSchool::V2::FPlayerFlow PlayerFlow;
	FishSchool::V2::FRenderer Renderer;
	FFishSchoolStats CachedFishSchoolStats;
	FTimerHandle DepthBufferTimerHandle;
	TWeakObjectPtr<UEnhancedInputComponent> BoundInput;
	TArray<uint32> InputBindingHandles;
	EDistanceMode CurrentDistanceAdaptiveMode = EDistanceMode::Far;
	bool bDepthBufferWarmupFinished = false;
	FVector3f PlayerPosition = FVector3f::ZeroVector;
	FVector3f DestinationToPlayerDirection = FVector3f::ZeroVector;
	float MinDistanceToDestinationSquare = 0.0f;
	float GridOuterSphereRadiusSq = 0.0f;
	float GridDoubleOuterSphereRadiusSq = 0.0f;
};
