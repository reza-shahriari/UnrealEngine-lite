// Copyright Epic Games, Inc. All Rights Reserved.

#include "LearningTrainer.h"

#include "LearningObservation.h"
#include "LearningAction.h"

#include "HAL/Platform.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

namespace UE::Learning
{
	FSubprocess::~FSubprocess()
	{
		Terminate();
	}

	bool FSubprocess::Launch(const FString& Path, const FString& Params, const ESubprocessFlags Flags)
	{
		ensureMsgf(!bIsLaunched, TEXT("Subprocess already launched."));

		Terminate();

		const bool bCreatePipes = !(Flags & ESubprocessFlags::NoRedirectOutput);
		const bool bHideWindow = !(Flags & ESubprocessFlags::ShowWindow);

		if (bCreatePipes && !FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
		{
			return false;
		}

		ProcessHandle = FPlatformProcess::CreateProc(*Path, *Params, false, bHideWindow, bHideWindow, nullptr, 0, *FPaths::RootDir(), WritePipe, ReadPipe);
		bIsLaunched = true;
		return true;
	}

	bool FSubprocess::IsRunning() const
	{
		return bIsLaunched && FPlatformProcess::IsProcRunning(const_cast<FProcHandle&>(ProcessHandle));
	}

	void FSubprocess::Terminate()
	{
		if (IsRunning())
		{
			UE_LOG(LogLearning, Display, TEXT("Terminating Subprocess..."));

			FPlatformProcess::TerminateProc(ProcessHandle, true);
		}

		Update();
	}

	bool FSubprocess::Update()
	{
		// Do nothing if the process is not launched
		if (!bIsLaunched)
		{
			return false;
		}

		// Append the process stdout to the buffer
		OutputBuffer += FPlatformProcess::ReadPipe(ReadPipe);

		// Output all the complete lines
		int32 LineStartIdx = 0;
		for (int32 Idx = 0; Idx < OutputBuffer.Len(); Idx++)
		{
			if (OutputBuffer[Idx] == '\r' || OutputBuffer[Idx] == '\n')
			{
				UE_LOG(LogLearning, Display, TEXT("Subprocess: %s"), *OutputBuffer.Mid(LineStartIdx, Idx - LineStartIdx));

				if (OutputBuffer[Idx] == '\r' && Idx + 1 < OutputBuffer.Len() && OutputBuffer[Idx + 1] == '\n')
				{
					Idx++;
				}

				LineStartIdx = Idx + 1;
			}
		}

		// Remove all the complete lines from the buffer
		OutputBuffer.MidInline(LineStartIdx, MAX_int32, EAllowShrinking::Yes);

		// If the process is no longer running then close the pipes
		if (!IsRunning())
		{
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
			ReadPipe = nullptr;
			WritePipe = nullptr;
			bIsLaunched = false;
			return false;
		}
		
		return true;
	}

}

namespace UE::Learning::Trainer
{

	TSharedPtr<FJsonObject> ConvertObservationSchemaToJSON(
		const Observation::FSchema& ObservationSchema,
		const Observation::FSchemaElement& ObservationSchemaElement)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("VectorSize"), ObservationSchema.GetObservationVectorSize(ObservationSchemaElement));
		Object->SetNumberField(TEXT("EncodedSize"), ObservationSchema.GetEncodedVectorSize(ObservationSchemaElement));

