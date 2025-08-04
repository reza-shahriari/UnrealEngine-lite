// Copyright Epic Games, Inc. All Rights Reserved.

#include "StateTreeTransitionDetails.h"
#include "Debugger/StateTreeDebuggerUIExtensions.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDetailChildrenBuilder.h"
#include "IPropertyUtilities.h"
#include "ScopedTransaction.h"
#include "StateTreeDescriptionHelpers.h"
#include "StateTreeEditor.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNodeUtils.h"
#include "StateTreeEditorStyle.h"
#include "StateTreePropertyHelpers.h"
#include "StateTreeTypes.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SButton.h"
#include "TextStyleDecorator.h"
#include "Framework/Application/SlateApplication.h"
#include "StateTreeBindingExtension.h"
#include "StateTreeScopedEditorDataFixer.h"

#define LOCTEXT_NAMESPACE "StateTreeEditor"

TSharedRef<IPropertyTypeCustomization> FStateTreeTransitionDetails::MakeInstance()
{
	return MakeShareable(new FStateTreeTransitionDetails);
}

void FStateTreeTransitionDetails::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
	StructProperty = StructPropertyHandle;
	PropUtils = StructCustomizationUtils.GetPropertyUtilities();

	// Find StateTreeEditorData associated with this panel.
	UStateTreeEditorData* EditorData = nullptr;
	const TArray<TWeakObjectPtr<>>& Objects = PropUtils->GetSelectedObjects();
	for (const TWeakObjectPtr<>& WeakObject : Objects)
	{
		if (const UObject* Object = WeakObject.Get())
		{
			if (UStateTreeEditorData* OuterEditorData = Object->GetTypedOuter<UStateTreeEditorData>())
			{
				EditorData = OuterEditorData;
				break;
			}
		}
	}

	TriggerProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, Trigger));
	PriorityProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, Priority));
	RequiredEventProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, RequiredEvent));
	DelegateListener = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, DelegateListener));
	StateProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, State));
	DelayTransitionProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, bDelayTransition));
	DelayDurationProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, DelayDuration));
	DelayRandomVarianceProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, DelayRandomVariance));
	ConditionsProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, Conditions));
	IDProperty = StructProperty->GetChildHandle(GET_MEMBER_NAME_CHECKED(FStateTreeTransition, ID));

	HeaderRow
		.RowTag(StructProperty->GetProperty()->GetFName())
		.WholeRowContent()
		.VAlign(VAlign_Center)
		[
			// Border to capture mouse clicks on the row (used for right click menu).
			SAssignNew(RowBorder, SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(0.f)
			.ForegroundColor(this, &FStateTreeTransitionDetails::GetContentRowColor)
			.OnMouseButtonDown(this, &FStateTreeTransitionDetails::OnRowMouseDown)
			.OnMouseButtonUp(this, &FStateTreeTransitionDetails::OnRowMouseUp)
			[

				SNew(SHorizontalBox)

				// Icon
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
				[
					SNew(SImage)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Image(FStateTreeEditorStyle::Get().GetBrush("StateTreeEditor.Transition.Goto"))
				]

				// Description
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 1.f, 0.f, 0.f))
				[
					SNew(SRichTextBlock)
					.Text(this, &FStateTreeTransitionDetails::GetDescription)
					.TextStyle(&FStateTreeEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>("Details.Normal"))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					+SRichTextBlock::Decorator(FTextStyleDecorator::Create(TEXT(""), FStateTreeEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>("Details.Normal")))
					+SRichTextBlock::Decorator(FTextStyleDecorator::Create(TEXT("b"), FStateTreeEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>("Details.Bold")))
					+SRichTextBlock::Decorator(FTextStyleDecorator::Create(TEXT("i"), FStateTreeEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>("Details.Italic")))
					+SRichTextBlock::Decorator(FTextStyleDecorator::Create(TEXT("s"), FStateTreeEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>("Details.Subdued")))
				]
				// Debug and property widgets
				+ SHorizontalBox::Slot()
				.FillContentWidth(1.0f, 0.0f) // grow, no shrinking
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.Padding(FMargin(8.f, 0.f, 2.f, 0.f))
				[
					SNew(SHorizontalBox)
					// Debugger labels
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						UE::StateTreeEditor::DebuggerExtensions::CreateTransitionWidget(StructPropertyHandle, EditorData)
					]
					
					// Options
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SComboButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.OnGetMenuContent(this, &FStateTreeTransitionDetails::GenerateOptionsMenu)
						.ToolTipText(LOCTEXT("ItemActions", "Item actions"))
						.HasDownArrow(false)
						.ContentPadding(FMargin(4.f, 2.f))
						.ButtonContent()
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.ChevronDown"))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
				]
			]
		]
		.CopyAction(FUIAction(FExecuteAction::CreateSP(this, &FStateTreeTransitionDetails::OnCopyTransition)))
		.PasteAction(FUIAction(FExecuteAction::CreateSP(this, &FStateTreeTransitionDetails::OnPasteTransition)));
}
 
