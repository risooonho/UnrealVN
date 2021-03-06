// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "EnginePrivate.h"
#include "Net/UnrealNetwork.h"
#include "GeneralProjectSettings.h"

FNetworkVersion::FGetLocalNetworkVersionOverride FNetworkVersion::GetLocalNetworkVersionOverride;
FNetworkVersion::FIsNetworkCompatibleOverride FNetworkVersion::IsNetworkCompatibleOverride;
const uint32 FNetworkVersion::InternalProtocolVersion = 2;

uint32 FNetworkVersion::GetLocalNetworkVersion()
{
	if ( GetLocalNetworkVersionOverride.IsBound() )
	{
		const uint32 LocalNetworkVersion = GetLocalNetworkVersionOverride.Execute();

		UE_LOG( LogNet, Log, TEXT( "GetLocalNetworkVersionOverride: LocalNetworkVersion: %i" ), LocalNetworkVersion );

		return LocalNetworkVersion;
	}

	// Get the project name (NOT case sensitive)
	const FString ProjectName( FString( FApp::GetGameName() ).ToLower() );

	// Get the project version string (IS case sensitive!)
	const FString& ProjectVersion = Cast<UGeneralProjectSettings>(UGeneralProjectSettings::StaticClass()->GetDefaultObject())->ProjectVersion;

	// Start with engine version as seed, and then hash with project name + project version
	const uint32 VersionHash = FCrc::StrCrc32( *ProjectVersion, FCrc::StrCrc32( *ProjectName, GEngineNetVersion ) );

	// Hash with internal protocol version
	const uint32 LocalNetworkVersion = FCrc::MemCrc32( &InternalProtocolVersion, sizeof( InternalProtocolVersion ), VersionHash );

	UE_LOG( LogNet, Log, TEXT( "GetLocalNetworkVersion: GEngineNetVersion: %i, ProjectName: %s, ProjectVersion: %s, InternalProtocolVersion: %i, LocalNetworkVersion: %i" ), GEngineNetVersion, *ProjectName, *ProjectVersion, InternalProtocolVersion, LocalNetworkVersion );

	return LocalNetworkVersion;
}

bool FNetworkVersion::IsNetworkCompatible( const uint32 LocalNetworkVersion, const uint32 RemoteNetworkVersion )
{
	if ( IsNetworkCompatibleOverride.IsBound() )
	{
		return IsNetworkCompatibleOverride.Execute( LocalNetworkVersion, RemoteNetworkVersion );
	}

	return LocalNetworkVersion == RemoteNetworkVersion;
}

// ----------------------------------------------------------------
