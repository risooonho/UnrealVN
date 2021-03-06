// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	InstancedFoliage.cpp: Instanced foliage implementation.
=============================================================================*/

#include "FoliagePrivate.h"
#include "InstancedFoliage.h"
#include "MessageLog.h"
#include "UObjectToken.h"
#include "MapErrors.h"
#include "Components/ModelComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Serialization/CustomVersion.h"
#include "ProceduralFoliageComponent.h"
#include "ProceduralFoliageBlockingVolume.h"
#include "ProceduralFoliageActor.h"

#define LOCTEXT_NAMESPACE "InstancedFoliage"

#define DO_FOLIAGE_CHECK			0			// whether to validate foliage data during editing.
#define FOLIAGE_CHECK_TRANSFORM		0			// whether to compare transforms between render and painting data.


DEFINE_LOG_CATEGORY_STATIC(LogInstancedFoliage, Log, All);


// Custom serialization version for all packages containing Instance Foliage
struct FFoliageCustomVersion
{
	enum Type
	{
		// Before any version changes were made in the plugin
		BeforeCustomVersionWasAdded = 0,
		// Converted to use HierarchicalInstancedStaticMeshComponent
		FoliageUsingHierarchicalISMC = 1,
		// Changed Component to not RF_Transactional
		HierarchicalISMCNonTransactional = 2,
		// Added FoliageTypeUpdateGuid
		AddedFoliageTypeUpdateGuid = 3,
		// Use a GUID to determine whic procedural actor spawned us
		ProceduralGuid = 4,
		// Support for cross-level bases 
		CrossLevelBase = 5,
		// FoliageType for details customization
		FoliageTypeCustomization = 6,
		// FoliageType for details customization continued
		FoliageTypeCustomizationScaling = 7,
		// -----<new versions can be added above this line>-------------------------------------------------
		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	// The GUID for this custom version number
	const static FGuid GUID;

private:
	FFoliageCustomVersion() {}
};

const FGuid FFoliageCustomVersion::GUID(0x430C4D19, 0x71544970, 0x87699B69, 0xDF90B0E5);
// Register the custom version with core
FCustomVersionRegistration GRegisterFoliageCustomVersion(FFoliageCustomVersion::GUID, FFoliageCustomVersion::LatestVersion, TEXT("FoliageVer"));


// Legacy (< FFoliageCustomVersion::CrossLevelBase) serializer
FArchive& operator<<(FArchive& Ar, FFoliageInstance_Deprecated& Instance)
{
	Ar << Instance.Base;
	Ar << Instance.Location;
	Ar << Instance.Rotation;
	Ar << Instance.DrawScale3D;

	if (Ar.CustomVer(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::FoliageUsingHierarchicalISMC)
	{
		int32 OldClusterIndex;
		Ar << OldClusterIndex;
		Ar << Instance.PreAlignRotation;
		Ar << Instance.Flags;

		if (OldClusterIndex == INDEX_NONE)
		{
			// When converting, we need to skip over any instance that was previously deleted but still in the Instances array.
			Instance.Flags |= FOLIAGE_InstanceDeleted;
		}
	}
	else
	{
		Ar << Instance.PreAlignRotation;
		Ar << Instance.Flags;
	}
	
	Ar << Instance.ZOffset;

#if WITH_EDITORONLY_DATA
	if (!Ar.ArIsFilterEditorOnly && Ar.CustomVer(FFoliageCustomVersion::GUID) >= FFoliageCustomVersion::ProceduralGuid)
	{
		Ar << Instance.ProceduralGuid;
	}
#endif

	return Ar;
}

//
// Serializers for struct data
//
FArchive& operator<<(FArchive& Ar, FFoliageInstance& Instance)
{
	Ar << Instance.Location;
	Ar << Instance.Rotation;
	Ar << Instance.DrawScale3D;
	Ar << Instance.PreAlignRotation;
	Ar << Instance.ProceduralGuid;
	Ar << Instance.Flags;
	Ar << Instance.ZOffset;
	Ar << Instance.BaseId;

	return Ar;
}

static void ConvertDeprecatedFoliageMeshes(
	AInstancedFoliageActor* IFA, 
	const TMap<UFoliageType*, TUniqueObj<FFoliageMeshInfo_Deprecated>>& FoliageMeshesDeprecated, 
	TMap<UFoliageType*, TUniqueObj<FFoliageMeshInfo>>& FoliageMeshes)
{
#if WITH_EDITORONLY_DATA	
	for (auto Pair : FoliageMeshesDeprecated)
	{
		auto& FoliageMesh = FoliageMeshes.Add(Pair.Key);
		const auto& FoliageMeshDeprecated = Pair.Value;
		
		FoliageMesh->Component = FoliageMeshDeprecated->Component;
		FoliageMesh->FoliageTypeUpdateGuid = FoliageMeshDeprecated->FoliageTypeUpdateGuid;

		FoliageMesh->Instances.Reserve(FoliageMeshDeprecated->Instances.Num());

		for (const FFoliageInstance_Deprecated& DeprecatedInstance : FoliageMeshDeprecated->Instances)
		{
			FFoliageInstance Instance;
			static_cast<FFoliageInstancePlacementInfo&>(Instance) = DeprecatedInstance;
			Instance.BaseId = IFA->InstanceBaseCache.AddInstanceBaseId(DeprecatedInstance.Base);
			Instance.ProceduralGuid = DeprecatedInstance.ProceduralGuid;

			FoliageMesh->Instances.Add(Instance);
		}
	}

	// there were no cross-level references before
	check(IFA->InstanceBaseCache.InstanceBaseLevelMap.Num() <= 1); 
	// populate WorldAsset->BasePtr map
	IFA->InstanceBaseCache.InstanceBaseLevelMap.Empty();
	auto& BaseList = IFA->InstanceBaseCache.InstanceBaseLevelMap.Add(TAssetPtr<UWorld>(Cast<UWorld>(IFA->GetLevel()->GetOuter())));
	for (auto& BaseInfoPair : IFA->InstanceBaseCache.InstanceBaseMap)
	{
		BaseList.Add(BaseInfoPair.Value.BasePtr);
	}
#endif//WITH_EDITORONLY_DATA	
}

/**
*	FFoliageInstanceCluster_Deprecated
*/
struct FFoliageInstanceCluster_Deprecated
{
	UInstancedStaticMeshComponent* ClusterComponent;
	FBoxSphereBounds Bounds;

#if WITH_EDITORONLY_DATA
	TArray<int32> InstanceIndices;	// index into editor editor Instances array
#endif

	friend FArchive& operator<<(FArchive& Ar, FFoliageInstanceCluster_Deprecated& OldCluster)
	{
		check(Ar.CustomVer(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::FoliageUsingHierarchicalISMC);

		Ar << OldCluster.Bounds;
		Ar << OldCluster.ClusterComponent;

#if WITH_EDITORONLY_DATA
		if (!Ar.ArIsFilterEditorOnly ||
			Ar.UE4Ver() < VER_UE4_FOLIAGE_SETTINGS_TYPE)
		{
			Ar << OldCluster.InstanceIndices;
		}
#endif

		return Ar;
	}
};

FArchive& operator<<(FArchive& Ar, FFoliageMeshInfo_Deprecated& MeshInfo)
{
	if (Ar.CustomVer(FFoliageCustomVersion::GUID) >= FFoliageCustomVersion::FoliageUsingHierarchicalISMC)
	{
		Ar << MeshInfo.Component;
	}
	else
	{
		TArray<FFoliageInstanceCluster_Deprecated> OldInstanceClusters;
		Ar << OldInstanceClusters;
	}
	
#if WITH_EDITORONLY_DATA
	if ((!Ar.ArIsFilterEditorOnly || Ar.UE4Ver() < VER_UE4_FOLIAGE_SETTINGS_TYPE) && 
		(!(Ar.GetPortFlags() & PPF_DuplicateForPIE)))
	{
		Ar << MeshInfo.Instances;
	}

	if (!Ar.ArIsFilterEditorOnly && Ar.CustomVer(FFoliageCustomVersion::GUID) >= FFoliageCustomVersion::AddedFoliageTypeUpdateGuid)
	{
		Ar << MeshInfo.FoliageTypeUpdateGuid;
	}
#endif

	return Ar;
}

FArchive& operator<<(FArchive& Ar, FFoliageMeshInfo& MeshInfo)
{
	Ar << MeshInfo.Component;

#if WITH_EDITORONLY_DATA
	if (!Ar.ArIsFilterEditorOnly && !(Ar.GetPortFlags() & PPF_DuplicateForPIE))
	{
		Ar << MeshInfo.Instances;
	}

	if (!Ar.ArIsFilterEditorOnly)
	{
		Ar << MeshInfo.FoliageTypeUpdateGuid;
	}
		
	// Serialize the transient data for undo.
	if (Ar.IsTransacting())
	{
		Ar << *MeshInfo.InstanceHash;
		Ar << MeshInfo.ComponentHash;
		Ar << MeshInfo.SelectedIndices;
	}
#endif

	return Ar;
}

//
// UFoliageType
//

UFoliageType::UFoliageType(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Density = 100.0f;
	Radius = 0.0f;
	AlignToNormal = true;
	RandomYaw = true;
	Scaling = EFoliageScaling::Uniform;
	ScaleX.Min = 1.0f;
	ScaleY.Min = 1.0f;
	ScaleZ.Min = 1.0f;
	ScaleX.Max = 1.0f;
	ScaleY.Max = 1.0f;
	ScaleZ.Max = 1.0f;
	AlignMaxAngle = 0.0f;
	RandomPitchAngle = 0.0f;
	GroundSlopeAngle.Min = 0.0f;
	GroundSlopeAngle.Max = 45.0f;
	Height.Min = -262144.0f;
	Height.Max = 262144.0f;
	ZOffset.Min = 0.0f;
	ZOffset.Max = 0.0f;
	CullDistance.Min = 0;
	CullDistance.Max = 0;
	MinimumLayerWeight = 0.5f;
	DisplayOrder = 0;
	IsSelected = false;
	ReapplyDensityAmount = 1.0f;
	CollisionWithWorld = false;
	CollisionScale = FVector(0.9f, 0.9f, 0.9f);
	VertexColorMask = FOLIAGEVERTEXCOLORMASK_Disabled;
	VertexColorMaskThreshold = 0.5f;

	bEnableStaticLighting = true;
	CastShadow = true;
	bCastDynamicShadow = true;
	bCastStaticShadow = true;
	bAffectDynamicIndirectLighting = false;
	// Most of the high instance count foliage like grass causes performance problems with distance field lighting
	bAffectDistanceFieldLighting = false;
	bCastShadowAsTwoSided = false;
	bReceivesDecals = false;

	bOverrideLightMapRes = false;
	OverriddenLightMapRes = 8;

	BodyInstance.SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	/** Ecosystem settings*/
	AverageSpreadDistance = 50;
	SpreadVariance = 150;
	bGrowsInShade = false;
	SeedsPerStep = 3;
	OverlapPriority = 0.f;
	NumSteps = 3;
	MinScale = 1.f;
	MaxScale = 3.f;
	ChangeCount = 0;
	InitialSeedDensity = 1.f;
	CollisionRadius = 100.f;
	ShadeRadius = 100.f;
	InitialMaxAge = 0.f;
	MaxAge = 10.f;

	FRichCurve* Curve = ScaleCurve.GetRichCurve();
	Curve->AddKey(0.f, 0.f);
	Curve->AddKey(1.f, 1.f);

	UpdateGuid = FGuid::NewGuid();

	// Deprecated since FFoliageCustomVersion::FoliageTypeCustomization
#if WITH_EDITORONLY_DATA
	ScaleMinX_DEPRECATED = 1.0f;
	ScaleMinY_DEPRECATED = 1.0f;
	ScaleMinZ_DEPRECATED = 1.0f;
	ScaleMaxX_DEPRECATED = 1.0f;
	ScaleMaxY_DEPRECATED = 1.0f;
	ScaleMaxZ_DEPRECATED = 1.0f;
	HeightMin_DEPRECATED = -262144.0f;
	HeightMax_DEPRECATED = 262144.0f;
	ZOffsetMin_DEPRECATED = 0.0f;
	ZOffsetMax_DEPRECATED = 0.0f;
	UniformScale_DEPRECATED = true;
	GroundSlope_DEPRECATED = 45.0f;
#endif// WITH_EDITORONLY_DATA
}

void UFoliageType::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FFoliageCustomVersion::GUID);

	if (LandscapeLayer_DEPRECATED != NAME_None && LandscapeLayers.Num() == 0)	//we now store an array of names so initialize the array with the old name
	{
		LandscapeLayers.Add(LandscapeLayer_DEPRECATED);
		LandscapeLayer_DEPRECATED = NAME_None;
	}

#if WITH_EDITORONLY_DATA
	if (Ar.IsLoading())
	{
		if (Ar.CustomVer(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::FoliageTypeCustomization)
		{
			ScaleX.Min = ScaleMinX_DEPRECATED;
			ScaleX.Max = ScaleMaxX_DEPRECATED;

			ScaleY.Min = ScaleMinY_DEPRECATED;
			ScaleY.Max = ScaleMaxY_DEPRECATED;

			ScaleZ.Min = ScaleMinZ_DEPRECATED;
			ScaleZ.Max = ScaleMaxZ_DEPRECATED;

			Height.Min = HeightMin_DEPRECATED;
			Height.Max = HeightMax_DEPRECATED;

			ZOffset.Min = ZOffsetMin_DEPRECATED;
			ZOffset.Max = ZOffsetMax_DEPRECATED;

			CullDistance.Min = StartCullDistance_DEPRECATED;
			CullDistance.Max = EndCullDistance_DEPRECATED;
		}
		
		if (Ar.CustomVer(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::FoliageTypeCustomizationScaling)
		{
			Scaling = UniformScale_DEPRECATED ? EFoliageScaling::Uniform : EFoliageScaling::Free;
			
			GroundSlopeAngle.Min = MinGroundSlope_DEPRECATED;
			GroundSlopeAngle.Max = GroundSlope_DEPRECATED;
		}
	}
#endif// WITH_EDITORONLY_DATA
}

FVector UFoliageType::GetRandomScale() const
{
	FVector Result(1.0f);
	float LockRand = 0.0f;

	switch (Scaling)
	{
	case EFoliageScaling::Uniform:
		Result.X = ScaleX.Interpolate(FMath::FRand());
		Result.Y = Result.X;
		Result.Z = Result.X;
		break;
	
	case EFoliageScaling::Free:
		Result.X = ScaleX.Interpolate(FMath::FRand());
		Result.Y = ScaleY.Interpolate(FMath::FRand());
		Result.Z = ScaleZ.Interpolate(FMath::FRand());
		break;
	
	case EFoliageScaling::LockXY:
		LockRand = FMath::FRand();
		Result.X = ScaleX.Interpolate(LockRand);
		Result.Y = ScaleY.Interpolate(LockRand);
		Result.Z = ScaleZ.Interpolate(FMath::FRand());
		break;
	
	case EFoliageScaling::LockXZ:
		LockRand = FMath::FRand();
		Result.X = ScaleX.Interpolate(LockRand);
		Result.Y = ScaleY.Interpolate(FMath::FRand());
		Result.Z = ScaleZ.Interpolate(LockRand);
	
	case EFoliageScaling::LockYZ:
		LockRand = FMath::FRand();
		Result.X = ScaleX.Interpolate(FMath::FRand());
		Result.Y = ScaleY.Interpolate(LockRand);
		Result.Z = ScaleZ.Interpolate(LockRand);
	}
	
	return Result;
}

UFoliageType_InstancedStaticMesh::UFoliageType_InstancedStaticMesh(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Mesh = nullptr;
}


float UFoliageType::GetMaxRadius() const
{
	return FMath::Max(CollisionRadius, ShadeRadius);
}

float UFoliageType::GetScaleForAge(const float Age) const
{
	const FRichCurve* Curve = ScaleCurve.GetRichCurveConst();
	const float Time = FMath::Clamp(MaxAge == 0 ? 1.f : Age / MaxAge, 0.f, 1.f);
	const float Scale = Curve->Eval(Time);
	return MinScale + (MaxScale - MinScale) * Scale;
}

float UFoliageType::GetInitAge(FRandomStream& RandomStream) const
{
	return RandomStream.FRandRange(0, InitialMaxAge);
}

float UFoliageType::GetNextAge(const float CurrentAge, const int32 NumSteps) const
{
	float NewAge = CurrentAge;
	for (int32 Count = 0; Count < NumSteps; ++Count)
	{
		const float GrowAge = NewAge + 1;
		if (GrowAge <= MaxAge)
		{
			NewAge = GrowAge;
		}
		else
		{
			break;
		}
	}

	return NewAge;
}

#if WITH_EDITOR
void UFoliageType::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Ensure that OverriddenLightMapRes is a factor of 4
	OverriddenLightMapRes = OverriddenLightMapRes > 4 ? OverriddenLightMapRes + 3 & ~3 : 4;
	++ChangeCount;

	UpdateGuid = FGuid::NewGuid();

	// Notify any currently-loaded InstancedFoliageActors
	if (IsFoliageReallocationRequiredForPropertyChange(PropertyChangedEvent))
	{
		for (TObjectIterator<AInstancedFoliageActor> It(RF_ClassDefaultObject | RF_PendingKill); It; ++It)
		{
			It->NotifyFoliageTypeChanged(this);
		}
	}
}
#endif

//
// FFoliageMeshInfo
//
FFoliageMeshInfo::FFoliageMeshInfo()
	: Component(nullptr)
#if WITH_EDITOR
	, InstanceHash(GIsEditor ? new FFoliageInstanceHash() : nullptr)
#endif
{ }


#if WITH_EDITOR

void FFoliageMeshInfo::CheckValid()
{
#if DO_FOLIAGE_CHECK
	int32 ClusterTotal = 0;
	int32 ComponentTotal = 0;

	for (FFoliageInstanceCluster& Cluster : InstanceClusters)
	{
		check(Cluster.ClusterComponent != nullptr);
		ClusterTotal += Cluster.InstanceIndices.Num();
		ComponentTotal += Cluster.ClusterComponent->PerInstanceSMData.Num();
	}

	check(ClusterTotal == ComponentTotal);

	int32 FreeTotal = 0;
	int32 InstanceTotal = 0;
	for (int32 InstanceIdx = 0; InstanceIdx < Instances.Num(); InstanceIdx++)
	{
		if (Instances[InstanceIdx].ClusterIndex != -1)
		{
			InstanceTotal++;
		}
		else
		{
			FreeTotal++;
		}
	}

	check( ClusterTotal == InstanceTotal );
	check( FreeInstanceIndices.Num() == FreeTotal );

	InstanceHash->CheckInstanceCount(InstanceTotal);

	int32 ComponentHashTotal = 0;
	for (const auto& Pair : ComponentHash)
	{
		ComponentHashTotal += Pair.Value().Num();
	}
	check( ComponentHashTotal == InstanceTotal);

#if FOLIAGE_CHECK_TRANSFORM
	// Check transforms match up with editor data
	int32 MismatchCount = 0;
	for( int32 ClusterIdx=0;ClusterIdx<InstanceClusters.Num();ClusterIdx++ )
	{
		TArray<int32> Indices = InstanceClusters(ClusterIdx).InstanceIndices;
		UInstancedStaticMeshComponent* Comp = InstanceClusters(ClusterIdx).ClusterComponent;
		for( int32 InstIdx=0;InstIdx<Indices.Num();InstIdx++ )
		{
			int32 InstanceIdx = Indices(InstIdx);

			FTransform InstanceToWorldEd = Instances(InstanceIdx).GetInstanceTransform();
			FTransform InstanceToWorldCluster = Comp->PerInstanceSMData(InstIdx).Transform * Comp->GetComponentToWorld();

			if( !InstanceToWorldEd.Equals(InstanceToWorldCluster) )
			{
				Comp->PerInstanceSMData(InstIdx).Transform = InstanceToWorldEd.ToMatrixWithScale();
				MismatchCount++;
			}
		}
	}

	if( MismatchCount != 0 )
	{
		UE_LOG(LogInstancedFoliage, Log, TEXT("%s: transform mismatch: %d"), *InstanceClusters(0).ClusterComponent->StaticMesh->GetName(), MismatchCount);
	}
#endif

#endif
}

void FFoliageMeshInfo::UpdateComponentSettings(const UFoliageType* InSettings)
{
	if (Component)
	{
		Component->Mobility = InSettings->bEnableStaticLighting ? EComponentMobility::Static : EComponentMobility::Movable;
		Component->InstanceStartCullDistance = InSettings->CullDistance.Min;
		Component->InstanceEndCullDistance = InSettings->CullDistance.Max;

		Component->CastShadow = InSettings->CastShadow;
		Component->bCastDynamicShadow = InSettings->bCastDynamicShadow;
		Component->bCastStaticShadow = InSettings->bCastStaticShadow;
		Component->bAffectDynamicIndirectLighting = InSettings->bAffectDynamicIndirectLighting;
		Component->bAffectDistanceFieldLighting = InSettings->bAffectDistanceFieldLighting;
		Component->bCastShadowAsTwoSided = InSettings->bCastShadowAsTwoSided;
		Component->bReceivesDecals = InSettings->bReceivesDecals;
		Component->bOverrideLightMapRes = InSettings->bOverrideLightMapRes;
		Component->OverriddenLightMapRes = InSettings->OverriddenLightMapRes;

		Component->BodyInstance.CopyBodyInstancePropertiesFrom(&InSettings->BodyInstance);
	}
}

void FFoliageMeshInfo::AddInstance(AInstancedFoliageActor* InIFA, const UFoliageType* InSettings, const FFoliageInstance& InNewInstance, UActorComponent* InBaseComponent)
{
	FFoliageInstance Instance = InNewInstance;
	Instance.BaseId = InIFA->InstanceBaseCache.AddInstanceBaseId(InBaseComponent);
	AddInstance(InIFA, InSettings, Instance);
}

void FFoliageMeshInfo::AddInstance(AInstancedFoliageActor* InIFA, const UFoliageType* InSettings, const FFoliageInstance& InNewInstance)
{
	InIFA->Modify();

	if (Component == nullptr)
	{
		Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(InIFA, NAME_None, RF_Transactional);

		Component->StaticMesh = InSettings->GetStaticMesh();
		Component->bSelectable = true;
		Component->bHasPerInstanceHitProxies = true;
		Component->InstancingRandomSeed = FMath::Rand();

		UpdateComponentSettings(InSettings);

		Component->AttachTo(InIFA->GetRootComponent());
		
		if (InIFA->GetRootComponent()->IsRegistered())
		{
			Component->RegisterComponent();
		}

		// Use only instance translation as a component transform
		Component->SetWorldTransform(InIFA->GetRootComponent()->ComponentToWorld);

		// Add the new component to the transaction buffer so it will get destroyed on undo
		Component->Modify();
		// We don't want to track changes to instances later so we mark it as non-transactional
		Component->ClearFlags(RF_Transactional);
	}
	else
	{
		Component->InvalidateLightingCache();
	}

	// Add the instance taking either a free slot or adding a new item.
	int32 InstanceIndex = Instances.Add(InNewInstance);
	FFoliageInstance& AddedInstance = Instances[InstanceIndex];

	// Add the instance to the hash
	AddToBaseHash(InstanceIndex);
	InstanceHash->InsertInstance(AddedInstance.Location, InstanceIndex);
	// Calculate transform for the instance
	FTransform InstanceToWorld = InNewInstance.GetInstanceWorldTransform();

	// Add the instance to the component
	Component->AddInstanceWorldSpace(InstanceToWorld);

	// Update PrimitiveComponent's culling distance taking into account the radius of the bounds, as
	// it is based on the center of the component's bounds.
//	float CullDistance = InSettings->EndCullDistance > 0 ? (float)InSettings->EndCullDistance + BestCluster->Bounds.SphereRadius : 0.f;
//	Component->LDMaxDrawDistance = CullDistance;
//	Component->CachedMaxDrawDistance = CullDistance;

	CheckValid();
}

void FFoliageMeshInfo::RemoveInstances(AInstancedFoliageActor* InIFA, const TArray<int32>& InInstancesToRemove)
{
	if (InInstancesToRemove.Num())
	{
		check(Component);
		InIFA->Modify();

		TSet<int32> InstancesToRemove;
		for (int32 Instance : InInstancesToRemove)
		{
			InstancesToRemove.Add(Instance);
		}

		while(InstancesToRemove.Num())
		{
			// Get an item from the set for processing
			auto It = InstancesToRemove.CreateConstIterator();
			int32 InstanceIndex = *It;		
			int32 InstanceIndexToRemove = InstanceIndex;

			FFoliageInstance& Instance = Instances[InstanceIndex];

			// remove from hash
			RemoveFromBaseHash(InstanceIndex);
			InstanceHash->RemoveInstance(Instance.Location, InstanceIndex);

			// remove from the component
			Component->RemoveInstance(InstanceIndex);

			// Remove it from the selection.
			SelectedIndices.Remove(InstanceIndex);

			// remove from instances array
			Instances.RemoveAtSwap(InstanceIndex);

			// update hashes for swapped instance
			if (InstanceIndex != Instances.Num() && Instances.Num())
			{
				// Instance hash
				FFoliageInstance& SwappedInstance = Instances[InstanceIndex];
				InstanceHash->RemoveInstance(SwappedInstance.Location, Instances.Num());
				InstanceHash->InsertInstance(SwappedInstance.Location, InstanceIndex);

				// Component hash
				auto* InstanceSet = ComponentHash.Find(SwappedInstance.BaseId);
				if (InstanceSet)
				{
					InstanceSet->Remove(Instances.Num());
					InstanceSet->Add(InstanceIndex);
				}

				// Selection
				if (SelectedIndices.Contains(Instances.Num()))
				{
					SelectedIndices.Remove(Instances.Num());
					SelectedIndices.Add(InstanceIndex);
				}

				// Removal list
				if (InstancesToRemove.Contains(Instances.Num()))
				{
					// The item from the end of the array that we swapped in to InstanceIndex is also on the list to remove.
					// Remove the item at the end of the array and leave InstanceIndex in the removal list.
					InstanceIndexToRemove = Instances.Num();
				}
			}

			// Remove the removed item from the removal list
			InstancesToRemove.Remove(InstanceIndexToRemove);
		}
			
		CheckValid();
	}
}

void FFoliageMeshInfo::PreMoveInstances(AInstancedFoliageActor* InIFA, const TArray<int32>& InInstancesToMove)
{
	// Remove instances from the hash
	for (TArray<int32>::TConstIterator It(InInstancesToMove); It; ++It)
	{
		int32 InstanceIndex = *It;
		const FFoliageInstance& Instance = Instances[InstanceIndex];
		InstanceHash->RemoveInstance(Instance.Location, InstanceIndex);
	}
}


void FFoliageMeshInfo::PostUpdateInstances(AInstancedFoliageActor* InIFA, const TArray<int32>& InInstancesUpdated, bool bReAddToHash)
{
	if (InInstancesUpdated.Num())
	{
		check(Component);

		for (TArray<int32>::TConstIterator It(InInstancesUpdated); It; ++It)
		{
			int32 InstanceIndex = *It;
			const FFoliageInstance& Instance = Instances[InstanceIndex];

			FTransform InstanceToWorld = Instance.GetInstanceWorldTransform();

			Component->UpdateInstanceTransform(InstanceIndex, InstanceToWorld, true);

			// Re-add instance to the hash if requested
			if (bReAddToHash)
			{
				InstanceHash->InsertInstance(Instance.Location, InstanceIndex);
			}
		}

		Component->InvalidateLightingCache();
		Component->MarkRenderStateDirty();
	}
}

void FFoliageMeshInfo::PostMoveInstances(AInstancedFoliageActor* InIFA, const TArray<int32>& InInstancesMoved)
{
	PostUpdateInstances(InIFA, InInstancesMoved, true);
}

void FFoliageMeshInfo::DuplicateInstances(AInstancedFoliageActor* InIFA, UFoliageType* InSettings, const TArray<int32>& InInstancesToDuplicate)
{
	for (int32 InstanceIndex : InInstancesToDuplicate)
	{
		const FFoliageInstance TempInstance = Instances[InstanceIndex];
		AddInstance(InIFA, InSettings, TempInstance);
	}
}

/* Get the number of placed instances */
int32 FFoliageMeshInfo::GetInstanceCount() const
{
	return Instances.Num();
}

void FFoliageMeshInfo::AddToBaseHash(int32 InstanceIndex)
{
	FFoliageInstance& Instance = Instances[InstanceIndex];
	ComponentHash.FindOrAdd(Instance.BaseId).Add(InstanceIndex);
}

void FFoliageMeshInfo::RemoveFromBaseHash(int32 InstanceIndex)
{
	FFoliageInstance& Instance = Instances[InstanceIndex];

	// Remove current base link
	auto* InstanceSet = ComponentHash.Find(Instance.BaseId);
	if (InstanceSet)
	{
		InstanceSet->Remove(InstanceIndex);
		if (InstanceSet->Num() == 0)
		{
			// Remove the component from the component hash if this is the last instance.
			ComponentHash.Remove(Instance.BaseId);
		}
	}
}

// Destroy existing clusters and reassign all instances to new clusters
void FFoliageMeshInfo::ReallocateClusters(AInstancedFoliageActor* InIFA, UFoliageType* InSettings)
{
	if (Component != nullptr)
	{
		Component->UnregisterComponent();
		Component->bAutoRegister = false;
		Component = nullptr;
	}

	// Remove everything
	TArray<FFoliageInstance> OldInstances;
	Exchange(Instances, OldInstances);
	InstanceHash->Empty();
	ComponentHash.Empty();
	SelectedIndices.Empty();

	// Copy the UpdateGuid from the foliage type
	FoliageTypeUpdateGuid = InSettings->UpdateGuid;

	// Re-add
	for (FFoliageInstance& Instance : OldInstances)
	{
		if ((Instance.Flags & FOLIAGE_InstanceDeleted) == 0)
		{
			AddInstance(InIFA, InSettings, Instance);
		}
	}
}

void FFoliageMeshInfo::ReapplyInstancesToComponent()
{
	if (Component)
	{
		Component->UnregisterComponent();
		Component->ClearInstances();

		for (auto& Instance : Instances)
		{
			Component->AddInstanceWorldSpace(Instance.GetInstanceWorldTransform());
		}

		if (SelectedIndices.Num())
		{
			if (Component->SelectedInstances.Num() != Component->PerInstanceSMData.Num())
			{
				Component->SelectedInstances.Init(false, Component->PerInstanceSMData.Num());
			}
			for (int32 i : SelectedIndices)
			{
				Component->SelectedInstances[i] = true;
			}
		}

		Component->RegisterComponent();
	}
}


// Update settings in the clusters based on the current settings (eg culling distance, collision, ...)
//void FFoliageMeshInfo::UpdateClusterSettings(AInstancedFoliageActor* InIFA)
//{
//	for (FFoliageInstanceCluster& Cluster : InstanceClusters)
//	{
//		UInstancedStaticMeshComponent* ClusterComponent = Cluster.ClusterComponent;
//		ClusterComponent->MarkRenderStateDirty();
//
//		// Copy settings
//		ClusterComponent->InstanceStartCullDistance = Settings->StartCullDistance;
//		ClusterComponent->InstanceEndCullDistance = Settings->EndCullDistance;
//
//		// Update PrimitiveComponent's culling distance taking into account the radius of the bounds, as
//		// it is based on the center of the component's bounds.
//		float CullDistance = Settings->EndCullDistance > 0 ? (float)Settings->EndCullDistance + Cluster.Bounds.SphereRadius : 0.f;
//		ClusterComponent->LDMaxDrawDistance = CullDistance;
//		ClusterComponent->CachedMaxDrawDistance = CullDistance;
//	}
//
//	InIFA->MarkComponentsRenderStateDirty();
//}

void FFoliageMeshInfo::GetInstancesInsideSphere(const FSphere& Sphere, TArray<int32>& OutInstances)
{
	auto TempInstances = InstanceHash->GetInstancesOverlappingBox(FBox::BuildAABB(Sphere.Center, FVector(Sphere.W)));
	for (int32 Idx : TempInstances)
	{
		if (FSphere(Instances[Idx].Location, 0.f).IsInside(Sphere))
		{
			OutInstances.Add(Idx);
		}
	}
}

// Returns whether or not there is are any instances overlapping the sphere specified
bool FFoliageMeshInfo::CheckForOverlappingSphere(const FSphere& Sphere)
{
	auto TempInstances = InstanceHash->GetInstancesOverlappingBox(FBox::BuildAABB(Sphere.Center, FVector(Sphere.W)));
	for (int32 Idx : TempInstances)
	{
		if (FSphere(Instances[Idx].Location, 0.f).IsInside(Sphere))
		{
			return true;
		}
	}
	return false;
}

// Returns whether or not there is are any instances overlapping the instance specified, excluding the set of instances provided
bool FFoliageMeshInfo::CheckForOverlappingInstanceExcluding(int32 TestInstanceIdx, float Radius, TSet<int32>& ExcludeInstances)
{
	FSphere Sphere(Instances[TestInstanceIdx].Location, Radius);

	auto TempInstances = InstanceHash->GetInstancesOverlappingBox(FBox::BuildAABB(Sphere.Center, FVector(Sphere.W)));
	for (int32 Idx : TempInstances)
	{
		if (Idx != TestInstanceIdx && !ExcludeInstances.Contains(Idx) && FSphere(Instances[Idx].Location, 0.f).IsInside(Sphere))
		{
			return true;
		}
	}
	return false;
}

void FFoliageMeshInfo::SelectInstances(AInstancedFoliageActor* InIFA, bool bSelect)
{
	if (Component)
	{
		InIFA->Modify();
	
		if (bSelect)
		{
			for (int32 i = 0; i < Component->PerInstanceSMData.Num(); ++i)
			{
				SelectedIndices.Add(i);
			}
		}
		else
		{
			SelectedIndices.Empty();
		}

		// Apply selections to the component
		Component->SelectedInstances.Init(bSelect, Component->PerInstanceSMData.Num());
		Component->ReleasePerInstanceRenderData();
		Component->MarkRenderStateDirty();
	}
}

void FFoliageMeshInfo::SelectInstances(AInstancedFoliageActor* InIFA, bool bSelect, TArray<int32>& InInstances)
{
	if (InInstances.Num())
	{
		check(Component);
		InIFA->Modify();

		if (bSelect)
		{
			// Apply selections to the component
			Component->ReleasePerInstanceRenderData();
			Component->MarkRenderStateDirty();

			if (Component->SelectedInstances.Num() != Component->PerInstanceSMData.Num())
			{
				Component->SelectedInstances.Init(false, Component->PerInstanceSMData.Num());
			}

			for (int32 i : InInstances)
			{
				SelectedIndices.Add(i);
				Component->SelectedInstances[i] = true;
			}
		}
		else
		{
			if (Component->SelectedInstances.Num())
			{
				Component->ReleasePerInstanceRenderData();
				Component->MarkRenderStateDirty();

				for (int32 i : InInstances)
				{
					SelectedIndices.Remove(i);
					Component->SelectedInstances[i] = false;
				}
			}
		}
	}
}

#endif	//WITH_EDITOR

//
// AInstancedFoliageActor
//
AInstancedFoliageActor::AInstancedFoliageActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	USceneComponent* SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent0"));
	RootComponent = SceneComponent;
	RootComponent->Mobility = EComponentMobility::Static;
	
	SetActorEnableCollision(true);
#if WITH_EDITORONLY_DATA
	bListedInSceneOutliner = false;
#endif // WITH_EDITORONLY_DATA
	PrimaryActorTick.bCanEverTick = false;
}

AInstancedFoliageActor* AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(UWorld* InWorld, bool bCreateIfNone)
{
	return GetInstancedFoliageActorForLevel(InWorld->GetCurrentLevel(), bCreateIfNone);
}

AInstancedFoliageActor* AInstancedFoliageActor::GetInstancedFoliageActorForLevel(ULevel* InLevel, bool bCreateIfNone /* = false */)
{
	AInstancedFoliageActor* IFA = nullptr;
	if (InLevel)
	{
		IFA = InLevel->InstancedFoliageActor.Get();
		
		if (!IFA && bCreateIfNone)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.OverrideLevel = InLevel;
			IFA = InLevel->GetWorld()->SpawnActor<AInstancedFoliageActor>(SpawnParams);
			InLevel->InstancedFoliageActor = IFA;
		}
	}

