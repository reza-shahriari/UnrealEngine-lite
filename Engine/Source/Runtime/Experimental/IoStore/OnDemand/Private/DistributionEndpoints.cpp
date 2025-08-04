// Copyright Epic Games, Inc. All Rights Reserved.

#include "DistributionEndpoints.h"

#include "Algo/RemoveIf.h"
#include "Containers/StringConv.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "IO/Http/Client.h"
#include "IO/IoBuffer.h"
#include "IO/IoStoreOnDemand.h"
#include "Logging/StructuredLog.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/MemoryReader.h"
#include "Statistics.h"
#include "Tasks/Task.h"

#if !UE_BUILD_SHIPPING
#include "Internationalization/Regex.h"
#endif // !UE_BUILD_SHIPPING

namespace UE::IoStore
{

static int32 GDistributedEndpointTimeout = 30;
static FAutoConsoleVariableRef CVar_DistributedEndpointTimeout(
	TEXT("ias.DistributedEndpointTimeout"),
	GDistributedEndpointTimeout,
	TEXT("How long to wait (in seconds) for a distributed endoint resolve request before timing out")
);

static FDistributionEndpoints::EResult ParseResponse(FIoBuffer, TArray<FString>&);

FDistributionEndpoints::EResult FDistributionEndpoints::ResolveEndpoints(const FString& DistributionUrl, TArray<FString>& OutServiceUrls)
{
	FEventRef Event;
	return ResolveEndpoints(DistributionUrl, OutServiceUrls, *Event.Get());
}

FDistributionEndpoints::EResult FDistributionEndpoints::ResolveEndpoints(const FString& DistributionUrl, TArray<FString>& OutServiceUrls, FEvent& Event)
{
	using namespace HTTP;

	TRACE_CPUPROFILER_EVENT_SCOPE(FDistributionEndpoints::ResolveEndpoints);

	UE_LOG(LogIas, Log, TEXT("Resolving distributed endpoint '%s'"), *DistributionUrl);

	std::atomic_bool bHasResponse = false;
	EResult Result = EResult::Failure;
	auto OnRequestStatus = [
		&bHasResponse,
		&OutServiceUrls,
		&Event,
		&Result,
		Dest=FIoBuffer()
		] (const FTicketStatus& Status) mutable
	{
		if (Status.GetId() == FTicketStatus::EId::Error)
		{
			FTicketStatus::FError Error = Status.GetError();
			UE_LOGFMT(LogIas, Warning, "ResolveEndpoints Error: {ErrorReason} ({ErrorCode})", Error.Reason, Error.Code);
			return;
		}

		if (Status.GetId() >= FTicketStatus::EId::Cancelled)
		{
			return;
		}

		if (Status.GetId() == FTicketStatus::EId::Content)
		{
			bHasResponse = true;
			Result = ParseResponse(Dest, OutServiceUrls);
			Event.Trigger();
			return;
		}

		FResponse& Response = Status.GetResponse();
		if (Response.GetStatus() != EStatusCodeClass::Successful)
		{
			UE_LOGFMT(LogIas, Warning, "ResolveEndpoints failed with HTTP response {ResponseCode}", Response.GetStatusCode());
			return;
		}

		Response.SetDestination(&Dest);
	};

	auto AnsiUrl = StringCast<ANSICHAR>(*DistributionUrl, DistributionUrl.Len());

	FEventLoop Loop;
	if (GDistributedEndpointTimeout >= 0)
	{
		Loop.SetFailTimeout(GDistributedEndpointTimeout * 1000);
	}

	FEventLoop::FRequestParams RequestParams = { .bAllowChunked = false };
	FRequest Request = Loop.Get(AnsiUrl, &RequestParams);
	Request.Header("Accept", "application/json");
	Loop.Send(MoveTemp(Request), OnRequestStatus);

	while (Loop.Tick(-1))
		;

#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("Ias.SkipDevCDNs")))
	{
		FString ConfigRegex;
		GConfig->GetString(TEXT("Ias"), TEXT("DevelopmentCDNPattern"), ConfigRegex, GEngineIni);

		if(!ConfigRegex.IsEmpty())
		{
			FRegexPattern Pattern(ConfigRegex);

			const int32 OriginaNum = OutServiceUrls.Num();
			OutServiceUrls.SetNum(Algo::StableRemoveIf(OutServiceUrls, [&Pattern](const FString& Entry)
			{
				FRegexMatcher Regex = FRegexMatcher(Pattern, Entry);
				return Regex.FindNext();
			}));

			UE_LOG(LogIas, Log, TEXT("Removed %d development CDNs from the distributed endpoint list"), OriginaNum - OutServiceUrls.Num());
		}
	}
#endif //!UE_BUILD_SHIPPING

	UE_CLOG(Result == EResult::Success, LogIas, Log, TEXT("Successfully resolved distributed endpoint '%s' %d urls found"), *DistributionUrl, OutServiceUrls.Num());
	UE_CLOG(Result != EResult::Success, LogIas, Log, TEXT("Failed to resolve distributed endpoint '%s'"), *DistributionUrl);

	return Result;
}

static FDistributionEndpoints::EResult ParseResponse(FIoBuffer Data, TArray<FString>& OutUrls)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FDistributionEndpoints::ParseResponse);

	using FJsonValuePtr = TSharedPtr<FJsonValue>;
	using FJsonObjPtr = TSharedPtr<FJsonObject>;
	using FJsonReader = TJsonReader<UTF8CHAR>;
	using FJsonReaderPtr = TSharedRef<FJsonReader>;

	FMemoryView JsonView = Data.GetView();
	FUtf8StringView Json((UTF8CHAR*)JsonView.GetData(), int32(JsonView.GetSize()));
	
	FJsonReaderPtr JsonReader = TJsonReaderFactory<UTF8CHAR>::CreateFromView(Json);

	FJsonObjPtr JsonObj;
	if (!FJsonSerializer::Deserialize(JsonReader, JsonObj))
	{
		return FDistributionEndpoints::EResult::Failure;
	}

	if (!JsonObj->HasTypedField< EJson::Array>(TEXT("distributions")))
	{
		return FDistributionEndpoints::EResult::Failure;
	}

	TArray<FJsonValuePtr> JsonValues = JsonObj->GetArrayField(TEXT("distributions"));
	OutUrls.Reserve(JsonValues.Num());

	for (const FJsonValuePtr& JsonValue : JsonValues)
	{
		FString ServiceUrl = JsonValue->AsString();
		if (ServiceUrl.EndsWith(TEXT("/")))
		{
			ServiceUrl.LeftInline(ServiceUrl.Len() - 1);
		}
		OutUrls.Add(MoveTemp(ServiceUrl));
	}

	return !OutUrls.IsEmpty() ? FDistributionEndpoints::EResult::Success : FDistributionEndpoints::EResult::Failure;
}

} // namespace UE::IoStore
