// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuCOE/SMutableConstantsWidget.h"

#include "Algo/StableSort.h"
#include "Framework/Views/TableViewMetadata.h"
#include "MuCOE/SMutableCodeViewer.h"
#include "MuT/TypeInfo.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Views/STileView.h"
#include "MuCOE/Widgets/MutableMultiPageListView.h"

class ITableRow;
class SWidget;


#define LOCTEXT_NAMESPACE "CustomizableObjectEditor"

// Private namespace with utility functionality used by this slate object
namespace
{
	/** Provided a byte count this function proceeds to output that byte value as text alongside with it's unit of mesure (Bytes, KB...).
	 * @param SizeInBytes The amount of bytes to convert to a formatted text that represents it.
	 * @return A text representing the value provided as Bytes, Kilobytes, Megabytes or Gigabytes. 
	 */
	FText GenerateTextForSize(const uint64 SizeInBytes)
	{
		FString Unit = TEXT("Bytes");
		double Value = SizeInBytes;
		
		// B to KB
		if (SizeInBytes >= 1024)
		{
			Unit = TEXT("KB");
			Value = SizeInBytes / 1024.0;
			
			// KB to MB
			if (Value >= 1024.0)
			{
				Unit = TEXT("MB");
				Value = Value / 1024.0;
				
				// MB to GB
				if (Value >= 1024.0)
				{
					Unit = TEXT("GB");
					Value = Value / 1024.0;
				}
			}
		}

		FString OutputString = FString::Printf(TEXT("%.2f"), Value);
		OutputString.Append(" ");
		OutputString.Append(Unit);

		return FText::FromString(OutputString);
	}

	
	/**
	* Get the amount of channels for all the buffers in the provided BuffSetet
	* @param InBufferSet : The bufferset whose channels we want to count.
	* @return : the amount of channels found 
	*/
	int32 GetChannelCountOfBufferSet(const mu::FMeshBufferSet& InBufferSet)
	{
		int32 MeshBufferChannelsCount = 0;
		for (const mu::FMeshBuffer& MeshBuffer : InBufferSet.Buffers)
		{
			MeshBufferChannelsCount += MeshBuffer.Channels.Num();
		}
		return MeshBufferChannelsCount;
	}


	/**
	 * Get the amount of channels found in all buffers found in the provided mutable mesh
	 * @param MeshPtr Pointer to the mesh whose channels we want to count
	 * @return : the amount of channels found.
	 */
	int32 GetMeshChannelCount (TSharedPtr<const mu::FMesh> MeshPtr)
	{
		check(MeshPtr)

		int32 ChannelCount = 0;
		ChannelCount += GetChannelCountOfBufferSet(MeshPtr->GetVertexBuffers());
		ChannelCount += GetChannelCountOfBufferSet(MeshPtr->GetIndexBuffers());
		return ChannelCount;
	}
}


#pragma region SUPPORT_CLASSES


namespace MeshConstantTitles
{
	static const FName MeshID("Id");
	static const FName MeshVertices("Vertices");
	static const FName MeshIndices("Indices");
	static const FName MeshChannels("BufferChannels");
	static const FName MeshMemory("Memory");
}

class SMutableConstantMeshRow final : public SMultiColumnTableRow<TSharedPtr<FMutableConstantMeshElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantMeshElement>& InRowElement)
	{
		check(InRowElement);
		RowElement = InRowElement;

		SMultiColumnTableRow< TSharedPtr<FMutableConstantMeshElement> >::Construct(
		STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView
		);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		check(RowElement);
		check(RowElement->MeshPtr)
		
		// Index
		if (ColumnName == MeshConstantTitles::MeshID)
		{
			const FText IndexAsText = FText::AsNumber(RowElement->IndexOnSourceVector);
			return SNew(STextBlock).Text(IndexAsText);
		}

		// The amount of mesh vertex buffer and image buffer channels
		if (ColumnName == MeshConstantTitles::MeshChannels)
		{
			TSharedPtr<const mu::FMesh> Mesh = RowElement->MeshPtr;
			const int32 ChannelCount =  GetMeshChannelCount(Mesh);
			const FText ChannelCountText = FText::AsNumber(ChannelCount);
			return SNew(STextBlock).Text(ChannelCountText);
		}
		
		// The amount of indices of the mesh
		if (ColumnName == MeshConstantTitles::MeshIndices)
		{
			TSharedPtr<const mu::FMesh> Mesh = RowElement->MeshPtr;
			const FText IndexCount = FText::AsNumber(Mesh->GetIndexCount());
			return SNew(STextBlock).Text(IndexCount);
		}

		// The amount of vertices of the mesh
		if (ColumnName == MeshConstantTitles::MeshVertices)
		{
			TSharedPtr<const mu::FMesh> Mesh = RowElement->MeshPtr;
			const FText VertexCount = FText::AsNumber(Mesh->GetVertexCount());
			return SNew(STextBlock).Text(VertexCount);
		}

		// Memory used by the mesh 
		if (ColumnName == MeshConstantTitles::MeshMemory)
		{
			TSharedPtr<const mu::FMesh> Mesh = RowElement->MeshPtr;
			const FText SizeAsText = GenerateTextForSize(Mesh->GetDataSize());
			return SNew(STextBlock).Text(SizeAsText);
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FMutableConstantMeshElement> RowElement;
	
};

class SMutableConstantStringRow final : public STableRow<TSharedPtr<FMutableConstantStringElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantStringElement>& InRowElement)
	{
		check(InRowElement);
		
		// Generate the text to be displayed taking in mind the string value held by the constant to be able to "preview"
		// it for easier navigation
		const FString MainString = FString::FromInt(InRowElement->IndexOnSourceVector) + FString(TEXT("_STR "));
		const FString ConstantStringText = InRowElement->MutableString;
		const FString GlimpseConstantText = ConstantStringText.Left(GlimpseCharacterCount);
		
		// Compose the FStrings to produce the UI text to be displayed
		FString UiString = MainString + "\"" + GlimpseConstantText;
		if (ConstantStringText.Len() > GlimpseConstantText.Len())
		{
			// Shortening occured, adding "..." to show it to the user
			UiString.Append("...");
		}
		UiString.Append("\"");
		
		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(FText::FromString(UiString))
		];
		
		STableRow< TSharedPtr<FMutableConstantStringElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}


private:

	/** Determines the amount of characters to be displayed on the string constant UI text as a preview of the value
	 * of the actual string constant.
	 */
	const uint32 GlimpseCharacterCount = 8;
};

namespace ImageConstantTitles
{
	static const FName ImageID("Id");
	static const FName ImageSize("Resolution");
	static const FName ImageMipMaps("MipMapCount");
	static const FName ImageFormat("Format");
	static const FName ImageTotalMemory("MemorySize");
}

