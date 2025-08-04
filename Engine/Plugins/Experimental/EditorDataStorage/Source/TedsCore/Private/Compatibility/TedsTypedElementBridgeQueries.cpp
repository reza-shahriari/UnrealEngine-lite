// Copyright Epic Games, Inc. All Rights Reserved.

#include "Compatibility/TedsTypedElementBridgeQueries.h"

#include "Compatibility/TedsTypedElementBridge.h"
#include "Compatibility/Columns/TypedElement.h"
#include "Elements/Columns/TypedElementCompatibilityColumns.h"
#include "Elements/Common/EditorDataStorageFeatures.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementList.h"
#include "Elements/Framework/TypedElementQueryBuilder.h"
#include "Elements/Interfaces/TypedElementDataStorageInterface.h"
#include "HAL/IConsoleManager.h"

namespace UE::Editor::DataStorage::Compatibility::Private
{
	bool bBridgeEnabled = false;
	FAutoConsoleVariableRef CVarBridgeEnabled(
		TEXT("TEDS.TypedElementBridge.Enable"),
		bBridgeEnabled,
		TEXT("Automatically populated TEDS with TypedElementHandles"));
} // namespace UE::Editor::DataStorage::Compatibility::Private

uint8 UTypedElementBridgeDataStorageFactory::GetOrder() const
{
	return 110;
}

void UTypedElementBridgeDataStorageFactory::PreRegister(UE::Editor::DataStorage::ICoreProvider& DataStorage)
{
	Super::PreRegister(DataStorage);
	DebugEnabledDelegateHandle = UE::Editor::DataStorage::Compatibility::Private::CVarBridgeEnabled->OnChangedDelegate().AddUObject(this, &UTypedElementBridgeDataStorageFactory::HandleOnEnabled);
}

void UTypedElementBridgeDataStorageFactory::PreShutdown(UE::Editor::DataStorage::ICoreProvider& DataStorage)
{
	UE::Editor::DataStorage::Compatibility::Private::CVarBridgeEnabled->OnChangedDelegate().Remove(DebugEnabledDelegateHandle);
	DebugEnabledDelegateHandle.Reset();
	CleanupTypedElementColumns(DataStorage);

	Super::PreShutdown(DataStorage);
}

void UTypedElementBridgeDataStorageFactory::RegisterQueries(UE::Editor::DataStorage::ICoreProvider& DataStorage)
{
	Super::RegisterQueries(DataStorage);

	if (IsEnabled())
	{
		RegisterQuery_NewUObject(DataStorage);
	}
}

bool UTypedElementBridgeDataStorageFactory::IsEnabled()
{
	return UE::Editor::DataStorage::Compatibility::Private::CVarBridgeEnabled->GetBool();
}

void UTypedElementBridgeDataStorageFactory::RegisterQuery_NewUObject(UE::Editor::DataStorage::ICoreProvider& DataStorage)
{
	using namespace UE::Editor::DataStorage::Queries;
	
	RemoveTypedElementRowHandleQuery = DataStorage.RegisterQuery(
		Select()
			.ReadOnly<UE::Editor::DataStorage::Compatibility::FTypedElementColumn>()
		.Compile());
}

void UTypedElementBridgeDataStorageFactory::UnregisterQuery_NewUObject(UE::Editor::DataStorage::ICoreProvider& DataStorage)
{
}

void UTypedElementBridgeDataStorageFactory::CleanupTypedElementColumns(UE::Editor::DataStorage::ICoreProvider& DataStorage)
{
	using namespace UE::Editor::DataStorage::Queries;

	// Remove any TEv1 handles
	{
		TArray<RowHandle> Handles;
		DataStorage.RunQuery(
			RemoveTypedElementRowHandleQuery,
			CreateDirectQueryCallbackBinding(
				[&Handles](IDirectQueryContext& Context, const RowHandle*)
				{
					Handles.Append(Context.GetRowHandles());
				}));
		
		DataStorage.BatchAddRemoveColumns(TConstArrayView<RowHandle>(Handles), {}, {Compatibility::FTypedElementColumn::StaticStruct()});
	}
}

void UTypedElementBridgeDataStorageFactory::HandleOnEnabled(IConsoleVariable* CVar)
{
	using namespace UE::Editor::DataStorage;
	ICoreProvider* DataStorage = GetMutableDataStorageFeature<ICoreProvider>(StorageFeatureName);
	bool bIsEnabled = CVar->GetBool();

	if (bIsEnabled)
	{
		RegisterQuery_NewUObject(*DataStorage);
		Compatibility::OnTypedElementBridgeEnabled().Broadcast(bIsEnabled);
	}
	else
	{
		Compatibility::OnTypedElementBridgeEnabled().Broadcast(bIsEnabled);
		UnregisterQuery_NewUObject(*DataStorage);
		CleanupTypedElementColumns(*DataStorage);
	}
}
