// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "UnrealEd.h"
#include "Engine/BookMark.h"
#include "StaticMeshResources.h"
#include "EditorSupportDelegates.h"
#include "MouseDeltaTracker.h"
#include "ScopedTransaction.h"
#include "SurfaceIterators.h"
#include "SoundDefinitions.h"
#include "LevelEditor.h"
#include "Toolkits/ToolkitManager.h"
#include "EditorLevelUtils.h"
#include "DynamicMeshBuilder.h"

#include "ActorEditorUtils.h"
#include "EditorStyle.h"
#include "ComponentVisualizer.h"
#include "SNotificationList.h"
#include "NotificationManager.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Polys.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/LevelStreaming.h"

/*------------------------------------------------------------------------------
	FEditorModeTools.

	The master class that handles tracking of the current mode.
------------------------------------------------------------------------------*/

FEditorModeTools::FEditorModeTools()
	:	PivotShown( 0 )
	,	Snapping( 0 )
	,	SnappedActor( 0 )
	,	TranslateRotateXAxisAngle(0)
	,	DefaultID(FBuiltinEditorModes::EM_Default)
	,	WidgetMode( FWidget::WM_None )
	,	OverrideWidgetMode( FWidget::WM_None )
	,	bShowWidget( 1 )
	,	bHideViewportUI(false)
	,	CoordSystem(COORD_World)
	,	bIsTracking(false)
{
	// Load the last used settings
	LoadConfig();

	// Register our callback for actor selection changes
	USelection::SelectNoneEvent.AddRaw(this, &FEditorModeTools::OnEditorSelectNone);
	USelection::SelectionChangedEvent.AddRaw(this, &FEditorModeTools::OnEditorSelectionChanged);
	USelection::SelectObjectEvent.AddRaw(this, &FEditorModeTools::OnEditorSelectionChanged);

	if( GEditor )
	{
		// Register our callback for undo/redo
		GEditor->RegisterForUndo(this);
	}
}

FEditorModeTools::~FEditorModeTools()
{
	// Should we call Exit on any modes that are still active, or is it too late?
	USelection::SelectionChangedEvent.RemoveAll(this);
	USelection::SelectNoneEvent.RemoveAll(this);
	USelection::SelectObjectEvent.RemoveAll(this);

	GEditor->UnregisterForUndo(this);
}

/**
 * Loads the state that was saved in the INI file
 */
void FEditorModeTools::LoadConfig(void)
{
	GConfig->GetBool(TEXT("FEditorModeTools"),TEXT("ShowWidget"),bShowWidget,
		GEditorUserSettingsIni);

	const bool bGetRawValue = true;
	int32 Bogus = (int32)GetCoordSystem(bGetRawValue);
	GConfig->GetInt(TEXT("FEditorModeTools"),TEXT("CoordSystem"),Bogus,
		GEditorUserSettingsIni);
	SetCoordSystem((ECoordSystem)Bogus);


	LoadWidgetSettings();
}

/**
 * Saves the current state to the INI file
 */
void FEditorModeTools::SaveConfig(void)
{
	GConfig->SetBool(TEXT("FEditorModeTools"),TEXT("ShowWidget"),bShowWidget,
		GEditorUserSettingsIni);

	const bool bGetRawValue = true;
	GConfig->SetInt(TEXT("FEditorModeTools"),TEXT("CoordSystem"),(int32)GetCoordSystem(bGetRawValue),
		GEditorUserSettingsIni);

	SaveWidgetSettings();
}

TSharedPtr<class IToolkitHost> FEditorModeTools::GetToolkitHost() const
{
	TSharedPtr<class IToolkitHost> Result = ToolkitHost.Pin();
	check(ToolkitHost.IsValid());
	return Result;
}

void FEditorModeTools::SetToolkitHost(TSharedRef<class IToolkitHost> InHost)
{
	checkf(!ToolkitHost.IsValid(), TEXT("SetToolkitHost can only be called once"));
	ToolkitHost = InHost;
}

class USelection* FEditorModeTools::GetSelectedActors() const
{
	return GEditor->GetSelectedActors();
}