class SMutableConstantImageRow final : public SMultiColumnTableRow<TSharedPtr<FMutableConstantImageElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantImageElement>& InRowElement)
	{
		check(InRowElement);
		RowElement = InRowElement;
		
		SMultiColumnTableRow< TSharedPtr<FMutableConstantImageElement> >::Construct(
	STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView
		);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		check(RowElement);

		const mu::FImage* Image = RowElement->ImagePtr.Get();
		
		// Index
		if (ColumnName == ImageConstantTitles::ImageID)
		{
			const FText IndexAsText = FText::AsNumber(RowElement->IndexOnSourceVector);
			return SNew(STextBlock).Text(IndexAsText);
		}

		// ImageSize (Resolution size)
		if (ColumnName == ImageConstantTitles::ImageSize)
		{
			const FString XSizeString = FString::FromInt( Image->GetSizeX());
			const FString YSizeString = FString::FromInt( Image->GetSizeY());
			const FString XSign = FString("x");
			const FText ImageResolution = FText::FromString( XSizeString + XSign + YSizeString);
			
			return SNew(STextBlock).Text(ImageResolution);
		}

		// Image Mip maps (LODs)
		if (ColumnName == ImageConstantTitles::ImageMipMaps)
		{
			const FText LodCount = FText::AsNumber(Image->GetLODCount());
			return SNew(STextBlock).Text(LodCount);
		}

		// Image format
		if (ColumnName == ImageConstantTitles::ImageFormat)
		{
			const mu::EImageFormat ImageFormat = Image->GetFormat();
			const uint8 ImageFormatValue = static_cast<uint8>(ImageFormat);
			const FText FormatAsText = FText::FromString(
				FString(mu::TypeInfo::s_imageFormatName[ImageFormatValue]));
			
			return SNew(STextBlock).Text(FormatAsText);
		}

		// Memory
		if (ColumnName == ImageConstantTitles::ImageTotalMemory)
		{
			const uint32 SizeInBytes = Image->GetDataSize();
			const FText SizeAsText = GenerateTextForSize(SizeInBytes);

			// Return the text with the size
			return SNew(STextBlock).Text(SizeAsText);
		}

		checkNoEntry();
		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FMutableConstantImageElement> RowElement;
};


class SMutableConstantLayoutRow final : public STableRow<TSharedPtr<FMutableConstantLayoutElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantLayoutElement>& InRowElement)
	{
		check(InRowElement);
		const FText LayoutProxyText = FText::Format( LOCTEXT("LayoutConstantProxyLabel", "{0}_LAYOUT "),InRowElement->IndexOnSourceVector);

		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(LayoutProxyText)
		];

		STableRow< TSharedPtr<FMutableConstantLayoutElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}
	
};

class SMutableConstantProjectorRow final : public STableRow<TSharedPtr<FMutableConstantProjectorElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantProjectorElement>& InRowElement)
	{
		check(InRowElement);
		const FText ProjectorProxyText = FText::Format( LOCTEXT("ProjectorConstantProxyLabel", "{0}_PROJECTOR "),InRowElement->IndexOnSourceVector);
		
		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(ProjectorProxyText)
		];

		STableRow< TSharedPtr<FMutableConstantProjectorElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}
	
};

class SMutableConstantMatrixRow final : public STableRow<TSharedPtr<FMutableConstantMatrixElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantMatrixElement>& InRowElement)
	{
		check(InRowElement);
		const FText MatrixProxyText = FText::Format( LOCTEXT("MatrixConstantProxyLabel", "{0}_MATRIX "),InRowElement->IndexOnSourceVector);

		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(MatrixProxyText)
		];

		STableRow< TSharedPtr<FMutableConstantMatrixElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}
};

class SMutableConstantShapeRow final : public STableRow<TSharedPtr<FMutableConstantShapeElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantShapeElement>& InRowElement)
	{
		check(InRowElement);
		const FText ShapeProxyText = FText::Format( LOCTEXT("ShapeConstantProxyLabel", "{0}_SHAPE "),InRowElement->IndexOnSourceVector);

		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(ShapeProxyText)
		];

		STableRow< TSharedPtr<FMutableConstantShapeElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}
};

class SMutableConstantCurveRow final : public STableRow<TSharedPtr<FMutableConstantCurveElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantCurveElement>& InRowElement)
	{
		check(InRowElement);
		const FText CurveProxyText = FText::Format( LOCTEXT("CurveConstantProxyLabel", "{0}_CURVE "),InRowElement->IndexOnSourceVector);

		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(CurveProxyText)
		];

		STableRow< TSharedPtr<FMutableConstantCurveElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}
};


class SMutableConstantSkeletonRow final : public STableRow<TSharedPtr<FMutableConstantSkeletonElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantSkeletonElement>& InRowElement)
	{
		check(InRowElement);
		const FText SkeletonProxyText = FText::Format( LOCTEXT("SkeletonConstantProxyLabel", "{0}_SKELETON "),InRowElement->IndexOnSourceVector);

		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(SkeletonProxyText)
		];

		STableRow< TSharedPtr<FMutableConstantSkeletonElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}
};

class SMutableConstantPhysicsRow final : public STableRow<TSharedPtr<FMutableConstantPhysicsElement>>
{
public:
	void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& InOwnerTableView, const TSharedPtr<FMutableConstantPhysicsElement>& InRowElement)
	{
		check(InRowElement);
		const FText PhysicsProxyText = FText::Format( LOCTEXT("PhysicsConstantProxyLabel", "{0}_PHYSICS "),InRowElement->IndexOnSourceVector);

		this->ChildSlot
		[
			SNew(STextBlock)
			.Text(PhysicsProxyText)
		];

		STableRow< TSharedPtr<FMutableConstantPhysicsElement> >::ConstructInternal(
			STableRow::FArguments()
			.ShowSelection(true)
			, InOwnerTableView);
	}
};

#pragma endregion SUPPORT_CLASSES



