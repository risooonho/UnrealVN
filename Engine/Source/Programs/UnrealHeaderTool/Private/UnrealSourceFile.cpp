// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "UnrealHeaderTool.h"
#include "UnrealSourceFile.h"
#include "ParserHelper.h"

void FUnrealSourceFile::AddDefinedClass(UClass* Class)
{
	DefinedClasses.Add(Class);
}

FString FUnrealSourceFile::GetFileId() const
{
	FString StdFilename = Filename;

	FPaths::MakeStandardFilename(StdFilename);

	FStringOutputDevice Out;

	// Standard filename will always start with "..\..\..\", so we are removing it.
	StdFilename = StdFilename.Mid(9);
	for (auto Char : StdFilename)
	{
		if (Char == 0)
		{
			break;
		}
		else if (FChar::IsAlnum(Char))
		{
			Out.AppendChar(Char);
		}
		else
		{
			Out.AppendChar('_');
		}
	}

	return Out;
}

FString FUnrealSourceFile::GetStrippedFilename() const
{
	return FPaths::GetBaseFilename(Filename);
}

FString FUnrealSourceFile::GetGeneratedMacroName(FClassMetaData* ClassData, const TCHAR* Suffix) const
{
	return GetGeneratedMacroName(ClassData->GetGeneratedBodyLine(), Suffix);
}

FString FUnrealSourceFile::GetGeneratedMacroName(int32 LineNumber, const TCHAR* Suffix) const
{
	if (Suffix != nullptr)
	{
		return FString::Printf(TEXT("%s_%d%s"), *GetFileId(), LineNumber, Suffix);
	}

	return FString::Printf(TEXT("%s_%d"), *GetFileId(), LineNumber);
}

FString FUnrealSourceFile::GetGeneratedBodyMacroName(int32 LineNumber, bool bLegacy) const
{
	return GetGeneratedMacroName(LineNumber, *FString::Printf(TEXT("%s%s"), TEXT("_GENERATED_BODY"), bLegacy ? TEXT("_LEGACY") : TEXT("")));
}

void FUnrealSourceFile::SetGeneratedFilename(FString GeneratedFilename)
{
	this->GeneratedFilename = MoveTemp(GeneratedFilename);
}

void FUnrealSourceFile::SetHasChanged(bool bHasChanged)
{
	this->bHasChanged = bHasChanged;
}

void FUnrealSourceFile::SetModuleRelativePath(FString ModuleRelativePath)
{
	this->ModuleRelativePath = MoveTemp(ModuleRelativePath);
}

void FUnrealSourceFile::SetIncludePath(FString IncludePath)
{
	this->IncludePath = MoveTemp(IncludePath);
}

const FString& FUnrealSourceFile::GetContent() const
{
	return Content;
}

void FUnrealSourceFile::MarkDependenciesResolved()
{
	bDependenciesResolved = true;
}

bool FUnrealSourceFile::AreDependenciesResolved() const
{
	return bDependenciesResolved;
}

void FUnrealSourceFile::MarkAsParsed()
{
	bParsed = true;
}

bool FUnrealSourceFile::IsParsed() const
{
	return bParsed;
}

bool FUnrealSourceFile::HasChanged() const
{
	return bHasChanged;
}
