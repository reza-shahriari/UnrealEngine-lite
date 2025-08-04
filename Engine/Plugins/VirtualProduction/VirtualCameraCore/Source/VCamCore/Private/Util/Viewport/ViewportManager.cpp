﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "ViewportManager.h"

#include "LogVCamCore.h"
#include "VCamComponent.h"
#include "Output/VCamOutputProviderBase.h"

#include "Misc/CoreDelegates.h"

namespace UE::VCamCore
{
	FViewportManagerBase::FViewportManagerBase(
		IViewportLocker& ViewportLocker,
		IViewportResolutionChanger& ResolutionChanger,
		FOverrideShouldHaveOwnership OverrideShouldHaveOwnership
		)
		: ViewportLocker(ViewportLocker)
		, ResolutionChanger(ResolutionChanger)
		, OverrideShouldHaveOwnership(MoveTemp(OverrideShouldHaveOwnership))
		, LockManager(
			ViewportLocker,
			FViewportLockManager::FHasViewportOwnership::CreateRaw(this, &FViewportManagerBase::HasViewportOwnership)
		)
		, ResolutionManager(
			ResolutionChanger,
			FViewportLockManager::FHasViewportOwnership::CreateRaw(this, &FViewportManagerBase::HasViewportOwnership)
		)
	{
		FCoreDelegates::OnEndFrame.AddRaw(this, &FViewportManagerBase::OnEndOfFrame);
		ViewportOwnership.OnOwnershipChanged().AddLambda([this](auto, auto)
		{
			RequestLockRefresh();
		});
	}

	FViewportManagerBase::~FViewportManagerBase()
	{
		FCoreDelegates::OnEndFrame.RemoveAll(this);
	}

	void FViewportManagerBase::RegisterVCamComponent(UVCamComponent& Component)
	{
		if (!RegisteredVCams.Contains(&Component))
		{
			RegisteredVCams.Add(&Component);
			RequestLockRefresh();
		}
	}

	void FViewportManagerBase::UnregisterVCamComponent(UVCamComponent& Component)
	{
		if (RegisteredVCams.Contains(&Component))
		{
			RegisteredVCams.Remove(&Component);
			RemoveOwnership(Component);
		}
	}

	void FViewportManagerBase::OnEndOfFrame()
	{
		// This sets bHasRequestedLockRefresh if anything is removed
		const TInlineComponentArray<TWeakObjectPtr<UVCamComponent>> RemovedThisTick = CleanseRegisteredVCams();
		// If this changes anything, it triggers OnOwnershipChanged setting bHasRequestedLockRefresh = true
		UpdateAllOwnership(RemovedThisTick);
		// If user manually unlocks the viewport, immediately lock it again. Sets bHasRequestedLockRefresh = true if user changed pilot state.
		UpdatePilotState();
		
		bHasRequestedResolutionRefresh |= bHasRequestedLockRefresh;
		if (!bHasRequestedLockRefresh && !bHasRequestedResolutionRefresh)
		{
			return;
		}
		UE_LOG(LogVCamCore, Log,
			TEXT("Refreshing viewport locks: bHasRequestedLockRefresh=%d, bHasRequestedResolutionRefresh=%d"),
			bHasRequestedLockRefresh, bHasRequestedResolutionRefresh
			);
		
		if (bHasRequestedLockRefresh)
		{
			bHasRequestedLockRefresh = false;
			LockManager.UpdateViewportLockState(RegisteredVCams);
			
			// Lock state may have changed - so update internal cache but skip setting bHasRequestedLockRefresh = true
			UpdatePilotStateNoRefresh();
		}
		if (bHasRequestedResolutionRefresh)
		{
			bHasRequestedResolutionRefresh = false;
			ResolutionManager.UpdateViewportLockState(RegisteredVCams);
		}
	}

	TInlineComponentArray<TWeakObjectPtr<UVCamComponent>> FViewportManagerBase::CleanseRegisteredVCams()
	{
		TInlineComponentArray<TWeakObjectPtr<UVCamComponent>> Removed;
		for (auto It = RegisteredVCams.CreateIterator(); It; ++It)
		{
			if (!It->IsValid())
			{
				Removed.Add(*It);
				RequestLockRefresh();
				It.RemoveCurrent();
			}
		}
		return Removed;
	}

	void FViewportManagerBase::UpdatePilotState(bool bAllowRefresh)
	{
		const bool bViewport1 = ViewportLocker.IsViewportLocked(EVCamTargetViewportID::Viewport1);
		const bool bViewport2 = ViewportLocker.IsViewportLocked(EVCamTargetViewportID::Viewport2);
		const bool bViewport3 = ViewportLocker.IsViewportLocked(EVCamTargetViewportID::Viewport3);
		const bool bViewport4 = ViewportLocker.IsViewportLocked(EVCamTargetViewportID::Viewport4);

		const auto HasViewportChanged = [this](int32 Index)
		{
			EVCamTargetViewportID ViewportId = static_cast<EVCamTargetViewportID>(Index);
			const bool bIsLocked = ViewportLocker.IsViewportLocked(ViewportId);

			const TWeakObjectPtr<const UVCamOutputProviderBase>* PossibleOwner = ViewportOwnership.GetOwner(ViewportId);
			const UVCamOutputProviderBase* Owner = PossibleOwner ? PossibleOwner->Get() : nullptr;
			const bool bIsViewportReadyForVCam = Owner && !bIsLocked && LockManager.WantsToLockViewportTo(*Owner);
			const bool bHasPilotStateChanged = bIsLocked != ViewportShouldPilotStates[Index];
			
			const bool bHasChanged = bHasPilotStateChanged || bIsViewportReadyForVCam;
			return bHasChanged;
		};
		const bool bHasChanged = HasViewportChanged(0) || HasViewportChanged(1) || HasViewportChanged(2) || HasViewportChanged(3);
		if (bAllowRefresh && bHasChanged)
		{
			RequestLockRefresh();
		}
		
		ViewportShouldPilotStates[0] = bViewport1;
		ViewportShouldPilotStates[1] = bViewport2;
		ViewportShouldPilotStates[2] = bViewport3;
		ViewportShouldPilotStates[3] = bViewport4;
	}