void SMutableConstantsWidget::Construct(const FArguments& InArgs,const mu::FProgram* InMutableProgramPtr,  TSharedPtr<SMutableCodeViewer> InMutableCodeViewerPtr)
{
	// A pointer to MutableCodeViewerPtr is required in order to be able to invoke the preview of our constants
	check (InMutableCodeViewerPtr);
	MutableCodeViewerPtr = InMutableCodeViewerPtr;
	
	// A pointer to the mutable program object is required to get the constants data
	check(InMutableProgramPtr);
	SetProgram(InMutableProgramPtr);
	
	// Formatting constants
	constexpr uint32 InBetweenListsVerticalPadding = 4;
	
	// Vertical size for each entry 
	constexpr float ProxyEntryHeight = 20;
	
	// Panel title
	const FText ConstantsPanelTitle = LOCTEXT("ConstantsPannelName", "Constants : ");

	// Hack to allow us to later set the array to be used. if no array is provided then the children slate to contain them later will not exist
	TArray<TSharedPtr<FMutableConstantImageElement>> TempImageElementsEmptyArray;
	
	// Image Constants List View object and Handler
	TSharedPtr<SListView<TSharedPtr<FMutableConstantImageElement>>> ConstantImagesListView =
		SNew(SListView<TSharedPtr<FMutableConstantImageElement>>)
	.OnGenerateRow(this,&SMutableConstantsWidget::OnGenerateImageRow)
		// We do require to provide something here or the slate to contain the children will not get generated
	.ListItemsSource(&TempImageElementsEmptyArray)		
	.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedImageChanged)
	.SelectionMode(ESelectionMode::Single)
	.HeaderRow
	(
		SNew(SHeaderRow)
		+ SHeaderRow::Column(ImageConstantTitles::ImageID)
		.DefaultLabel(FText(LOCTEXT("ImageId", "ID")))
		.OnSort(this, &SMutableConstantsWidget::OnImageTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetImageListColumnSortMode, ImageConstantTitles::ImageID )
		.FillWidth(0.28f)
	
		+ SHeaderRow::Column(ImageConstantTitles::ImageSize)
		.DefaultLabel(FText(LOCTEXT("ImageResolution","Resolution")))
		.DefaultTooltip(FText(LOCTEXT("ImageResolutionColumnToolTip","Pixel resolution")))
		.OnSort(this, &SMutableConstantsWidget::OnImageTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetImageListColumnSortMode, ImageConstantTitles::ImageSize )
		
		+ SHeaderRow::Column(ImageConstantTitles::ImageMipMaps)
		.DefaultLabel(FText(LOCTEXT("ImageMipMaps","Mip Maps")))
		.DefaultTooltip(FText(LOCTEXT("ImageMipMapsColumnToolTip","Amount of Mip maps")))
		.OnSort(this, &SMutableConstantsWidget::OnImageTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetImageListColumnSortMode, ImageConstantTitles::ImageMipMaps )

		+ SHeaderRow::Column(ImageConstantTitles::ImageFormat)
		.DefaultLabel(FText(LOCTEXT("ImageFormat","Format")))
		.DefaultTooltip(FText(LOCTEXT("ImageFormatColumnToolTip","Image Format")))
		.OnSort(this, &SMutableConstantsWidget::OnImageTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetImageListColumnSortMode, ImageConstantTitles::ImageFormat )

		+ SHeaderRow::Column(ImageConstantTitles::ImageTotalMemory)
		.DefaultLabel(FText(LOCTEXT("ImageMemorySize","Size")))
		.DefaultTooltip(FText(LOCTEXT("ImageMemorySizeColumnToolTip","Memory size")))
		.OnSort(this, &SMutableConstantsWidget::OnImageTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetImageListColumnSortMode, ImageConstantTitles::ImageTotalMemory )

	);


	TArray<TSharedPtr<FMutableConstantMeshElement>> TempMeshElementsEmptyArray;

	// Handled list view for mutable constant meshes
	TSharedPtr<SListView<TSharedPtr<FMutableConstantMeshElement>>> ConstantMeshesListView =
		SNew(SListView<TSharedPtr<FMutableConstantMeshElement>>)
	.OnGenerateRow(this,&SMutableConstantsWidget::OnGenerateMeshRow)
		// We do require to provide something here or the slate to contain the children will not get generated
	.ListItemsSource(&TempMeshElementsEmptyArray)		
	.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedMeshChanged)
	.SelectionMode(ESelectionMode::Single)
	.HeaderRow
	(
		SNew(SHeaderRow)
		+ SHeaderRow::Column(MeshConstantTitles::MeshID)
		.DefaultLabel(FText(LOCTEXT("MeshId", "ID")))
		.OnSort(this, &SMutableConstantsWidget::OnMeshTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetMeshListColumnSortMode, MeshConstantTitles::MeshID)
		.FillWidth(0.28f)
	
		+ SHeaderRow::Column(MeshConstantTitles::MeshVertices)
		.DefaultLabel(FText(LOCTEXT("MeshVerticesCount","Vertices")))
		.DefaultTooltip(FText(LOCTEXT("MeshVerticesCountColumnToolTip","Amount of vertice")))
		.OnSort(this, &SMutableConstantsWidget::OnMeshTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetMeshListColumnSortMode, MeshConstantTitles::MeshVertices)

		+ SHeaderRow::Column(MeshConstantTitles::MeshIndices)
		.DefaultLabel(FText(LOCTEXT("MeshIndicesCount","Indices")))
		.DefaultTooltip(FText(LOCTEXT("MeshIndicesCountColumnToolTip","Amount of indices")))
		.OnSort(this, &SMutableConstantsWidget::OnMeshTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetMeshListColumnSortMode, MeshConstantTitles::MeshIndices)

		+ SHeaderRow::Column(MeshConstantTitles::MeshChannels)
		.DefaultLabel(FText(LOCTEXT("MeshVertexChannelsCount","Channels")))
		.DefaultTooltip(FText(LOCTEXT("MeshVertexChannelsCountColumnToolTip","Amount of channels")))
		.OnSort(this, &SMutableConstantsWidget::OnMeshTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetMeshListColumnSortMode, MeshConstantTitles::MeshChannels)
		
		+ SHeaderRow::Column(MeshConstantTitles::MeshMemory)
		.DefaultLabel(FText(LOCTEXT("MeshMemory","Size")))
		.DefaultTooltip(FText(LOCTEXT("MeshMemoryColumnToolTip","Memory size")))
		.OnSort(this, &SMutableConstantsWidget::OnMeshTableSortRequested)
		.SortMode(this, &SMutableConstantsWidget::GetMeshListColumnSortMode, MeshConstantTitles::MeshMemory)
	);
	

	// Child structure
	this->ChildSlot
	[
		SNew(SScrollBox)
		.Orientation(EOrientation::Orient_Vertical)
		
		// String constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(StringsExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnStringsRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawStringsAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantStringsSlate,STileView<TSharedPtr<FMutableConstantStringElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedStringChanged)
				.ListItemsSource(&ConstantStringElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this, &SMutableConstantsWidget::OnGenerateStringRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]

		// Image constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(ImagesExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnImagesRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawImagesAreaTitle)
			.BodyContent()
			[
				// Custom slate that will handle the updating of the elements displayed by the ConstantImagesListView object
					SAssignNew(ImageListViewHandler,SMutableMultiPageListView<TSharedPtr<FMutableConstantImageElement>>)
						.HostedListView(ConstantImagesListView)
						.ElementsToSeparateInPages(ConstantImageElements)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]
		
		// Mesh Constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(MeshesExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnMeshesRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawMeshesAreaTitle)
			.BodyContent()
			[
				// Custom slate that will handle the updating of the elements displayed by the ConstantMeshesListView object
				SAssignNew(MeshListViewHandler ,SMutableMultiPageListView<TSharedPtr<FMutableConstantMeshElement>>)
				.HostedListView(ConstantMeshesListView)	
				.ElementsToSeparateInPages(ConstantMeshElements)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]
		
		// Layout Constants		
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(LayoutsExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnLayoutsRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawLayoutsAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantLayoutsSlate,STileView<TSharedPtr<FMutableConstantLayoutElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedLayoutChanged)
				.ListItemsSource(&ConstantLayoutElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this,&SMutableConstantsWidget::OnGenerateLayoutRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]
		
		// Projector Constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(ProjectorsExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnProjectorsRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawProjectorsAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantProjectorsSlate,STileView<TSharedPtr<FMutableConstantProjectorElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedProjectorChanged)
				.ListItemsSource(&ConstantProjectorElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this,&SMutableConstantsWidget::OnGenerateProjectorRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]
		
		// Matrix Constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(MatricesExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnMatricesRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawMatricesAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantMatricesSlate,STileView<TSharedPtr<FMutableConstantMatrixElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedMatrixChanged)
				.ListItemsSource(&ConstantMatrixElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this,&SMutableConstantsWidget::OnGenerateMatrixRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]
		
		// Shape Constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(ShapesExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnShapesRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawShapesAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantShapesSlate,STileView<TSharedPtr<FMutableConstantShapeElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedShapeChanged)
				.ListItemsSource(&ConstantShapeElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this,&SMutableConstantsWidget::OnGenerateShapeRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]
		
		// Curve Constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(CurvesExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnCurvesRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawCurvesAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantCurvesSlate,STileView<TSharedPtr<FMutableConstantCurveElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedCurveChanged)
				.ListItemsSource(&ConstantCurveElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this,&SMutableConstantsWidget::OnGenerateCurveRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]

		+ SScrollBox::Slot()
		[
			SNew(SSeparator)
		]
		
		// Skeleton Constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(SkeletonsExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnSkeletonsRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawSkeletonsAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantSkeletonsSlate,STileView<TSharedPtr<FMutableConstantSkeletonElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedSkeletonChanged)
				.ListItemsSource(&ConstantSkeletonElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this,&SMutableConstantsWidget::OnGenerateSkeletonRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]


		// Physics Constants
		+ SScrollBox::Slot()
		.Padding(0,InBetweenListsVerticalPadding)
		[
			SAssignNew(PhysicsExpandableArea,SExpandableArea)
			.OnAreaExpansionChanged(this,&SMutableConstantsWidget::OnPhysicsRegionExpansionChanged)
			.InitiallyCollapsed(true)
			.AreaTitle(this,&SMutableConstantsWidget::OnDrawPhysicsAreaTitle)
			.BodyContent()
			[
				SAssignNew(ConstantPhysicsSlate,STileView<TSharedPtr<FMutableConstantPhysicsElement>>)
				.OnSelectionChanged(this,&SMutableConstantsWidget::OnSelectedPhysicsChanged)
				.ListItemsSource(&ConstantPhysicsElements)
				.ItemHeight(ProxyEntryHeight)
				.OnGenerateTile(this,&SMutableConstantsWidget::OnGeneratePhysicsRow)
				.SelectionMode(ESelectionMode::Single)
			]
		]
	];

	// Store all the expandable areas so they are later reachable using loops
	ExpandableAreas.Add(StringsExpandableArea);
	ExpandableAreas.Add(ImagesExpandableArea);
	ExpandableAreas.Add(MeshesExpandableArea);
	ExpandableAreas.Add(LayoutsExpandableArea);
	ExpandableAreas.Add(ProjectorsExpandableArea);
	ExpandableAreas.Add(ShapesExpandableArea);
	ExpandableAreas.Add(CurvesExpandableArea);
	ExpandableAreas.Add(MatricesExpandableArea);
	ExpandableAreas.Add(SkeletonsExpandableArea);
	ExpandableAreas.Add(PhysicsExpandableArea);
}



