// Copyright Epic Games, Inc. All Rights Reserved.

#include "STimingProfilerWindow.h"

#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Docking/WorkspaceItem.h"
#include "Modules/ModuleManager.h"
#include "SlateOptMacros.h"
#include "Widgets/Docking/SDockTab.h"

// TraceInsights
#include "Insights/InsightsStyle.h"
#include "Insights/TimingProfiler/TimingProfilerManager.h"
#include "Insights/TimingProfiler/Widgets/SFrameTrack.h"
#include "Insights/TimingProfiler/Widgets/SStatsView.h"
#include "Insights/TimingProfiler/Widgets/STimersView.h"
#include "Insights/TimingProfiler/Widgets/STimerTreeView.h"
#include "Insights/TimingProfiler/Widgets/STimingProfilerToolbar.h"
#include "Insights/ViewModels/TimingGraphTrack.h"
#include "Insights/Widgets/SLogView.h"
#include "Insights/Widgets/STimingView.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

const FName FTimingProfilerTabs::ToolbarID(TEXT("Toolbar")); // DEPRECATED
const FName FTimingProfilerTabs::FramesTrackID(TEXT("Frames"));
const FName FTimingProfilerTabs::TimingViewID(TEXT("TimingView"));
const FName FTimingProfilerTabs::TimersID(TEXT("Timers"));
const FName FTimingProfilerTabs::CallersID(TEXT("Callers"));
const FName FTimingProfilerTabs::CalleesID(TEXT("Callees"));
const FName FTimingProfilerTabs::StatsCountersID(TEXT("StasCounters"));
const FName FTimingProfilerTabs::LogViewID(TEXT("LogView"));

////////////////////////////////////////////////////////////////////////////////////////////////////

#define LOCTEXT_NAMESPACE "UE::Insights::TimingProfiler"