	return IFA;
}


int32 AInstancedFoliageActor::GetOverlappingSphereCount(const UFoliageType* FoliageType, const FSphere& Sphere) const
{
	if (const FFoliageMeshInfo* MeshInfo = FindMesh(FoliageType))
	{
		if (MeshInfo->Component)
		{
			return MeshInfo->Component->GetOverlappingSphereCount(Sphere);
		}
	}

	return 0;
}


int32 AInstancedFoliageActor::GetOverlappingBoxCount(const UFoliageType* FoliageType, const FBox& Box) const
{
	if(const FFoliageMeshInfo* MeshInfo = FindMesh(FoliageType))
	{
		if(MeshInfo->Component)
		{
			return MeshInfo->Component->GetOverlappingBoxCount(Box);
		}
	}

	return 0;
}


void AInstancedFoliageActor::GetOverlappingBoxTransforms(const UFoliageType* FoliageType, const FBox& Box, TArray<FTransform>& OutTransforms) const
{
	if(const FFoliageMeshInfo* MeshInfo = FindMesh(FoliageType))
	{
		if(MeshInfo->Component)
		{
			MeshInfo->Component->GetOverlappingBoxTransforms(Box, OutTransforms);
		}
	}
}


UFoliageType* AInstancedFoliageActor::GetSettingsForMesh(const UStaticMesh* InMesh, FFoliageMeshInfo** OutMeshInfo)
{
	UFoliageType* Type = nullptr;
	FFoliageMeshInfo* MeshInfo = nullptr;

	for (auto& MeshPair : FoliageMeshes)
	{
		UFoliageType* Settings = MeshPair.Key;
		if (Settings && Settings->GetStaticMesh() == InMesh)
		{
			Type = MeshPair.Key;
			MeshInfo = &*MeshPair.Value;
			break;
		}
	}

	if (OutMeshInfo)
	{
		*OutMeshInfo = MeshInfo;
	}
	return Type;
}