void SMutableConstantsWidget::SetProgram(const mu::FProgram* InProgram)
{
	// Make sure we do not process the setting of the same program object as the one already set
	if (this->MutableProgramPtr == InProgram)
	{
		return;
	}
	
	// Set only once the program that is being used. No further updates should be required
	this->MutableProgramPtr = InProgram;
	
	// Generate the backend for the lists used in this object
	if (MutableProgramPtr != nullptr)
	{
		LoadConstantElements();
	}
}

#pragma region Row Generation

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateStringRow(
	TSharedPtr<FMutableConstantStringElement> MutableConstantStringElement, const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantStringRow> Row = SNew(SMutableConstantStringRow, OwnerTable, MutableConstantStringElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateImageRow(
	TSharedPtr<FMutableConstantImageElement> MutableConstantImageElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantImageRow> Row = SNew(SMutableConstantImageRow,OwnerTable,MutableConstantImageElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateMeshRow(
	TSharedPtr<FMutableConstantMeshElement> MutableConstantMeshElement, const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantMeshRow> Row = SNew(SMutableConstantMeshRow, OwnerTable, MutableConstantMeshElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateLayoutRow(
	TSharedPtr<FMutableConstantLayoutElement> MutableConstantLayoutElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantLayoutRow> Row = SNew(SMutableConstantLayoutRow,OwnerTable,MutableConstantLayoutElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateProjectorRow(
	TSharedPtr<FMutableConstantProjectorElement> MutableConstantProjectorElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantProjectorRow> Row = SNew(SMutableConstantProjectorRow,OwnerTable,MutableConstantProjectorElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateMatrixRow(
	TSharedPtr<FMutableConstantMatrixElement> MutableConstantMatrixElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantMatrixRow> Row = SNew(SMutableConstantMatrixRow,OwnerTable,MutableConstantMatrixElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateShapeRow(
	TSharedPtr<FMutableConstantShapeElement> MutableConstantShapeElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantShapeRow> Row = SNew(SMutableConstantShapeRow,OwnerTable,MutableConstantShapeElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateCurveRow(
	TSharedPtr<FMutableConstantCurveElement> MutableConstantCurveElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantCurveRow> Row = SNew(SMutableConstantCurveRow,OwnerTable,MutableConstantCurveElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGenerateSkeletonRow(
	TSharedPtr<FMutableConstantSkeletonElement> MutableConstantSkeletonElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantSkeletonRow> Row = SNew(SMutableConstantSkeletonRow,OwnerTable,MutableConstantSkeletonElement);
	return Row;
}

TSharedRef<ITableRow> SMutableConstantsWidget::OnGeneratePhysicsRow(
	TSharedPtr<FMutableConstantPhysicsElement> MutableConstantPhysicsElement,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	TSharedRef<SMutableConstantPhysicsRow> Row = SNew(SMutableConstantPhysicsRow,OwnerTable,MutableConstantPhysicsElement);
	return Row;
}

void SMutableConstantsWidget::OnImageTableSortRequested(EColumnSortPriority::Type ColumnSortPriority, const FName& ColumnID,
                                                        EColumnSortMode::Type ColumnSortMode)
{
	ImageConstantsLastSortedColumnID = ColumnID;
	ImageListSortMode = ColumnSortMode;

	Algo::StableSort(*ConstantImageElements, [&](const TSharedPtr<FMutableConstantImageElement>& A, const TSharedPtr<FMutableConstantImageElement>& B)
	{
		// Sort by image id
		if (ColumnID == ImageConstantTitles::ImageID)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->IndexOnSourceVector < B->IndexOnSourceVector;
			}
			return A->IndexOnSourceVector > B->IndexOnSourceVector;
		}
		
		// Sort Image mip maps
		if (ColumnID == ImageConstantTitles::ImageMipMaps)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->ImagePtr->GetLODCount() < B->ImagePtr->GetLODCount();
			}
			return A->ImagePtr->GetLODCount() > B->ImagePtr->GetLODCount();
		}

		// Sort by image format
		if (ColumnID == ImageConstantTitles::ImageFormat)
		{
			const uint8 AImageFormatValue = static_cast<uint8>(A->ImagePtr->GetFormat());
			const FString AImageFormatString = FString(mu::TypeInfo::s_imageFormatName[AImageFormatValue]);
			
			const uint8 BImageFormatValue = static_cast<uint8>(B->ImagePtr->GetFormat());
			const FString BImageFormatString = FString(mu::TypeInfo::s_imageFormatName[BImageFormatValue]);
			
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return AImageFormatString.Compare(BImageFormatString) < 0;
			}
			return AImageFormatString.Compare(BImageFormatString) > 0;
		}
		
		
		// Sort by image size
		if (ColumnID == ImageConstantTitles::ImageSize)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->ImagePtr->GetSizeX() *  A->ImagePtr->GetSizeY() <
					B->ImagePtr->GetSizeX() *  B->ImagePtr->GetSizeY();
			}
			return A->ImagePtr->GetSizeX() *  A->ImagePtr->GetSizeY() >
					B->ImagePtr->GetSizeX() *  B->ImagePtr->GetSizeY();
		}
		
		
		// Sort by image used memory
		if (ColumnID == ImageConstantTitles::ImageTotalMemory)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->ImagePtr->GetDataSize() < B->ImagePtr->GetDataSize();
			}
			return A->ImagePtr->GetDataSize() > B->ImagePtr->GetDataSize();
		}
		
		return false;
	});
	
	if (ImageListViewHandler)
	{
		ImageListViewHandler->RegeneratePage();
	}
}


EColumnSortMode::Type SMutableConstantsWidget::GetImageListColumnSortMode(FName ColumnName) const
{
	if (ImageConstantsLastSortedColumnID != ColumnName)
	{
		return EColumnSortMode::None;
	}

	return ImageListSortMode;
}


void SMutableConstantsWidget::OnMeshTableSortRequested(EColumnSortPriority::Type ColumnSortPriority, const FName& ColumnID, EColumnSortMode::Type ColumnSortMode)
{
	// If the colum has been sorted on one way now do the inverse way of sorting
	MeshConstantsLastSortedColumnID = ColumnID;
	MeshListSortMode = ColumnSortMode;

	Algo::StableSort(*ConstantMeshElements, [&](const TSharedPtr<FMutableConstantMeshElement>& A, const TSharedPtr<FMutableConstantMeshElement>& B)
	{
		// Sort by mesh id
		if (ColumnID == MeshConstantTitles::MeshID)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->IndexOnSourceVector < B->IndexOnSourceVector;
			}
			return A->IndexOnSourceVector > B->IndexOnSourceVector;
		}
	
		// Sort by vertex count
		if (ColumnID == MeshConstantTitles::MeshVertices)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->MeshPtr->GetVertexCount() < B->MeshPtr->GetVertexCount();
			}
			return A->MeshPtr->GetVertexCount() > B->MeshPtr->GetVertexCount();
		}

		// Sort by index count
		if (ColumnID == MeshConstantTitles::MeshIndices)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->MeshPtr->GetIndexCount() < B->MeshPtr->GetIndexCount();
			}
			return A->MeshPtr->GetIndexCount() > B->MeshPtr->GetIndexCount();
		}

		// Sort by the amount of channels in the vertex and index buffers
		if (ColumnID == MeshConstantTitles::MeshChannels)
		{
			const int32 AChannelCount = GetMeshChannelCount(A->MeshPtr);
			const int32 BChannelCount = GetMeshChannelCount(B->MeshPtr);
			
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return AChannelCount < BChannelCount;
			}
			return AChannelCount > BChannelCount;
		}
		
		// Sort by the amount memory used by the mesh
		if (ColumnID == MeshConstantTitles::MeshMemory)
		{
			if (ColumnSortMode == EColumnSortMode::Ascending)
			{
				return A->MeshPtr->GetDataSize() < B->MeshPtr->GetDataSize();
			}
			return A->MeshPtr->GetDataSize() > B->MeshPtr->GetDataSize();
		}
	
		return false;
	});
	
	if (MeshListViewHandler)
	{
		MeshListViewHandler->RegeneratePage();
	}
}