	void FViewportManagerBase::UpdateAllOwnership(const TInlineComponentArray<TWeakObjectPtr<UVCamComponent>>& RemovedThisTick)
	{
		// Prevent dead objects from taking up ownership slots
		const auto CleanUpOwnership = [this, &RemovedThisTick](EVCamTargetViewportID Viewport)
		{
			ViewportOwnership.RemovePotentialOwnerIf(Viewport, [&RemovedThisTick, Viewport](const TWeakObjectPtr<const UVCamOutputProviderBase>& WeakOutputProvider)
			{
				const UVCamOutputProviderBase* OutputProvider = WeakOutputProvider.Get();
				return !OutputProvider
					// Undo & redo will leave the output provider UObject valid but the VCamComponent will be marked pending destroy
					// TWeakObjectPtr::IsValid nor IsValid it detect its destruction correctly in that case ... so we'll ask the passed in array instead.
					|| RemovedThisTick.Contains(OutputProvider->GetVCamComponent())
					// Output provider has changed target since?
					|| OutputProvider->GetTargetViewport() != Viewport;
			});
		};
		CleanUpOwnership(EVCamTargetViewportID::Viewport1);
		CleanUpOwnership(EVCamTargetViewportID::Viewport2);
		CleanUpOwnership(EVCamTargetViewportID::Viewport3);
		CleanUpOwnership(EVCamTargetViewportID::Viewport4);

		// Add or remove ownership on all registered output providers
		for (const TWeakObjectPtr<UVCamComponent>& WeakVCam : RegisteredVCams)
		{
			UVCamComponent* VCam = WeakVCam.Get();
			checkf(VCam, TEXT("CleanseRegisteredVCams was supposed to have run before."));

			const FVCamViewportLocker& LockState = VCam->GetViewportLockState();
			for (UVCamOutputProviderBase* OutputProvider : VCam->GetOutputProviders())
			{
				if (OutputProvider)
				{
					UpdateOwnershipFor(LockState, *OutputProvider);
				}
			}
		}
	}

	void FViewportManagerBase::UpdateOwnershipFor(const FVCamViewportLocker& OwnerLockState, const UVCamOutputProviderBase& OutputProvider)
	{
		if (DetermineOwnershipFor(OutputProvider, OwnerLockState))
		{
			const EVCamTargetViewportID TargetViewportID = OutputProvider.GetTargetViewport();
			ViewportOwnership.TryTakeOwnership(&OutputProvider, TargetViewportID);
		}
		else
		{
			ViewportOwnership.ReleaseOwnership(&OutputProvider);
		}
	}

	bool FViewportManagerBase::DetermineOwnershipFor(const UVCamOutputProviderBase& OutputProvider, const FVCamViewportLocker& OwnerLockState) const
	{
		const TOptional<bool> OverrideOwnershipOptional = OverrideShouldHaveOwnership.IsBound() ? OverrideShouldHaveOwnership.Execute(OutputProvider) :  TOptional<bool>{};
		if (OverrideOwnershipOptional)
		{
			return *OverrideOwnershipOptional;
		}
		
		const EVCamTargetViewportID TargetViewportID = OutputProvider.GetTargetViewport();
		const bool bShouldHaveOwnership = OutputProvider.IsOutputting()
			&& (OutputProvider.NeedsForceLockToViewport() || OwnerLockState.ShouldLock(TargetViewportID));
		return bShouldHaveOwnership;
	}

	void FViewportManagerBase::RemoveOwnership(const UVCamComponent& Component)
	{
		const auto CleanseOwnership = [&Component](const TWeakObjectPtr<const UVCamOutputProviderBase>& WeakOutputProvider)
		{
			const UVCamOutputProviderBase* OutputProvider = WeakOutputProvider.Get();
			// IsIn handles edge cases more gracefully (e.g. this finds objects that are not in the OutputProviders array)
			return !OutputProvider || OutputProvider->IsIn(&Component);
		};
		ViewportOwnership.RemovePotentialOwnerIf(EVCamTargetViewportID::Viewport1, CleanseOwnership);
		ViewportOwnership.RemovePotentialOwnerIf(EVCamTargetViewportID::Viewport2, CleanseOwnership);
		ViewportOwnership.RemovePotentialOwnerIf(EVCamTargetViewportID::Viewport3, CleanseOwnership);
		ViewportOwnership.RemovePotentialOwnerIf(EVCamTargetViewportID::Viewport4, CleanseOwnership);
		
		RequestLockRefresh();
	}

	bool FViewportManagerBase::HasViewportOwnership(const UVCamOutputProviderBase& OutputProvider) const
	{
		return ViewportOwnership.IsOwnedBy(OutputProvider.GetTargetViewport(), &OutputProvider);
	}
}