FFoliageMeshInfo* AInstancedFoliageActor::FindMesh(const UFoliageType* InType)
{
	TUniqueObj<FFoliageMeshInfo>* MeshInfoEntry = FoliageMeshes.Find(InType);
	FFoliageMeshInfo* MeshInfo = MeshInfoEntry ? &MeshInfoEntry->Get() : nullptr;
	return MeshInfo;
}

const FFoliageMeshInfo* AInstancedFoliageActor::FindMesh(const UFoliageType* InType) const
{
	const TUniqueObj<FFoliageMeshInfo>* MeshInfoEntry = FoliageMeshes.Find(InType);
	const FFoliageMeshInfo* MeshInfo = MeshInfoEntry ? &MeshInfoEntry->Get() : nullptr;
	return MeshInfo;
}


#if WITH_EDITOR
void AInstancedFoliageActor::MoveInstancesForMovedComponent(UActorComponent* InComponent)
{
	const auto BaseId = InstanceBaseCache.GetInstanceBaseId(InComponent);
	if (BaseId == FFoliageInstanceBaseCache::InvalidBaseId)
	{
		return;
	}

	bool bUpdatedInstances = false;
	bool bFirst = true;

	const auto OldBaseInfo = InstanceBaseCache.GetInstanceBaseInfo(BaseId);
	const auto NewBaseInfo = InstanceBaseCache.UpdateInstanceBaseInfoTransform(InComponent);

	FMatrix DeltaTransfrom = 
		FTranslationMatrix(-OldBaseInfo.CachedLocation) *
		FInverseRotationMatrix(OldBaseInfo.CachedRotation) *
		FScaleMatrix(NewBaseInfo.CachedDrawScale / OldBaseInfo.CachedDrawScale) *
		FRotationMatrix(NewBaseInfo.CachedRotation) *
		FTranslationMatrix(NewBaseInfo.CachedLocation);

	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		const auto* InstanceSet = MeshInfo.ComponentHash.Find(BaseId);
		if (InstanceSet && InstanceSet->Num())
		{
			if (bFirst)
			{
				bFirst = false;
				Modify();
			}
					
			for (int32 InstanceIndex : *InstanceSet)
			{
				FFoliageInstance& Instance = MeshInfo.Instances[InstanceIndex];

				MeshInfo.InstanceHash->RemoveInstance(Instance.Location, InstanceIndex);

				// Apply change
				FMatrix NewTransform =
					FRotationMatrix(Instance.Rotation) *
					FTranslationMatrix(Instance.Location) *
					DeltaTransfrom;

				// Extract rotation and position
				Instance.Location = NewTransform.GetOrigin();
				Instance.Rotation = NewTransform.Rotator();

				// Apply render data
				check(MeshInfo.Component);
				MeshInfo.Component->UpdateInstanceTransform(InstanceIndex, Instance.GetInstanceWorldTransform(), true);

				// Re-add the new instance location to the hash
				MeshInfo.InstanceHash->InsertInstance(Instance.Location, InstanceIndex);
			}
		}
	}
}