EColumnSortMode::Type SMutableConstantsWidget::GetMeshListColumnSortMode(FName ColumnName) const
{
	if (MeshConstantsLastSortedColumnID != ColumnName)
	{
		return EColumnSortMode::None;
	}

	return MeshListSortMode;
}


#pragma endregion 

#pragma  region  Expansions Handling

void SMutableConstantsWidget::OnStringsRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(StringsExpandableArea);
	}
}

void SMutableConstantsWidget::OnImagesRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(ImagesExpandableArea);
	}
}

void SMutableConstantsWidget::OnMeshesRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(MeshesExpandableArea);
	}
}

void SMutableConstantsWidget::OnLayoutsRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(LayoutsExpandableArea);
	}
}

void SMutableConstantsWidget::OnProjectorsRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(ProjectorsExpandableArea);
	}
}

void SMutableConstantsWidget::OnMatricesRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(MatricesExpandableArea);
	}
}

void SMutableConstantsWidget::OnShapesRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(ShapesExpandableArea);
	}
}

void SMutableConstantsWidget::OnCurvesRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(CurvesExpandableArea);
	}
}

void SMutableConstantsWidget::OnSkeletonsRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(SkeletonsExpandableArea);
	}
}

void SMutableConstantsWidget::OnPhysicsRegionExpansionChanged(bool bExpanded)
{
	if (bExpanded)
	{
		ContractExpandableAreas(PhysicsExpandableArea);
	}
}