class USelection* FEditorModeTools::GetSelectedObjects() const
{
	return GEditor->GetSelectedObjects();
}

UWorld* FEditorModeTools::GetWorld() const
{
	return GEditor->GetEditorWorldContext().World();
}

void FEditorModeTools::OnEditorSelectionChanged(UObject* NewSelection)
{
	// If selecting an actor, move the pivot location.
	AActor* Actor = Cast<AActor>(NewSelection);
	if (Actor != NULL)
	{
		//@fixme - why isn't this using UObject::IsSelected()?
		if ( GEditor->GetSelectedActors()->IsSelected( Actor ) )
		{
			SetPivotLocation( Actor->GetActorLocation(), false );

			// If this actor wasn't part of the original selection set during pie/sie, clear it now
			if ( GEditor->ActorsThatWereSelected.Num() > 0 )
			{
				AActor* EditorActor = EditorUtilities::GetEditorWorldCounterpartActor( Actor );
				if ( !EditorActor || !GEditor->ActorsThatWereSelected.Contains(EditorActor) )
				{
					GEditor->ActorsThatWereSelected.Empty();
				}
			}
		}
		else if ( GEditor->ActorsThatWereSelected.Num() > 0 )
		{
			// Clear the selection set
			GEditor->ActorsThatWereSelected.Empty();
		}
	}

	for (const auto& Pair : FEditorModeRegistry::Get().GetFactoryMap())
	{
		Pair.Value->OnSelectionChanged(*this, NewSelection);
	}
}

void FEditorModeTools::OnEditorSelectNone()
{
	GEditor->SelectNone( false, true );
	GEditor->ActorsThatWereSelected.Empty();
}

/** 
 * Sets the pivot locations
 * 
 * @param Location 		The location to set
 * @param bIncGridBase	Whether or not to also set the GridBase
 */
void FEditorModeTools::SetPivotLocation( const FVector& Location, const bool bIncGridBase )
{
	CachedLocation = PivotLocation = SnappedLocation = Location;
	if ( bIncGridBase )
	{
		GridBase = Location;
	}
}

ECoordSystem FEditorModeTools::GetCoordSystem(bool bGetRawValue)
{
	if (!bGetRawValue && GetWidgetMode() == FWidget::WM_Scale )
	{
		return COORD_Local;
	}
	else
	{
		return CoordSystem;
	}
}

void FEditorModeTools::SetCoordSystem(ECoordSystem NewCoordSystem)
{
	CoordSystem = NewCoordSystem;
}

void FEditorModeTools::SetDefaultMode ( FEditorModeID InDefaultID )
{
	DefaultID = InDefaultID;
}

void FEditorModeTools::ActivateDefaultMode()
{
	ActivateMode( DefaultID );

	check( IsModeActive( DefaultID ) );
}

void FEditorModeTools::DeactivateModeAtIndex(int32 InIndex)
{
	check( InIndex >= 0 && InIndex < Modes.Num() );

	auto& Mode = Modes[InIndex];

	Mode->Exit();
	RecycledModes.Add( Mode->GetID(), Mode );
	Modes.RemoveAt( InIndex );
}

/**
 * Deactivates an editor mode. 
 * 
 * @param InID		The ID of the editor mode to deactivate.
 */
void FEditorModeTools::DeactivateMode( FEditorModeID InID )
{
	// Find the mode from the ID and exit it.
	for( int32 Index = Modes.Num() - 1; Index >= 0; --Index )
	{
		auto& Mode = Modes[Index];
		if( Mode->GetID() == InID )
		{
			DeactivateModeAtIndex(Index);
			break;
		}
	}

	if( Modes.Num() == 0 )
	{
		// Ensure the default mode is active if there are no active modes.
		ActivateDefaultMode();
	}
}

void FEditorModeTools::DeactivateAllModes()
{
	for( int32 Index = Modes.Num() - 1; Index >= 0; --Index )
	{
		DeactivateModeAtIndex(Index);
	}
}