void AInstancedFoliageActor::DeleteInstancesForComponent(UActorComponent* InComponent)
{
	const auto BaseId = InstanceBaseCache.GetInstanceBaseId(InComponent);
	// Instances with empty base has BaseId==InvalidBaseId, we should not delete these
	if (BaseId == FFoliageInstanceBaseCache::InvalidBaseId)
	{
		return;
	}
		
	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		const auto* InstanceSet = MeshInfo.ComponentHash.Find(BaseId);
		if (InstanceSet)
		{
			MeshInfo.RemoveInstances(this, InstanceSet->Array());
		}
	}
}

void AInstancedFoliageActor::DeleteInstancesForComponent(UActorComponent* InComponent, const UFoliageType* FoliageType)
{
	const auto BaseId = InstanceBaseCache.GetInstanceBaseId(InComponent);
	// Instances with empty base has BaseId==InvalidBaseId, we should not delete these
	if (BaseId == FFoliageInstanceBaseCache::InvalidBaseId)
	{
		return;
	}
	
	FFoliageMeshInfo* MeshInfo = FindMesh(FoliageType);
	if (MeshInfo)
	{
		const auto* InstanceSet = MeshInfo->ComponentHash.Find(BaseId);
		if (InstanceSet)
		{
			MeshInfo->RemoveInstances(this, InstanceSet->Array());
		}
	}
}

