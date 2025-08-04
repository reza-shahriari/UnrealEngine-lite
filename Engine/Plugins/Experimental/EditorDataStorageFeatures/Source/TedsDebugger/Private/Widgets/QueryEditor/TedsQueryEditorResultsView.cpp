﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "TedsQueryEditorResultsView.h"

#include "SWarningOrErrorBox.h"
#include "TedsOutlinerModule.h"
#include "TedsTableViewerColumn.h"
#include "Elements/Common/EditorDataStorageFeatures.h"
#include "Elements/Framework/TypedElementQueryBuilder.h"
#include "Elements/Interfaces/TypedElementDataStorageInterface.h"
#include "Elements/Interfaces/TypedElementDataStorageUiInterface.h"
#include "Modules/ModuleManager.h"
#include "QueryEditor/TedsQueryEditorModel.h"
#include "TedsRowViewNode.h"
#include "Elements/Columns/TypedElementHiearchyColumns.h"
#include "Widgets/STedsTableViewer.h"
#include "Widgets/SRowDetails.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TedsDebuggerModule"

namespace UE::Editor::DataStorage::Debug::QueryEditor
{
	SResultsView::~SResultsView()
	{
		Model->GetModelChangedDelegate().Remove(ModelChangedDelegateHandle);
	}

	void SResultsView::Construct(const FArguments& InArgs, FTedsQueryEditorModel& InModel)
	{
		Model = &InModel;
		ModelChangedDelegateHandle = Model->GetModelChangedDelegate().AddRaw(this, &SResultsView::OnModelChanged);
		UiProvider = GetMutableDataStorageFeature<IUiProvider>(UiFeatureName);
		checkf(UiProvider, TEXT("Cannot create SResultsView without TEDS UI"));

		// RowQueryHandle = InvalidQueryHandle;

		// Create a custom column for the table viewer to display row handles
		CreateRowHandleColumn();
	
		RowQueryStack = MakeShared<QueryStack::FRowViewNode>(FRowHandleArrayView(TableViewerRows.GetData(), TableViewerRows.Num(),
			FRowHandleArrayView::EFlags::IsSorted | FRowHandleArrayView::EFlags::IsUnique));

		ChildSlot
		[
			SNew(SVerticalBox)
			+SVerticalBox::Slot()
			[
				SNew(SSplitter)
				+SSplitter::Slot()
				.Value(0.5f)
				[
					SAssignNew(TableViewer, STedsTableViewer)
					.QueryStack(RowQueryStack)
					.EmptyRowsMessage(LOCTEXT("EmptyRowsMessage", "The provided query has no results."))
					.OnSelectionChanged(STedsTableViewer::FOnSelectionChanged::CreateLambda(
						[this](RowHandle SelectedRow)
							{
								if(RowDetailsWidget)
								{
									if(SelectedRow != InvalidRowHandle)
									{
										RowDetailsWidget->SetRow(SelectedRow);
									}
									else
									{
										RowDetailsWidget->ClearRow();
									}
							
								}
							}))
				]
				+SSplitter::Slot()
				.Value(0.5f)
				[
					SAssignNew(RowDetailsWidget, SRowDetails)
				]
		
			]
			+SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					ICoreProvider& TedsInterface = Model->GetTedsInterface();
					FQueryResult QueryResult = TedsInterface.RunQuery(CountQueryHandle);
					if (QueryResult.Completed == FQueryResult::ECompletion::Fully)
					{
						FString String = FString::Printf(TEXT("Element Count: %u"), QueryResult.Count);
						return FText::FromString(String);
					}
					else
					{
						return FText::FromString(TEXT("Invalid query"));
					}
				})
			]
		];

		if(RowHandleColumn)
		{
			TableViewer->AddCustomColumn(RowHandleColumn.ToSharedRef());
		}
	}

	void SResultsView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
	{
		using namespace UE::Editor::DataStorage::Queries;

		ICoreProvider& TedsInterface = Model->GetTedsInterface();

		
		if(bModelDirty)
		{
			{
				FQueryDescription CountQueryDescription = Model->GenerateNoSelectQueryDescription();
	
				if (CountQueryHandle != InvalidQueryHandle)
				{
					TedsInterface.UnregisterQuery(CountQueryHandle);
					CountQueryHandle = InvalidQueryHandle;
				}
				CountQueryHandle = TedsInterface.RegisterQuery(MoveTemp(CountQueryDescription));
			}

			{
				FQueryDescription TableViewerQueryDescription = Model->GenerateQueryDescription();

				RowHandle GeneralPurposeRowHandle = UiProvider->FindPurpose(UiProvider->GetGeneralWidgetPurposeID());
				RowHandle DefaultPurposeRowHandle = UiProvider->FindPurpose(UiProvider->GetDefaultWidgetPurposeID());

				// Temporarily add the default purpose as a parent of the general purpose so the debugger can support both
				TedsInterface.AddColumn(GeneralPurposeRowHandle, FTableRowParentColumn{.Parent = DefaultPurposeRowHandle});

				// Update the columns in the table viewer using the selection types from the query description
				TableViewer->SetColumns(TArray<TWeakObjectPtr<const UScriptStruct>>(TableViewerQueryDescription.SelectionTypes));

				// Remove the parenting chain after we have used it to generate widgets
				TedsInterface.RemoveColumns<FTableRowParentColumn>(GeneralPurposeRowHandle);

				// Since setting the columns clears all columns, we are going to re-add the custom column back
				if(RowHandleColumn)
				{
					TableViewer->AddCustomColumn(RowHandleColumn.ToSharedRef());
				}
		
				if (TableViewerQueryHandle != InvalidQueryHandle)
				{
					TedsInterface.UnregisterQuery(TableViewerQueryHandle);
					TableViewerQueryHandle = InvalidQueryHandle;
				}

				// Mass doesn't like empty queries, so we only set it if there are actual conditions
				if(TableViewerQueryDescription.ConditionTypes.Num() || TableViewerQueryDescription.SelectionTypes.Num())
				{
					TableViewerQueryHandle = TedsInterface.RegisterQuery(MoveTemp(TableViewerQueryDescription));
				}
			}
	
			bModelDirty = false;
		}

		TSet<RowHandle> NewTableViewerRows_Set;
	
		// Every frame we re-run the query to update the rows the table viewer is showing
		if(TableViewerQueryHandle != InvalidQueryHandle)
		{
			NewTableViewerRows_Set.Reserve(TableViewerRows_Set.Num());

			FQueryResult QueryResult = Model->GetTedsInterface().RunQuery(TableViewerQueryHandle,
				CreateDirectQueryCallbackBinding([&NewTableViewerRows_Set](const IDirectQueryContext& Context, const RowHandle*)
				{
					NewTableViewerRows_Set.Append(Context.GetRowHandles());
				}));
		}
	
		// Check if the two sets are equal, i.e not changes and no need to update the table viewer
		const bool bSetsEqual = (TableViewerRows_Set.Num() == NewTableViewerRows_Set.Num()) && TableViewerRows_Set.Includes(NewTableViewerRows_Set);

		if(!bSetsEqual)
		{
			Swap(TableViewerRows_Set, NewTableViewerRows_Set);
			TableViewerRows = TableViewerRows_Set.Array();

			RowQueryStack->ResetView(FRowHandleArrayView(TableViewerRows.GetData(), TableViewerRows.Num(),
				FRowHandleArrayView::EFlags::IsSorted | FRowHandleArrayView::EFlags::IsUnique));
		}
	}

	void SResultsView::OnModelChanged()
	{
		bModelDirty = true;
		Invalidate(EInvalidateWidgetReason::Layout);
	}

	void SResultsView::CreateRowHandleColumn()
	{
		auto AssignWidgetToColumn = [this](TUniquePtr<FTypedElementWidgetConstructor> Constructor, TConstArrayView<TWeakObjectPtr<const UScriptStruct>>)
		{
			TSharedPtr<FTypedElementWidgetConstructor> WidgetConstructor(Constructor.Release());
			RowHandleColumn = MakeShared<FTedsTableViewerColumn>(TEXT("Row Handle"), WidgetConstructor);
			return false;
		};
	

		UiProvider->CreateWidgetConstructors(
			UiProvider->FindPurpose(IUiProvider::FPurposeInfo("General", "Cell", "RowHandle").GeneratePurposeID()),
			FMetaDataView(), AssignWidgetToColumn);
	}
} // namespace UE::Editor::DataStorage::Debug::QueryEditor

#undef LOCTEXT_NAMESPACE
