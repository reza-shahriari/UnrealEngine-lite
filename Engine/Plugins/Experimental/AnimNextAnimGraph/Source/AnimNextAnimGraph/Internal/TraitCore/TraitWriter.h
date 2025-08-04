// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "Serialization/MemoryWriter.h"

#include "TraitCore/LatentPropertyHandle.h"
#include "TraitCore/NodeHandle.h"
#include "TraitCore/NodeTemplateRegistryHandle.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AnimNext
{
	struct FNodeTemplate;

	/**
	  * FTraitWriter
	  *
	  * The trait writer is used to write a serialized binary blob that contains
	  * the anim graph data. An anim graph contains the following:
	  *     - A list of FNodeTemplates that the nodes use
	  *     - The graph shared data (FNodeDescription for every node)
	  */
	class FTraitWriter final : public FMemoryWriter
	{
	public:
		enum class EErrorState
		{
			None,					// All good, no error
			TooManyNodes,			// Exceeded the maximum number of nodes in a graph, @see FNodeDescription::MAXIMUM_COUNT
			NodeTemplateNotFound,	// Failed to find a necessary node template
			NodeTemplateTooLarge,	// Exceeded the maximum node template size, @see FNodeTemplate::MAXIMUM_SIZE
			NodeHandleNotFound,		// Failed to find the mapping for a node handle, it was likely not registered
		};

		ANIMNEXTANIMGRAPH_API FTraitWriter();

		// Registers an instance of the provided node template and assigns a node handle and node UID to it
		[[nodiscard]] ANIMNEXTANIMGRAPH_API FNodeHandle RegisterNode(const FNodeTemplate& NodeTemplate);

		// Called before node writing can begin
		ANIMNEXTANIMGRAPH_API void BeginNodeWriting();

		// Called once node writing has terminated
		ANIMNEXTANIMGRAPH_API void EndNodeWriting();

		// Writes out the provided node using the trait properties
		// Nodes must be written in the same order they were registered in
		ANIMNEXTANIMGRAPH_API void WriteNode(
			const FNodeHandle NodeHandle,
			const TFunction<FString(uint32 TraitIndex, FName PropertyName)>& GetTraitProperty,
			const TFunction<uint16(uint32 TraitIndex, FName PropertyName)>& GetTraitLatentPropertyIndex
			);

		// Returns the error state
		[[nodiscard]] ANIMNEXTANIMGRAPH_API EErrorState GetErrorState() const;

		// Returns the populated raw graph shared data buffer
		[[nodiscard]] ANIMNEXTANIMGRAPH_API const TArray<uint8>& GetGraphSharedData() const;

		// Returns the list of referenced UObjects in this graph
		[[nodiscard]] ANIMNEXTANIMGRAPH_API const TArray<UObject*>& GetGraphReferencedObjects() const;

		// Returns the list of referenced soft objects in this graph
		[[nodiscard]] ANIMNEXTANIMGRAPH_API const TArray<FSoftObjectPath>& GetGraphReferencedSoftObjects() const;

		// FArchive implementation
		ANIMNEXTANIMGRAPH_API virtual FArchive& operator<<(UObject*& Obj) override;
		ANIMNEXTANIMGRAPH_API virtual FArchive& operator<<(FObjectPtr& Obj) override;
		ANIMNEXTANIMGRAPH_API virtual FArchive& operator<<(FSoftObjectPath& Value) override;
		ANIMNEXTANIMGRAPH_API virtual FArchive& operator<<(FSoftObjectPtr& Value) override;
		ANIMNEXTANIMGRAPH_API virtual FArchive& operator<<(FWeakObjectPtr& Value) override;

	private:
		struct FNodeMapping
		{
			// The node handle for this entry (encoded as a node ID)
			FNodeHandle NodeHandle;

			// The node template handle the node uses
			FNodeTemplateRegistryHandle NodeTemplateHandle;

			// The unique node template index that we'll serialize
			uint32 NodeTemplateIndex;
		};

		TArray<uint8> GraphSharedDataArchiveBuffer;
		TArray<FNodeMapping> NodeMappings;

		// To track the node registration process
		FNodeID NextNodeID;

		// To track node writing
		TArray<UObject*> GraphReferencedObjects;
		TArray<FSoftObjectPath> GraphReferencedSoftObjects;
		uint32 NumNodesWritten;
		bool bIsNodeWriting;

		EErrorState ErrorState;
	};
}
#endif
