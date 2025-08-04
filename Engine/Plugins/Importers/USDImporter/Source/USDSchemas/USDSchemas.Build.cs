// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;

namespace UnrealBuildTool.Rules
{
	public class USDSchemas : ModuleRules
	{
		public USDSchemas(ReadOnlyTargetRules Target) : base(Target)
		{
			bUseRTTI = true;

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Boost",
					"CinematicCamera",
					"Core",
					"CoreUObject",
					"Engine",
					"GeometryCache",
					"GeometryCore", // for FitKDOP
					"HairStrandsCore",
					"InterchangeCore",
					"InterchangeEngine",
					"InterchangeFactoryNodes",
					"InterchangeNodes",
					"InterchangePipelines",
					"LiveLinkAnimationCore",
					"LiveLinkComponents",
					"LiveLinkInterface",
					"MeshDescription",
					"RenderCore",
					"RHI", // For FMaterialUpdateContext and the right way of updating material instance constants
					"Slate",
					"SlateCore",
					"StaticMeshDescription",
					"UnrealUSDWrapper",
					"USDClasses",
				}
			);

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"USDUtilities", // Public because we moved some headers here so that we can use them from Interchange
				}
			);

			if (Target.bBuildEditor)
			{
				PrivateDependencyModuleNames.AddRange(
					new string[]
					{
						"AudioEditor", // For USoundFactory, which we use for parsing UsdMediaSpatialAudio
						"BlueprintGraph",
						"GeometryCacheUSD",
						"HairStrandsEditor",
						"Kismet",
						"LiveLinkGraphNode",
						"MaterialEditor",
						"MeshUtilities",
						"PhysicsUtilities", // For generating UPhysicsAssets for SkeletalMeshes and ConvexDecompTool
						"PropertyEditor",
						"SparseVolumeTexture",
						"UnrealEd",
					}
				);

				if (Target.Platform == UnrealTargetPlatform.Win64)
				{
					PrivateDependencyModuleNames.AddRange(
						new string[]
						{
							"MaterialX"
						}
					);
				}
			}

			PrivateDefinitions.Add("SUPPRESS_PER_MODULE_INLINE_FILE"); // This module does not use core's standard operator new/delete overloads
		}
	}
}
