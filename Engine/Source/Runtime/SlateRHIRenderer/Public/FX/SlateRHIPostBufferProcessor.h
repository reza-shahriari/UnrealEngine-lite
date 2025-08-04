// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/TextureRenderTarget2D.h"
#include "Layout/PaintGeometry.h"
#include "Rendering/RenderingCommon.h"
#include "Rendering/SlateRenderer.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "Interfaces/SlateRHIRenderingPolicyInterface.h"
#include "ScreenPass.h"

#include "SlateRHIPostBufferProcessor.generated.h"

class USlateRHIPostBufferProcessor;

/**
 * Proxy for post buffer processor that the renderthread uses to perform processing
 * This proxy exists because generally speaking usage on UObjects on the renderthread
 * is a race condition due to UObjects being managed / updated by the game thread
 */
class FSlateRHIPostBufferProcessorProxy : public TSharedFromThis<FSlateRHIPostBufferProcessorProxy>
{

public:

	virtual ~FSlateRHIPostBufferProcessorProxy()
	{
	}

	/** Called on the render thread to run a post processing operation on the input texture and produce the output texture. */
	virtual void PostProcess_Renderthread(FRDGBuilder& GraphBuilder, const FScreenPassTexture& InputTexture, const FScreenPassTexture& OutputTexture)
	{
	}

	UE_DEPRECATED(5.5, "Use RDG version instead.")
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	virtual void PostProcess_Renderthread(FRHICommandListImmediate& RHICmdList, FRHITexture* Src, FRHITexture* Dst, FIntRect SrcRect, FIntRect DstRect, FSlateRHIRenderingPolicyInterface InRenderingPolicy)
	{
	}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	/** 
	 * Called when an post buffer update element is added to a renderbatch, 
	 * gives proxies a chance to queue updates to their renderthread values based on the UObject processor.
	 * These updates should likely be guarded by an 'FRenderCommandFence' to avoid duplicate updates
	 */
	virtual void OnUpdateValuesRenderThread()
	{
	}

	/**
	 * Set the UObject that we are a renderthread proxy for, useful for doing gamethread updates from the proxy
	 */
	void SetOwningProcessorObject(USlateRHIPostBufferProcessor* InParentObject)
	{
		ParentObject = InParentObject;
	}

protected:

	/** Pointer to processor that we are a proxy for, external design constraints should ensure that this is always valid */
	TWeakObjectPtr<USlateRHIPostBufferProcessor> ParentObject;
};

/**
 * Base class for types that can process the backbuffer scene into the slate post buffer.
 * 
 * Implement 'PostProcess' in your derived class. Additionally, you need to create a renderthread proxy that derives from 'FSlateRHIPostBufferProcessorProxy'
 * For an example see: USlatePostBufferBlur.
 */
UCLASS(MinimalAPI, Abstract, Blueprintable, CollapseCategories)
class USlateRHIPostBufferProcessor : public UObject
{
	GENERATED_BODY()

public:

	virtual ~USlateRHIPostBufferProcessor()
	{
	}

	/**
	 * Overridable postprocess for the given source scene backbuffer provided in 'Src' into 'Dst'
	 * You must override this method. In your override, you should copy params before executing 'ENQUEUE_RENDER_COMMAND'.
	 * This allows you to avoid render & game thread race conditions. See 'SlatePostBufferBlur' for example.
	 * Also avoid capturing [this] in your override, to avoid possible GC issues with the processor instance.
	 *
	 * @param InViewInfo			'FViewportInfo' resource used to get backbuffer in standalone
	 * @param InViewportTexture		'FSlateRenderTargetRHI' resource used to get the 'BufferedRT' viewport texture used in PIE
	 * @param InElementWindowSize	Size of window being rendered, used to determine if using stereo rendering or not.
	 * @param InRenderingPolicy		Slate RHI RenderingPolicy
	 * @param InSlatePostBuffer		Texture render target used for final output
	 */
	UE_DEPRECATED(5.5, "This path is longer supported. Get the render proxy instead.")
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	virtual void PostProcess(FRenderResource* InViewInfo, FRenderResource* InViewportTexture, FVector2D InElementWindowSize, FSlateRHIRenderingPolicyInterface InRenderingPolicy, UTextureRenderTarget2D* InSlatePostBuffer)
	{
	}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	/**
	 * Gets proxy for this post buffer processor, for execution on the renderthread
	 */
	virtual TSharedPtr<FSlateRHIPostBufferProcessorProxy> GetRenderThreadProxy()
	{
		return nullptr;
	}
};
