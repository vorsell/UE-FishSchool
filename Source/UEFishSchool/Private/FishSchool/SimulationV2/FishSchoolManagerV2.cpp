// Copyright (c) 2026 Vorsel.

#include "FishSchool/SimulationV2/FishSchoolManagerV2.h"

#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "FishSchool/SimulationV2/FishSchoolCsv.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SceneView.h"

AFishSchoolManagerV2::AFishSchoolManagerV2()
{
	PrimaryActorTick.bCanEverTick = true;

	FishISMComponent = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("FishISM"));
	SetRootComponent(FishISMComponent.Get());
	FishISMComponent->SetCastShadow(false);
	FishISMComponent->SetMobility(EComponentMobility::Movable);
	FishISMComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FishISMComponent->SetGenerateOverlapEvents(false);

	DataFilePath = FPaths::ProjectDir() / TEXT("SourceAssets/FishSchoolState");
	InitDistanceAdaptiveParameters();
}

void AFishSchoolManagerV2::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	UEnhancedInputComponent* Input = PC ? Cast<UEnhancedInputComponent>(PC->InputComponent) : nullptr;
	if (Input == nullptr)
	{
		return;
	}

	// 保留已有输入动作。记录此次绑定，结束时只解除这个 Manager 建立的绑定。
	BoundInput = Input;
	const auto Bind = [this, Input](const UInputAction* Action, void (AFishSchoolManagerV2::*Handler)())
	{
		if (Action)
		{
			InputBindingHandles.Add(Input->BindAction(Action, ETriggerEvent::Started, this, Handler).GetHandle());
		}
	};
	Bind(SaveFishSchoolAction.Get(), &AFishSchoolManagerV2::HandleSaveFishSchool);
	Bind(StartFishSchoolAction.Get(), &AFishSchoolManagerV2::HandleStartFishSchool);
	Bind(SaveGridDataAction.Get(), &AFishSchoolManagerV2::HandleExportOccupiedGridCounts);
	Bind(SaveChunkDataAction.Get(), &AFishSchoolManagerV2::HandleExportRenderChunkStats);
}

void AFishSchoolManagerV2::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bIsPlaying = false;
	GetWorldTimerManager().ClearTimer(DepthBufferTimerHandle);
	if (UEnhancedInputComponent* Input = BoundInput.Get())
	{
		for (const uint32 Handle : InputBindingHandles)
		{
			Input->RemoveBindingByHandle(Handle);
		}
	}
	InputBindingHandles.Reset();
	BoundInput.Reset();
	Renderer.Shutdown();
	Super::EndPlay(EndPlayReason);
}

void AFishSchoolManagerV2::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bIsPlaying)
	{
		return;
	}

	// 更新本帧的距离档位、通道几何和捕食者位置，供并行任务只读。
	UpdatePlayerPosition();
	UpdateDistanceAdaptiveMode();
	const FFishSchoolDistanceParamsV2* ActiveParams = bUseDistanceAdaptiveParams
		? &GetDistanceAdaptiveParamSet(CurrentDistanceAdaptiveMode) : nullptr;
	const bool bUsePlayerFlow = ActiveParams != nullptr &&
		(CurrentDistanceAdaptiveMode == EDistanceMode::Near || CurrentDistanceAdaptiveMode == EDistanceMode::VeryNear);
	const FishSchool::V2::FPlayerFlowSettings FlowSettings{PlayerFlowLengthScale, PlayerFlowRadiusScale,
	                                       PlayerFlowSideStrength, PlayerFlowRadialStrength};
	PlayerFlow.Prepare(Grid, FlowSettings, FishSchoolDestination, PlayerPosition, DestinationToPlayerDirection,
	                   OrbitAxis, MinDistanceToPlayer, bUsePlayerFlow);
	Simulation.UpdatePredatorPosition(PlayerPosition);

	// 交换后，网格、可见性和渲染使用同一读取缓冲，仿真写入下一缓冲。
	Simulation.SwapBuffers();
	const TConstArrayView<FVector3f> Positions = Simulation.ReadPositions();
	const TConstArrayView<FVector3f> Directions = Simulation.ReadDirections();
	const int32 DenseFishThreshold = ActiveParams ? ActiveParams->MinInvisibleFishCount : MinInvisibleFishCount;
	Grid.Build(Positions, DenseFishThreshold, bUsePrefixSumParallel);

	// 档位选择决定是否请求深度判断，预热状态只影响本帧是否执行。
	const bool bWantsDepth = ActiveParams ? ActiveParams->bEnableDepthBuffer : bUseDepthBuffer;
	const bool bDepthActive = bWantsDepth && bDepthBufferWarmupFinished;
	if (bDepthActive)
	{
		Visibility.EnsureInitialized(Grid, MakeVisibilitySettings(ActiveParams));
	}

	FMatrix ViewProjection = FMatrix::Identity;
	const bool bHasProjection = bDepthActive && TryGetViewProjectionMatrix(ViewProjection);
	Visibility.Update(Grid, bHasProjection ? &ViewProjection : nullptr);

	Simulation.Step(Grid, Visibility, PlayerFlow, MakeBehaviorSettings(ActiveParams), DeltaTime, RandomSeed);
	++RandomSeed;
	const FishSchool::V2::FChunkSubmitSettings SubmitSettings{
		ActiveParams ? ActiveParams->ChunkVisibleThreshold : ChunkVisibleThreshold,
		ActiveParams ? ActiveParams->ChunkLowRenderThreshold : ChunkLowRenderThreshold,
		ActiveParams ? ActiveParams->LowRenderChunkSubmitInterval : LowRenderChunkSubmitInterval
	};
	Renderer.Render(Grid, Visibility, Positions, Directions, SubmitSettings);
	UpdateStats();
}

