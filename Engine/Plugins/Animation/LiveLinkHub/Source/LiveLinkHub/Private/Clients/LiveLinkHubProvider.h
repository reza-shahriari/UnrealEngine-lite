// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Clients/GameThreadMessageHandler.h"
#include "Clients/LiveLinkHubClientsModel.h"
#include "Clients/LiveLinkHubUEClientInfo.h"
#include "Engine/TimerHandle.h"
#include "IMessageContext.h"
#include "LiveLinkHubMessages.h"
#include "LiveLinkProviderImpl.h"
#include "Templates/Function.h"


class ILiveLinkHubSessionManager;
struct FLiveLinkHubClientId;
struct FLiveLinkHubConnectMessage;
struct FLiveLinkHubUEClientInfo;


/** 
 * LiveLink Provider that allows getting more information about a UE client by communicating with a LiveLinkHub MessageBus Source.
 */
class FLiveLinkHubProvider : public FLiveLinkProvider, public ILiveLinkHubClientsModel, public TSharedFromThis<FLiveLinkHubProvider>
{
public:
	using FLiveLinkProvider::SendClearSubjectToConnections;
	using FLiveLinkProvider::GetLastSubjectStaticDataStruct;
	using FLiveLinkProvider::GetProviderName;

	/**
	 * Create a message bus handler that will dispatch messages on the game thread. 
	 * This is useful to receive some messages on AnyThread and delegate others on the game thread (ie. for methods that will trigger UI updates which need to happen on game thread. )
	 */
	template <typename MessageType>
	TSharedRef<TGameThreadMessageHandler<MessageType, FLiveLinkHubProvider>> MakeHandler(typename TGameThreadMessageHandler<MessageType, FLiveLinkHubProvider>::FuncType Func)
	{
		return MakeShared<TGameThreadMessageHandler<MessageType, FLiveLinkHubProvider>>(this, Func);
	}

	FLiveLinkHubProvider(const TSharedRef<ILiveLinkHubSessionManager>& InSessionManager, const FString& ProviderName);

	virtual ~FLiveLinkHubProvider() override;

	//~ Begin LiveLinkProvider interface
	virtual bool ShouldTransmitToSubject_AnyThread(FName SubjectName, FMessageAddress Address) const override;
	virtual TOptional<FLiveLinkHubUEClientInfo> GetClientInfo(FLiveLinkHubClientId InClient) const override;
	//~ End LiveLinkProvider interface

	/**
	 * Restore a client, calling this will modify the client ID if it matches an existing connection.
	 */
	void AddRestoredClient(FLiveLinkHubUEClientInfo& InOutRestoredClientInfo);

	/** Retrieve the existing client map. */
	const TMap<FLiveLinkHubClientId, FLiveLinkHubUEClientInfo>& GetClientsMap() const { return ClientsMap; }

	/** 
	 * Timecode settings that should be shared to connected editors. 
	 * @Note If ClientId is not provided or invalid, the message will be broadcast to all connected clients.
	 */
	void UpdateTimecodeSettings(const FLiveLinkHubTimecodeSettings& InSettings, const FLiveLinkHubClientId& ClientId = FLiveLinkHubClientId{});

	/**
	 * Reset timecode settings for all connected clients. 
	 * @Note If ClientId is not provided or invalid, the message will be broadcast to all connected clients.
	 */
	void ResetTimecodeSettings(const FLiveLinkHubClientId& ClientId = FLiveLinkHubClientId{});

	/** Frame Lock settings that should be shared to connected editors. */
	void UpdateCustomTimeStepSettings(const FLiveLinkHubCustomTimeStepSettings& InSettings, const FLiveLinkHubClientId& ClientId = FLiveLinkHubClientId{});

	/** 
	 * Reset Frame Lock settings on connected editors. 
	 * @Note If ClientId is not provided or invalid, the message will be broadcast to all connected clients.
	 */
	void ResetCustomTimeStepSettings(const FLiveLinkHubClientId& ClientId = FLiveLinkHubClientId{});

	/**
	 * Send a disconnect message to all connected clients.
	 */
	void DisconnectAll();

	/**
	 * Send a disconnect message to a single client.
	 */
	void DisconnectClient(const FLiveLinkHubClientId& ClientId);

	/** Called when the topology mode changed for this app. */
	void PostUpdateTopologyMode(ELiveLinkTopologyMode TopologyMode);

private:
	/** Handle a connection message resulting from a livelink hub message bus source connecting to this provider. */
	void HandleHubConnectMessage(const FLiveLinkHubConnectMessage& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);
	
