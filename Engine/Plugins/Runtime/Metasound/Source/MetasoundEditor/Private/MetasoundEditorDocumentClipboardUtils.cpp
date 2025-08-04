// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundEditorDocumentClipboardUtils.h"

#include "Algo/Transform.h"
#include "EdGraphUtilities.h"
#include "Exporters/Exporter.h"
#include "Factories.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Logging/TokenizedMessage.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendSearchEngine.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateInput.h"
#include "ScopedTransaction.h"
#include "UnrealExporter.h"

namespace Metasound::Editor
{
	void FDocumentClipboardUtils::ProcessPastedInputNodes(FMetasoundAssetBase& OutAsset, TArray<UMetasoundEditorGraphNode*>& OutPastedNodes)
	{
		using namespace Engine;
		using namespace Frontend;

		FMetaSoundFrontendDocumentBuilder& Builder = FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(OutAsset.GetOwningAsset());

		TMap<FName, TObjectPtr<UMetasoundEditorGraphInput>> MappedGeneratedInputNames;
		UMetasoundEditorGraph& Graph = *CastChecked<UMetasoundEditorGraph>(&OutAsset.GetGraphChecked());
		for (int32 Index = OutPastedNodes.Num() - 1; Index >= 0; --Index)
		{
			UMetasoundEditorGraphInputNode* InputNode = Cast<UMetasoundEditorGraphInputNode>(OutPastedNodes[Index]);
			if (!InputNode)
			{
				continue;
			}

			InputNode->CreateNewGuid();
			TObjectPtr<UMetasoundEditorGraphInput>& Input = InputNode->Input;
			if (!Input || !Graph.ContainsInput(*Input))
			{
				Input = nullptr;

				bool bNameMatchFound = false;
				const FMetasoundEditorGraphVertexNodeBreadcrumb& Breadcrumb = InputNode->GetBreadcrumb();
				Graph.IterateInputs([&Input, &InputNode, &Graph, &Breadcrumb, &bNameMatchFound](UMetasoundEditorGraphInput& TestInput)
				{
					FConstNodeHandle InputHandle = TestInput.GetConstNodeHandle();
					FConstOutputHandle TestOutput = InputHandle->GetConstOutputs().Last();
					const bool bTypeMatches = TestOutput->GetDataType() == Breadcrumb.DataType;
					const bool bAccessMatches = TestOutput->GetVertexAccessType() == Breadcrumb.AccessType;
					const bool bNameMatches = InputHandle->GetNodeName() == Breadcrumb.MemberName;
					bNameMatchFound |= bNameMatches;
					if (bTypeMatches && bAccessMatches && bNameMatches)
					{
						Input = &TestInput;
					}
				});

				if (!Input)
				{
					FDataTypeRegistryInfo Info;
					if (!Input && IDataTypeRegistry::Get().GetDataTypeInfo(Breadcrumb.DataType, Info))
					{
						const FName InputName = Breadcrumb.MemberName;
						if (TObjectPtr<UMetasoundEditorGraphInput>* InputNodeHandle = MappedGeneratedInputNames.Find(InputName))
						{
							Input = *InputNodeHandle;
						}
						else
						{
							FCreateNodeVertexParams VertexParams;
							VertexParams.DataType = Breadcrumb.DataType;
							VertexParams.AccessType = Breadcrumb.AccessType;

							TArray<FMetasoundFrontendClassInputDefault> InputDefaults;
							Algo::Transform(Breadcrumb.DefaultLiterals, InputDefaults, [](const TPair<FGuid, FMetasoundFrontendLiteral>& Pair)
							{
								return FMetasoundFrontendClassInputDefault(Pair.Key, Pair.Value);
							});

							const FMetasoundFrontendNode* NewNode = nullptr;
							{
								FMetasoundFrontendClassInput ClassInput = FGraphBuilder::CreateUniqueClassInput(*OutAsset.GetOwningAsset(), VertexParams, InputDefaults, &Breadcrumb.MemberName);
								ClassInput.Metadata = Breadcrumb.VertexMetadata;
								NewNode = Builder.AddGraphInput(MoveTemp(ClassInput));
							}

							if (NewNode)
							{
								Input = Graph.FindOrAddInput(NewNode->GetID());
								if (Breadcrumb.MemberMetadataPath.IsSet())
								{
									UObject* MemberMetadata = Breadcrumb.MemberMetadataPath->TryLoad();
									if (UMetasoundEditorGraphMemberDefaultLiteral* DefaultLiteral = Cast<UMetasoundEditorGraphMemberDefaultLiteral>(MemberMetadata))
									{
										Builder.ClearMemberMetadata(NewNode->GetID());
										UMetaSoundEditorSubsystem& MetaSoundEditorSubsystem = UMetaSoundEditorSubsystem::GetChecked();
										TSubclassOf<UMetasoundEditorGraphMemberDefaultLiteral> LiteralClass = MetaSoundEditorSubsystem.GetLiteralClassForType(Breadcrumb.DataType);
										MetaSoundEditorSubsystem.BindMemberMetadata(Builder, *Input, LiteralClass, DefaultLiteral);
									}
								}
								MappedGeneratedInputNames.Add(Breadcrumb.MemberName, Input);
							}
						}
					}
				}
			}

			if (Input)
			{
				const FMetasoundFrontendNode* InputTemplateNode = FInputNodeTemplate::CreateNode(Builder, Input->GetMemberName());
				if (ensure(InputTemplateNode))
				{
					FGuid TemplateNodeID = InputTemplateNode->GetID();
					InputNode->NodeID = TemplateNodeID;

					// Remove default node location from input node. 
					// Correct node location from the ed graph node will be set subsequently in ProcessPastedNodePositions
					TArray<FGuid> NodeLocationGuids;
					InputTemplateNode->Style.Display.Locations.GetKeys(NodeLocationGuids);
					if (!NodeLocationGuids.IsEmpty())
					{
						Builder.RemoveNodeLocation(TemplateNodeID);
					}
				}
			}
			else
			{
				constexpr bool bAllowShrinking = false;
				Graph.RemoveNode(InputNode);
				OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
	}

	void FDocumentClipboardUtils::ProcessPastedOutputNodes(FMetasoundAssetBase& OutAsset, TArray<UMetasoundEditorGraphNode*>& OutPastedNodes, FDocumentPasteNotifications& OutNotifications)
	{
		using namespace Frontend;
		using namespace Engine;
		FMetaSoundFrontendDocumentBuilder& Builder = FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(OutAsset.GetOwningAsset());
		UMetasoundEditorGraph& Graph = *CastChecked<UMetasoundEditorGraph>(&OutAsset.GetGraphChecked());
		for (int32 Index = OutPastedNodes.Num() - 1; Index >= 0; --Index)
		{
			UMetasoundEditorGraphOutputNode* OutputNode = Cast<UMetasoundEditorGraphOutputNode>(OutPastedNodes[Index]);
			if (!OutputNode)
			{
				continue;
			}

			OutputNode->CreateNewGuid();

			if (OutputNode->Output && Graph.ContainsOutput(*OutputNode->Output))
			{
				auto IsOtherMatchingNode = [&OutputNode](const TObjectPtr<UEdGraphNode>& EdNode)
				{
					if (OutputNode != EdNode.Get())
					{
						if (UMetasoundEditorGraphOutputNode* OtherOutputNode = Cast<UMetasoundEditorGraphOutputNode>(EdNode))
						{
							return OutputNode->GetNodeID() == OtherOutputNode->GetNodeID();
						}
					}
					return false;
				};

				// Can only have one output reference node
				if (Graph.Nodes.ContainsByPredicate(IsOtherMatchingNode))
				{
					OutNotifications.bPastedNodesAddMultipleOutputNodes = true;
					Graph.RemoveNode(OutputNode);
					OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
				}
			}
			else
			{
				// Add output if doesn't exist 
				const FMetasoundEditorGraphVertexNodeBreadcrumb& Breadcrumb = OutputNode->GetBreadcrumb();

				FDataTypeRegistryInfo Info;
				if (IDataTypeRegistry::Get().GetDataTypeInfo(Breadcrumb.DataType, Info))
				{
					FCreateNodeVertexParams VertexParams;
					VertexParams.DataType = Breadcrumb.DataType;
					VertexParams.AccessType = Breadcrumb.AccessType;

					FMetasoundFrontendClassOutput ClassOutput = FGraphBuilder::CreateUniqueClassOutput(*OutAsset.GetOwningAsset(), VertexParams, &Breadcrumb.MemberName);
					ClassOutput.Metadata = Breadcrumb.VertexMetadata;

					if (const FMetasoundFrontendNode* NewNode = Builder.AddGraphOutput(ClassOutput))
					{
						UMetasoundEditorGraphOutput* Output = Graph.FindOrAddOutput(NewNode->GetID());
						if (Output)
						{
							if (Breadcrumb.MemberMetadataPath.IsSet())
							{
								UObject* MemberMetadata = Breadcrumb.MemberMetadataPath->TryLoad();
								if (UMetasoundEditorGraphMemberDefaultLiteral* DefaultLiteral = Cast<UMetasoundEditorGraphMemberDefaultLiteral>(MemberMetadata))
								{
									Builder.ClearMemberMetadata(ClassOutput.NodeID);
									UMetaSoundEditorSubsystem& MetaSoundEditorSubsystem = UMetaSoundEditorSubsystem::GetChecked();
									TSubclassOf<UMetasoundEditorGraphMemberDefaultLiteral> LiteralClass = MetaSoundEditorSubsystem.GetLiteralClassForType(Breadcrumb.DataType);
									MetaSoundEditorSubsystem.BindMemberMetadata(Builder, *Output, LiteralClass, DefaultLiteral);
								}
							}

							// Remove default node location from output node. 
							// Correct node location from the ed graph node will be set subsequently in ProcessPastedNodePositions
							TArray<FGuid> NodeLocationGuids;
							NewNode->Style.Display.Locations.GetKeys(NodeLocationGuids);
							if (!NodeLocationGuids.IsEmpty())
							{
								Builder.RemoveNodeLocation(NewNode->GetID());
							}

							OutputNode->Output = Output;
						}
						else
						{
							Graph.RemoveNode(OutputNode);
							OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
						}
					}
				}
			}
		}
	}

	void FDocumentClipboardUtils::ProcessPastedVariableNodes(FMetasoundAssetBase& OutAsset, TArray<UMetasoundEditorGraphNode*>& OutPastedNodes, FDocumentPasteNotifications& OutNotifications)
	{
		using namespace Frontend;

		OutNotifications.bPastedNodesAddMultipleVariableSetters = false;

		UMetasoundEditorGraph& Graph = *CastChecked<UMetasoundEditorGraph>(&OutAsset.GetGraphChecked());
		FMetaSoundFrontendDocumentBuilder& DocBuilder = Graph.GetBuilderChecked().GetBuilder();
		TMap<FName, TObjectPtr<UMetasoundEditorGraphVariable>> MappedGeneratedVariableNames;
		Graph.IterateVariables([&MappedGeneratedVariableNames](UMetasoundEditorGraphVariable& Variable)
		{
			MappedGeneratedVariableNames.Add(Variable.GetMemberName(), &Variable);
		});

		for (int32 Index = OutPastedNodes.Num() - 1; Index >= 0; --Index)
		{
			UMetasoundEditorGraphVariableNode* VariableNode = Cast<UMetasoundEditorGraphVariableNode>(OutPastedNodes[Index]);
			if (!VariableNode)
			{
				continue;
			}

			VariableNode->CreateNewGuid();

			TObjectPtr<UMetasoundEditorGraphVariable>& Variable = VariableNode->Variable;
			if (!Variable || !Graph.ContainsVariable(*Variable) || Variable->GetFrontendVariable() == nullptr)
			{
				const FMetasoundEditorGraphMemberNodeBreadcrumb& Breadcrumb = VariableNode->Breadcrumb;
				const FName BaseName = Breadcrumb.MemberName;
				TObjectPtr<UMetasoundEditorGraphVariable> CachedVariable = MappedGeneratedVariableNames.FindRef(BaseName);
				const FMetasoundFrontendVariable* FrontendVariable = nullptr;
				if (CachedVariable)
				{
					FrontendVariable = CachedVariable->GetFrontendVariable();
				}

				if (FrontendVariable && FrontendVariable->TypeName == Breadcrumb.DataType)
				{
					Variable = CachedVariable;
				}
				else
				{
					const FMetasoundFrontendLiteral* Literal = Breadcrumb.DefaultLiterals.Find(Frontend::DefaultPageID);
					const FName VariableName = FGraphBuilder::GenerateUniqueVariableName(DocBuilder, BaseName.ToString());
					FrontendVariable = DocBuilder.AddGraphVariable(
						VariableName,
						Breadcrumb.DataType,
						Literal,
						&Breadcrumb.VertexMetadata.GetDisplayName(),
						&Breadcrumb.VertexMetadata.GetDescription()
					);

					Variable = Graph.FindOrAddVariable(FrontendVariable->Name);
					check(Variable);

					VariableNode->CacheBreadcrumb(); // Name of referenced variable/node state has changed so make sure up-to-date in case breadcrumb is used later
					MappedGeneratedVariableNames.Add(BaseName, Variable);
				}
			}

			const FMetasoundFrontendVariable* FrontendVariable = Variable->GetFrontendVariable();
			if (ensure(FrontendVariable))
			{
				// Can only have one mutator/setter node
				const FNodeClassName NodeClassName = VariableNode->GetClassName().ToNodeClassName();
				const bool bMatchesMutatorNodeID = VariableNode->GetNodeID() == FrontendVariable->MutatorNodeID;
				bool bIsDuplicateMutatorNode = false;
				if (const FMetasoundFrontendNode* MutatorNode = DocBuilder.FindNode(FrontendVariable->MutatorNodeID))
				{
					const FMetasoundFrontendClass* MutatorClass = DocBuilder.FindDependency(MutatorNode->ClassID);
					check(MutatorClass);
					bIsDuplicateMutatorNode = MutatorClass->Metadata.GetClassName() == NodeClassName;
				}
				if (bMatchesMutatorNodeID || bIsDuplicateMutatorNode)
				{
					OutNotifications.bPastedNodesAddMultipleVariableSetters = true;
					OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
					Graph.RemoveNode(VariableNode);
				}
				else
				{
					// Add new variable node
					FMetasoundFrontendClass FrontendClass;
					bool bDidFindClassWithName = ISearchEngine::Get().FindClassWithHighestVersion(NodeClassName, FrontendClass);
					if (ensure(bDidFindClassWithName))
					{
						if (const FMetasoundFrontendNode* NewNode = DocBuilder.AddGraphVariableNode(FrontendVariable->Name, FrontendClass.Metadata.GetType()))
						{
							VariableNode->SetNodeID(NewNode->GetID());
						}
						else
						{
							OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
							Graph.RemoveNode(VariableNode);
						}
					}
				}
			}
			else
			{
				OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
				Graph.RemoveNode(VariableNode);
			}
		}
	}

	void FDocumentClipboardUtils::ProcessPastedExternalNodes(FMetasoundAssetBase& OutAsset, TArray<UMetasoundEditorGraphNode*>& OutPastedNodes, FDocumentPasteNotifications& OutNotifications)
	{
		using namespace Engine;
		using namespace Frontend;

		OutNotifications.bPastedNodesCreateLoop = false;

		UMetasoundEditorGraph& Graph = *CastChecked<UMetasoundEditorGraph>(&OutAsset.GetGraphChecked());
		for (int32 Index = OutPastedNodes.Num() - 1; Index >= 0; --Index)
		{
			UMetasoundEditorGraphExternalNode* ExternalNode = Cast<UMetasoundEditorGraphExternalNode>(OutPastedNodes[Index]);
			if (!ExternalNode)
			{
				continue;
			}

			ExternalNode->CreateNewGuid();
			FMetasoundFrontendClassMetadata LookupMetadata;
			const FMetasoundEditorGraphNodeBreadcrumb& Breadcrumb = ExternalNode->GetBreadcrumb();
			LookupMetadata.SetClassName(Breadcrumb.ClassName);
			LookupMetadata.SetType(EMetasoundFrontendClassType::External);
			const FNodeRegistryKey PastedRegistryKey(LookupMetadata);
			UObject& MetaSound = *OutAsset.GetOwningAsset();

			if (const FMetasoundAssetBase* Asset = IMetaSoundAssetManager::GetChecked().FindAsset(PastedRegistryKey))
			{
				if (OutAsset.AddingReferenceCausesLoop(*Asset))
				{
					FMetasoundFrontendClass MetaSoundClass;
					FMetasoundFrontendRegistryContainer::Get()->FindFrontendClassFromRegistered(PastedRegistryKey, MetaSoundClass);
					FString FriendlyClassName = MetaSoundClass.Metadata.GetDisplayName().ToString();
					if (FriendlyClassName.IsEmpty())
					{
						FriendlyClassName = MetaSoundClass.Metadata.GetClassName().ToString();
					}
					UE_LOG(LogMetaSound, Warning, TEXT("Failed to paste node with class '%s'.  Class would introduce cyclic asset dependency."), *FriendlyClassName);
					OutNotifications.bPastedNodesCreateLoop = true;
					OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
					Graph.RemoveNode(ExternalNode);
				}
				else
				{
					FMetaSoundFrontendDocumentBuilder& Builder = IDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(OutAsset.GetOwningAsset());
					const FMetasoundFrontendNode* NewNode = Builder.AddNodeByClassName(Breadcrumb.ClassName);
					if (NewNode)
					{
						const FGuid& NewNodeID = NewNode->GetID();
						ExternalNode->NodeID = NewNodeID;
						Builder.SetNodeConfiguration(NewNodeID, Breadcrumb.NodeConfiguration);
					}
					else
					{
						OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
						Graph.RemoveNode(ExternalNode);
					}
				}
			}
			else
			{
				if (const INodeTemplate* Template = INodeTemplateRegistry::Get().FindTemplate(Breadcrumb.ClassName))
				{
					FMetaSoundFrontendDocumentBuilder& Builder = IDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(OutAsset.GetOwningAsset());
					const FNodeTemplateGenerateInterfaceParams TemplateParams = Breadcrumb.TemplateParams.IsSet() ? *Breadcrumb.TemplateParams : FNodeTemplateGenerateInterfaceParams();
					const FMetasoundFrontendNode* TemplateNode = Builder.AddNodeByTemplate(*Template, TemplateParams);
					if (TemplateNode)
					{
						ExternalNode->NodeID = TemplateNode->GetID();
					}
					else
					{
						OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
						Graph.RemoveNode(ExternalNode);
					}
				}
				else
				{
					FMetasoundFrontendClass ExternalClass;
					if (ISearchEngine::Get().FindClassWithHighestVersion(Breadcrumb.ClassName, ExternalClass))
					{
						FMetaSoundFrontendDocumentBuilder& Builder = IDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(OutAsset.GetOwningAsset());
						const FMetasoundFrontendNode* NewNode = Builder.AddNodeByClassName(Breadcrumb.ClassName);
						if (NewNode)
						{
							const FGuid& NewNodeID = NewNode->GetID();
							ExternalNode->NodeID = NewNodeID;
							Builder.SetNodeConfiguration(NewNodeID, Breadcrumb.NodeConfiguration);
						}
						else
						{
							OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
							Graph.RemoveNode(ExternalNode);
						}
					}
					else
					{
						OutPastedNodes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
						Graph.RemoveNode(ExternalNode);
						UE_LOG(LogMetaSound, Warning, TEXT("Cannot add pasted node with class '%s': Node class not found"), *Breadcrumb.ClassName.ToString());
					}
				}
			}
		}
	}

	void FDocumentClipboardUtils::ProcessPastedCommentNodes(FMetasoundAssetBase& OutAsset, const TArrayView<UMetasoundEditorGraphCommentNode*> CommentNodes)
	{
		using namespace Engine;
		using namespace Frontend;

		UMetasoundEditorGraph& Graph = *CastChecked<UMetasoundEditorGraph>(&OutAsset.GetGraphChecked());
		UMetaSoundBuilderBase& Builder = FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(*OutAsset.GetOwningAsset());

		for (UMetasoundEditorGraphCommentNode* CommentNode : CommentNodes)
		{
			// Regenerate id
			CommentNode->CreateNewGuid();
			CommentNode->SetCommentID(CommentNode->NodeGuid);

			// Update frontend node
			FMetaSoundFrontendGraphComment& NewComment = Builder.FindOrAddGraphComment(CommentNode->GetCommentID());
			UMetasoundEditorGraphCommentNode::ConvertToFrontendComment(*CommentNode, NewComment);
		}
	}

	void FDocumentClipboardUtils::ProcessPastedNodePositions(FMetasoundAssetBase& OutAsset, const FVector2D& InLocation, TArray<UMetasoundEditorGraphNode*>& OutPastedNodes, const TArrayView<UMetasoundEditorGraphCommentNode*> CommentNodes)
	{
		using namespace Frontend;

		// Find average midpoint of nodes and offset subgraph accordingly
		FVector2D AvgNodePosition = FVector2D::ZeroVector;
		for (UEdGraphNode* Node : OutPastedNodes)
		{
			AvgNodePosition.X += Node->NodePosX;
			AvgNodePosition.Y += Node->NodePosY;
		}
		for (UEdGraphNode* Node : CommentNodes)
		{
			AvgNodePosition.X += Node->NodePosX;
			AvgNodePosition.Y += Node->NodePosY;
		}

		if (!OutPastedNodes.IsEmpty())
		{
			float InvNumNodes = 1.0f / (OutPastedNodes.Num() + CommentNodes.Num());
			AvgNodePosition.X *= InvNumNodes;
			AvgNodePosition.Y *= InvNumNodes;
		}

		// Set new node positions
		for (UEdGraphNode* GraphNode : OutPastedNodes)
		{
			GraphNode->NodePosX = (GraphNode->NodePosX - AvgNodePosition.X) + InLocation.X;
			GraphNode->NodePosY = (GraphNode->NodePosY - AvgNodePosition.Y) + InLocation.Y;

			GraphNode->SnapToGrid(SNodePanel::GetSnapGridSize());
			if (UMetasoundEditorGraphNode* MetasoundGraphNode = Cast<UMetasoundEditorGraphNode>(GraphNode))
			{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
				FNodeHandle NodeHandle = MetasoundGraphNode->GetNodeHandle();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
				if (ensure(NodeHandle->IsValid()))
				{
					const FVector2D NewNodeLocation = FVector2D(GraphNode->NodePosX, GraphNode->NodePosY);
					FMetasoundFrontendNodeStyle NodeStyle = NodeHandle->GetNodeStyle();
					NodeStyle.Display.Locations.FindOrAdd(MetasoundGraphNode->NodeGuid) = NewNodeLocation;
					NodeHandle->SetNodeStyle(NodeStyle);
				}
			}
		}

		// Set new comment node positions 
		for (UMetasoundEditorGraphCommentNode* CommentNode : CommentNodes)
		{
			CommentNode->NodePosX = (CommentNode->NodePosX - AvgNodePosition.X) + InLocation.X;
			CommentNode->NodePosY = (CommentNode->NodePosY - AvgNodePosition.Y) + InLocation.Y;
			CommentNode->UpdateFrontendNodeLocation();
		}
	}

	class FMemberClipboardObjectTextFactory : public FCustomizableTextObjectFactory
	{
	public:

		FMemberClipboardObjectTextFactory()
			: FCustomizableTextObjectFactory(GWarn)
			, Member(nullptr)
		{
		}

		class UMetasoundEditorGraphMember* Member;

	protected:

		virtual bool CanCreateClass(UClass* InObjectClass, bool& bOmitSubObjs) const override
		{
			if (InObjectClass->IsChildOf(UMetasoundEditorGraphMember::StaticClass()))
			{
				return true;
			}
			return false;
		}

		virtual void ProcessConstructedObject(UObject* CreatedObject) override
		{
			if (CreatedObject->IsA<UMetasoundEditorGraphMember>())
			{
				Member = CastChecked<UMetasoundEditorGraphMember>(CreatedObject);
			}
		}
	};

	void FDocumentClipboardUtils::ProcessPastedNodeConnections(FMetasoundAssetBase& OutAsset, TArray<UMetasoundEditorGraphNode*>& OutPastedNodes)
	{
		using namespace Frontend;

		for (UEdGraphNode* GraphNode : OutPastedNodes)
		{
			for (UEdGraphPin* Pin : GraphNode->Pins)
			{
				if (Pin->Direction != EGPD_Input)
				{
					continue;
				}

				FInputHandle InputHandle = FGraphBuilder::GetInputHandleFromPin(Pin);
				if (InputHandle->IsValid() && InputHandle->GetDataType() != GetMetasoundDataTypeName<FTrigger>())
				{
					FMetasoundFrontendLiteral LiteralValue;
					if (FGraphBuilder::GetPinLiteral(*Pin, LiteralValue))
					{
						if (const FMetasoundFrontendLiteral* ClassDefault = InputHandle->GetClassDefaultLiteral())
						{
							// Check equivalence with class default and don't set if they are equal. Copied node
							// pin has no information to indicate whether or not the literal was already set.
							if (!LiteralValue.IsEqual(*ClassDefault))
							{
								InputHandle->SetLiteral(LiteralValue);
							}
						}
						else
						{
							InputHandle->SetLiteral(LiteralValue);
						}
					}
				}

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (UMetasoundEditorGraphNode* Node = Cast<UMetasoundEditorGraphNode>(LinkedPin->GetOwningNode()))
					{
						FGraphBuilder::ConnectNodes(*Pin, *LinkedPin, false /* bConnectEdPins */);
					}
				}
			}
		}
	}

	TArray<UEdGraphNode*> FDocumentClipboardUtils::PasteClipboardString(const FText& InTransactionText, const FString& InClipboardString, const FVector2D& InLocation, UObject& OutMetaSound, FDocumentPasteNotifications& OutNotifications)
	{
		FMetasoundAssetBase* Asset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&OutMetaSound);
		check(Asset);

		const FScopedTransaction Transaction(InTransactionText);

		OutMetaSound.Modify();
		Asset->GetGraphChecked().Modify();

		TArray<UMetasoundEditorGraphCommentNode*> PastedCommentNodes;
		TArray<UMetasoundEditorGraphNode*> PastedGraphNodes;
		{
			TSet<UEdGraphNode*> PastedNodeSet;
			FEdGraphUtilities::ImportNodesFromText(Asset->GetGraph(), InClipboardString, PastedNodeSet);

			auto CastToMetaSoundNode = [](UEdGraphNode* Node) { return Cast<UMetasoundEditorGraphNode>(Node); };
			Algo::TransformIf(PastedNodeSet, PastedGraphNodes, CastToMetaSoundNode, CastToMetaSoundNode);

			auto CastToCommentNode = [](UEdGraphNode* Node) { return Cast<UMetasoundEditorGraphCommentNode>(Node); };
			Algo::TransformIf(PastedNodeSet, PastedCommentNodes, CastToCommentNode, CastToCommentNode);
		}

		TArray<UEdGraphNode*> PastedNodes;
		if (PastedGraphNodes.IsEmpty() && PastedCommentNodes.IsEmpty())
		{
			return PastedNodes;
		}

		ProcessPastedCommentNodes(*Asset, PastedCommentNodes);
		ProcessPastedInputNodes(*Asset, PastedGraphNodes);
		ProcessPastedOutputNodes(*Asset, PastedGraphNodes, OutNotifications);
		ProcessPastedVariableNodes(*Asset, PastedGraphNodes, OutNotifications);
		ProcessPastedExternalNodes(*Asset, PastedGraphNodes, OutNotifications);
		ProcessPastedNodePositions(*Asset, InLocation, PastedGraphNodes, PastedCommentNodes);
		ProcessPastedNodeConnections(*Asset, PastedGraphNodes);

		PastedNodes.Append(MoveTemp(PastedGraphNodes));
		PastedNodes.Append(MoveTemp(PastedCommentNodes));

		return PastedNodes;
	}