void SMutableConstantsWidget::ContractExpandableAreas(const TSharedPtr<SExpandableArea>& InException)
{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	if(!InException)
	{
		UE_LOG(LogTemp,Warning,TEXT("No valid expandable area has been provided as exception : All expandable areas will therefore get contracted"));
	}
#endif
	
	for (TSharedPtr<SExpandableArea>& CurrentExpandableArea : ExpandableAreas)
	{
		if (CurrentExpandableArea == InException)
		{
			continue;
		}

		CurrentExpandableArea->SetExpanded(false);
	}
}

#pragma endregion 

#pragma  region Element caches loading

void SMutableConstantsWidget::LoadConstantElements()
{
	LoadConstantStrings();
	LoadConstantImages();
	LoadConstantMeshes();
	LoadConstantLayouts();
	LoadConstantProjectors();
	LoadConstantMatrices();
	LoadConstantShapes();
	LoadConstantCurves();
	LoadConstantSkeletons();
	LoadConstantPhysics();
}

void SMutableConstantsWidget::LoadConstantStrings()
{
	check (MutableProgramPtr);

	const int32 ConstantsCount = MutableProgramPtr->ConstantStrings.Num();
	ConstantStringElements.Empty(ConstantsCount);
	uint64 ConstantStringsAccumulatedSize = 0;
	
	for (int32 StringAddressIndex = 0; StringAddressIndex < ConstantsCount; StringAddressIndex++)
	{
		TSharedPtr<FMutableConstantStringElement> ConstantStringElement = MakeShared<FMutableConstantStringElement>();
		ConstantStringElement->MutableString = MutableProgramPtr->ConstantStrings[StringAddressIndex];
		ConstantStringElement->IndexOnSourceVector = StringAddressIndex;
		
		// Cache resource size
		// in case we change the type of the contents of the mu::string we check its size as if it was a vector<>
		ConstantStringsAccumulatedSize += ConstantStringElement->MutableString.GetAllocatedSize();
		
		ConstantStringElements.Add(ConstantStringElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantStringsFormattedSize = GenerateTextForSize(ConstantStringsAccumulatedSize);
}

void SMutableConstantsWidget::LoadConstantImages()
{
	check (MutableProgramPtr);

	const int32 ConstantsCount = MutableProgramPtr->ConstantImages.Num();
	if (ConstantImageElements)
	{
		ConstantImageElements->Empty(ConstantsCount);
	}
	else
	{
		ConstantImageElements = MakeShared<TArray<TSharedPtr<FMutableConstantImageElement>>>();
		ConstantImageElements->Reserve(ConstantsCount);
	}
	
	uint64 ConstantImagesAccumulatedSize = 0;

	for (int32 ImageIndex = 0; ImageIndex < ConstantsCount; ImageIndex++)
	{
		TSharedPtr<FMutableConstantImageElement> ConstantImageElement = MakeShared<FMutableConstantImageElement>();

		MutableProgramPtr->GetConstant(ImageIndex, ConstantImageElement->ImagePtr, 0,
			[this](int32 x, int32 y, int32 m, mu::EImageFormat f, mu::EInitializationType i) { return MakeShared<mu::FImage>(x, y, m, f, i); });

		ConstantImageElement->IndexOnSourceVector = ImageIndex;
		
		ConstantImagesAccumulatedSize += ConstantImageElement->ImagePtr->GetDataSize();
		
		ConstantImageElements->Add(ConstantImageElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantImagesFormattedSize = GenerateTextForSize(ConstantImagesAccumulatedSize);
}

void SMutableConstantsWidget::LoadConstantMeshes()
{
	check (MutableProgramPtr);
	
	TArray<TSharedPtr<const mu::FMesh>> AllMeshes;
	AllMeshes.Reserve(MutableProgramPtr->ConstantMeshesPermanent.Num() + MutableProgramPtr->ConstantMeshesStreamed.Num() );
	AllMeshes.Append(MutableProgramPtr->ConstantMeshesPermanent);
	for (const TPair<uint32, TSharedPtr<const mu::FMesh>>& Entry : MutableProgramPtr->ConstantMeshesStreamed)
	{
		AllMeshes.Add(Entry.Value);
	}

	const int32 ConstantsCount = AllMeshes.Num();
	if (ConstantMeshElements)
	{
		ConstantMeshElements->Empty(ConstantsCount);
	}
	else
	{
		ConstantMeshElements = MakeShared<TArray<TSharedPtr<FMutableConstantMeshElement>>>();
		ConstantMeshElements->Reserve(ConstantsCount);
	}
	
	uint64 ConstantMeshesAccumulatedSize = 0;
	
	for (int32 MeshIndex = 0; MeshIndex < ConstantsCount; MeshIndex++)
	{
		TSharedPtr< FMutableConstantMeshElement> ConstantMeshElement = MakeShared<FMutableConstantMeshElement>();
		ConstantMeshElement->MeshPtr = AllMeshes[MeshIndex];
		ConstantMeshElement->IndexOnSourceVector = MeshIndex;

		// Actual core disk size would be:
		//mu::FOutputMemoryStream Stream;
		//mu::FOutputArchive Archive{ &Stream };
		//ConstantMeshElement->MeshPtr->Serialise(Archive);
		//uint64 OtherSize = Stream.GetBufferSize();

		const uint64 ThisMeshSize = ConstantMeshElement->MeshPtr->GetDataSize();
		ConstantMeshesAccumulatedSize += ThisMeshSize;
		
		ConstantMeshElements->Add(ConstantMeshElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantMeshesFormattedSize = GenerateTextForSize(ConstantMeshesAccumulatedSize);
}

void SMutableConstantsWidget::LoadConstantLayouts()
{
	check (MutableProgramPtr);
	const int32 ConstantsCount = MutableProgramPtr->ConstantLayouts.Num();
	ConstantLayoutElements.Empty(ConstantsCount);
	
	mu::FOutputMemoryStream Stream;
	mu::FOutputArchive Archive{&Stream};
	
	for (int32 LayoutIndex = 0; LayoutIndex < ConstantsCount; LayoutIndex++)
	{		
		TSharedPtr<FMutableConstantLayoutElement> ConstantLayoutElement = MakeShared<FMutableConstantLayoutElement>();
		ConstantLayoutElement->Layout = MutableProgramPtr->ConstantLayouts[LayoutIndex];
		ConstantLayoutElement->IndexOnSourceVector = LayoutIndex;
		
		// Cache resource for later GetBufferSize() call
		ConstantLayoutElement->Layout->Serialise(Archive);

		ConstantLayoutElements.Add(ConstantLayoutElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantLayoutsFormattedSize = GenerateTextForSize(Stream.GetBufferSize());
}

void SMutableConstantsWidget::LoadConstantSkeletons()
{
	check (MutableProgramPtr);
	const int32 ConstantsCount = MutableProgramPtr->ConstantSkeletons.Num();
	ConstantSkeletonElements.Empty(ConstantsCount);

	mu::FOutputMemoryStream Stream;
	mu::FOutputArchive Archive{&Stream};
	
	for (int32 SkeletonIndex = 0; SkeletonIndex < ConstantsCount; SkeletonIndex++)
	{
		TSharedPtr<FMutableConstantSkeletonElement> ConstantSkeletonElement = MakeShared<FMutableConstantSkeletonElement>();
		ConstantSkeletonElement->Skeleton = MutableProgramPtr->ConstantSkeletons[SkeletonIndex];
		ConstantSkeletonElement->IndexOnSourceVector = SkeletonIndex;
		
		ConstantSkeletonElement->Skeleton->Serialise(Archive);

		ConstantSkeletonElements.Add(ConstantSkeletonElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantSkeletonsFormattedSize = GenerateTextForSize(Stream.GetBufferSize());
}

void SMutableConstantsWidget::LoadConstantPhysics()
{
	check (MutableProgramPtr);
	const int32 ConstantsCount = MutableProgramPtr->ConstantPhysicsBodies.Num();
	ConstantPhysicsElements.Empty(ConstantsCount);

	mu::FOutputMemoryStream Stream;
	mu::FOutputArchive Archive{&Stream};
	
	for (int32 PhysicsIndex = 0; PhysicsIndex < ConstantsCount; PhysicsIndex++)
	{
		TSharedPtr<FMutableConstantPhysicsElement> ConstantPhysicsElement = MakeShared<FMutableConstantPhysicsElement>();
		ConstantPhysicsElement->Physics = MutableProgramPtr->ConstantPhysicsBodies[PhysicsIndex];
		ConstantPhysicsElement->IndexOnSourceVector = PhysicsIndex;
	
		ConstantPhysicsElement->Physics->Serialise(Archive);

		ConstantPhysicsElements.Add(ConstantPhysicsElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantPhysicsFormattedSize = GenerateTextForSize(Stream.GetBufferSize());
}


void SMutableConstantsWidget::LoadConstantProjectors()
{
	check (MutableProgramPtr);
	const int32 ConstantsCount = MutableProgramPtr->ConstantProjectors.Num();
	ConstantProjectorElements.Empty(ConstantsCount);

	mu::FOutputMemoryStream Stream;
	mu::FOutputArchive Archive{&Stream};
	
	for (int32 ProjectorIndex = 0; ProjectorIndex < ConstantsCount; ProjectorIndex++)
	{
		TSharedPtr<FMutableConstantProjectorElement> ConstantProjectorElement = MakeShared<FMutableConstantProjectorElement>();
		ConstantProjectorElement->Projector = &(MutableProgramPtr->ConstantProjectors[ProjectorIndex]);
		ConstantProjectorElement->IndexOnSourceVector = ProjectorIndex;
		
		ConstantProjectorElement->Projector->Serialise(Archive);
		
		ConstantProjectorElements.Add(ConstantProjectorElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantProjectorsFormattedSize = GenerateTextForSize(Stream.GetBufferSize());
}

void SMutableConstantsWidget::LoadConstantMatrices()
{
	check (MutableProgramPtr);
	const int32 ConstantsCount = MutableProgramPtr->ConstantMatrices.Num();
	ConstantMatrixElements.Empty(ConstantsCount);

	mu::FOutputMemoryStream Stream;
	mu::FOutputArchive Archive{&Stream};
	
	for (int32 MatrixIndex = 0; MatrixIndex < ConstantsCount; MatrixIndex++)
	{
		TSharedPtr<FMutableConstantMatrixElement> ConstantMatrixElement = MakeShared<FMutableConstantMatrixElement>();
		ConstantMatrixElement->Matrix = MutableProgramPtr->ConstantMatrices[MatrixIndex];
		ConstantMatrixElement->IndexOnSourceVector = MatrixIndex;
		
		Archive << ConstantMatrixElement->Matrix;
		
		ConstantMatrixElements.Add(ConstantMatrixElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantMatricesFormattedSize = GenerateTextForSize(Stream.GetBufferSize());
}

void SMutableConstantsWidget::LoadConstantShapes()
{
	check (MutableProgramPtr);
	const int32 ConstantsCount = MutableProgramPtr->ConstantShapes.Num();
	ConstantShapeElements.Empty(ConstantsCount);

	mu::FOutputMemoryStream Stream;
	mu::FOutputArchive Archive{&Stream};	
	
	for (int32 ShapeIndex = 0; ShapeIndex < ConstantsCount; ShapeIndex++)
	{
		TSharedPtr<FMutableConstantShapeElement> ConstantShapeElement = MakeShared<FMutableConstantShapeElement>();
		ConstantShapeElement->Shape = &(MutableProgramPtr->ConstantShapes[ShapeIndex]);
		ConstantShapeElement->IndexOnSourceVector = ShapeIndex;
		
		ConstantShapeElement->Shape->Serialise(Archive);

		ConstantShapeElements.Add(ConstantShapeElement);
	}
	
	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantShapesFormattedSize = GenerateTextForSize(Stream.GetBufferSize());
}

void SMutableConstantsWidget::LoadConstantCurves()
{
	check (MutableProgramPtr);
	const int32 ConstantsCount = MutableProgramPtr->ConstantCurves.Num();
	ConstantCurveElements.Empty(ConstantsCount);
	
	mu::FOutputMemoryStream Stream;
	mu::FOutputArchive Archive{&Stream};
	
	for (int32 CurveIndex = 0; CurveIndex < ConstantsCount; CurveIndex++)
	{
		TSharedPtr<FMutableConstantCurveElement> ConstantCurveElement = MakeShared<FMutableConstantCurveElement>();
		ConstantCurveElement->Curve = MutableProgramPtr->ConstantCurves[CurveIndex];
		ConstantCurveElement->IndexOnSourceVector = CurveIndex;

		Archive << ConstantCurveElement->Curve;
		
		ConstantCurveElements.Add(ConstantCurveElement);
	}

	// Cache the size in memory of the constants as a formatted text so it is able to be be later used by the UI
	ConstantCurvesFormattedSize = GenerateTextForSize(Stream.GetBufferSize());
}

#pragma  endregion


#pragma region Previewer invocation methods


void SMutableConstantsWidget::ClearSelectedConstantItems(mu::EDataType ExceptionDataType /* = mu::EDataType::None */) const
{
	if (ExceptionDataType != mu::EDataType::Mesh)
	{
		MeshListViewHandler->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::String)
	{
		ConstantStringsSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::Layout)
	{
		ConstantLayoutsSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::Projector)
	{
		ConstantProjectorsSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::Matrix)
	{
		ConstantMatricesSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::Shape)
	{
		ConstantShapesSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::Curve)
	{
		ConstantCurvesSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::Skeleton)
	{
		ConstantSkeletonsSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::PhysicsAsset)
	{
		ConstantPhysicsSlate->ClearSelection();
	}
	if (ExceptionDataType != mu::EDataType::Image)
	{
		ImageListViewHandler->ClearSelection();
		//ConstantImagesListView->ClearSelection();
	}

	// todo: if you add more slates for the types update this to make sure they behave as the others
}

void SMutableConstantsWidget::OnSelectedStringChanged(
	TSharedPtr<FMutableConstantStringElement> MutableConstantStringElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantStringElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::String;

		// Clear the selected CodeViewer row and all other constant viewer slates that are not the type provided.
		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);
		
		// Ask the Code viewer to present the element held on your element on the previewer window
		MutableCodeViewerPtr->PreviewMutableString(MutableConstantStringElement->MutableString);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantStringElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedImageChanged(
	TSharedPtr<FMutableConstantImageElement> MutableConstantImageElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantImageElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Image;

		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);
		
		MutableCodeViewerPtr->PreviewMutableImage(MutableConstantImageElement->ImagePtr);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantImageElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedMeshChanged(
	TSharedPtr<FMutableConstantMeshElement> MutableConstantMeshElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantMeshElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Mesh;
		
		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);

		MutableCodeViewerPtr->PreviewMutableMesh(MutableConstantMeshElement->MeshPtr);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantMeshElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedLayoutChanged(
	TSharedPtr<FMutableConstantLayoutElement> MutableConstantLayoutElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantLayoutElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Layout;
		
		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);

		MutableCodeViewerPtr->PreviewMutableLayout(MutableConstantLayoutElement->Layout);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantLayoutElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedProjectorChanged(
	TSharedPtr<FMutableConstantProjectorElement> MutableConstantProjectorElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantProjectorElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Projector;

		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);

		MutableCodeViewerPtr->PreviewMutableProjector(MutableConstantProjectorElement->Projector);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantProjectorElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedMatrixChanged(
	TSharedPtr<FMutableConstantMatrixElement> MutableConstantMatrixElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantMatrixElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Matrix;

		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);

		MutableCodeViewerPtr->PreviewMutableMatrix(MutableConstantMatrixElement->Matrix);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantMatrixElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedShapeChanged(
	TSharedPtr<FMutableConstantShapeElement> MutableConstantShapeElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantShapeElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Shape;

		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);

		MutableCodeViewerPtr->PreviewMutableShape(MutableConstantShapeElement->Shape);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantShapeElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedCurveChanged(
	TSharedPtr<FMutableConstantCurveElement> MutableConstantCurveElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantCurveElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Curve;

		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);
		
		MutableCodeViewerPtr->PreviewMutableCurve(MutableConstantCurveElement->Curve);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantCurveElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedSkeletonChanged(
	TSharedPtr<FMutableConstantSkeletonElement> MutableConstantSkeletonElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantSkeletonElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::Skeleton;

		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);

		MutableCodeViewerPtr->PreviewMutableSkeleton(MutableConstantSkeletonElement->Skeleton);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantSkeletonElement->IndexOnSourceVector);
	}
}

void SMutableConstantsWidget::OnSelectedPhysicsChanged(
	TSharedPtr<FMutableConstantPhysicsElement> MutableConstantPhysicsElement,
	ESelectInfo::Type SelectionType) const
{
	if (MutableConstantPhysicsElement)
	{
		static constexpr mu::EDataType SlateDataType = mu::EDataType::PhysicsAsset;

		MutableCodeViewerPtr->ClearSelectedTreeRow();
		ClearSelectedConstantItems(SlateDataType);

		MutableCodeViewerPtr->PreviewMutablePhysics(MutableConstantPhysicsElement->Physics);
		MutableCodeViewerPtr->CacheAddressesRelatedWithConstantResource(SlateDataType,MutableConstantPhysicsElement->IndexOnSourceVector);
	}
}

#pragma endregion 

#pragma region Region title drawing callback methods


FText SMutableConstantsWidget::OnDrawStringsAreaTitle() const
{
	return FText::Format( LOCTEXT("StringConstantsTitle", "String Constants ({0}) : {1} "),ConstantStringElements.Num(), ConstantStringsFormattedSize);
}

FText SMutableConstantsWidget::OnDrawImagesAreaTitle() const
{
	return FText::Format( LOCTEXT("ImageConstantsTitle", "Image Constants ({0}) : {1} "), ConstantImageElements->Num(), ConstantImagesFormattedSize);
}

FText SMutableConstantsWidget::OnDrawMeshesAreaTitle() const
{
	return FText::Format(LOCTEXT("MeshConstantsTitle", "Mesh Constants ({0}) : {1} "), ConstantMeshElements->Num(), ConstantMeshesFormattedSize) ;
}

FText SMutableConstantsWidget::OnDrawLayoutsAreaTitle() const
{
	return FText::Format(LOCTEXT("LayoutConstantsTitle", "Layout Constants ({0}) : {1} "), ConstantLayoutElements.Num(), ConstantLayoutsFormattedSize) ;
}

FText SMutableConstantsWidget::OnDrawProjectorsAreaTitle() const
{
	return  FText::Format( LOCTEXT("ProjectorConstantsTitle", "Projector Constants ({0}) : {1} "), ConstantProjectorElements.Num(),ConstantProjectorsFormattedSize);
}

FText SMutableConstantsWidget::OnDrawMatricesAreaTitle() const
{
	return FText::Format(LOCTEXT("MatrixConstantsTitle", "Matrix Constants ({0}) : {1} "), ConstantMatrixElements.Num(),ConstantMatricesFormattedSize) ;
}

FText SMutableConstantsWidget::OnDrawShapesAreaTitle() const
{
	return FText::Format(LOCTEXT("ShapeConstantsTitle", "Shape Constants ({0}) : {1} "),ConstantShapeElements.Num(), ConstantShapesFormattedSize) ;
}

FText SMutableConstantsWidget::OnDrawCurvesAreaTitle() const
{
	return FText::Format(LOCTEXT("CurveConstantsTitle", "Curve Constants ({0}) : {1} "), ConstantCurveElements.Num(), ConstantCurvesFormattedSize) ;
}

FText SMutableConstantsWidget::OnDrawSkeletonsAreaTitle() const
{
	return FText::Format( LOCTEXT("SkeletonConstantsTitle", "Skeleton Constants ({0}) : {1} "), ConstantSkeletonElements.Num(), ConstantSkeletonsFormattedSize);
}

FText SMutableConstantsWidget::OnDrawPhysicsAreaTitle() const
{
	return FText::Format( LOCTEXT("PhysicsConstantsTitle", "Physics Constants ({0}) : {1} "), ConstantPhysicsElements.Num(), ConstantPhysicsFormattedSize);
}

#pragma endregion


#undef LOCTEXT_NAMESPACE
