// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/EngineTypes.h"
#include "Tickable.h"

#include "epic_rtc/plugins/signalling/websocket.h"
#include "epic_rtc_helper/memory/ref_count_impl_helper.h"

class IWebSocket;

namespace UE::PixelStreaming2
{
	class PIXELSTREAMING2RTC_API FEpicRtcWebsocket : public EpicRtcWebsocketInterface, public FTickableGameObject
	{
	public:
		FEpicRtcWebsocket(bool bKeepAlive = true, TSharedPtr<IWebSocket> WebSocket = nullptr, TFunction<void(void)> OnMaxReconnectAttemptsExceeded = []() {});
		virtual ~FEpicRtcWebsocket() = default;

		// Begin EpicRtcWebsocketInterface
		virtual EpicRtcBool Connect(EpicRtcStringView Url, EpicRtcWebsocketObserverInterface* Observer) override;
		virtual void		Disconnect(const EpicRtcStringView Reason) override;
		virtual void		Send(EpicRtcStringView Message) override;
		// End EpicRtcWebsocketInterface

		// Begin FTickableGameObject
		virtual void		Tick(float DeltaTime) override;
		virtual bool		IsTickableInEditor() const override { return true; }
		virtual bool		IsTickableWhenPaused() const override { return true; }
		FORCEINLINE TStatId GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(EpicRtcWebSocket, STATGROUP_Tickables); }
		// End FTickableGameObject

	private:
		void OnConnected();
		void OnConnectionError(const FString& Error);
		void OnClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
		void OnMessage(const FString& Msg);
		void OnBinaryMessage(const uint8* Data, int32 Length, bool bIsLastFragment);
		void Reconnect();
		void KeepAlive();

	private:
		FDelegateHandle OnConnectedHandle;
		FDelegateHandle OnConnectionErrorHandle;
		FDelegateHandle OnClosedHandle;
		FDelegateHandle OnMessageHandle;
		FDelegateHandle OnBinaryMessageHandle;

	private:
		FString											Url;
		FString											LastError;
		TSharedPtr<IWebSocket>							WebSocket;
		TRefCountPtr<EpicRtcWebsocketObserverInterface> Observer;
		uint64											LastKeepAliveCycles = 0;
		uint64											LastReconnectCycles = 0;
		uint64											NumReconnectAttempts = 0;
		bool											bSendKeepAlive = false;
		bool											bReconnectOnError = false;
		bool											bCloseRequested = false;
		TFunction<void(void)>							OnMaxReconnectAttemptsExceeded;

	public:
		// Begin EpicRtcRefCountInterface
		EPICRTC_REFCOUNT_INTERFACE_IN_PLACE
		// End EpicRtcRefCountInterface
	};

} // namespace UE::PixelStreaming2