// Copyright (c) 2026 Vorsel.

#include "FishSchool/SimulationV2/FishSchoolCsv.h"

#include "FishSchool/SimulationV2/FishSchoolGrid.h"
#include "FishSchool/SimulationV2/FishSchoolRenderer.h"
#include "FishSchool/SimulationV2/FishSchoolVisibility.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace FishSchool::V2::Csv
{
	namespace
	{
		bool SaveCsv(const FString& FilePath, const FString& Content)
		{
			const FString Folder = FPaths::GetPath(FilePath);
			if (!Folder.IsEmpty())
			{
				IFileManager::Get().MakeDirectory(*Folder, true);
			}
			return FFileHelper::SaveStringToFile(Content, *FilePath);
		}
	}

	FString GetFishStateFilePath(const FString& DataFolder, int32 NumFishes)
	{
		return DataFolder / FString::Printf(TEXT("FishState_%d.csv"), NumFishes);
	}

	bool SaveFishState(const FString& FilePath, TConstArrayView<FVector3f> Positions,
	                   TConstArrayView<FVector3f> Directions)
	{
		if (Positions.Num() != Directions.Num())
		{
			return false;
		}

		FString FileContent = TEXT("PosX,PosY,PosZ,FwdX,FwdY,FwdZ\n");
		for (int32 i = 0; i < Positions.Num(); ++i)
		{
			const FVector3f& Loc = Positions[i];
			const FVector3f& Fwd = Directions[i];

			FileContent += FString::Printf(
				TEXT("%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
				Loc.X, Loc.Y, Loc.Z, Fwd.X, Fwd.Y, Fwd.Z
			);
		}

		return SaveCsv(FilePath, FileContent);
	}

	bool LoadFishState(const FString& FilePath,
	                   TArrayView<FVector3f> PreviousPositions, TArrayView<FVector3f> PreviousDirections,
	                   TArrayView<FVector3f> NextPositions, TArrayView<FVector3f> NextDirections)
	{
		const int32 NumFishes = PreviousPositions.Num();
		if (PreviousDirections.Num() != NumFishes || NextPositions.Num() != NumFishes ||
			NextDirections.Num() != NumFishes)
		{
			return false;
		}

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
		{
			return false;
		}

		int32 FishIndex = 0;
		// 按有效数据行的顺序分配鱼 ID，跳过表头、空行和列数不符的行。
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

			// 两份仿真缓冲写入相同的初始位置和归一化朝向，首次交换缓冲后仍是同一状态。
			PreviousPositions[FishIndex] = Pos;
			NextPositions[FishIndex] = Pos;
			PreviousDirections[FishIndex] = Dir;
			NextDirections[FishIndex] = Dir;
			++FishIndex;
		}

		if (FishIndex != NumFishes)
		{
			UE_LOG(LogTemp, Warning, TEXT("Fish count mismatch in file: expected %d, got %d"), NumFishes, FishIndex);
			return false;
		}

		return true;
	}

	bool ExportOccupiedGridCounts(const FString& FilePath, const FGrid& Grid, const FVisibility& Visibility)
	{
		FString Content;
		Content.Reserve(1024 * 1024);
		Content += TEXT("CellIndex,X,Y,Z,FishCount,CellStatus\n");

		const TConstArrayView<int32> CellCounts = Grid.GetCellCounts();
		const bool bUseVisibility = Visibility.HasValidData();
		const TConstArrayView<ECellStatus> CellStatuses = bUseVisibility
			                                                  ? Visibility.GetCellStatuses()
			                                                  : TConstArrayView<ECellStatus>();
		// 按激活格列表导出有鱼的 Cell，便于和网格索引按同一顺序核对。
		for (const int32 CellIndex : Grid.GetActivatedCells())
		{
			const int32 FishCount = CellCounts[CellIndex];
			if (FishCount <= 0)
			{
				continue;
			}
			const FIntVector Cell = Grid.GetCellCoordinates(CellIndex);
			const ECellStatus Status = bUseVisibility ? CellStatuses[CellIndex] : ECellStatus::Empty;

			Content += FString::Printf(
				TEXT("%d,%d,%d,%d,%d,%d\n"),
				CellIndex, Cell.X, Cell.Y, Cell.Z, FishCount, static_cast<int32>(Status)
			);
		}

		return SaveCsv(FilePath, Content);
	}

	bool ExportRenderChunkStats(const FString& FilePath, const FRenderer& Renderer)
	{
		FString Content;
		Content.Reserve(1024 * 1024);
		Content += TEXT("ChunkId,NumVisibleHigh,NumLowRender,LastSubmittedCount,TicksSinceSubmit,State\n");

		// 每个已创建的 Chunk 都占一行，包括空块，便于跨帧按 ID 对照。
		for (int32 ChunkId = 0; ChunkId < Renderer.GetNumChunks(); ++ChunkId)
		{
			const FRenderChunkStats& Chunk = Renderer.GetChunkStats(ChunkId);
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

		return SaveCsv(FilePath, Content);
	}
} // namespace FishSchool::V2::Csv