void AInstancedFoliageActor::DeleteInstancesForProceduralFoliageComponent(const UProceduralFoliageComponent* ProceduralFoliageComponent)
{
	const FGuid& ProceduralGuid = ProceduralFoliageComponent->GetProceduralGuid();
	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		TArray<int32> InstancesToRemove;
		for (int32 InstanceIdx = 0; InstanceIdx < MeshInfo.Instances.Num(); InstanceIdx++)
		{
			if (MeshInfo.Instances[InstanceIdx].ProceduralGuid == ProceduralGuid)
			{
				InstancesToRemove.Add(InstanceIdx);
			}
		}

		if (InstancesToRemove.Num())
		{
			MeshInfo.RemoveInstances(this, InstancesToRemove);
		}
	}
}

void AInstancedFoliageActor::MoveInstancesForComponentToCurrentLevel(UActorComponent* InComponent)
{
	AInstancedFoliageActor* NewIFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(InComponent->GetWorld(), true);
	const auto SourceBaseId = InstanceBaseCache.GetInstanceBaseId(InComponent);
	
	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		UFoliageType* FoliageType = MeshPair.Key;

		// Duplicate the foliage type if it's not shared
		if (FoliageType->GetOutermost() == GetOutermost())
		{
			FoliageType = (UFoliageType*)StaticDuplicateObject(FoliageType, NewIFA, nullptr, RF_AllFlags & ~(RF_Standalone | RF_Public));
		}
		
		const auto* InstanceSet = MeshInfo.ComponentHash.Find(SourceBaseId);
		if (InstanceSet)
		{
			FFoliageMeshInfo* NewMeshInfo = NewIFA->FindOrAddMesh(FoliageType);

			// Add the foliage to the new level
			for (int32 InstanceIndex : *InstanceSet)
			{
				NewMeshInfo->AddInstance(NewIFA, FoliageType, MeshInfo.Instances[InstanceIndex], InComponent);
			}

			// Remove from old level
			MeshInfo.RemoveInstances(this, InstanceSet->Array());
		}
	}
}

void AInstancedFoliageActor::MoveInstancesToNewComponent(UPrimitiveComponent* InOldComponent, UPrimitiveComponent* InNewComponent)
{
	AInstancedFoliageActor* NewIFA = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(InNewComponent->GetTypedOuter<ULevel>(), true);

	const auto OldBaseId = this->InstanceBaseCache.GetInstanceBaseId(InOldComponent);
	const auto NewBaseId = NewIFA->InstanceBaseCache.AddInstanceBaseId(InNewComponent);

	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		UFoliageType* FoliageType = MeshPair.Key;

		// Duplicate the foliage type if it's not shared
		if (FoliageType->GetOutermost() == GetOutermost())
		{
			FoliageType = (UFoliageType*)StaticDuplicateObject(FoliageType, NewIFA, nullptr, RF_AllFlags & ~(RF_Standalone | RF_Public));
		}

		TSet<int32> InstanceSet;
		if (MeshInfo.ComponentHash.RemoveAndCopyValue(OldBaseId, InstanceSet) && InstanceSet.Num())
		{
			// For same FoliageActor can just remap the instances, otherwise we have to do a more complex move
			if (NewIFA == this)
			{
				// Update the instances
				for (int32 InstanceIndex : InstanceSet)
				{
					MeshInfo.Instances[InstanceIndex].BaseId = NewBaseId;
				}
				
				// Update the hash
				MeshInfo.ComponentHash.Add(NewBaseId, MoveTemp(InstanceSet));
				
			}
			else
			{
				FFoliageMeshInfo* NewMeshInfo = NewIFA->FindOrAddMesh(FoliageType);

				// Add the foliage to the new level
				for (int32 InstanceIndex : InstanceSet)
				{
					FFoliageInstance NewInstance = MeshInfo.Instances[InstanceIndex];
					NewInstance.BaseId = NewBaseId;
					NewMeshInfo->AddInstance(NewIFA, FoliageType, NewInstance);
				}

				// Remove from old level
				MeshInfo.RemoveInstances(this, InstanceSet.Array());
			}
		}
	}
}

void AInstancedFoliageActor::MoveSelectedInstancesToLevel(ULevel* InTargetLevel)
{
	if (InTargetLevel == GetLevel() || !HasSelectedInstances())
	{
		return;
	}
		
	AInstancedFoliageActor* TargetIFA = GetInstancedFoliageActorForLevel(InTargetLevel, /*bCreateIfNone*/ true);
	
	Modify();
	TargetIFA->Modify();
	
	// Do move
	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		UFoliageType* FoliageType = MeshPair.Key;

		if (MeshInfo.SelectedIndices.Num())
		{
			FFoliageMeshInfo* TargetMeshInfo = nullptr;
			UFoliageType* TargetFoliageType = TargetIFA->AddFoliageType(FoliageType, &TargetMeshInfo);

			// Add selected instances to the target actor
			for (int32 InstanceIndex : MeshInfo.SelectedIndices)
			{
				FFoliageInstance& Instance = MeshInfo.Instances[InstanceIndex];
				TargetMeshInfo->AddInstance(TargetIFA, TargetFoliageType, Instance, InstanceBaseCache.GetInstanceBasePtr(Instance.BaseId).Get());
			}

			// Remove selected instances from this actor
			MeshInfo.RemoveInstances(this, MeshInfo.SelectedIndices.Array());
		}
	}
}

TMap<UFoliageType*, TArray<const FFoliageInstancePlacementInfo*>> AInstancedFoliageActor::GetInstancesForComponent(UActorComponent* InComponent)
{
	TMap<UFoliageType*, TArray<const FFoliageInstancePlacementInfo*>> Result;
	const auto BaseId = InstanceBaseCache.GetInstanceBaseId(InComponent);

	for (auto& MeshPair : FoliageMeshes)
	{
		const FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		const auto* InstanceSet = MeshInfo.ComponentHash.Find(BaseId);
		if (InstanceSet)
		{
			TArray<const FFoliageInstancePlacementInfo*>& Array = Result.Add(MeshPair.Key, TArray<const FFoliageInstancePlacementInfo*>());
			Array.Empty(InstanceSet->Num());

			for (int32 InstanceIndex : *InstanceSet)
			{
				const FFoliageInstancePlacementInfo* Instance = &MeshInfo.Instances[InstanceIndex];
				Array.Add(Instance);
			}
		}
	}

	return Result;
}

FFoliageMeshInfo* AInstancedFoliageActor::FindOrAddMesh(UFoliageType* InType)
{
	TUniqueObj<FFoliageMeshInfo>* MeshInfoEntry = FoliageMeshes.Find(InType);
	FFoliageMeshInfo* MeshInfo = MeshInfoEntry ? &MeshInfoEntry->Get() : AddMesh(InType);
	return MeshInfo;
}


void UpdateSettingsBounds(const UStaticMesh* InMesh, UFoliageType_InstancedStaticMesh* Settings)
{
	const FBoxSphereBounds MeshBounds = InMesh->GetBounds();

	Settings->MeshBounds = MeshBounds;

	// Make bottom only bound
	FBox LowBound = MeshBounds.GetBox();
	LowBound.Max.Z = LowBound.Min.Z + (LowBound.Max.Z - LowBound.Min.Z) * 0.1f;

	float MinX = FLT_MAX, MaxX = FLT_MIN, MinY = FLT_MAX, MaxY = FLT_MIN;
	Settings->LowBoundOriginRadius = FVector::ZeroVector;

	if (InMesh->RenderData)
	{
		FPositionVertexBuffer& PositionVertexBuffer = InMesh->RenderData->LODResources[0].PositionVertexBuffer;
		for (uint32 Index = 0; Index < PositionVertexBuffer.GetNumVertices(); ++Index)
		{
			const FVector& Pos = PositionVertexBuffer.VertexPosition(Index);
			if (Pos.Z < LowBound.Max.Z)
			{
				MinX = FMath::Min(MinX, Pos.X);
				MinY = FMath::Min(MinY, Pos.Y);
				MaxX = FMath::Max(MaxX, Pos.X);
				MaxY = FMath::Max(MaxY, Pos.Y);
			}
		}
	}

	Settings->LowBoundOriginRadius = FVector((MinX + MaxX), (MinY + MaxY), FMath::Sqrt(FMath::Square(MaxX - MinX) + FMath::Square(MaxY - MinY))) * 0.5f;
}


#if WITH_EDITORONLY_DATA
FFoliageMeshInfo* AInstancedFoliageActor::UpdateMeshSettings(const UStaticMesh* InMesh, const UFoliageType_InstancedStaticMesh* DefaultSettings)
{
	if (UFoliageType* OldSettings = GetSettingsForMesh(InMesh))
	{
		MarkPackageDirty();

		UFoliageType_InstancedStaticMesh* NewSettings = DuplicateObject<UFoliageType_InstancedStaticMesh>(DefaultSettings, this);
		UpdateSettingsBounds(InMesh, NewSettings);

		TUniqueObj<FFoliageMeshInfo> MeshInfo;
		FoliageMeshes.RemoveAndCopyValue(OldSettings, MeshInfo);
		MeshInfo->FoliageTypeUpdateGuid = NewSettings->UpdateGuid;
		MeshInfo->UpdateComponentSettings(NewSettings);
		return &*FoliageMeshes.Add(NewSettings, MoveTemp(MeshInfo));
	}

	return nullptr;
}
#endif