void FEditorModeTools::DestroyMode( FEditorModeID InID )
{
	// Find the mode from the ID and exit it.
	for( int32 Index = Modes.Num() - 1; Index >= 0; --Index )
	{
		auto& Mode = Modes[Index];
		if ( Mode->GetID() == InID )
		{
			// Deactivate and destroy
			DeactivateModeAtIndex(Index);
			break;
		}
	}

	RecycledModes.Remove(InID);
}

/**
 * Activates an editor mode. Shuts down all other active modes which cannot run with the passed in mode.
 * 
 * @param InID		The ID of the editor mode to activate.
 * @param bToggle	true if the passed in editor mode should be toggled off if it is already active.
 */
void FEditorModeTools::ActivateMode( FEditorModeID InID, bool bToggle )
{
	if (InID == FBuiltinEditorModes::EM_Default)
	{
		InID = DefaultID;
	}

	// Check to see if the mode is already active
	if( IsModeActive(InID) )
	{
		// The mode is already active toggle it off if we should toggle off already active modes.
		if( bToggle )
		{
			DeactivateMode( InID );
		}
		// Nothing more to do
		return;
	}

	// Recycle a mode or factory a new one
	TSharedPtr<FEdMode> Mode = RecycledModes.FindRef( InID );

	if ( Mode.IsValid() )
	{
		RecycledModes.Remove( InID );
	}
	else
	{
		Mode = FEditorModeRegistry::Get().CreateMode( InID, *this );
	}

	if( !Mode.IsValid() )
	{
		UE_LOG(LogEditorModes, Log, TEXT("FEditorModeTools::ActivateMode : Couldn't find mode '%s'."), *InID.ToString() );
		// Just return and leave the mode list unmodified
		return;
	}

	// Remove anything that isn't compatible with this mode
	for( int32 ModeIndex = Modes.Num() - 1; ModeIndex >= 0; --ModeIndex )
	{
		const bool bModesAreCompatible = Mode->IsCompatibleWith( Modes[ModeIndex]->GetID() ) || Modes[ModeIndex]->IsCompatibleWith( Mode->GetID() );
		if ( !bModesAreCompatible )
		{
			DeactivateModeAtIndex(ModeIndex);
		}
	}

	Modes.Add( Mode );

	// Enter the new mode
	Mode->Enter();
	
	// Update the editor UI
	FEditorSupportDelegates::UpdateUI.Broadcast();
}

bool FEditorModeTools::EnsureNotInMode(FEditorModeID ModeID, const FText& ErrorMsg, bool bNotifyUser) const
{
	// We're in a 'safe' mode if we're not in the specified mode.
	const bool bInASafeMode = !IsModeActive(ModeID);
	if( !bInASafeMode && !ErrorMsg.IsEmpty() )
	{
		// Do we want to display this as a notification or a dialog to the user
		if ( bNotifyUser )
		{
			FNotificationInfo Info( ErrorMsg );
			FSlateNotificationManager::Get().AddNotification( Info );
		}
		else
		{
			FMessageDialog::Open( EAppMsgType::Ok, ErrorMsg );
		}		
	}
	return bInASafeMode;
}

FEdMode* FEditorModeTools::FindMode( FEditorModeID InID )
{
	for( auto& Mode : Modes )
	{
		if( Mode->GetID() == InID )
		{
			return Mode.Get();
		}
	}

	return NULL;
}

/**
 * Returns a coordinate system that should be applied on top of the worldspace system.
 */

FMatrix FEditorModeTools::GetCustomDrawingCoordinateSystem()
{
	FMatrix Matrix = FMatrix::Identity;

	switch (GetCoordSystem())
	{
		case COORD_Local:
		{
			// Let the current mode have a shot at setting the local coordinate system.
			// If it doesn't want to, create it by looking at the currently selected actors list.

			bool CustomCoordinateSystemProvided = false;
			for (const auto& Mode : Modes)
			{
				if (Mode->GetCustomDrawingCoordinateSystem(Matrix, nullptr))
				{
					CustomCoordinateSystemProvided = true;
					break;
				}
			}

			if (!CustomCoordinateSystemProvided)
			{
				const int32 Num = GetSelectedActors()->CountSelections<AActor>();

				// Coordinate system needs to come from the last actor selected
				if (Num > 0)
				{
					Matrix = FRotationMatrix(GetSelectedActors()->GetBottom<AActor>()->GetActorRotation());
				}
			}

			if (!Matrix.Equals(FMatrix::Identity))
			{
				Matrix.RemoveScaling();
			}
		}
		break;

		case COORD_World:
			break;

		default:
			break;
	}

	return Matrix;
}

