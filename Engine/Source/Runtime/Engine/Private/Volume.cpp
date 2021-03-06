// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	Volume.cpp: AVolume and subclasses
=============================================================================*/

#include "EnginePrivate.h"
#include "Components/BrushComponent.h"

#if WITH_EDITOR
/** Define static delegate */
AVolume::FOnVolumeShapeChanged AVolume::OnVolumeShapeChanged;
#endif

DEFINE_LOG_CATEGORY(LogVolume);

AVolume::AVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GetBrushComponent()->AlwaysLoadOnClient = true;
	GetBrushComponent()->AlwaysLoadOnServer = true;
	static FName CollisionProfileName(TEXT("OverlapAll"));
	GetBrushComponent()->SetCollisionProfileName(CollisionProfileName);
	GetBrushComponent()->bGenerateOverlapEvents = true;
	bReplicateMovement = false;
#if WITH_EDITORONLY_DATA
	bActorLabelEditable = true;
#endif // WITH_EDITORONLY_DATA

	bCanBeDamaged = false;
}

#if WITH_EDITOR

void AVolume::PostEditImport()
{
	Super::PostEditImport();

	OnVolumeShapeChanged.Broadcast(*this);
}

void AVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	static FName BrushBuilder(TEXT("BrushBuilder"));

	// The brush builder that created this volume has changed. Notify listeners
	if( PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive && PropertyChangedEvent.MemberProperty && PropertyChangedEvent.MemberProperty->GetFName() == BrushBuilder )
	{
		OnVolumeShapeChanged.Broadcast(*this);
	}
}

#endif // WITH_EDITOR

bool AVolume::EncompassesPoint(FVector Point, float SphereRadius/*=0.f*/, float* OutDistanceToPoint)
{
	if(GetBrushComponent())
	{
#if WITH_PHYSX
		FVector ClosestPoint;
		float Distance = GetBrushComponent()->GetDistanceToCollision(Point, ClosestPoint);
#else
		FBoxSphereBounds Bounds = BrushComponent->CalcBounds(BrushComponent->ComponentToWorld);
		float Distance = FMath::Sqrt(Bounds.GetBox().ComputeSquaredDistanceToPoint(Point));
#endif

		if(OutDistanceToPoint)
		{
			*OutDistanceToPoint = Distance;
		}

		return Distance >= 0.f && Distance <= SphereRadius;
	}
	else
	{
		UE_LOG(LogVolume, Log, TEXT("AVolume::EncompassesPoint : No BrushComponent"));
		return false;
	}
}

bool AVolume::IsLevelBoundsRelevant() const
{
	return false;
}
	
bool AVolume::IsStaticBrush() const
{
	return false;
}

bool AVolume::IsVolumeBrush() const
{
	return true;
}