UFoliageType* AInstancedFoliageActor::AddFoliageType(const UFoliageType* InType, FFoliageMeshInfo** OutInfo)
{
	FFoliageMeshInfo* MeshInfo = nullptr;
	UFoliageType* FoliageType = const_cast<UFoliageType*>(InType);
	
	if (FoliageType->GetOuter() == this || FoliageType->IsAsset())
	{
		auto* MeshInfoPtr = FoliageMeshes.Find(FoliageType);
		if (!MeshInfoPtr)
		{
			MarkPackageDirty();
			MeshInfo = &FoliageMeshes.Add(FoliageType).Get();
		}
		else
		{
			MeshInfo = &MeshInfoPtr->Get();
		}
	}
	else
	{
		// Unique meshes only
		// Multiple entries for same static mesh can be added using FoliageType as an asset
		FoliageType = GetSettingsForMesh(FoliageType->GetStaticMesh(), &MeshInfo);
		if (FoliageType == nullptr)
		{
			FoliageType = DuplicateObject<UFoliageType>(InType, this);
			MarkPackageDirty();
			MeshInfo = &FoliageMeshes.Add(FoliageType).Get();
		}
	}

	if (OutInfo)
	{
		*OutInfo = MeshInfo;
	}

	return FoliageType;
}

FFoliageMeshInfo* AInstancedFoliageActor::AddMesh(UStaticMesh* InMesh, UFoliageType** OutSettings, const UFoliageType_InstancedStaticMesh* DefaultSettings)
{
	check(GetSettingsForMesh(InMesh) == nullptr);

	MarkPackageDirty();

	UFoliageType_InstancedStaticMesh* Settings = nullptr;
#if WITH_EDITORONLY_DATA
	if (DefaultSettings)
	{
		// TODO: Can't we just use this directly?
		Settings = DuplicateObject<UFoliageType_InstancedStaticMesh>(DefaultSettings, this);
	}
	else
#endif
	{
		Settings = NewObject<UFoliageType_InstancedStaticMesh>(this);
	}
	Settings->Mesh = InMesh;

	FFoliageMeshInfo* MeshInfo = AddMesh(Settings);
	UpdateSettingsBounds(InMesh, Settings);

	if (OutSettings)
	{
		*OutSettings = Settings;
	}

	return MeshInfo;
}

FFoliageMeshInfo* AInstancedFoliageActor::AddMesh(UFoliageType* InType)
{
	check(FoliageMeshes.Find(InType) == nullptr);

	MarkPackageDirty();

	if (InType->DisplayOrder == 0)
	{
		int32 MaxDisplayOrder = 0;
		for (auto& MeshPair : FoliageMeshes)
		{
			if (MeshPair.Key->DisplayOrder > MaxDisplayOrder)
			{
				MaxDisplayOrder = MeshPair.Key->DisplayOrder;
			}
		}
		InType->DisplayOrder = MaxDisplayOrder + 1;
	}

	FFoliageMeshInfo* MeshInfo = &*FoliageMeshes.Add(InType);
	MeshInfo->FoliageTypeUpdateGuid = InType->UpdateGuid;
	InType->IsSelected = true;

	return MeshInfo;
}

void AInstancedFoliageActor::RemoveFoliageType(UFoliageType** InFoliageTypes, int32 Num)
{
	Modify();
	UnregisterAllComponents();

	// Remove all components for this mesh from the Components array.
	for (int32 FoliageTypeIdx = 0; FoliageTypeIdx < Num; ++FoliageTypeIdx)
	{
		const UFoliageType* FoliageType = InFoliageTypes[FoliageTypeIdx];
		FFoliageMeshInfo* MeshInfo = FindMesh(FoliageType);
		if (MeshInfo)
		{
			if (MeshInfo->Component)
			{
				MeshInfo->Component->bAutoRegister = false;
			}

			FoliageMeshes.Remove(FoliageType);
		}
	}
		
	RegisterAllComponents();
}

void AInstancedFoliageActor::SelectInstance(UInstancedStaticMeshComponent* InComponent, int32 InInstanceIndex, bool bToggle)
{
	Modify();

	// If we're not toggling, we need to first deselect everything else
	if (!bToggle)
	{
		for (auto& MeshPair : FoliageMeshes)
		{
			FFoliageMeshInfo& MeshInfo = *MeshPair.Value;

			if (MeshInfo.SelectedIndices.Num() > 0)
			{
				check(MeshInfo.Component);
				if (MeshInfo.Component->SelectedInstances.Num() > 0)
				{
					MeshInfo.Component->SelectedInstances.Empty();
					MeshInfo.Component->ReleasePerInstanceRenderData();
					MeshInfo.Component->MarkRenderStateDirty();
				}
			}

			MeshInfo.SelectedIndices.Empty();
		}
	}

	if (InComponent)
	{
		UFoliageType* Type = nullptr;
		FFoliageMeshInfo* MeshInfo = nullptr;

		for (auto& MeshPair : FoliageMeshes)
		{
			if (MeshPair.Value->Component == InComponent)
			{
				Type = MeshPair.Key;
				MeshInfo = &MeshPair.Value.Get();
			}
		}
		
		if (MeshInfo)
		{
			bool bIsSelected = MeshInfo->SelectedIndices.Contains(InInstanceIndex);

			// Deselect if it's already selected.
			if (InInstanceIndex < InComponent->SelectedInstances.Num())
			{
				InComponent->SelectedInstances[InInstanceIndex] = false;
				InComponent->ReleasePerInstanceRenderData();
				InComponent->MarkRenderStateDirty();
			}

			if (bIsSelected)
			{
				MeshInfo->SelectedIndices.Remove(InInstanceIndex);
			}

			if (!bToggle || !bIsSelected)
			{
				// Add the selection
				if (InComponent->SelectedInstances.Num() < InComponent->PerInstanceSMData.Num())
				{
					InComponent->SelectedInstances.Init(false, InComponent->PerInstanceSMData.Num());
				}
				InComponent->SelectedInstances[InInstanceIndex] = true;
				InComponent->ReleasePerInstanceRenderData();
				InComponent->MarkRenderStateDirty();

				MeshInfo->SelectedIndices.Add(InInstanceIndex);
			}
		}
	}
}

bool AInstancedFoliageActor::HasSelectedInstances() const
{
	for (const auto& MeshPair : FoliageMeshes)
	{
		if (MeshPair.Value->SelectedIndices.Num() > 0)
		{
			return true;
		}
	}
	
	return false;
}

void AInstancedFoliageActor::PostEditUndo()
{
	Super::PostEditUndo();

	FlushRenderingCommands();
	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
		MeshInfo.ReapplyInstancesToComponent();
	}
}

bool AInstancedFoliageActor::ShouldExport()
{
	return false;
}

bool AInstancedFoliageActor::ShouldImport(FString* ActorPropString, bool IsMovingLevel)
{
	return false;
}

void AInstancedFoliageActor::ApplySelectionToComponents(bool bApply)
{
	for (auto& MeshPair : FoliageMeshes)
	{
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;

		if (bApply)
		{
			if (MeshInfo.SelectedIndices.Num() > 0)
			{
				check(MeshInfo.Component);

				// Apply any selections in the component
				MeshInfo.Component->SelectedInstances.Init(false, MeshInfo.Component->PerInstanceSMData.Num());
				for (int32 i : MeshInfo.SelectedIndices)
				{
					MeshInfo.Component->SelectedInstances[i] = true;
				}

				MeshInfo.Component->ReleasePerInstanceRenderData();
				MeshInfo.Component->MarkRenderStateDirty();
			}
		}
		else		
		{
			if (MeshInfo.Component && MeshInfo.Component->SelectedInstances.Num() > 0)
			{
				// remove any selections in the component
				MeshInfo.Component->SelectedInstances.Empty();

				MeshInfo.Component->ReleasePerInstanceRenderData();
				MeshInfo.Component->MarkRenderStateDirty();
			}
		}
	}
}

bool AInstancedFoliageActor::GetSelectionLocation(FVector& OutLocation) const
{
	for (const auto& MeshPair: FoliageMeshes)
	{
		const FFoliageMeshInfo& MeshInfo = MeshPair.Value.Get();
		if (MeshInfo.SelectedIndices.Num())
		{
			const int32 InstanceIdx = (*MeshInfo.SelectedIndices.CreateConstIterator());
			OutLocation = MeshInfo.Instances[InstanceIdx].Location;
			return true;
		}
	}
	return false;
}

void AInstancedFoliageActor::MapRebuild()
{
	// Map rebuild may have modified the BSP's ModelComponents and thrown the previous ones away.
	// Most BSP-painted foliage is attached to a Brush's UModelComponent which persist across rebuilds,
	// but any foliage attached directly to the level BSP's ModelComponents will need to try to find a new base.

	TMap<UFoliageType*, TArray<FFoliageInstance>> NewInstances;
	TArray<UModelComponent*> RemovedModelComponents;
	UWorld* World = GetWorld();
	check(World);

	// For each foliage brush, represented by the mesh/info pair
	for (auto& MeshPair : FoliageMeshes)
	{
		// each target component has some foliage instances
		FFoliageMeshInfo const& MeshInfo = *MeshPair.Value;
		UFoliageType* Settings = MeshPair.Key;
		check(Settings);

		for (auto& ComponentFoliagePair : MeshInfo.ComponentHash)
		{
			// BSP components are UModelComponents - they are the only ones we need to change
			auto BaseComponentPtr = InstanceBaseCache.GetInstanceBasePtr(ComponentFoliagePair.Key);
			UModelComponent* TargetComponent = Cast<UModelComponent>(BaseComponentPtr.Get());
		
			// Check if it's part of a brush. We only need to fix up model components that are part of the level BSP.
			if (TargetComponent && Cast<ABrush>(TargetComponent->GetOuter()) == nullptr)
			{
				// Delete its instances later
				RemovedModelComponents.Add(TargetComponent);

				// We have to test each instance to see if we can migrate it across
				for (int32 InstanceIdx : ComponentFoliagePair.Value)
				{
					// Use a line test against the world. This is not very reliable as we don't know the original trace direction.
					check(MeshInfo.Instances.IsValidIndex(InstanceIdx));
					FFoliageInstance const& Instance = MeshInfo.Instances[InstanceIdx];

					FFoliageInstance NewInstance = Instance;

					FTransform InstanceToWorld = Instance.GetInstanceWorldTransform();
					FVector Down(-FVector::UpVector);
					FVector Start(InstanceToWorld.TransformPosition(FVector::UpVector));
					FVector End(InstanceToWorld.TransformPosition(Down));

					FHitResult Result;
					bool bHit = World->LineTraceSingleByObjectType(Result, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), FCollisionQueryParams(true));

					if (bHit && Result.Component.IsValid() && Result.Component->IsA(UModelComponent::StaticClass()))
					{
						NewInstance.BaseId = InstanceBaseCache.AddInstanceBaseId(Result.Component.Get());
						NewInstances.FindOrAdd(Settings).Add(NewInstance);
					}
				}
			}
		}
	}

	// Remove all existing & broken instances & component references.
	for (UModelComponent* Component : RemovedModelComponents)
	{
		DeleteInstancesForComponent(Component);
	}

	// And then finally add our new instances to the correct target components.
	for (auto& NewInstancePair : NewInstances)
	{
		UFoliageType* Settings = NewInstancePair.Key;
		check(Settings);
		FFoliageMeshInfo& MeshInfo = *FindOrAddMesh(Settings);
		for (FFoliageInstance& Instance : NewInstancePair.Value)
		{
			MeshInfo.AddInstance(this, Settings, Instance);
		}
	}
}

