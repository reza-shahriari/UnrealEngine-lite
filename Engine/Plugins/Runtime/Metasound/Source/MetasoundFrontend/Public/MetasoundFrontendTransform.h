// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Internationalization/Text.h"
#include "Interfaces/MetasoundFrontendInterfaceRegistry.h"
#include "MetasoundAssetBase.h"
#include "MetasoundFrontendController.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundVertex.h"
#include "Templates/Function.h"

#define UE_API METASOUNDFRONTEND_API


// Forward Declarations
struct FMetaSoundFrontendDocumentBuilder;


namespace Metasound::Frontend
{
	namespace DocumentTransform
	{
#if WITH_EDITOR
		using FGetNodeDisplayNameProjection = TFunction<FText(const FNodeHandle&)>;
		using FGetNodeDisplayNameProjectionRef = TFunctionRef<FText(const FNodeHandle&)>;

		METASOUNDFRONTEND_API bool GetVersioningLoggingEnabled();
		METASOUNDFRONTEND_API void SetVersioningLoggingEnabled(bool bIsEnabled);
		METASOUNDFRONTEND_API void RegisterNodeDisplayNameProjection(FGetNodeDisplayNameProjection&& InNameProjection);
		METASOUNDFRONTEND_API FGetNodeDisplayNameProjectionRef GetNodeDisplayNameProjection();
#endif // WITH_EDITOR
	}

	/** Interface for transforms applied to documents. */
	class IDocumentTransform
	{
	public:
		virtual ~IDocumentTransform() = default;

		/** Return true if InDocument was modified, false otherwise. */
		virtual bool Transform(FDocumentHandle InDocument) const = 0;

		/** Return true if InDocument was modified, false otherwise.
			* This function is soft deprecated.  It is not pure virtual
			* to grandfather in old transform implementation. Old transforms
			* should be deprecated and rewritten to use the Controller-less
			* API in the interest of better performance and simplicity.
			*/
		UE_API virtual bool Transform(FMetasoundFrontendDocument& InOutDocument) const;
	};

	/** Interface for transforms applied to a graph. */
	class IGraphTransform
	{
	public:
		virtual ~IGraphTransform() = default;

		UE_DEPRECATED(5.3, "Deprecated: unused function which encouraged breaking const behavior of transform state.")
		virtual FMetasoundFrontendDocument& GetOwningDocument() const { static FMetasoundFrontendDocument BaseDoc; return BaseDoc; };

		/** Return true if the graph was modified, false otherwise. */
		virtual bool Transform(FMetasoundFrontendGraph& InOutGraph) const = 0;
	};

	/** Interface for transforming a node. */
	class INodeTransform
	{
	public:
		virtual ~INodeTransform() = default;

		/** Return true if the node was modified, false otherwise. */
		UE_API virtual bool Transform(const FGuid& InNodeID, FMetaSoundFrontendDocumentBuilder& OutBuilder) const;

		UE_DEPRECATED(5.4, "Use transform overload with node ID and builder parameters to apply node transform to builder's underlying document")
		UE_API virtual bool Transform(FMetasoundFrontendNode& InOutNode) const;
	};

	/** Adds or swaps document members (inputs, outputs) and removing any document members where necessary and adding those missing. */
	class FModifyRootGraphInterfaces : public IDocumentTransform
	{
	public:
		UE_API FModifyRootGraphInterfaces(const TArray<FMetasoundFrontendInterface>& InInterfacesToRemove, const TArray<FMetasoundFrontendInterface>& InInterfacesToAdd);
		UE_API FModifyRootGraphInterfaces(const TArray<FMetasoundFrontendVersion>& InInterfaceVersionsToRemove, const TArray<FMetasoundFrontendVersion>& InInterfaceVersionsToAdd);

#if WITH_EDITOR
		// Whether or not to propagate node locations to new members. Setting to false
		// results in members not having a default physical location in the editor graph.
		UE_API void SetDefaultNodeLocations(bool bInSetDefaultNodeLocations);
#endif // WITH_EDITOR