void AFishSchoolManagerV2::HandleStartFishSchool()
{
	check(NumFishTraversalTasksPowerOfTwo >= 0 && NumFishTraversalTasksPowerOfTwo < 31);
	check(MaxUpdateKNNBlockPowerOfTwo >= 0 && MaxUpdateKNNBlockPowerOfTwo < 31);
	check(FishOriginalLength > 0.0f);

	// 重启时从一份新的初始状态开始，并取消上次仿真的预热定时器。
	bIsPlaying = false;
	GetWorldTimerManager().ClearTimer(DepthBufferTimerHandle);
	NumFishes = MaxNumFishes;
	FishSchoolDestination = FVector3f(GetActorLocation());
	CurrentDistanceAdaptiveMode = EDistanceMode::Far;
	DestinationToPlayerDirection = FVector3f::ZeroVector;
	MinDistanceToDestinationSquare = FMath::Square(MinDistanceToDestination);

	const int32 NumTraversalTasks = 1 << NumFishTraversalTasksPowerOfTwo;
	const FishSchool::V2::FGridSettings GridSettings{NumCellsX, NumCellsY, NumCellsZ, StaticCellSize, NumTraversalTasks};
	Grid.Initialize(GridSettings, FishSchoolDestination, NumFishes);
	Visibility.Update(Grid, nullptr);

	const float ReferenceFishSpeed = ReferenceFishSpeedInBodyLengths * FishTargetLength;
	FishSchool::V2::FSimulationSettings SimulationSettings;
	SimulationSettings.NumTraversalTasks = NumTraversalTasks;
	SimulationSettings.NumCachedNeighbours = NumCachedNeighbours;
	SimulationSettings.NumUpdateKNNBlock = 1 << MaxUpdateKNNBlockPowerOfTwo;
	SimulationSettings.RandomArraySize = RandomArraySize;
	SimulationSettings.NumPredators = NumPredators;
	SimulationSettings.MinFishSeparationDistance = MinFishSeparationDistanceInBodyLengths * FishTargetLength;
	SimulationSettings.ReferenceFishSpeed = ReferenceFishSpeed;
	SimulationSettings.RandomSpeedWeight = RandomSpeedWeightInBodyLengths * FishTargetLength;
	SimulationSettings.CurrentPredatorAvoidSpeed = ReferenceFishSpeed * PredatorAvoidSpeedMultiplier;
	SimulationSettings.MinDistanceToDestinationSquare = MinDistanceToDestinationSquare;
	Simulation.Initialize(Grid, NumFishes, SimulationSettings);

	// 优先恢复旧存档。读档失败时重建两份缓冲，并按旧版策略等待深度预热。
	const FString StatePath = FishSchool::V2::Csv::GetFishStateFilePath(DataFilePath, NumFishes);
	const bool bLoadedSavedState = FishSchool::V2::Csv::LoadFishState(StatePath,
		Simulation.EditPreviousPositions(), Simulation.EditPreviousDirections(),
		Simulation.EditNextPositions(), Simulation.EditNextDirections());
	const bool bWaitForWarmup = !bLoadedSavedState && bUseDepthBuffer && DepthBufferWarmupDelay > 0.0f;
	bDepthBufferWarmupFinished = !bWaitForWarmup;
	if (!bLoadedSavedState)
	{
		Simulation.RandomInitialize(FishSchoolDestination, FishSpawnBoxSideLength);
		UE_LOG(LogTemp, Display, TEXT("Random Init Positions And Directions."));
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Loaded saved fish state."));
	}

	// 准备距离分档所需的网格范围和首帧玩家位置。
	PrecomputeDistanceRanges();
	UpdatePlayerPosition();

	const FishSchool::V2::FRenderSettings RenderSettings{bUseChunkedISM, FIntVector(NumRenderChunksX, NumRenderChunksY, NumRenderChunksZ),
	                                     FishTargetLength / FishOriginalLength};
	Renderer.Initialize(*FishISMComponent, Grid, Simulation.ReadPositions(), Simulation.ReadDirections(), RenderSettings);
	bIsPlaying = true;

	if (bWaitForWarmup)
	{
		GetWorldTimerManager().SetTimer(DepthBufferTimerHandle, this, &AFishSchoolManagerV2::FinishDepthWarmup,
		                                DepthBufferWarmupDelay, false);
	}

	const FVector HalfExtent(NumCellsX / 2, NumCellsY / 2, NumCellsZ / 2);
	const FVector DrawExtent = Grid.GetCellSize() * HalfExtent;
	DrawDebugSphere(GetWorld(), FVector(FishSchoolDestination), 20, 20, FColor::Red, true);
	DrawDebugBox(GetWorld(), FVector(Grid.GetOrigin()) + DrawExtent, DrawExtent, FColor::Red, true);
}