	void FDocumentClipboardUtils::CopyMemberToClipboard(UMetasoundEditorGraphMember* Content)
	{
		// Clear the mark state for saving.
		UnMarkAllObjects(EObjectMark(OBJECTMARK_TagExp | OBJECTMARK_TagImp));

		// Export the clipboard to text.
		FStringOutputDevice Archive;
		const FExportObjectInnerContext Context;
		UExporter::ExportToOutputDevice(&Context, Content, nullptr, Archive, TEXT("copy"), 0, PPF_ExportsNotFullyQualified | PPF_Copy | PPF_Delimited, false, Content->GetOuter());
		FPlatformApplicationMisc::ClipboardCopy(*Archive);
	}

	const UMetasoundEditorGraphMember* FDocumentClipboardUtils::GetMemberFromClipboard()
	{
		// Get the text from the clipboard.
		FString ClipboardText;
		FPlatformApplicationMisc::ClipboardPaste(ClipboardText);

		FMemberClipboardObjectTextFactory Factory;
		if (Factory.CanCreateObjectsFromText(ClipboardText))
		{
			Factory.ProcessBuffer(GetTransientPackage(), RF_Transactional, ClipboardText);
			return Factory.Member;
		}
		
		return nullptr;
	}

	const bool FDocumentClipboardUtils::CanImportMemberFromText(const FString& TextToImport)
	{
		FMemberClipboardObjectTextFactory Factory;
		return Factory.CanCreateObjectsFromText(TextToImport);
	}
} // namespace Metasound::Editor