		// Override function used to match removed members with added members, allowing
		// transform to preserve connections made between removed interface members & new interface members
		// that may be related but not be named the same.
		UE_API void SetNamePairingFunction(const TFunction<bool(FName, FName)>& InNamePairingFunction);

		UE_API virtual bool Transform(FDocumentHandle InDocument) const override;
		UE_API virtual bool Transform(FMetasoundFrontendDocument& InOutDocument) const override;

	private:
		bool AddMissingVertices(FGraphHandle GraphHandle) const;
		void Init(const TFunction<bool(FName, FName)>* InNamePairingFunction = nullptr);
		bool SwapPairedVertices(FGraphHandle GraphHandle) const;
		bool RemoveUnsupportedVertices(FGraphHandle GraphHandle) const;
		bool UpdateInterfacesInternal(FDocumentHandle DocumentHandle) const;

#if WITH_EDITORONLY_DATA
		void UpdateAddedVertexNodePositions(FGraphHandle GraphHandle) const;

		bool bSetDefaultNodeLocations = true;
#endif // WITH_EDITORONLY_DATA

		TArray<FMetasoundFrontendInterface> InterfacesToRemove;
		TArray<FMetasoundFrontendInterface> InterfacesToAdd;

		using FVertexPair = TTuple<FMetasoundFrontendClassVertex, FMetasoundFrontendClassVertex>;
		TArray<FVertexPair> PairedInputs;
		TArray<FVertexPair> PairedOutputs;

		struct FInputData
		{
			FMetasoundFrontendClassInput Input;
			const FMetasoundFrontendInterface* InputInterface = nullptr;
		};

		struct FOutputData
		{
			FMetasoundFrontendClassOutput Output;
			const FMetasoundFrontendInterface* OutputInterface = nullptr;
		};

		TArray<FInputData> InputsToAdd;
		TArray<FMetasoundFrontendClassInput> InputsToRemove;
		TArray<FOutputData> OutputsToAdd;
		TArray<FMetasoundFrontendClassOutput> OutputsToRemove;

	};

	class FUpdateRootGraphInterface : public IDocumentTransform
	{
	public:
		FUpdateRootGraphInterface(const FMetasoundFrontendVersion& InInterfaceVersion, const FString& InOwningAssetName=FString(TEXT("Unknown")))
		{
		}

		UE_DEPRECATED(5.5, "RootGraph update is now handled privately by internal MetaSound asset management")
		virtual bool Transform(FDocumentHandle InDocument) const override { return false; }
	};

	/** Completely rebuilds the graph connecting a preset's inputs to the reference
		* document's root graph. It maintains previously set input values entered upon 
		* the presets wrapping graph. */
	class FRebuildPresetRootGraph : public IDocumentTransform
	{
	public:
		/** Create transform.
			* @param InReferenceDocument - The document containing the wrapped MetaSound graph.
			*/
		FRebuildPresetRootGraph(FConstDocumentHandle InReferencedDocument)
			: ReferencedDocument(InReferencedDocument)
		{
		}

		UE_API FRebuildPresetRootGraph(const FMetasoundFrontendDocument& InReferencedDocument);

		UE_API virtual bool Transform(FDocumentHandle InDocument) const override;
		UE_API virtual bool Transform(FMetasoundFrontendDocument& InOutDocument) const override;

	private:

		// Get the class inputs needed for this preset. Input literals set on 
		// the preset graph will be used if they are set and are marked as inheriting
		// the default from the referenced graph.
		UE_API TArray<FMetasoundFrontendClassInput> GenerateRequiredClassInputs(FDocumentHandle& InDocumentHandle, const FConstGraphHandle& InParentGraph, TSet<FName>& OutInputsInheritingDefault) const;

		// Get the class Outputs needed for this preset.
		UE_API TArray<FMetasoundFrontendClassOutput> GenerateRequiredClassOutputs(FDocumentHandle& InDocumentHandle, const FConstGraphHandle& InParentGraph) const;