FMatrix FEditorModeTools::GetCustomInputCoordinateSystem()
{
	return GetCustomDrawingCoordinateSystem();
}

/** Gets the widget axis to be drawn */
EAxisList::Type FEditorModeTools::GetWidgetAxisToDraw( FWidget::EWidgetMode InWidgetMode ) const
{
	EAxisList::Type OutAxis = EAxisList::All;
	for( int Index = Modes.Num() - 1; Index >= 0 ; Index-- )
	{
		if ( Modes[Index]->ShouldDrawWidget() )
		{
			OutAxis = Modes[Index]->GetWidgetAxisToDraw( InWidgetMode );
			break;
		}
	}

	return OutAxis;
}

/** Mouse tracking interface.  Passes tracking messages to all active modes */
bool FEditorModeTools::StartTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport)
{
	bIsTracking = true;
	bool bTransactionHandled = false;

	CachedLocation = PivotLocation;	// Cache the pivot location

	for ( const auto& Mode : Modes)
	{
		bTransactionHandled |= Mode->StartTracking(InViewportClient, InViewport);
	}

	return bTransactionHandled;
}

/** Mouse tracking interface.  Passes tracking messages to all active modes */
bool FEditorModeTools::EndTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport)
{
	bIsTracking = false;
	bool bTransactionHandled = false;

	for ( const auto& Mode : Modes)
	{
		bTransactionHandled |= Mode->EndTracking(InViewportClient, InViewportClient->Viewport);
	}

	CachedLocation = PivotLocation;	// Clear the pivot location
	
	return bTransactionHandled;
}

bool FEditorModeTools::AllowsViewportDragTool() const
{
	bool bCanUseDragTool = false;
	for (const TSharedPtr<FEdMode>& Mode : Modes)
	{
		bCanUseDragTool |= Mode->AllowsViewportDragTool();
	}
	return bCanUseDragTool;
}

/** Notifies all active modes that a map change has occured */
void FEditorModeTools::MapChangeNotify()
{
	for ( const auto& Mode : Modes)
	{
		Mode->MapChangeNotify();
	}
}


/** Notifies all active modes to empty their selections */
void FEditorModeTools::SelectNone()
{
	for ( const auto& Mode : Modes)
	{
		Mode->SelectNone();
	}
}

/** Notifies all active modes of box selection attempts */
bool FEditorModeTools::BoxSelect( FBox& InBox, bool InSelect )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->BoxSelect( InBox, InSelect );
	}
	return bHandled;
}

/** Notifies all active modes of frustum selection attempts */
bool FEditorModeTools::FrustumSelect( const FConvexVolume& InFrustum, bool InSelect )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->FrustumSelect( InFrustum, InSelect );
	}
	return bHandled;
}


/** true if any active mode uses a transform widget */
bool FEditorModeTools::UsesTransformWidget() const
{
	bool bUsesTransformWidget = false;
	for( const auto& Mode : Modes)
	{
		bUsesTransformWidget |= Mode->UsesTransformWidget();
	}

	return bUsesTransformWidget;
}

/** true if any active mode uses the passed in transform widget */
bool FEditorModeTools::UsesTransformWidget( FWidget::EWidgetMode CheckMode ) const
{
	bool bUsesTransformWidget = false;
	for( const auto& Mode : Modes)
	{
		bUsesTransformWidget |= Mode->UsesTransformWidget(CheckMode);
	}

	return bUsesTransformWidget;
}

/** Sets the current widget axis */
void FEditorModeTools::SetCurrentWidgetAxis( EAxisList::Type NewAxis )
{
	for( const auto& Mode : Modes)
	{
		Mode->SetCurrentWidgetAxis( NewAxis );
	}
}