void AFishSchoolManagerV2::UpdatePlayerPosition()
{
	const ACharacter* Character = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0);
	if (Character && Character->GetCapsuleComponent())
	{
		PlayerPosition = FVector3f(Character->GetCapsuleComponent()->GetComponentLocation());
	}
}

void AFishSchoolManagerV2::PrecomputeDistanceRanges()
{
	const FIntVector Dimensions = Grid.GetDimensions();
	const float CellSize = Grid.GetCellSize();
	const FVector3f HalfExtent(Dimensions.X * CellSize * 0.5f, Dimensions.Y * CellSize * 0.5f,
	                           Dimensions.Z * CellSize * 0.5f);
	const float OuterRadius = HalfExtent.Length();
	GridOuterSphereRadiusSq = FMath::Square(OuterRadius);
	GridDoubleOuterSphereRadiusSq = FMath::Square(OuterRadius * 2.0f);
}

FishSchool::V2::FVisibilitySettings AFishSchoolManagerV2::MakeVisibilitySettings(
	const FFishSchoolDistanceParamsV2* ActiveParams) const
{
	check(NumDenseGridTraversalTasksPowerOfTwo >= 0 && NumDenseGridTraversalTasksPowerOfTwo < 31);
	check(NumActivatedGridTraversalTasksPowerOfTwo >= 0 && NumActivatedGridTraversalTasksPowerOfTwo < 31);
	FishSchool::V2::FVisibilitySettings Settings;
	Settings.DepthBufferSizeX = ActiveParams ? ActiveParams->DepthBufferSizeX : GridDepthBufferSizeX;
	Settings.DepthBufferSizeY = ActiveParams ? ActiveParams->DepthBufferSizeY : GridDepthBufferSizeY;
	Settings.ViewBorderExpandPercent = ActiveParams ? ActiveParams->ViewBorderExpandPercent : ViewBorderExpandPercent;
	Settings.DilationRadius = ActiveParams ? ActiveParams->GridVisibilityDilationRadius : GridVisibilityDilationRadius;
	Settings.NumDenseTraversalTasks = 1 << NumDenseGridTraversalTasksPowerOfTwo;
	Settings.NumActivatedTraversalTasks = 1 << NumActivatedGridTraversalTasksPowerOfTwo;

	return Settings;
}

void AFishSchoolManagerV2::FinishDepthWarmup()
{
	bDepthBufferWarmupFinished = true;
	UE_LOG(LogTemp, Display, TEXT("DepthBuffer Warmup Finished!"));
}

bool AFishSchoolManagerV2::TryGetViewProjectionMatrix(FMatrix& OutMatrix) const
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
	UGameViewportClient* ViewportClient = LocalPlayer ? LocalPlayer->ViewportClient : nullptr;
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		return false;
	}

	FSceneViewProjectionData ProjectionData;
	if (!LocalPlayer->GetProjectionData(ViewportClient->Viewport, ProjectionData, INDEX_NONE))
	{
		return false;
	}
	OutMatrix = ProjectionData.ComputeViewProjectionMatrix();
	return true;
}

FishSchool::V2::FBehaviorSettings AFishSchoolManagerV2::MakeBehaviorSettings(
	const FFishSchoolDistanceParamsV2* ActiveParams) const
{
	FishSchool::V2::FBehaviorSettings Settings;
	Settings.FishSchoolDestination = FishSchoolDestination;
	Settings.OrbitAxis = OrbitAxis;
	Settings.ReferenceFishRotationSpeed = ActiveParams ? ActiveParams->RotationSpeed : ReferenceFishRotationSpeed;
	Settings.DestinationWeight = ActiveParams ? ActiveParams->DestinationWeight : DestinationWeight;
	Settings.SchoolCenterWeight = SchoolCenterWeight;
	Settings.AverageForwardWeight = AverageForwardWeight;
	Settings.SeparationWeight = ActiveParams ? ActiveParams->SeparationWeight : SeparationWeight;
	Settings.OrbitWeight = ActiveParams ? ActiveParams->OrbitWeight : OrbitWeight;
	Settings.PlayerFlowWeight = PlayerFlowWeight;
	Settings.RandomDirectionWeight = ActiveParams ? ActiveParams->RandomDirectionWeight : RandomDirectionWeight;
	Settings.RandomRotationWeight = RandomRotationWeight;
	return Settings;
}