		// Add inputs to parent graph and connect to wrapped graph node.
		UE_API void AddAndConnectInputs(const TArray<FMetasoundFrontendClassInput>& InClassInputs, FGraphHandle& InParentGraphHandle, FNodeHandle& InReferencedNode) const;

		// Add outputs to parent graph and connect to wrapped graph node.
		UE_API void AddAndConnectOutputs(const TArray<FMetasoundFrontendClassOutput>& InClassOutputs, FGraphHandle& InParentGraphHandle, FNodeHandle& InReferencedNode) const;

#if WITH_EDITORONLY_DATA
		using FMemberIDToMetadataMap = TMap<FGuid, TObjectPtr<UMetaSoundFrontendMemberMetadata>>;
		UE_API void AddMemberMetadata(const FMemberIDToMetadataMap& InCachedMemberMetadata, FGraphHandle& InPresetGraphHandle, FMemberIDToMetadataMap& InOutMemberMetadata) const;
#endif // WITH_EDITORONLY_DATA

		FConstDocumentHandle ReferencedDocument = IDocumentController::GetInvalidHandle();
	};

	/** Automatically updates all nodes and respective dependencies in graph where
		* newer versions exist in the loaded MetaSound Class Node Registry.
		*/
	class FAutoUpdateRootGraph 
	{
	public:
		/** Construct an AutoUpdate transform
			*
			* @param InDebugAssetPath - Asset path used for debug logs on warnings and errors.
			* @param bInLogWarningOnDroppedConnections - If true, warnings will be logged if a node update results in a dropped connection.
			*/
		FAutoUpdateRootGraph(FString&& InDebugAssetPath, bool bInLogWarningOnDroppedConnection)
			: DebugAssetPath(MoveTemp(InDebugAssetPath))
			, bLogWarningOnDroppedConnection(bInLogWarningOnDroppedConnection)
		{
		}

		UE_API bool Transform(FDocumentHandle InDocument);

	private:
		// Keeps track of classes already updated so node check can be avoided.
		// Hack to avoid issue where earlier auto-update passes on pages can
		// clear out internal change state of a class in the registry causing
		// nodes to get ignored on later page auto-update passes.
		TSet<FGuid> UpdatedClasses;
		const FString DebugAssetPath;
		bool bLogWarningOnDroppedConnection;
	};

	/** Sets the document's graph class, optionally updating the namespace and variant. */
	class FRenameRootGraphClass : public IDocumentTransform
	{
		const FMetasoundFrontendClassName NewClassName;

	public:
		UE_DEPRECATED(5.5, "Use FMetasoundFrontendDocumentBuilder::GenerateNewClassName instead")
		static bool Generate(FDocumentHandle InDocument, const FGuid& InGuid, const FName Namespace = { }, const FName Variant = { })
		{
			return false;
		}

		UE_DEPRECATED(5.5, "Use FMetasoundFrontendDocumentBuilder::GenerateNewClassName instead")
		static bool Generate(FMetasoundFrontendDocument& InDocument, const FGuid& InGuid, const FName Namespace = { }, const FName Variant = { })
		{
			return false;
		}

		UE_DEPRECATED(5.5, "Use FMetasoundFrontendDocumentBuilder::GenerateNewClassName instead")
		FRenameRootGraphClass(const FMetasoundFrontendClassName InClassName)
			: NewClassName(InClassName)
		{
		}

		UE_DEPRECATED(5.5, "Use FMetasoundFrontendDocumentBuilder::GenerateNewClassName instead")
		UE_API bool Transform(FDocumentHandle InDocument) const override;

		UE_DEPRECATED(5.5, "Use FMetasoundFrontendDocumentBuilder::GenerateNewClassName instead")
		UE_API bool Transform(FMetasoundFrontendDocument& InOutDocument) const override;
	};
} // Metasound::Frontend

#undef UE_API
