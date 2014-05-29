// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "AssetToolsPrivatePCH.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions"

void FAssetTypeActions_World::OpenAssetEditor( const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor )
{
	for (auto ObjIt = InObjects.CreateConstIterator(); ObjIt; ++ObjIt)
	{
		auto World = Cast<UWorld>(*ObjIt);
		if (World != NULL && World != GEditor->GetEditorWorldContext().World())
		{
			// If there are any unsaved changes to the current level, see if the user wants to save those first.
			bool bPromptUserToSave = true;
			bool bSaveMapPackages = true;
			bool bSaveContentPackages = true;
			if (FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages))
			{
				const FString FileToOpen = FPackageName::LongPackageNameToFilename(World->GetOutermost()->GetName(), FPackageName::GetMapPackageExtension());
				const bool bLoadAsTemplate = false;
				const bool bShowProgress = true;
				FEditorFileUtils::LoadMap(FileToOpen, bLoadAsTemplate, bShowProgress);
			}

			// We can only edit one world at a time... so just break after the first valid world to load
			break;
		}
	}
}

#undef LOCTEXT_NAMESPACE