	/** Handle a client info message being received. Happens when new information about a client is received (ie. Client has changed map) */
	void HandleClientInfoMessage(const FLiveLinkClientInfoMessage& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);

	/** Handle a client (UE or Hub) sending a disconnect request. */
	void HandleHubDisconnectMessage(const FLiveLinkHubDisconnectMessage& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);

	/** Send timecode settings to connected UE Clients. */
	void SendTimecodeSettings(const FLiveLinkHubTimecodeSettings& InSettings, const FLiveLinkHubClientId& ClientId);

	/** Send CustomTimeStep settings to connected UE Clients. */
	void SendCustomTimeStepSettings(const FLiveLinkHubCustomTimeStepSettings& InSettings, const FLiveLinkHubClientId& ClientId);

	/** Send a message to clients that are connected and enabled through the hub clients list. */
	template<typename MessageType>
	void SendMessageToEnabledClients(MessageType* Message, EMessageFlags Flags = EMessageFlags::None)
	{
		TArray<FMessageAddress> AllAddresses;
		GetConnectedAddresses(AllAddresses);

		TArray<FMessageAddress> EnabledAddresses = AllAddresses.FilterByPredicate([this](const FMessageAddress& Address)
		{
			return ShouldTransmitToClient_AnyThread(Address);
		});

		SendMessage(Message, EnabledAddresses, Flags);
	}

	/**
	 * Whether a message should be transmitted to a particular client, identified by a message address.
	 * You can specify an additional filter method if you want to filter based on the client info.
	 **/
	bool ShouldTransmitToClient_AnyThread(FMessageAddress Address, TFunctionRef<bool(const FLiveLinkHubUEClientInfo* ClientInfoPtr)> AdditionalFilter = [](const FLiveLinkHubUEClientInfo* ClientInfoPtr){ return true; }) const;

	/** Broadcast a message telling all UE clients to connect to this instance. */
	void ConnectToAllHubClients();

	/** Broadcast a message telling all Hub clients to connect to this instance. */
	void ConnectToAllUEClients();

	void CloseConnections(const TArray<FMessageAddress>& ClosedAddresses);

	/** 
	 * Get the client id that corresponds to this address from our cache. 
	 * May return an invalid ID if the address is not in the cache (ie. If client is disconnecting))
	 **/
	FLiveLinkHubClientId AddressToClientId(const FMessageAddress& Address) const;
protected:
	//~ Begin ILiveLinkHubClientsModel interface
	virtual void OnConnectionsClosed(const TArray<FMessageAddress>& ClosedAddresses) override;
	virtual TArray<FLiveLinkHubClientId> GetSessionClients() const override;
	virtual TMap<FName, FString> GetAnnotations() const override;
	virtual TArray<FLiveLinkHubClientId> GetDiscoveredClients() const override;
	virtual FText GetClientDisplayName(FLiveLinkHubClientId InAddress) const override;
	virtual FOnClientEvent& OnClientEvent() override
	{
		return OnClientEventDelegate;
	}
	virtual FText GetClientStatus(FLiveLinkHubClientId Client) const override;
	virtual bool IsClientEnabled(FLiveLinkHubClientId Client) const override;
	virtual bool IsClientConnected(FLiveLinkHubClientId Client) const override;
	virtual void SetClientEnabled(FLiveLinkHubClientId Client, bool bInEnable) override;
	virtual bool IsSubjectEnabled(FLiveLinkHubClientId Client, FName SubjectName) const override;
	virtual void SetSubjectEnabled(FLiveLinkHubClientId Client, FName SubjectName, bool bInEnable) override;
	//~ End ILiveLinkHubClientsModel interface

private:
	/** Handle to the timer responsible for validating the livelinkprovider's connections.*/
	FTimerHandle ValidateConnectionsTimer;
	/** List of information we have on clients we have discovered. */
	TMap<FLiveLinkHubClientId, FLiveLinkHubUEClientInfo> ClientsMap;
	/** Delegate called when the provider receives a client change. */
	FOnClientEvent OnClientEventDelegate;
	/** Annotations sent with every message from this provider. In our case it's use to disambiguate a livelink hub provider from other livelink providers.*/
	TMap<FName, FString> Annotations;
	/** LiveLinkHub session manager. */
	TWeakPtr<ILiveLinkHubSessionManager> SessionManager;
	/** Cache used to retrieve the client id from a message bus address. */
	TMap<FMessageAddress, FLiveLinkHubClientId> AddressToIdCache;

	/** Lock used to access the clients map from different threads. */
	mutable FRWLock ClientsMapLock;
};