void AFishSchoolManagerV2::UpdateStats()
{
	CachedFishSchoolStats.FishCount = NumFishes;
	CachedFishSchoolStats.ActiveCellCount = Grid.GetActivatedCells().Num();
	const float ActiveCount = static_cast<float>(CachedFishSchoolStats.ActiveCellCount);
	CachedFishSchoolStats.DenseCellPercent = ActiveCount > 0.0f ? Grid.GetDenseCells().Num() / ActiveCount : 0.0f;
	CachedFishSchoolStats.AvgFishCountInActiveCells = ActiveCount > 0.0f ? NumFishes / ActiveCount : 0.0f;

	CachedFishSchoolStats.VisibleHighFishCount = 0;
	for (int32 ChunkId = 0; ChunkId < Renderer.GetNumChunks(); ++ChunkId)
	{
		CachedFishSchoolStats.VisibleHighFishCount += Renderer.GetChunkStats(ChunkId).NumVisibleHigh;
	}
}

void AFishSchoolManagerV2::InitDistanceAdaptiveParameters()
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

const FFishSchoolDistanceParamsV2& AFishSchoolManagerV2::GetDistanceAdaptiveParamSet(EDistanceMode Mode) const
{
	switch (Mode)
	{
	case EDistanceMode::VeryNear:
		return VeryNearAdaptiveParams;
	case EDistanceMode::Near:
		return NearAdaptiveParams;
	case EDistanceMode::Mid:
		return MidAdaptiveParams;
	case EDistanceMode::Far:
	default:
		return FarAdaptiveParams;
	}
}

void AFishSchoolManagerV2::UpdateDistanceAdaptiveMode()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDistanceAdaptiveMode);

	if (!bUseDistanceAdaptiveParams)
	{
		return;
	}

	// 玩家相对鱼群中心（也就是 destination）的向量
	const FVector3f Diff = PlayerPosition - FishSchoolDestination;
	const float DistSq = Diff.SizeSquared();

	EDistanceMode NewMode = EDistanceMode::Far;

	// 由近到远比较三个半径，选出本帧使用的档位。
	if (DistSq <= MinDistanceToDestinationSquare)
	{
		NewMode = EDistanceMode::VeryNear;
	}
	else if (DistSq <= GridOuterSphereRadiusSq)
	{
		NewMode = EDistanceMode::Near;
	}
	else if (DistSq <= GridDoubleOuterSphereRadiusSq)
	{
		NewMode = EDistanceMode::Mid;
	}
	else
	{
		NewMode = EDistanceMode::Far;
	}

	CurrentDistanceAdaptiveMode = NewMode;

	// destination 指向 player，用来串球
	DestinationToPlayerDirection = Diff.GetSafeNormal();
}

void AFishSchoolManagerV2::HandleExportRenderChunkStats()
{
	const FString FilePath = DataFilePath / TEXT("RenderChunkStats.csv");
	if (FishSchool::V2::Csv::ExportRenderChunkStats(FilePath, Renderer))
	{
		UE_LOG(LogTemp, Display, TEXT("Render chunk stats exported: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to export render chunk stats: %s"), *FilePath);
	}
}

void AFishSchoolManagerV2::HandleSaveFishSchool()
{
	UE_LOG(LogTemp, Display, TEXT("Saving Fish School!"));
	bIsPlaying = false;

	const FString FilePath = FishSchool::V2::Csv::GetFishStateFilePath(DataFilePath, NumFishes);

	if (FishSchool::V2::Csv::SaveFishState(FilePath, Simulation.ReadPositions(), Simulation.ReadDirections()))
	{
		UE_LOG(LogTemp, Display, TEXT("Fish state saved to: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save fish state: %s"), *FilePath);
	}
}

void AFishSchoolManagerV2::HandleExportOccupiedGridCounts()
{
	const FString FilePath = DataFilePath / TEXT("OccupiedGridCounts.csv");
	if (FishSchool::V2::Csv::ExportOccupiedGridCounts(FilePath, Grid, Visibility))
	{
		UE_LOG(LogTemp, Display, TEXT("Occupied grid counts exported: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to export occupied grid counts: %s"), *FilePath);
	}
}