/** Notifies all active modes of mouse click messages. */
bool FEditorModeTools::HandleClick(FEditorViewportClient* InViewportClient,  HHitProxy *HitProxy, const FViewportClick& Click )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->HandleClick(InViewportClient, HitProxy, Click);
	}

	return bHandled;
}

/** true if the passed in brush actor should be drawn in wireframe */	
bool FEditorModeTools::ShouldDrawBrushWireframe( AActor* InActor ) const
{
	bool bShouldDraw = false;
	for( const auto& Mode : Modes)
	{
		bShouldDraw |= Mode->ShouldDrawBrushWireframe( InActor );
	}

	if( Modes.Num() == 0 )
	{
		// We can get into a state where there are no active modes at editor startup if the builder brush is created before the default mode is activated.
		// Ensure we can see the builder brush when no modes are active.
		bShouldDraw = true;
	}
	return bShouldDraw;
}

/** true if brush vertices should be drawn */
bool FEditorModeTools::ShouldDrawBrushVertices() const
{
	// Currently only geometry mode being active prevents vertices from being drawn.
	return !IsModeActive( FBuiltinEditorModes::EM_Geometry );
}

/** Ticks all active modes */
void FEditorModeTools::Tick( FEditorViewportClient* ViewportClient, float DeltaTime )
{
	// Remove anything pending destruction
	for( int32 Index = Modes.Num() - 1; Index >= 0; --Index)
	{
		if (Modes[Index]->IsPendingDeletion())
		{
			DeactivateModeAtIndex(Index);
		}
	}
	
	if (Modes.Num() == 0)
	{
		// Ensure the default mode is active if there are no active modes.
		ActivateDefaultMode();
	}

	for( const auto& Mode : Modes)
	{
		Mode->Tick( ViewportClient, DeltaTime );
	}
}

/** Notifies all active modes of any change in mouse movement */
bool FEditorModeTools::InputDelta( FEditorViewportClient* InViewportClient,FViewport* InViewport,FVector& InDrag,FRotator& InRot,FVector& InScale )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->InputDelta( InViewportClient, InViewport, InDrag, InRot, InScale );
	}
	return bHandled;
}

/** Notifies all active modes of captured mouse movement */	
bool FEditorModeTools::CapturedMouseMove( FEditorViewportClient* InViewportClient, FViewport* InViewport, int32 InMouseX, int32 InMouseY )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->CapturedMouseMove( InViewportClient, InViewport, InMouseX, InMouseY );
	}
	return bHandled;
}

/** Notifies all active modes of keyboard input */
bool FEditorModeTools::InputKey(FEditorViewportClient* InViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	bool bHandled = false;
	for (const auto& Mode : Modes)
	{
		bHandled |= Mode->InputKey( InViewportClient, Viewport, Key, Event );
	}
	return bHandled;
}

/** Notifies all active modes of axis movement */
bool FEditorModeTools::InputAxis(FEditorViewportClient* InViewportClient, FViewport* Viewport, int32 ControllerId, FKey Key, float Delta, float DeltaTime)
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->InputAxis( InViewportClient, Viewport, ControllerId, Key, Delta, DeltaTime );
	}
	return bHandled;
}

bool FEditorModeTools::MouseEnter( FEditorViewportClient* InViewportClient, FViewport* Viewport, int32 X, int32 Y )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->MouseEnter( InViewportClient, Viewport, X, Y );
	}
	return bHandled;
}

bool FEditorModeTools::MouseLeave( FEditorViewportClient* InViewportClient, FViewport* Viewport )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->MouseLeave( InViewportClient, Viewport );
	}
	return bHandled;
}

/** Notifies all active modes that the mouse has moved */
bool FEditorModeTools::MouseMove( FEditorViewportClient* InViewportClient, FViewport* Viewport, int32 X, int32 Y )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->MouseMove( InViewportClient, Viewport, X, Y );
	}
	return bHandled;
}

bool FEditorModeTools::ReceivedFocus( FEditorViewportClient* InViewportClient, FViewport* Viewport )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->ReceivedFocus( InViewportClient, Viewport );
	}
	return bHandled;
}

