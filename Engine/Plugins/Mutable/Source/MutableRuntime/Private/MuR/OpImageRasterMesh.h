// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/ParallelFor.h"
#include "MuR/ImagePrivate.h"
#include "MuR/Platform.h"
#include "MuR/MeshPrivate.h"
#include "MuR/Raster.h"
#include "MuR/ConvertData.h"

namespace mu
{

	class WhitePixelProcessor
	{
	public:
		inline void ProcessPixel(uint8* pBufferPos, float[1]) const
		{
			pBufferPos[0] = 255;
		}

		inline void operator()(uint8* BufferPos, float Interpolators[1]) const
		{
			ProcessPixel(BufferPos, Interpolators);
		}
	};


	inline void ImageRasterMesh( const FMesh* pMesh, FImage* pImage, int32 LayoutIndex, uint64 BlockId,
		UE::Math::TIntVector2<uint16> CropMin, UE::Math::TIntVector2<uint16> UncroppedSize )
	{
		MUTABLE_CPUPROFILER_SCOPE(ImageRasterMesh);

		if (pMesh->GetVertexCount() == 0)
		{
			return;
		}

		check( pImage->GetFormat()== EImageFormat::L_UByte );

		int32 sizeX = pImage->GetSizeX();
		int32 sizeY = pImage->GetSizeY();

		// Get the vertices
		int32 vertexCount = pMesh->GetVertexCount();
		TArray< RasterVertex<1> > vertices;
		vertices.SetNumZeroed(vertexCount);

		UntypedMeshBufferIteratorConst texIt( pMesh->GetVertexBuffers(), EMeshBufferSemantic::TexCoords, 0 );
		if (!texIt.ptr())
		{
			ensure(false);
			return;
		}

		for ( int32 v=0; v<vertexCount; ++v )
		{
            float uv[2] = {0.0f,0.0f};
			ConvertData( 0, uv, EMeshBufferFormat::Float32, texIt.ptr(), texIt.GetFormat() );
			ConvertData( 1, uv, EMeshBufferFormat::Float32, texIt.ptr(), texIt.GetFormat() );

			bool bUseCropping = UncroppedSize[0] > 0;
			if (bUseCropping)
			{
				vertices[v].x = uv[0] * UncroppedSize[0] - CropMin[0];
				vertices[v].y = uv[1] * UncroppedSize[1] - CropMin[1];
			}
			else
			{
				vertices[v].x = uv[0] * sizeX;
				vertices[v].y = uv[1] * sizeY;
			}
			++texIt;
		}

		// Get the indices
		int32 faceCount = pMesh->GetFaceCount();
		TArray<int32> indices;
		indices.SetNumZeroed(faceCount * 3);

		UntypedMeshBufferIteratorConst indIt( pMesh->GetIndexBuffers(), EMeshBufferSemantic::VertexIndex, 0 );
		for ( int32 i=0; i<faceCount*3; ++i )
		{
            uint32_t index=0;
			ConvertData( 0, &index, EMeshBufferFormat::UInt32, indIt.ptr(), indIt.GetFormat() );

			indices[i] = index;
			++indIt;
		}

        UntypedMeshBufferIteratorConst bloIt( pMesh->GetVertexBuffers(), EMeshBufferSemantic::LayoutBlock, LayoutIndex );

        if (BlockId== FLayoutBlock::InvalidBlockId || bloIt.GetElementSize()==0 )
		{
			// Raster all the faces
            WhitePixelProcessor pixelProc;
			const TArrayView<uint8> ImageData = pImage->DataStorage.GetLOD(0);

			//for ( int f=0; f<faceCount; ++f )
			const auto& ProcessFace = [
				vertices, indices, ImageData, sizeX, sizeY, pixelProc
			] (int32 f)
			{
				constexpr int32 NumInterpolators = 1;
				Triangle<NumInterpolators>(ImageData.GetData(), ImageData.Num(),
					sizeX, sizeY,
					1,
					vertices[indices[f * 3 + 0]],
					vertices[indices[f * 3 + 1]],
					vertices[indices[f * 3 + 2]],
					pixelProc,
					false);
			};

			ParallelFor(faceCount, ProcessFace);
		}
		else
		{
			// Raster only the faces in the selected block
			check(bloIt.GetComponents() == 1);

			// Get the block per vertex
			TArray<uint64> VertexBlockIds;
			VertexBlockIds.SetNumZeroed(vertexCount);

			if (bloIt.GetFormat() == EMeshBufferFormat::UInt16)
			{
				// Relative blocks.
				const uint16* SourceIds = reinterpret_cast<const uint16*>(bloIt.ptr());
				for (int32 i = 0; i < vertexCount; ++i)
				{
					uint64 Id = SourceIds[i];
					Id = Id | (uint64(pMesh->MeshIDPrefix)<<32);
					VertexBlockIds[i] = Id;
				}
			}
			else if (bloIt.GetFormat() == EMeshBufferFormat::UInt64)
			{
				// Absolute blocks.
				const uint64* SourceIds = reinterpret_cast<const uint64*>(bloIt.ptr());
				for (int32 i = 0; i < vertexCount; ++i)
				{
					uint64 Id = SourceIds[i];
					VertexBlockIds[i] = Id;
				}
			}
			else
			{
				// Format not supported
				check(false);
			}

            WhitePixelProcessor pixelProc;

			const TArrayView<uint8> ImageData = pImage->DataStorage.GetLOD(0); 

			const auto& ProcessFace = [
				vertices, indices, VertexBlockIds, BlockId, ImageData, sizeX, sizeY, pixelProc
			] (int32 f)
			{
				// TODO: Select faces outside for loop?
				if (VertexBlockIds[indices[f * 3 + 0]] == BlockId)
				{
					constexpr int32 NumInterpolators = 1;
					Triangle<NumInterpolators>(ImageData.GetData(), ImageData.Num(),
						sizeX, sizeY,
						1,
						vertices[indices[f * 3 + 0]],
						vertices[indices[f * 3 + 1]],
						vertices[indices[f * 3 + 2]],
						pixelProc,
						false);
				}
			};

			ParallelFor(faceCount, ProcessFace);
		}

	}

}