void FStateTreeTransitionDetails::CustomizeChildren(TSharedRef<class IPropertyHandle> StructPropertyHandle, class IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
	check(TriggerProperty);
	check(RequiredEventProperty);
	check(DelegateListener);
	check(DelayTransitionProperty);
	check(DelayDurationProperty);
	check(DelayRandomVarianceProperty);
	check(StateProperty);
	check(ConditionsProperty);
	check(IDProperty);

	TWeakPtr<FStateTreeTransitionDetails> WeakSelf = SharedThis(this);
	auto IsNotCompletionTransition = [WeakSelf]()
	{
		if (const TSharedPtr<FStateTreeTransitionDetails> Self = WeakSelf.Pin())
		{
			return !EnumHasAnyFlags(Self->GetTrigger(), EStateTreeTransitionTrigger::OnStateCompleted) ? EVisibility::Visible : EVisibility::Collapsed;
		}
		return EVisibility::Collapsed;
	};

	if (UE::StateTree::Editor::GbDisplayItemIds)
	{
		StructBuilder.AddProperty(IDProperty.ToSharedRef());
	}
	
	// Trigger
	StructBuilder.AddProperty(TriggerProperty.ToSharedRef());

	// Show event only when the trigger is set to Event. 
	StructBuilder.AddProperty(RequiredEventProperty.ToSharedRef())
		.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda([WeakSelf]()
		{
			if (const TSharedPtr<FStateTreeTransitionDetails> Self = WeakSelf.Pin())
			{
				return (Self->GetTrigger() == EStateTreeTransitionTrigger::OnEvent) ? EVisibility::Visible : EVisibility::Collapsed;
			}
			return EVisibility::Collapsed;
		})));

	IDetailPropertyRow& DelegateDispatcherRow = StructBuilder.AddProperty(DelegateListener.ToSharedRef())
		.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda([WeakSelf]()
		{
			if (const TSharedPtr<FStateTreeTransitionDetails> Self = WeakSelf.Pin())
			{
				return (Self->GetTrigger() == EStateTreeTransitionTrigger::OnDelegate) ? EVisibility::Visible : EVisibility::Collapsed;
			}
			return EVisibility::Collapsed;
		})));

	FGuid ID;
	UE::StateTree::PropertyHelpers::GetStructValue<FGuid>(IDProperty, ID);
	DelegateListener->SetInstanceMetaData(UE::PropertyBinding::MetaDataStructIDName, LexToString(ID));

	// State
	StructBuilder.AddProperty(StateProperty.ToSharedRef());

	// Priority
	StructBuilder.AddProperty(PriorityProperty.ToSharedRef())
		.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda(IsNotCompletionTransition)));

	// Delay
	StructBuilder.AddProperty(DelayTransitionProperty.ToSharedRef())
		.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda(IsNotCompletionTransition)));
	StructBuilder.AddProperty(DelayDurationProperty.ToSharedRef())
		.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda(IsNotCompletionTransition)));
	StructBuilder.AddProperty(DelayRandomVarianceProperty.ToSharedRef())
		.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda(IsNotCompletionTransition)));

	// Show conditions always expanded, with simplified header (remove item count)
	IDetailPropertyRow& ConditionsRow = StructBuilder.AddProperty(ConditionsProperty.ToSharedRef());
	ConditionsRow.ShouldAutoExpand(true);

	constexpr bool bShowChildren = true;
	ConditionsRow.CustomWidget(bShowChildren)
		.RowTag(ConditionsProperty->GetProperty()->GetFName())
		.WholeRowContent()
		[
			SNew(SHorizontalBox)

			// Condition text
			+SHorizontalBox::Slot()
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				SNew(STextBlock)
				.Text(ConditionsProperty->GetPropertyDisplayName())
				.Font(IDetailLayoutBuilder::GetDetailFontBold())
			]

			// Conditions button
			+SHorizontalBox::Slot()
			.FillWidth(1.f)
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.Padding(FMargin(0, 0, 3, 0))
				[
					UE::StateTreeEditor::EditorNodeUtils::CreateAddNodePickerComboButton(
						LOCTEXT("TransitionConditionAddTooltip", "Add new Transition Condition"),
						UE::StateTree::Colors::Grey,
						ConditionsProperty,
						PropUtils.ToSharedRef())
				]
			]
		];
}