		switch (ObservationSchema.GetType(ObservationSchemaElement))
		{
		case Observation::EType::Null:
		{
			Object->SetStringField(TEXT("Type"), TEXT("Null"));
			break;
		}

		case Observation::EType::Continuous:
		{
			const Observation::FSchemaContinuousParameters Parameters = ObservationSchema.GetContinuous(ObservationSchemaElement);
			Object->SetStringField(TEXT("Type"), TEXT("Continuous"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			break;
		}

		case Observation::EType::DiscreteExclusive:
		{
			const Observation::FSchemaDiscreteExclusiveParameters Parameters = ObservationSchema.GetDiscreteExclusive(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("DiscreteExclusive"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			break;
		}

		case Observation::EType::DiscreteInclusive:
		{
			const Observation::FSchemaDiscreteInclusiveParameters Parameters = ObservationSchema.GetDiscreteInclusive(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("DiscreteInclusive"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			break;
		}

		case Observation::EType::NamedDiscreteExclusive:
		{
			const Observation::FSchemaNamedDiscreteExclusiveParameters Parameters = ObservationSchema.GetNamedDiscreteExclusive(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("NamedDiscreteExclusive"));

			TArray<TSharedPtr<FJsonValue>> ElementNames;
			for (int32 ElementIdx = 0; ElementIdx < Parameters.ElementNames.Num(); ElementIdx++)
			{
				ElementNames.Add(MakeShared<FJsonValueString>(Parameters.ElementNames[ElementIdx].ToString()));
			}

			Object->SetArrayField(TEXT("ElementNames"), ElementNames);
			break;
		}

		case Observation::EType::NamedDiscreteInclusive:
		{
			const Observation::FSchemaNamedDiscreteInclusiveParameters Parameters = ObservationSchema.GetNamedDiscreteInclusive(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("NamedDiscreteInclusive"));

			TArray<TSharedPtr<FJsonValue>> ElementNames;
			for (int32 ElementIdx = 0; ElementIdx < Parameters.ElementNames.Num(); ElementIdx++)
			{
				ElementNames.Add(MakeShared<FJsonValueString>(Parameters.ElementNames[ElementIdx].ToString()));
			}

			Object->SetArrayField(TEXT("ElementNames"), ElementNames);
			break;
		}

		case Observation::EType::And:
		{
			const Observation::FSchemaAndParameters Parameters = ObservationSchema.GetAnd(ObservationSchemaElement);
			
			Object->SetStringField(TEXT("Type"), TEXT("And"));

			TSharedPtr<FJsonObject> SubObject = MakeShared<FJsonObject>();
			for (int32 SubElementIdx = 0; SubElementIdx < Parameters.Elements.Num(); SubElementIdx++)
			{
				TSharedPtr<FJsonObject> SubElement = ConvertObservationSchemaToJSON(ObservationSchema, Parameters.Elements[SubElementIdx]);
				SubElement->SetNumberField(TEXT("Index"), SubElementIdx);
				SubObject->SetObjectField(Parameters.ElementNames[SubElementIdx].ToString(), SubElement);
			}

			Object->SetObjectField(TEXT("Elements"), SubObject);
			break;
		}

		case Observation::EType::OrExclusive:
		{
			const Observation::FSchemaOrExclusiveParameters Parameters = ObservationSchema.GetOrExclusive(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("OrExclusive"));
			Object->SetNumberField(TEXT("EncodingSize"), Parameters.EncodingSize);

			TSharedPtr<FJsonObject> SubObject = MakeShared<FJsonObject>();
			for (int32 SubElementIdx = 0; SubElementIdx < Parameters.Elements.Num(); SubElementIdx++)
			{
				TSharedPtr<FJsonObject> SubElement = ConvertObservationSchemaToJSON(ObservationSchema, Parameters.Elements[SubElementIdx]);
				SubElement->SetNumberField(TEXT("Index"), SubElementIdx);
				SubObject->SetObjectField(Parameters.ElementNames[SubElementIdx].ToString(), SubElement);
			}

			Object->SetObjectField(TEXT("Elements"), SubObject);
			break;
		}

		case Observation::EType::OrInclusive:
		{
			const Observation::FSchemaOrInclusiveParameters Parameters = ObservationSchema.GetOrInclusive(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("OrInclusive"));
			Object->SetNumberField(TEXT("AttentionEncodingSize"), Parameters.AttentionEncodingSize);
			Object->SetNumberField(TEXT("AttentionHeadNum"), Parameters.AttentionHeadNum);
			Object->SetNumberField(TEXT("ValueEncodingSize"), Parameters.ValueEncodingSize);

			TSharedPtr<FJsonObject> SubObject = MakeShared<FJsonObject>();
			for (int32 SubElementIdx = 0; SubElementIdx < Parameters.Elements.Num(); SubElementIdx++)
			{
				TSharedPtr<FJsonObject> SubElement = ConvertObservationSchemaToJSON(ObservationSchema, Parameters.Elements[SubElementIdx]);
				SubElement->SetNumberField(TEXT("Index"), SubElementIdx);
				SubObject->SetObjectField(Parameters.ElementNames[SubElementIdx].ToString(), SubElement);
			}

			Object->SetObjectField(TEXT("Elements"), SubObject);
			break;
		}

		case Observation::EType::Array:
		{
			const Observation::FSchemaArrayParameters Parameters = ObservationSchema.GetArray(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("Array"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			Object->SetObjectField(TEXT("Element"), ConvertObservationSchemaToJSON(ObservationSchema, Parameters.Element));
			break;
		}

		case Observation::EType::Set:
		{
			const Observation::FSchemaSetParameters Parameters = ObservationSchema.GetSet(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("Set"));
			Object->SetNumberField(TEXT("MaxNum"), Parameters.MaxNum);
			Object->SetNumberField(TEXT("AttentionEncodingSize"), Parameters.AttentionEncodingSize);
			Object->SetNumberField(TEXT("AttentionHeadNum"), Parameters.AttentionHeadNum);
			Object->SetNumberField(TEXT("ValueEncodingSize"), Parameters.ValueEncodingSize);
			Object->SetObjectField(TEXT("Element"), ConvertObservationSchemaToJSON(ObservationSchema, Parameters.Element));
			break;
		}

		case Observation::EType::Encoding:
		{
			const Observation::FSchemaEncodingParameters Parameters = ObservationSchema.GetEncoding(ObservationSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("Encoding"));
			Object->SetNumberField(TEXT("EncodingSize"), Parameters.EncodingSize);
			Object->SetObjectField(TEXT("Element"), ConvertObservationSchemaToJSON(ObservationSchema, Parameters.Element));
			break;
		}

		default:
			checkNoEntry();
		}

		return Object;
	}

	TSharedPtr<FJsonObject> ConvertActionSchemaToJSON(
		const Action::FSchema& ActionSchema,
		const Action::FSchemaElement& ActionSchemaElement)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("VectorSize"), ActionSchema.GetActionVectorSize(ActionSchemaElement));
		Object->SetNumberField(TEXT("DistributionSize"), ActionSchema.GetActionDistributionVectorSize(ActionSchemaElement));
		Object->SetNumberField(TEXT("EncodedSize"), ActionSchema.GetEncodedVectorSize(ActionSchemaElement));
		Object->SetNumberField(TEXT("ModifierSize"), ActionSchema.GetActionModifierVectorSize(ActionSchemaElement));

		switch (ActionSchema.GetType(ActionSchemaElement))
		{
		case Action::EType::Null:
		{
			Object->SetStringField(TEXT("Type"), TEXT("Null"));
			break;
		}

		case Action::EType::Continuous:
		{
			const Action::FSchemaContinuousParameters Parameters = ActionSchema.GetContinuous(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("Continuous"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			break;
		}

		case Action::EType::DiscreteExclusive:
		{
			const Action::FSchemaDiscreteExclusiveParameters Parameters = ActionSchema.GetDiscreteExclusive(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("DiscreteExclusive"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			break;
		}

		case Action::EType::DiscreteInclusive:
		{
			const Action::FSchemaDiscreteInclusiveParameters Parameters = ActionSchema.GetDiscreteInclusive(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("DiscreteInclusive"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			break;
		}

		case Action::EType::NamedDiscreteExclusive:
		{
			const Action::FSchemaNamedDiscreteExclusiveParameters Parameters = ActionSchema.GetNamedDiscreteExclusive(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("NamedDiscreteExclusive"));

			TArray<TSharedPtr<FJsonValue>> ElementNames;
			for (int32 ElementIdx = 0; ElementIdx < Parameters.ElementNames.Num(); ElementIdx++)
			{
				ElementNames.Add(MakeShared<FJsonValueString>(Parameters.ElementNames[ElementIdx].ToString()));
			}

			Object->SetArrayField(TEXT("ElementNames"), ElementNames);
			break;
		}

		case Action::EType::NamedDiscreteInclusive:
		{
			const Action::FSchemaNamedDiscreteInclusiveParameters Parameters = ActionSchema.GetNamedDiscreteInclusive(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("NamedDiscreteInclusive"));

			TArray<TSharedPtr<FJsonValue>> ElementNames;
			for (int32 ElementIdx = 0; ElementIdx < Parameters.ElementNames.Num(); ElementIdx++)
			{
				ElementNames.Add(MakeShared<FJsonValueString>(Parameters.ElementNames[ElementIdx].ToString()));
			}

			Object->SetArrayField(TEXT("ElementNames"), ElementNames);
			break;
		}

		case Action::EType::And:
		{
			const Action::FSchemaAndParameters Parameters = ActionSchema.GetAnd(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("And"));

			TSharedPtr<FJsonObject> SubObject = MakeShared<FJsonObject>();
			for (int32 SubElementIdx = 0; SubElementIdx < Parameters.Elements.Num(); SubElementIdx++)
			{
				TSharedPtr<FJsonObject> SubElement = ConvertActionSchemaToJSON(ActionSchema, Parameters.Elements[SubElementIdx]);
				SubElement->SetNumberField(TEXT("Index"), SubElementIdx);
				SubObject->SetObjectField(Parameters.ElementNames[SubElementIdx].ToString(), SubElement);
			}

			Object->SetObjectField(TEXT("Elements"), SubObject);
			break;
		}

		case Action::EType::OrExclusive:
		{
			const Action::FSchemaOrExclusiveParameters Parameters = ActionSchema.GetOrExclusive(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("OrExclusive"));

			TSharedPtr<FJsonObject> SubObject = MakeShared<FJsonObject>();
			for (int32 SubElementIdx = 0; SubElementIdx < Parameters.Elements.Num(); SubElementIdx++)
			{
				TSharedPtr<FJsonObject> SubElement = ConvertActionSchemaToJSON(ActionSchema, Parameters.Elements[SubElementIdx]);
				SubElement->SetNumberField(TEXT("Index"), SubElementIdx);
				SubObject->SetObjectField(Parameters.ElementNames[SubElementIdx].ToString(), SubElement);
			}

			Object->SetObjectField(TEXT("Elements"), SubObject);
			break;
		}

		case Action::EType::OrInclusive:
		{
			const Action::FSchemaOrInclusiveParameters Parameters = ActionSchema.GetOrInclusive(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("OrInclusive"));

			TSharedPtr<FJsonObject> SubObject = MakeShared<FJsonObject>();
			for (int32 SubElementIdx = 0; SubElementIdx < Parameters.Elements.Num(); SubElementIdx++)
			{
				TSharedPtr<FJsonObject> SubElement = ConvertActionSchemaToJSON(ActionSchema, Parameters.Elements[SubElementIdx]);
				SubElement->SetNumberField(TEXT("Index"), SubElementIdx);
				SubObject->SetObjectField(Parameters.ElementNames[SubElementIdx].ToString(), SubElement);
			}

			Object->SetObjectField(TEXT("Elements"), SubObject);
			break;
		}

		case Action::EType::Array:
		{
			const Action::FSchemaArrayParameters Parameters = ActionSchema.GetArray(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("Array"));
			Object->SetNumberField(TEXT("Num"), Parameters.Num);
			Object->SetObjectField(TEXT("Element"), ConvertActionSchemaToJSON(ActionSchema, Parameters.Element));
			break;
		}

		case Action::EType::Encoding:
		{
			const Action::FSchemaEncodingParameters Parameters = ActionSchema.GetEncoding(ActionSchemaElement);

			Object->SetStringField(TEXT("Type"), TEXT("Encoding"));
			Object->SetNumberField(TEXT("EncodingSize"), Parameters.EncodingSize);
			Object->SetObjectField(TEXT("Element"), ConvertActionSchemaToJSON(ActionSchema, Parameters.Element));
			break;
		}

		default:
			checkNoEntry();
		}

		return Object;
	}

	const TCHAR* GetDeviceString(const ETrainerDevice Device)
	{
		switch (Device)
		{
		case ETrainerDevice::GPU: return TEXT("GPU");
		case ETrainerDevice::CPU: return TEXT("CPU");
		default: checkNoEntry(); return TEXT("Unknown");
		}
	}

	const TCHAR* GetResponseString(const ETrainerResponse Response)
	{
		switch (Response)
		{
		case ETrainerResponse::Success: return TEXT("Success");
		case ETrainerResponse::Unexpected: return TEXT("Unexpected communication received");
		case ETrainerResponse::Completed: return TEXT("Training completed");
		case ETrainerResponse::Stopped: return TEXT("Training stopped");
		case ETrainerResponse::Timeout: return TEXT("Communication timeout");
		default: checkNoEntry(); return TEXT("Unknown");
		}
	}

	float DiscountFactorFromHalfLife(const float HalfLife, const float DeltaTime)
	{
		return FMath::Pow(0.5f, DeltaTime / FMath::Max(HalfLife, UE_SMALL_NUMBER));
	}

	float DiscountFactorFromHalfLifeSteps(const int32 HalfLifeSteps)
	{
		checkf(HalfLifeSteps >= 1, TEXT("Number of HalfLifeSteps should be at least 1 but got %i"), HalfLifeSteps);

		return FMath::Pow(0.5f, 1.0f / FMath::Max(HalfLifeSteps, 1));
	}

	FString GetPythonExecutablePath(const FString& IntermediateDir)
	{
		checkf(PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX, TEXT("Python only supported on Windows, Mac, and Linux."));

		return IntermediateDir / TEXT("PipInstall") / (PLATFORM_WINDOWS ? TEXT("Scripts/python.exe") : TEXT("bin/python3"));
	}

	FString GetSitePackagesPath(const FString& EngineDir)
	{
		checkf(PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX, TEXT("Python only supported on Windows, Mac, and Linux."));

		return EngineDir / TEXT("Plugins/Experimental/PythonFoundationPackages/Content/Python/Lib") / FPlatformMisc::GetUBTPlatform() / TEXT("site-packages");
	}

	FString GetPythonContentPath(const FString& EngineDir)
	{
		return EngineDir / TEXT("Plugins/Experimental/LearningAgents/Content/Python/");
	}

	FString GetProjectPythonContentPath()
	{
		return FPaths::ProjectContentDir() / TEXT("Python/");
	}

	FString GetIntermediatePath(const FString& IntermediateDir)
	{
		return IntermediateDir / TEXT("LearningAgents");
	}

}