namespace UE::Insights::TimingProfiler
{

////////////////////////////////////////////////////////////////////////////////////////////////////
// STimingProfilerWindow
////////////////////////////////////////////////////////////////////////////////////////////////////

STimingProfilerWindow::STimingProfilerWindow()
	: SMajorTabWindow(FInsightsManagerTabs::TimingProfilerTabId)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////

STimingProfilerWindow::~STimingProfilerWindow()
{
	CloseAllOpenTabs();

	check(LogView == nullptr);
	check(StatsView == nullptr);
	check(CalleesTreeView == nullptr);
	check(CallersTreeView == nullptr);
	check(TimersView == nullptr);
	check(TimingView == nullptr);
	check(FrameTrack == nullptr);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

const TCHAR* STimingProfilerWindow::GetAnalyticsEventName() const
{
	return TEXT("Insights.Usage.TimingProfiler");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::Reset()
{
	if (FrameTrack)
	{
		FrameTrack->Reset();
	}

	if (TimingView)
	{
		TimingView->Reset();
	}

	if (TimersView)
	{
		TimersView->Reset();
	}

	if (CallersTreeView)
	{
		CallersTreeView->Reset();
	}

	if (CalleesTreeView)
	{
		CalleesTreeView->Reset();
	}

	if (StatsView)
	{
		StatsView->Reset();
	}

	if (LogView)
	{
		LogView->Reset();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SDockTab> STimingProfilerWindow::SpawnTab_FramesTrack(const FSpawnTabArgs& Args)
{
	FTimingProfilerManager::Get()->SetFramesTrackVisible(true);

	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.ShouldAutosize(false)
		.TabRole(ETabRole::PanelTab)
		[
			SAssignNew(FrameTrack, SFrameTrack)
		];

	DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &STimingProfilerWindow::OnFramesTrackTabClosed));
	AddOpenTab(DockTab);

	return DockTab;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnFramesTrackTabClosed(TSharedRef<SDockTab> TabBeingClosed)
{
	FTimingProfilerManager::Get()->SetFramesTrackVisible(false);
	FrameTrack = nullptr;

	RemoveOpenTab(TabBeingClosed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SDockTab> STimingProfilerWindow::SpawnTab_TimingView(const FSpawnTabArgs& Args)
{
	FTimingProfilerManager::Get()->SetTimingViewVisible(true);

	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.ShouldAutosize(false)
		.TabRole(ETabRole::PanelTab)
		[
			SAssignNew(TimingView, STimingView, FInsightsManagerTabs::TimingProfilerTabId)
		];

	TimingView->Reset(true);
	TimingView->OnSelectionChanged().AddSP(this, &STimingProfilerWindow::OnTimeSelectionChanged);
	const double SelectionStartTime = FTimingProfilerManager::Get()->GetSelectionStartTime();
	const double SelectionEndTime = FTimingProfilerManager::Get()->GetSelectionEndTime();
	TimingView->SelectTimeInterval(SelectionStartTime, SelectionEndTime - SelectionStartTime);

	DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &STimingProfilerWindow::OnTimingViewTabClosed));
	AddOpenTab(DockTab);

	return DockTab;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnTimingViewTabClosed(TSharedRef<SDockTab> TabBeingClosed)
{
	FTimingProfilerManager::Get()->SetTimingViewVisible(false);
	if (TimingView)
	{
		TimingView->OnSelectionChanged().RemoveAll(this);
		TimingView = nullptr;
	}

	RemoveOpenTab(TabBeingClosed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SDockTab> STimingProfilerWindow::SpawnTab_Timers(const FSpawnTabArgs& Args)
{
	FTimingProfilerManager::Get()->SetTimersViewVisible(true);

	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.ShouldAutosize(false)
		.TabRole(ETabRole::PanelTab)
		[
			SAssignNew(TimersView, STimersView)
		];

	DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &STimingProfilerWindow::OnTimersTabClosed));
	AddOpenTab(DockTab);

	return DockTab;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnTimersTabClosed(TSharedRef<SDockTab> TabBeingClosed)
{
	FTimingProfilerManager::Get()->SetTimersViewVisible(false);
	TimersView = nullptr;

	RemoveOpenTab(TabBeingClosed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SDockTab> STimingProfilerWindow::SpawnTab_Callers(const FSpawnTabArgs& Args)
{
	FTimingProfilerManager::Get()->SetCallersTreeViewVisible(true);

	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.ShouldAutosize(false)
		.TabRole(ETabRole::PanelTab)
		[
			SAssignNew(CallersTreeView, STimerTreeView, LOCTEXT("CallersTreeViewName", "Callers"))
		];

	DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &STimingProfilerWindow::OnCallersTabClosed));
	AddOpenTab(DockTab);

	return DockTab;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnCallersTabClosed(TSharedRef<SDockTab> TabBeingClosed)
{
	FTimingProfilerManager::Get()->SetCallersTreeViewVisible(false);
	CallersTreeView = nullptr;

	RemoveOpenTab(TabBeingClosed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SDockTab> STimingProfilerWindow::SpawnTab_Callees(const FSpawnTabArgs& Args)
{
	FTimingProfilerManager::Get()->SetCalleesTreeViewVisible(true);

	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.ShouldAutosize(false)
		.TabRole(ETabRole::PanelTab)
		[
			SAssignNew(CalleesTreeView, STimerTreeView, LOCTEXT("CalleesTreeViewName", "Callees"))
		];

	DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &STimingProfilerWindow::OnCalleesTabClosed));
	AddOpenTab(DockTab);

	return DockTab;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnCalleesTabClosed(TSharedRef<SDockTab> TabBeingClosed)
{
	FTimingProfilerManager::Get()->SetCalleesTreeViewVisible(false);
	CalleesTreeView = nullptr;

	RemoveOpenTab(TabBeingClosed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SDockTab> STimingProfilerWindow::SpawnTab_StatsCounters(const FSpawnTabArgs& Args)
{
	FTimingProfilerManager::Get()->SetStatsCountersViewVisible(true);

	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.ShouldAutosize(false)
		.TabRole(ETabRole::PanelTab)
		[
			SAssignNew(StatsView, SStatsView)
		];

	DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &STimingProfilerWindow::OnStatsCountersTabClosed));
	AddOpenTab(DockTab);

	return DockTab;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnStatsCountersTabClosed(TSharedRef<SDockTab> TabBeingClosed)
{
	FTimingProfilerManager::Get()->SetStatsCountersViewVisible(false);
	StatsView = nullptr;

	RemoveOpenTab(TabBeingClosed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SDockTab> STimingProfilerWindow::SpawnTab_LogView(const FSpawnTabArgs& Args)
{
	FTimingProfilerManager::Get()->SetLogViewVisible(true);

	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.ShouldAutosize(false)
		.TabRole(ETabRole::PanelTab)
		[
			SAssignNew(LogView, SLogView)
		];

	DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &STimingProfilerWindow::OnLogViewTabClosed));
	AddOpenTab(DockTab);

	return DockTab;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnLogViewTabClosed(TSharedRef<SDockTab> TabBeingClosed)
{
	FTimingProfilerManager::Get()->SetLogViewVisible(false);
	LogView = nullptr;

	RemoveOpenTab(TabBeingClosed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::Construct(const FArguments& InArgs, const TSharedRef<SDockTab>& ConstructUnderMajorTab, const TSharedPtr<SWindow>& ConstructUnderWindow)
{
	TSharedPtr<FTimingProfilerManager> TimingProfilerManager = FTimingProfilerManager::Get();
	ensure(TimingProfilerManager.IsValid());

	SetCommandList(TimingProfilerManager->GetCommandList());

	SMajorTabWindow::FArguments Args;
	SMajorTabWindow::Construct(Args, ConstructUnderMajorTab, ConstructUnderWindow);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TSharedRef<FWorkspaceItem> STimingProfilerWindow::CreateWorkspaceMenuGroup()
{
	return GetTabManager()->AddLocalWorkspaceMenuCategory(LOCTEXT("TimingProfilerMenuGroupName", "Timing Insights"));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::RegisterTabSpawners()
{
	check(GetTabManager().IsValid());
	FTabManager* TabManagerPtr = GetTabManager().Get();
	check(GetWorkspaceMenuGroup().IsValid());
	const TSharedRef<FWorkspaceItem> Group = GetWorkspaceMenuGroup().ToSharedRef();

	IUnrealInsightsModule& InsightsModule = FModuleManager::LoadModuleChecked<IUnrealInsightsModule>("TraceInsights");
	const FInsightsMajorTabConfig& Config = InsightsModule.FindMajorTabConfig(FInsightsManagerTabs::TimingProfilerTabId);

	if (Config.ShouldRegisterMinorTab(FTimingProfilerTabs::FramesTrackID))
	{
		TabManagerPtr->RegisterTabSpawner(FTimingProfilerTabs::FramesTrackID, FOnSpawnTab::CreateRaw(this, &STimingProfilerWindow::SpawnTab_FramesTrack))
			.SetDisplayName(LOCTEXT("FramesTrackTabTitle", "Frames"))
			.SetIcon(FSlateIcon(FInsightsStyle::GetStyleSetName(), "Icons.FramesTrack"))
			.SetGroup(Group);
	}

	if (Config.ShouldRegisterMinorTab(FTimingProfilerTabs::TimingViewID))
	{
		TabManagerPtr->RegisterTabSpawner(FTimingProfilerTabs::TimingViewID, FOnSpawnTab::CreateRaw(this, &STimingProfilerWindow::SpawnTab_TimingView))
			.SetDisplayName(LOCTEXT("TimingViewTabTitle", "Timing View"))
			.SetIcon(FSlateIcon(FInsightsStyle::GetStyleSetName(), "Icons.TimingView"))
			.SetGroup(Group);
	}

	if (Config.ShouldRegisterMinorTab(FTimingProfilerTabs::TimersID))
	{
		TabManagerPtr->RegisterTabSpawner(FTimingProfilerTabs::TimersID, FOnSpawnTab::CreateRaw(this, &STimingProfilerWindow::SpawnTab_Timers))
			.SetDisplayName(LOCTEXT("TimersTabTitle", "Timers"))
			.SetIcon(FSlateIcon(FInsightsStyle::GetStyleSetName(), "Icons.TimersView"))
			.SetGroup(Group);
	}

	if (Config.ShouldRegisterMinorTab(FTimingProfilerTabs::CallersID))
	{
		TabManagerPtr->RegisterTabSpawner(FTimingProfilerTabs::CallersID, FOnSpawnTab::CreateRaw(this, &STimingProfilerWindow::SpawnTab_Callers))
			.SetDisplayName(LOCTEXT("CallersTabTitle", "Callers"))
			.SetIcon(FSlateIcon(FInsightsStyle::GetStyleSetName(), "Icons.CallersView"))
			.SetGroup(Group);
	}

	if (Config.ShouldRegisterMinorTab(FTimingProfilerTabs::CalleesID))
	{
		TabManagerPtr->RegisterTabSpawner(FTimingProfilerTabs::CalleesID, FOnSpawnTab::CreateRaw(this, &STimingProfilerWindow::SpawnTab_Callees))
			.SetDisplayName(LOCTEXT("CalleesTabTitle", "Callees"))
			.SetIcon(FSlateIcon(FInsightsStyle::GetStyleSetName(), "Icons.CalleesView"))
			.SetGroup(Group);
	}

	if (Config.ShouldRegisterMinorTab(FTimingProfilerTabs::StatsCountersID))
	{
		TabManagerPtr->RegisterTabSpawner(FTimingProfilerTabs::StatsCountersID, FOnSpawnTab::CreateRaw(this, &STimingProfilerWindow::SpawnTab_StatsCounters))
			.SetDisplayName(LOCTEXT("StatsCountersTabTitle", "Counters"))
			.SetIcon(FSlateIcon(FInsightsStyle::GetStyleSetName(), "Icons.CountersView"))
			.SetGroup(Group);
	}

	if (Config.ShouldRegisterMinorTab(FTimingProfilerTabs::LogViewID))
	{
		TabManagerPtr->RegisterTabSpawner(FTimingProfilerTabs::LogViewID, FOnSpawnTab::CreateRaw(this, &STimingProfilerWindow::SpawnTab_LogView))
			.SetDisplayName(LOCTEXT("LogViewTabTitle", "Log View"))
			.SetIcon(FSlateIcon(FInsightsStyle::GetStyleSetName(), "Icons.LogView"))
			.SetGroup(Group);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<FTabManager::FLayout> STimingProfilerWindow::CreateDefaultTabLayout() const
{
	return FTabManager::NewLayout("InsightsTimingProfilerLayout_v1.2")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				FTabManager::NewSplitter()
				->SetSizeCoefficient(0.65f)
				->SetOrientation(Orient_Vertical)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.1f)
					->SetHideTabWell(true)
					->AddTab(FTimingProfilerTabs::FramesTrackID, ETabState::OpenedTab)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.5f)
					->SetHideTabWell(true)
					->AddTab(FTimingProfilerTabs::TimingViewID, ETabState::OpenedTab)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.2f)
					->SetHideTabWell(true)
					->AddTab(FTimingProfilerTabs::LogViewID, ETabState::OpenedTab)
				)
			)
			->Split
			(
				FTabManager::NewSplitter()
				->SetSizeCoefficient(0.35f)
				->SetOrientation(Orient_Vertical)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.67f)
					->AddTab(FTimingProfilerTabs::TimersID, ETabState::OpenedTab)
					->AddTab(FTimingProfilerTabs::StatsCountersID, ETabState::OpenedTab)
					->SetForegroundTab(FTimingProfilerTabs::TimersID)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.165f)
					->SetHideTabWell(true)
					->AddTab(FTimingProfilerTabs::CallersID, ETabState::OpenedTab)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.165f)
					->SetHideTabWell(true)
					->AddTab(FTimingProfilerTabs::CalleesID, ETabState::OpenedTab)
				)
			)
		);
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////

TSharedRef<SWidget> STimingProfilerWindow::CreateToolbar(TSharedPtr<FExtender> Extender)
{
	IUnrealInsightsModule& InsightsModule = FModuleManager::LoadModuleChecked<IUnrealInsightsModule>("TraceInsights");
	const FInsightsMajorTabConfig& Config = InsightsModule.FindMajorTabConfig(FInsightsManagerTabs::TimingProfilerTabId);

	return SNew(STimingProfilerToolbar, Config).ToolbarExtender(Extender);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnTimeSelectionChanged(Timing::ETimeChangedFlags InFlags, double InStartTime, double InEndTime)
{
	if (InFlags != Timing::ETimeChangedFlags::Interactive)
	{
		FTimingProfilerManager::Get()->SetSelectedTimeRange(InStartTime, InEndTime);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool STimingProfilerWindow::IsTimerAddedToGraphs(uint32 TimerId) const
{
	if (FrameTrack.IsValid() &&
		FrameTrack->HasAnySeriesForTimer(TimerId))
	{
		return true;
	}

	if (TimingView.IsValid())
	{
		TSharedPtr<FTimingGraphTrack> GraphTrack = TimingView->GetMainTimingGraphTrack();
		if (GraphTrack.IsValid() &&
			GraphTrack->HasAnySeriesForTimer(TimerId))
		{
			return true;
		}
	}

	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void STimingProfilerWindow::OnTimerAddedToGraphsChanged(uint32 TimerId)
{
	const bool bIsAddedToGraphs = IsTimerAddedToGraphs(TimerId);

	if (TimersView.IsValid())
	{
		FTimerNodePtr TimerNode = TimersView->GetTimerNode(TimerId);
		if (TimerNode.IsValid())
		{
			TimerNode->SetAddedToGraphsFlag(bIsAddedToGraphs);
		}
	}

	if (CallersTreeView.IsValid())
	{
		FTimerNodePtr TimerNode = CallersTreeView->GetTimerNode(TimerId);
		if (TimerNode.IsValid())
		{
			TimerNode->SetAddedToGraphsFlag(bIsAddedToGraphs);
		}
	}

	if (CalleesTreeView.IsValid())
	{
		FTimerNodePtr TimerNode = CalleesTreeView->GetTimerNode(TimerId);
		if (TimerNode.IsValid())
		{
			TimerNode->SetAddedToGraphsFlag(bIsAddedToGraphs);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace UE::Insights::TimingProfiler

#undef LOCTEXT_NAMESPACE
