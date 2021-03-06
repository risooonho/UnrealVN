// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "Paper2DEditorPrivatePCH.h"
#include "AssetData.h"
#include "PhysicsEngine/BodySetup.h"
//////////////////////////////////////////////////////////////////////////
// UPaperSpriteActorFactory

UPaperSpriteActorFactory::UPaperSpriteActorFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("Paper2D", "PaperSpriteFactoryDisplayName", "Add Sprite");
	NewActorClass = APaperSpriteActor::StaticClass();
}

void UPaperSpriteActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	if (UPaperSprite* Sprite = Cast<UPaperSprite>(Asset))
	{
		GEditor->SetActorLabelUnique(NewActor, Sprite->GetName());

		APaperSpriteActor* TypedActor = CastChecked<APaperSpriteActor>(NewActor);
		UPaperSpriteComponent* RenderComponent = TypedActor->GetRenderComponent();
		check(RenderComponent);

		RenderComponent->UnregisterComponent();
		RenderComponent->SetSprite(Sprite);

		if (Sprite->BodySetup != nullptr)
		{
			RenderComponent->BodyInstance.CopyBodyInstancePropertiesFrom(&(Sprite->BodySetup->DefaultInstance));
		}

		RenderComponent->RegisterComponent();
	}
}

void UPaperSpriteActorFactory::PostCreateBlueprint(UObject* Asset, AActor* CDO)
{
	if (UPaperSprite* Sprite = Cast<UPaperSprite>(Asset))
	{
		if (APaperSpriteActor* TypedActor = Cast<APaperSpriteActor>(CDO))
		{
			UPaperSpriteComponent* RenderComponent = TypedActor->GetRenderComponent();
			check(RenderComponent);

			RenderComponent->SetSprite(Sprite);

			if (Sprite->BodySetup != nullptr)
			{
				RenderComponent->BodyInstance.CopyBodyInstancePropertiesFrom(&(Sprite->BodySetup->DefaultInstance));
			}
		}
	}
}

bool UPaperSpriteActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (AssetData.IsValid() && AssetData.GetClass()->IsChildOf(UPaperSprite::StaticClass()))
	{
		return true;
	}
	else
	{
		OutErrorMsg = NSLOCTEXT("Paper2D", "CanCreateActorFrom_NoSprite", "No sprite was specified.");
		return false;
	}
}
