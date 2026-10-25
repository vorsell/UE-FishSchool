#pragma once

#include "CoreMinimal.h"
#include "FishSchoolStats.generated.h"

USTRUCT(BlueprintType)
struct  FFishSchoolStats
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadOnly, Category = "Fish School Stats")
	int32 FishCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fish School Stats")
	int32 ActiveCellCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fish School Stats")
	int32 DenseCellCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fish School Stats")
	float DenseCellPercent = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Fish School Stats")
	float AvgFishCountInActiveCells = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Fish School Stats")
	int32 VisibleHighFishCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fish School Stats")
	int32 KNNCalcCount = 0;
};