bool FEditorModeTools::LostFocus( FEditorViewportClient* InViewportClient, FViewport* Viewport )
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->LostFocus( InViewportClient, Viewport );
	}
	return bHandled;
}

/** Draws all active mode components */	
void FEditorModeTools::DrawActiveModes( const FSceneView* InView, FPrimitiveDrawInterface* PDI )
{
	for( const auto& Mode : Modes)
	{
		Mode->Draw( InView, PDI );
	}
}

/** Renders all active modes */
void FEditorModeTools::Render( const FSceneView* InView, FViewport* Viewport, FPrimitiveDrawInterface* PDI )
{
	for( const auto& Mode : Modes)
	{
		Mode->Render( InView, Viewport, PDI );
	}
}

/** Draws the HUD for all active modes */
void FEditorModeTools::DrawHUD( FEditorViewportClient* InViewportClient,FViewport* Viewport, const FSceneView* View, FCanvas* Canvas )
{
	for( const auto& Mode : Modes)
	{
		Mode->DrawHUD( InViewportClient, Viewport, View, Canvas );
	}
}

/** Calls PostUndo on all active modes */
void FEditorModeTools::PostUndo(bool bSuccess)
{
	if (bSuccess)
	{
		for (const auto& Mode : Modes)
		{
			Mode->PostUndo();
		}
	}
}
void FEditorModeTools::PostRedo(bool bSuccess)
{
	PostUndo(bSuccess);
}

/** true if we should allow widget move */
bool FEditorModeTools::AllowWidgetMove() const
{
	bool bAllow = false;
	for( const auto& Mode : Modes)
	{
		bAllow |= Mode->AllowWidgetMove();
	}
	return bAllow;
}

bool FEditorModeTools::DisallowMouseDeltaTracking() const
{
	bool bDisallow = false;
	for( const auto& Mode : Modes)
	{
		bDisallow |= Mode->DisallowMouseDeltaTracking();
	}
	return bDisallow;
}

bool FEditorModeTools::GetCursor(EMouseCursor::Type& OutCursor) const
{
	bool bHandled = false;
	for( const auto& Mode : Modes)
	{
		bHandled |= Mode->GetCursor(OutCursor);
	}
	return bHandled;
}

/**
 * Used to cycle widget modes
 */
void FEditorModeTools::CycleWidgetMode (void)
{
	//make sure we're not currently tracking mouse movement.  If we are, changing modes could cause a crash due to referencing an axis/plane that is incompatible with the widget
	for(int32 ViewportIndex = 0;ViewportIndex < GEditor->LevelViewportClients.Num();ViewportIndex++)
	{
		FEditorViewportClient* ViewportClient = GEditor->LevelViewportClients[ ViewportIndex ];
		if (ViewportClient->IsTracking())
		{
			return;
		}
	}

	//only cycle when the mode is requesting the drawing of a widget
	if( GetShowWidget() )
	{
		const int32 CurrentWk = GetWidgetMode();
		int32 Wk = CurrentWk;
		do
		{
			Wk++;
			if ((Wk == FWidget::WM_TranslateRotateZ) && (!GetDefault<ULevelEditorViewportSettings>()->bAllowTranslateRotateZWidget))
			{
				Wk++;
			}
			// Roll back to the start if we go past FWidget::WM_Scale
			if( Wk >= FWidget::WM_Max)
			{
				Wk -= FWidget::WM_Max;
			}
		}
		while (!UsesTransformWidget((FWidget::EWidgetMode)Wk) && Wk != CurrentWk);
		SetWidgetMode( (FWidget::EWidgetMode)Wk );
		FEditorSupportDelegates::RedrawAllViewports.Broadcast();
	}
}

/**Save Widget Settings to Ini file*/
void FEditorModeTools::SaveWidgetSettings(void)
{
	GEditor->SaveEditorUserSettings();
}

/**Load Widget Settings from Ini file*/
void FEditorModeTools::LoadWidgetSettings(void)
{
}

/**
 * Returns a good location to draw the widget at.
 */

