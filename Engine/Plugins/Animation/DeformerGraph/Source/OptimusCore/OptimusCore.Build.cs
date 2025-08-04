// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
    public class OptimusCore : ModuleRules
    {
        public OptimusCore(ReadOnlyTargetRules Target) : base(Target)
		{
			NumIncludedBytesPerUnityCPPOverride = 688128; // best unity size found from using UBT ProfileUnitySizes mode

            PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"SlateCore"
				}
			);

            PrivateDependencyModuleNames.AddRange(
                new string[]
                {
					"ComputeFramework",
					"Core",
					"CoreUObject",
					"Engine",
					"OptimusSettings",
					"Projects",
					"RenderCore",
					"Renderer",
					"RHI",
					"RigVM",
					"ControlRig",
					"StaticMeshDescription",
					"MeshDescription"
				}
			);

            if (Target.bBuildEditor == true)
            {
	            PrivateDependencyModuleNames.AddRange(
		            new string[]
		            {
			            "RigVMDeveloper",
		            }
	            );
            }
			
			bDisableAutoRTFMInstrumentation = true;
        }
    }
}