#endif // WITH_EDITOR

struct FFoliageMeshInfo_Old
{
	TArray<FFoliageInstanceCluster_Deprecated> InstanceClusters;
	TArray<FFoliageInstance_Deprecated> Instances;
	UFoliageType_InstancedStaticMesh* Settings; // Type remapped via +ActiveClassRedirects
};
FArchive& operator<<(FArchive& Ar, FFoliageMeshInfo_Old& MeshInfo)
{
	Ar << MeshInfo.InstanceClusters;
	Ar << MeshInfo.Instances;
	Ar << MeshInfo.Settings;

	return Ar;
}

void AInstancedFoliageActor::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FFoliageCustomVersion::GUID);

#if WITH_EDITORONLY_DATA
	if (!Ar.ArIsFilterEditorOnly && Ar.CustomVer(FFoliageCustomVersion::GUID) >= FFoliageCustomVersion::CrossLevelBase)
	{
		Ar << InstanceBaseCache;
	}
#endif
	
	if (Ar.UE4Ver() < VER_UE4_FOLIAGE_SETTINGS_TYPE)
	{
#if WITH_EDITORONLY_DATA
		TMap<UFoliageType*, TUniqueObj<FFoliageMeshInfo_Deprecated>> FoliageMeshesDeprecated;
		TMap<UStaticMesh*, FFoliageMeshInfo_Old> OldFoliageMeshes;
		Ar << OldFoliageMeshes;
		for (auto& OldMeshInfo : OldFoliageMeshes)
		{
			FFoliageMeshInfo_Deprecated NewMeshInfo;

			NewMeshInfo.Instances = MoveTemp(OldMeshInfo.Value.Instances);
			
			UFoliageType_InstancedStaticMesh* FoliageType = OldMeshInfo.Value.Settings;
			if (FoliageType == nullptr)
			{
				// If the Settings object was null, eg the user forgot to save their settings asset, create a new one.
				FoliageType = NewObject<UFoliageType_InstancedStaticMesh>(this);
			}

			if (FoliageType->Mesh == nullptr)
			{
				FoliageType->Modify();
				FoliageType->Mesh = OldMeshInfo.Key;
			}
			else if (FoliageType->Mesh != OldMeshInfo.Key)
			{
				// If mesh doesn't match (two meshes sharing the same settings object?) then we need to duplicate as that is no longer supported
				FoliageType = (UFoliageType_InstancedStaticMesh*)StaticDuplicateObject(FoliageType, this, nullptr, RF_AllFlags & ~(RF_Standalone | RF_Public));
				FoliageType->Mesh = OldMeshInfo.Key;
			}
			NewMeshInfo.FoliageTypeUpdateGuid = FoliageType->UpdateGuid;
			FoliageMeshes_Deprecated.Add(FoliageType, TUniqueObj<FFoliageMeshInfo_Deprecated>(MoveTemp(NewMeshInfo)));
		}
#endif//WITH_EDITORONLY_DATA
	}
	else
	{
		if (Ar.CustomVer(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::CrossLevelBase)
		{
#if WITH_EDITORONLY_DATA
			Ar << FoliageMeshes_Deprecated;
#endif
		}
		else
		{
			Ar << FoliageMeshes;
		}
	}
	
	// Clean up any old cluster components and convert to hierarchical instanced foliage.
	if (Ar.CustomVer(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::FoliageUsingHierarchicalISMC)
	{
		TInlineComponentArray<UInstancedStaticMeshComponent*> ClusterComponents;
		GetComponents(ClusterComponents);
		for (UInstancedStaticMeshComponent* Component : ClusterComponents)
		{
			Component->bAutoRegister = false;
		}
	}
}

void AInstancedFoliageActor::PostInitProperties()
{
	Super::PostInitProperties();

#if WITH_EDITOR
	if (!IsTemplate())
	{
		GEngine->OnActorMoved().Remove(OnLevelActorMovedDelegateHandle);
		OnLevelActorMovedDelegateHandle = GEngine->OnActorMoved().AddUObject(this, &AInstancedFoliageActor::OnLevelActorMoved);

		GEngine->OnLevelActorDeleted().Remove(OnLevelActorDeletedDelegateHandle);
		OnLevelActorDeletedDelegateHandle = GEngine->OnLevelActorDeleted().AddUObject(this, &AInstancedFoliageActor::OnLevelActorDeleted);
	}
#endif
}

void AInstancedFoliageActor::BeginDestroy()
{
	Super::BeginDestroy();

#if WITH_EDITOR
	if (!IsTemplate())
	{
		GEngine->OnActorMoved().Remove(OnLevelActorMovedDelegateHandle);
		GEngine->OnLevelActorDeleted().Remove(OnLevelActorDeletedDelegateHandle);
	}
#endif
}

void AInstancedFoliageActor::PostLoad()
{
	Super::PostLoad();

	ULevel* OwningLevel = GetLevel();
	if (!OwningLevel->InstancedFoliageActor.IsValid())
	{
		OwningLevel->InstancedFoliageActor = this;
	}
	else
	{
		// Warn that there is more than one foliage actor in the scene
		UE_LOG(LogInstancedFoliage, Warning, TEXT("Level %s: has more than one instanced foliage actor: %s, %s"), 
			*OwningLevel->GetOutermost()->GetName(), 
			*OwningLevel->InstancedFoliageActor->GetName(),
			*this->GetName());
	}

#if WITH_EDITOR
	if (GIsEditor)
	{
		if (GetLinkerCustomVersion(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::CrossLevelBase)
		{
			ConvertDeprecatedFoliageMeshes(this, FoliageMeshes_Deprecated, FoliageMeshes);
			FoliageMeshes_Deprecated.Empty();
		}
				
		{
			bool bContainsNull = FoliageMeshes.Remove(nullptr) > 0;
			if (bContainsNull)
			{
				FMessageLog("MapCheck").Warning()
					->AddToken(FUObjectToken::Create(this))
					->AddToken(FTextToken::Create(LOCTEXT("MapCheck_Message_FoliageMissingStaticMesh", "Foliage instances for a missing static mesh have been removed.")))
					->AddToken(FMapErrorToken::Create(FMapErrors::FoliageMissingStaticMesh));
				while (bContainsNull)
				{
					bContainsNull = FoliageMeshes.Remove(nullptr) > 0;
				}
			}
		}
		for (auto& MeshPair : FoliageMeshes)
		{
			// Find the per-mesh info matching the mesh.
			FFoliageMeshInfo& MeshInfo = *MeshPair.Value;
			UFoliageType* FoliageType = MeshPair.Key;

			if (MeshInfo.Instances.Num() && MeshInfo.Component == nullptr)
			{
				const UStaticMesh* StaticMesh = FoliageType->GetStaticMesh();
				FFormatNamedArguments Arguments;
				if (StaticMesh)
				{
					Arguments.Add(TEXT("MeshName"), FText::FromString(StaticMesh->GetName()));
				}
				else
				{
					Arguments.Add(TEXT("MeshName"), FText::FromString(TEXT("None")));
				}

				FMessageLog("MapCheck").Warning()
					->AddToken(FUObjectToken::Create(this))
					->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_FoliageMissingComponent", "Foliage in this map is missing a component for static mesh {MeshName}. This has been repaired."),Arguments)))
					->AddToken(FMapErrorToken::Create(FMapErrors::FoliageMissingClusterComponent));

				MeshInfo.ReallocateClusters(this, MeshPair.Key);
			}

			// Update foliage components if the foliage settings object was changed while the level was not loaded.
			if (MeshInfo.FoliageTypeUpdateGuid != FoliageType->UpdateGuid)
			{
				if (MeshInfo.FoliageTypeUpdateGuid.IsValid())
				{
					MeshInfo.ReallocateClusters(this, MeshPair.Key);
				}
				MeshInfo.FoliageTypeUpdateGuid = FoliageType->UpdateGuid;
			}

			// Update the hash.
			MeshInfo.ComponentHash.Empty();
			MeshInfo.InstanceHash->Empty();
			for (int32 InstanceIdx = 0; InstanceIdx < MeshInfo.Instances.Num(); InstanceIdx++)
			{
				MeshInfo.AddToBaseHash(InstanceIdx);
				MeshInfo.InstanceHash->InsertInstance(MeshInfo.Instances[InstanceIdx].Location, InstanceIdx);
			}
	
			// Convert to Heirarchical foliage
			if (GetLinkerCustomVersion(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::FoliageUsingHierarchicalISMC)
			{
				MeshInfo.ReallocateClusters(this, MeshPair.Key);
			}

			if (GetLinkerCustomVersion(FFoliageCustomVersion::GUID) < FFoliageCustomVersion::HierarchicalISMCNonTransactional)
			{
				if (MeshInfo.Component)
				{
					MeshInfo.Component->ClearFlags(RF_Transactional);
				}
			}
		}

		// Clean up dead cross-level references
		FFoliageInstanceBaseCache::CompactInstanceBaseCache(this);

	}
#endif
}

#if WITH_EDITOR

void AInstancedFoliageActor::NotifyFoliageTypeChanged(UFoliageType* FoliageType)
{
	FFoliageMeshInfo* MeshInfo = FindMesh(FoliageType);
	if (MeshInfo)
	{
		MeshInfo->ReallocateClusters(this, FoliageType);
	}
}

void AInstancedFoliageActor::OnLevelActorMoved(AActor* InActor)
{
	UWorld* InWorld = InActor->GetWorld();
	
	if (!InWorld || !InWorld->IsGameWorld())
	{
		TInlineComponentArray<UActorComponent*> Components;
		InActor->GetComponents(Components);
		
		for (auto Component : Components)
		{
			MoveInstancesForMovedComponent(Component);
		}
	}
}

void AInstancedFoliageActor::OnLevelActorDeleted(AActor* InActor)
{
	UWorld* InWorld = InActor->GetWorld();
	
	if (!InWorld || !InWorld->IsGameWorld())
	{
		TInlineComponentArray<UActorComponent*> Components;
		InActor->GetComponents(Components);
		
		for (auto Component : Components)
		{
			DeleteInstancesForComponent(Component);
		}
	}
}

#endif

//
// Serialize all our UObjects for RTGC 
//
void AInstancedFoliageActor::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	AInstancedFoliageActor* This = CastChecked<AInstancedFoliageActor>(InThis);

	for (auto& MeshPair : This->FoliageMeshes)
	{
		Collector.AddReferencedObject(MeshPair.Key, This);
		FFoliageMeshInfo& MeshInfo = *MeshPair.Value;

		if (MeshInfo.Component)
		{
			Collector.AddReferencedObject(MeshInfo.Component, This);
		}
	}
	
	Super::AddReferencedObjects(This, Collector);
}

void AInstancedFoliageActor::ApplyWorldOffset(const FVector& InOffset, bool bWorldShift)
{
	Super::ApplyWorldOffset(InOffset, bWorldShift);

	if (GIsEditor)
	{
		for (auto& MeshPair : FoliageMeshes)
		{
			FFoliageMeshInfo& MeshInfo = *MeshPair.Value;

#if WITH_EDITORONLY_DATA
			InstanceBaseCache.UpdateInstanceBaseCachedTransforms();
			
			MeshInfo.InstanceHash->Empty();
			for (int32 InstanceIdx = 0; InstanceIdx < MeshInfo.Instances.Num(); InstanceIdx++)
			{
				FFoliageInstance& Instance = MeshInfo.Instances[InstanceIdx];
				Instance.Location += InOffset;
				// Rehash instance location
				MeshInfo.InstanceHash->InsertInstance(Instance.Location, InstanceIdx);
			}
#endif
		}
	}
}

bool AInstancedFoliageActor::FoliageTrace(const UWorld* InWorld, FHitResult& OutHit, const FDesiredFoliageInstance& DesiredInstance, FName InTraceTag, bool InbReturnFaceIndex)
{
	FCollisionQueryParams QueryParams(InTraceTag, true);
	QueryParams.bReturnFaceIndex = InbReturnFaceIndex;

	FVector StartTrace = DesiredInstance.StartTrace;

	TArray<FHitResult> Hits;

	bool bInsideProceduralVolume = false;
	FCollisionShape SphereShape;
	SphereShape.SetSphere(DesiredInstance.TraceRadius);
	InWorld->SweepMultiByObjectType(Hits, StartTrace, DesiredInstance.EndTrace, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic), SphereShape, QueryParams);


	for (const FHitResult& Hit : Hits)
	{
		if (DesiredInstance.PlacementMode == EFoliagePlacementMode::Procedural)
		{
			if (Hit.Actor.IsValid())	//if we hit the ProceduralFoliage blocking volume don't spawn instance
			{
				if (const AProceduralFoliageBlockingVolume* ProceduralFoliageBlockingVolume = Cast<AProceduralFoliageBlockingVolume>(Hit.Actor.Get()))
				{
					const AProceduralFoliageActor* ProceduralFoliageActor = ProceduralFoliageBlockingVolume->ProceduralFoliageActor;
					if (ProceduralFoliageActor == nullptr || ProceduralFoliageActor->ProceduralComponent == nullptr || ProceduralFoliageActor->ProceduralComponent->GetProceduralGuid() == DesiredInstance.ProceduralGuid)
					{
						return false;
					}
				}
				else if(Cast<AInstancedFoliageActor>(Hit.Actor.Get()))
				{
					return false;
				}
				else if (Cast<AProceduralFoliageActor>(Hit.Actor.Get()))	//we never want to collide with our spawning volume
				{
					continue;
				}

				if (bInsideProceduralVolume == false)
				{
					bInsideProceduralVolume = DesiredInstance.ProceduralVolumeBodyInstance->OverlapTest(Hit.ImpactPoint, FQuat::Identity, FCollisionShape::MakeSphere(1.f));	//make sphere of 1cm radius to test if we're in the procedural volume
				}
			}
		}
			
		// In the editor traces can hit "No Collision" type actors, so ugh.
		const FBodyInstance* BodyInstance = Hit.Component->GetBodyInstance();
		if (BodyInstance->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics || BodyInstance->GetResponseToChannel(ECC_WorldStatic) != ECR_Block)
		{
			continue;
		}

		if (Hit.Component.IsValid() && Hit.Component->GetComponentLevel())
		{
			OutHit = Hit;
			return (DesiredInstance.PlacementMode != EFoliagePlacementMode::Procedural) || bInsideProceduralVolume;
		}
	}

	return false;
}

bool AInstancedFoliageActor::CheckCollisionWithWorld(const UWorld* InWorld, const UFoliageType* Settings, const FFoliageInstance& Inst, const FVector& HitNormal, const FVector& HitLocation)
{
	FMatrix InstTransform = Inst.GetInstanceWorldTransform().ToMatrixWithScale();
	FVector LocalHit = InstTransform.InverseTransformPosition(HitLocation);

	if (Settings->CollisionWithWorld)
	{
		// Check for overhanging ledge
		{
			FVector LocalSamplePos[4] = { 
				FVector(Settings->LowBoundOriginRadius.Z, 0, 0),
				FVector(-Settings->LowBoundOriginRadius.Z, 0, 0),
				FVector(0, Settings->LowBoundOriginRadius.Z, 0),
				FVector(0, -Settings->LowBoundOriginRadius.Z, 0)
			};


			for (uint32 i = 0; i < 4; ++i)
			{
				FHitResult Hit;
				FVector SamplePos = InstTransform.TransformPosition(FVector(Settings->LowBoundOriginRadius.X, Settings->LowBoundOriginRadius.Y, 2.f) + LocalSamplePos[i]);
				float WorldRadius = (Settings->LowBoundOriginRadius.Z + 2.f)*FMath::Max(Inst.DrawScale3D.X, Inst.DrawScale3D.Y);
				FVector NormalVector = Settings->AlignToNormal ? HitNormal : FVector(0, 0, 1);
				if (AInstancedFoliageActor::FoliageTrace(InWorld, Hit, FDesiredFoliageInstance(SamplePos, SamplePos - NormalVector*WorldRadius)))
				{
					if (LocalHit.Z - Inst.ZOffset < Settings->LowBoundOriginRadius.Z)
					{
						continue;
					}
				}
				return false;
			}
		}

		// Check collision with Bounding Box
		{
			FBox MeshBox = Settings->MeshBounds.GetBox();
			MeshBox.Min.Z = FMath::Min(MeshBox.Max.Z, LocalHit.Z + Settings->MeshBounds.BoxExtent.Z * 0.05f);
			FBoxSphereBounds ShrinkBound(MeshBox);
			FBoxSphereBounds WorldBound = ShrinkBound.TransformBy(InstTransform);
			//::DrawDebugBox(World, WorldBound.Origin, WorldBound.BoxExtent, FColor::Red, true, 10.f);
			static FName NAME_FoliageCollisionWithWorld = FName(TEXT("FoliageCollisionWithWorld"));
			if (InWorld->OverlapBlockingTestByChannel(WorldBound.Origin, FQuat(Inst.Rotation), ECC_WorldStatic, FCollisionShape::MakeBox(ShrinkBound.BoxExtent * Inst.DrawScale3D * Settings->CollisionScale), FCollisionQueryParams(NAME_FoliageCollisionWithWorld, false)))
			{
				return false;
			}
		}
	}

	return true;
}

FPotentialInstance::FPotentialInstance(FVector InHitLocation, FVector InHitNormal, UPrimitiveComponent* InHitComponent, float InHitWeight, const FDesiredFoliageInstance& InDesiredInstance)
: HitLocation(InHitLocation)
, HitNormal(InHitNormal)
, HitComponent(InHitComponent)
, HitWeight(InHitWeight)
, DesiredInstance(InDesiredInstance)
{}

bool FPotentialInstance::PlaceInstance(const UWorld* InWorld, const UFoliageType* Settings, FFoliageInstance& Inst, bool bSkipCollision)
{
	if (DesiredInstance.PlacementMode != EFoliagePlacementMode::Procedural)
	{
		Inst.DrawScale3D = Settings->GetRandomScale();
	}
	else
	{
		//Procedural foliage uses age to get the scale
		Inst.DrawScale3D = FVector(Settings->GetScaleForAge(DesiredInstance.Age));
	}

	Inst.ZOffset = Settings->ZOffset.Interpolate(FMath::FRand());

	Inst.Location = HitLocation;

	if (DesiredInstance.PlacementMode != EFoliagePlacementMode::Procedural)
	{
		// Random yaw and optional random pitch up to the maximum
		Inst.Rotation = FRotator(FMath::FRand() * Settings->RandomPitchAngle, 0.f, 0.f);

		if (Settings->RandomYaw)
		{
			Inst.Rotation.Yaw = FMath::FRand() * 360.f;
		}
		else
		{
			Inst.Flags |= FOLIAGE_NoRandomYaw;
		}
	}
	else
	{
		Inst.Rotation = DesiredInstance.Rotation.Rotator();
		Inst.Flags |= FOLIAGE_NoRandomYaw;
	}


	if (Settings->AlignToNormal)
	{
		Inst.AlignToNormal(HitNormal, Settings->AlignMaxAngle);
	}

	// Apply the Z offset in local space
	if (FMath::Abs(Inst.ZOffset) > KINDA_SMALL_NUMBER)
	{
		Inst.Location = Inst.GetInstanceWorldTransform().TransformPosition(FVector(0, 0, Inst.ZOffset));
	}

	UModelComponent* ModelComponent = Cast<UModelComponent>(HitComponent);
	if (ModelComponent)
	{
		ABrush* BrushActor = ModelComponent->GetModel()->FindBrush(HitLocation);
		if (BrushActor)
		{
			HitComponent = BrushActor->GetBrushComponent();
		}
	}

	return bSkipCollision || AInstancedFoliageActor::CheckCollisionWithWorld(InWorld, Settings, Inst, HitNormal, HitLocation);
}

#undef LOCTEXT_NAMESPACE