FVector FEditorModeTools::GetWidgetLocation() const
{
	for (int Index = Modes.Num() - 1; Index >= 0 ; Index--)
	{
		if ( Modes[Index]->UsesTransformWidget() )
		{
			 return Modes[Index]->GetWidgetLocation();
		}
	}
	
	return FVector(EForceInit::ForceInitToZero);
}

/**
 * Changes the current widget mode.
 */

void FEditorModeTools::SetWidgetMode( FWidget::EWidgetMode InWidgetMode )
{
	WidgetMode = InWidgetMode;
}

/**
 * Allows you to temporarily override the widget mode.  Call this function again
 * with FWidget::WM_None to turn off the override.
 */

void FEditorModeTools::SetWidgetModeOverride( FWidget::EWidgetMode InWidgetMode )
{
	OverrideWidgetMode = InWidgetMode;
}

/**
 * Retrieves the current widget mode, taking overrides into account.
 */

FWidget::EWidgetMode FEditorModeTools::GetWidgetMode() const
{
	if( OverrideWidgetMode != FWidget::WM_None )
	{
		return OverrideWidgetMode;
	}

	return WidgetMode;
}

bool FEditorModeTools::GetShowFriendlyVariableNames() const
{
	return GetDefault<UEditorStyleSettings>()->bShowFriendlyNames;
}

/**
 * Sets a bookmark in the levelinfo file, allocating it if necessary.
 */

void FEditorModeTools::SetBookmark( uint32 InIndex, FEditorViewportClient* InViewportClient )
{
	UWorld* World = InViewportClient->GetWorld();
	if ( World )
	{
		AWorldSettings* WorldSettings = World->GetWorldSettings();

		// Verify the index is valid for the bookmark
		if ( WorldSettings && InIndex < AWorldSettings::MAX_BOOKMARK_NUMBER )
		{
			// If the index doesn't already have a bookmark in place, create a new one
			if ( !WorldSettings->BookMarks[ InIndex ] )
			{
				WorldSettings->BookMarks[InIndex] = NewObject<UBookMark>(WorldSettings);
			}

			UBookMark* CurBookMark = WorldSettings->BookMarks[ InIndex ];
			check(CurBookMark);
			check(InViewportClient);

			// Use the rotation from the first perspective viewport can find.
			FRotator Rotation(0,0,0);
			if( !InViewportClient->IsOrtho() )
			{
				Rotation = InViewportClient->GetViewRotation();
			}

			CurBookMark->Location = InViewportClient->GetViewLocation();
			CurBookMark->Rotation = Rotation;

			// Keep a record of which levels were hidden so that we can restore these with the bookmark
			CurBookMark->HiddenLevels.Empty();
			for ( int32 LevelIndex = 0 ; LevelIndex < World->StreamingLevels.Num() ; ++LevelIndex )
			{
				ULevelStreaming* StreamingLevel = World->StreamingLevels[LevelIndex];
				if ( StreamingLevel )
				{
					if( !StreamingLevel->bShouldBeVisibleInEditor )
					{
						CurBookMark->HiddenLevels.Add( StreamingLevel->GetFullName() );
					}
				}
			}
		}
	}
}

/**
 * Checks to see if a bookmark exists at a given index
 */

bool FEditorModeTools::CheckBookmark( uint32 InIndex, FEditorViewportClient* InViewportClient )
{
	UWorld* World = InViewportClient->GetWorld();
	if ( World )
	{
		AWorldSettings* WorldSettings = World->GetWorldSettings();
		if ( WorldSettings && InIndex < AWorldSettings::MAX_BOOKMARK_NUMBER && WorldSettings->BookMarks[ InIndex ] )
		{
			return ( WorldSettings->BookMarks[ InIndex ] ? true : false );
		}
	}

	return false;
}

/**
 * Retrieves a bookmark from the list.
 */