FSlateColor FStateTreeTransitionDetails::GetContentRowColor() const
{
	return UE::StateTreeEditor::DebuggerExtensions::IsTransitionEnabled(StructProperty)
		? FSlateColor::UseForeground()
		: FSlateColor::UseSubduedForeground();
}

FReply FStateTreeTransitionDetails::OnRowMouseDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply FStateTreeTransitionDetails::OnRowMouseUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		FWidgetPath WidgetPath = MouseEvent.GetEventPath() != nullptr ? *MouseEvent.GetEventPath() : FWidgetPath();
		FSlateApplication::Get().PushMenu(
			RowBorder.ToSharedRef(),
			WidgetPath,
			GenerateOptionsMenu(),
			MouseEvent.GetScreenSpacePosition(),
			FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

		return FReply::Handled();
	}
	
	return FReply::Unhandled();
}

TSharedRef<SWidget> FStateTreeTransitionDetails::GenerateOptionsMenu()
{
	FMenuBuilder MenuBuilder(/*ShouldCloseWindowAfterMenuSelection*/true, /*CommandList*/nullptr);

	MenuBuilder.BeginSection(FName("Edit"), LOCTEXT("Edit", "Edit"));

	// Copy
	MenuBuilder.AddMenuEntry(
		LOCTEXT("CopyItem", "Copy"),
		LOCTEXT("CopyItemTooltip", "Copy this item"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(
			FExecuteAction::CreateSP(this, &FStateTreeTransitionDetails::OnCopyTransition),
			FCanExecuteAction()
		));

	// Paste
	MenuBuilder.AddMenuEntry(
		LOCTEXT("PasteItem", "Paste"),
		LOCTEXT("PasteItemTooltip", "Paste into this item"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Paste"),
		FUIAction(
			FExecuteAction::CreateSP(this, &FStateTreeTransitionDetails::OnPasteTransition),
			FCanExecuteAction()
		));

	// Duplicate
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DuplicateItem", "Duplicate"),
		LOCTEXT("DuplicateItemTooltip", "Duplicate this item"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Duplicate"),
		FUIAction(
			FExecuteAction::CreateSP(this, &FStateTreeTransitionDetails::OnDuplicateTransition),
			FCanExecuteAction()
		));

	// Delete
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DeleteItem", "Delete"),
		LOCTEXT("DeleteItemTooltip", "Delete this item"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Delete"),
		FUIAction(
			FExecuteAction::CreateSP(this, &FStateTreeTransitionDetails::OnDeleteTransition),
			FCanExecuteAction()
		));

	// Delete
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DeleteAllItems", "Delete all"),
		LOCTEXT("DeleteAllItemsTooltip", "Delete all items"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Delete"),
		FUIAction(
			FExecuteAction::CreateSP(this, &FStateTreeTransitionDetails::OnDeleteAllTransitions),
			FCanExecuteAction()
		));

	MenuBuilder.EndSection();

	// Append debugger items.
	UE::StateTreeEditor::DebuggerExtensions::AppendTransitionMenuItems(MenuBuilder, StructProperty, GetEditorData());

	return MenuBuilder.MakeWidget();
}

void FStateTreeTransitionDetails::OnDeleteTransition() const
{
	const int32 Index = StructProperty->GetArrayIndex();
	if (const TSharedPtr<IPropertyHandle> ParentHandle = StructProperty->GetParentHandle())
	{
		if (const TSharedPtr<IPropertyHandleArray> ArrayHandle = ParentHandle->AsArray())
		{
			if (UStateTreeEditorData* EditorData = GetEditorData())
			{
				FScopedTransaction Transaction(LOCTEXT("DeleteTransition", "Delete Transition"));
				EditorData->Modify();

				ArrayHandle->DeleteItem(Index);

				UE::StateTreeEditor::FScopedEditorDataFixer DataFixer(EditorData);
				DataFixer.bRemoveInvalidBindings = true;
			}
		}
	}
}

void FStateTreeTransitionDetails::OnDeleteAllTransitions() const
{
	if (const TSharedPtr<IPropertyHandle> ParentHandle = StructProperty->GetParentHandle())
	{
		if (const TSharedPtr<IPropertyHandleArray> ArrayHandle = ParentHandle->AsArray())
		{
			if (UStateTreeEditorData* EditorData = GetEditorData())
			{
				FScopedTransaction Transaction(LOCTEXT("DeleteAllTransitions", "Delete All Transitions"));
				EditorData->Modify();

				ArrayHandle->EmptyArray();

				UE::StateTreeEditor::FScopedEditorDataFixer DataFixer(EditorData);
				DataFixer.bRemoveInvalidBindings = true;
			}
		}
	}
}

void FStateTreeTransitionDetails::OnDuplicateTransition() const
{
	TArray<UObject*> OuterObjects;
	StructProperty->GetOuterObjects(OuterObjects);
	if (OuterObjects.IsEmpty())
	{
		return;
	}

	const int32 ArrayIndex = StructProperty->GetArrayIndex();
	if (const TSharedPtr<IPropertyHandle> ParentHandle = StructProperty->GetParentHandle())
	{
		if (const TSharedPtr<IPropertyHandleArray> ArrayHandle = ParentHandle->AsArray())
		{
			if (UStateTreeEditorData* EditorData = GetEditorData())
			{
				FScopedTransaction Transaction(LOCTEXT("DuplicateTransition", "Duplicate Transition"));

				EditorData->Modify();

				ArrayHandle->DuplicateItem(ArrayIndex);

				TSharedRef<IPropertyHandle> DuplicatedStructHandle = ArrayHandle->GetElement(ArrayIndex);

				{
					UE::StateTreeEditor::FScopedEditorDataFixer DataFixer(EditorData);

					TArray<void*> RawNodeData;
					StructProperty->AccessRawData(RawNodeData);
					for (int32 Index = 0; Index < RawNodeData.Num(); ++Index)
					{
						if (FStateTreeTransition* Transition = static_cast<FStateTreeTransition*>(RawNodeData[Index]))
						{
							constexpr bool bShouldCopyBindings = true;
							constexpr bool bShouldReinstantiateInstanceObjects = false; // ArrayHandle has already deep copied UObjects
							constexpr bool bShouldRegenerateGUID = true;
							DataFixer.TransitionsToFix.Emplace(OuterObjects[Index], *Transition, bShouldCopyBindings, bShouldReinstantiateInstanceObjects, bShouldRegenerateGUID);
						}
					}
				}
			}
		}
	}
}

EStateTreeTransitionTrigger FStateTreeTransitionDetails::GetTrigger() const
{
	check(TriggerProperty);
	EStateTreeTransitionTrigger TriggerValue = EStateTreeTransitionTrigger::None;
	if (TriggerProperty.IsValid())
	{
		TriggerProperty->GetValue((uint8&)TriggerValue);
	}
	return TriggerValue;
}

bool FStateTreeTransitionDetails::GetDelayTransition() const
{
	check(DelayTransitionProperty);
	bool bDelayTransition = false;
	if (DelayTransitionProperty.IsValid())
	{
		DelayTransitionProperty->GetValue(bDelayTransition);
	}
	return bDelayTransition;
}

FText FStateTreeTransitionDetails::GetDescription() const
{
	check(StateProperty);

	const FStateTreeTransition* Transition = UE::StateTree::PropertyHelpers::GetStructPtr<FStateTreeTransition>(StructProperty);
	if (!Transition)
	{
		return LOCTEXT("MultipleSelected", "Multiple Selected");
	}

	return UE::StateTree::Editor::GetTransitionDesc(GetEditorData(), *Transition, EStateTreeNodeFormatting::RichText);
}

void FStateTreeTransitionDetails::OnCopyTransition() const
{
	FString Value;
	// Use PPF_Copy so that all properties get copied.
	if (StructProperty->GetValueAsFormattedString(Value, PPF_Copy) == FPropertyAccess::Success)
	{
		FPlatformApplicationMisc::ClipboardCopy(*Value);
	} 
}

UStateTreeEditorData* FStateTreeTransitionDetails::GetEditorData() const
{
	TArray<UObject*> OuterObjects;
	StructProperty->GetOuterObjects(OuterObjects);
	for (UObject* Outer : OuterObjects)
	{
		UStateTreeEditorData* OuterEditorData = Cast<UStateTreeEditorData>(Outer);
		if (OuterEditorData == nullptr)
		{
			OuterEditorData = Outer->GetTypedOuter<UStateTreeEditorData>();
		}
		if (OuterEditorData)
		{
			return OuterEditorData;
		}
	}
	return nullptr;
}

void FStateTreeTransitionDetails::OnPasteTransition() const
{
	TArray<UObject*> OuterObjects;
	StructProperty->GetOuterObjects(OuterObjects);
	if (OuterObjects.IsEmpty())
	{
		return;
	}

	FString PastedText;
	FPlatformApplicationMisc::ClipboardPaste(PastedText);

	if (PastedText.IsEmpty())
	{
		return;
	}

	if (UStateTreeEditorData* EditorData = GetEditorData())
	{
		FScopedTransaction Transaction(LOCTEXT("PasteTransition", "Paste Transition"));

		TArray<void*> RawData;
		StructProperty->AccessRawData(RawData);

		// Might modify the bindings
		EditorData->Modify();

		// Make sure we instantiate new objects when setting the value.
		// Calls NotifyPreChange for us, will also call NotifyFinishedChangingProperties
		StructProperty->SetValueFromFormattedString(PastedText, EPropertyValueSetFlags::InstanceObjects);

		{
			UE::StateTreeEditor::FScopedEditorDataFixer DataFixer(EditorData);

			for (int32 Index = 0; Index < RawData.Num(); Index++)
			{
				if (FStateTreeTransition* Transition = static_cast<FStateTreeTransition*>(RawData[Index]))
				{
					constexpr bool bShouldCopyBindings = true;
					constexpr bool bShouldReinstantiateInstanceData = false; // StructProperty has already done UObject deep copies
					constexpr bool bShouldRegenerateGUID = true;
					DataFixer.TransitionsToFix.Emplace(OuterObjects[Index], *Transition, bShouldCopyBindings, bShouldReinstantiateInstanceData, bShouldRegenerateGUID);
				}
			}
		}

		StructProperty->NotifyPostChange(EPropertyChangeType::ValueSet);
		StructProperty->NotifyFinishedChangingProperties();
	}

	if (PropUtils)
	{
		PropUtils->ForceRefresh();
	}
}

#undef LOCTEXT_NAMESPACE