void FEditorModeTools::JumpToBookmark( uint32 InIndex, bool bShouldRestoreLevelVisibility, FEditorViewportClient* InViewportClient )
{
	UWorld* World = InViewportClient->GetWorld();
	if ( World )
	{
		AWorldSettings* WorldSettings = World->GetWorldSettings();

		// Can only jump to a pre-existing bookmark
		if ( WorldSettings && InIndex < AWorldSettings::MAX_BOOKMARK_NUMBER && WorldSettings->BookMarks[ InIndex ] )
		{
			const UBookMark* CurBookMark = WorldSettings->BookMarks[ InIndex ];
			check(CurBookMark);

			// Set all level editing cameras to this bookmark
			for( int32 v = 0 ; v < GEditor->LevelViewportClients.Num() ; v++ )
			{
				GEditor->LevelViewportClients[v]->SetViewLocation( CurBookMark->Location );
				if( !GEditor->LevelViewportClients[v]->IsOrtho() )
				{
					GEditor->LevelViewportClients[v]->SetViewRotation( CurBookMark->Rotation );
				}
				GEditor->LevelViewportClients[v]->Invalidate();
			}
		}
	}
}


/**
 * Clears a bookmark
 */
void FEditorModeTools::ClearBookmark( uint32 InIndex, FEditorViewportClient* InViewportClient )
{
	UWorld* World = InViewportClient->GetWorld();
	if( World )
	{
		AWorldSettings* pWorldSettings = World->GetWorldSettings();

		// Verify the index is valid for the bookmark
		if ( pWorldSettings && InIndex < AWorldSettings::MAX_BOOKMARK_NUMBER )
		{
			pWorldSettings->BookMarks[ InIndex ] = NULL;
		}
	}
}

/**
* Clears all book marks
*/
void FEditorModeTools::ClearAllBookmarks( FEditorViewportClient* InViewportClient )
{
	for( int i = 0; i <  AWorldSettings::MAX_BOOKMARK_NUMBER; ++i )
	{
		ClearBookmark( i , InViewportClient );
	}
}

/**
 * Serializes the components for all modes.
 */

void FEditorModeTools::AddReferencedObjects( FReferenceCollector& Collector )
{
	for( int32 x = 0 ; x < Modes.Num() ; ++x )
	{
		Modes[x]->AddReferencedObjects( Collector );
	}
}

/**
 * Returns a pointer to an active mode specified by the passed in ID
 * If the editor mode is not active, NULL is returned
 */
FEdMode* FEditorModeTools::GetActiveMode( FEditorModeID InID )
{
	for( auto& Mode : Modes )
	{
		if( Mode->GetID() == InID )
		{
			return Mode.Get();
		}
	}
	return nullptr;
}

/**
 * Returns a pointer to an active mode specified by the passed in ID
 * If the editor mode is not active, NULL is returned
 */
const FEdMode* FEditorModeTools::GetActiveMode( FEditorModeID InID ) const
{
	for (const auto& Mode : Modes)
	{
		if (Mode->GetID() == InID)
		{
			return Mode.Get();
		}
	}

	return nullptr;
}

/**
 * Returns the active tool of the passed in editor mode.
 * If the passed in editor mode is not active or the mode has no active tool, NULL is returned
 */
const FModeTool* FEditorModeTools::GetActiveTool( FEditorModeID InID ) const
{
	const FEdMode* ActiveMode = GetActiveMode( InID );
	const FModeTool* Tool = NULL;
	if( ActiveMode )
	{
		Tool = ActiveMode->GetCurrentTool();
	}
	return Tool;
}

/** 
 * Returns true if the passed in editor mode is active 
 */
bool FEditorModeTools::IsModeActive( FEditorModeID InID ) const
{
	return GetActiveMode( InID ) != NULL;
}

/** 
 * Returns true if the default editor mode is active 
 */
bool FEditorModeTools::IsDefaultModeActive() const
{
	return IsModeActive(DefaultID);
}

/** 
 * Returns an array of all active modes
 */
void FEditorModeTools::GetActiveModes( TArray<FEdMode*>& OutActiveModes )
{
	OutActiveModes.Empty();
	// Copy into an array.  Do not let users modify the active list directly.
	for( auto& Mode : Modes)
	{
		OutActiveModes.Add(Mode.Get());
	}
}
