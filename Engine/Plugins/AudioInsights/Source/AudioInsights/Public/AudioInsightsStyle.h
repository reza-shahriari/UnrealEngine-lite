// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleMacros.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "Textures/SlateIcon.h"

#define LOCTEXT_NAMESPACE "AudioInsights"

namespace UE::Audio::Insights
{
	class AUDIOINSIGHTS_API FSlateStyle final : public FSlateStyleSet
	{
	public:
		static FSlateStyle& Get()
		{
			static FSlateStyle InsightsStyle;
			return InsightsStyle;
		}

		static FName GetStyleName()
		{
			const FLazyName StyleName = "AudioInsights";
			return StyleName.Resolve();
		}

		const FNumberFormattingOptions* GetAmpFloatFormat() const
		{
			static FNumberFormattingOptions FloatFormat;
			FloatFormat.MinimumIntegralDigits = 1;
			FloatFormat.MaximumIntegralDigits = 1;
			FloatFormat.MinimumFractionalDigits = 3;
			FloatFormat.MaximumFractionalDigits = 3;
			return &FloatFormat;
		};

		const FNumberFormattingOptions* GetDefaultFloatFormat() const
		{
			static FNumberFormattingOptions FloatFormat;
			FloatFormat.MinimumIntegralDigits = 1;
			FloatFormat.MinimumFractionalDigits = 4;
			FloatFormat.MaximumFractionalDigits = 4;
			return &FloatFormat;
		};

		const FNumberFormattingOptions* GetFreqFloatFormat() const
		{
			static FNumberFormattingOptions FloatFormat;
			FloatFormat.MinimumIntegralDigits = 1;
			FloatFormat.MaximumIntegralDigits = 5;
			FloatFormat.MinimumFractionalDigits = 0;
			FloatFormat.MaximumFractionalDigits = 2;
			return &FloatFormat;
		};

		const FNumberFormattingOptions* GetPitchFloatFormat() const
		{
			static FNumberFormattingOptions FloatFormat;
			FloatFormat.MinimumIntegralDigits = 1;
			FloatFormat.MaximumIntegralDigits = 3;
			FloatFormat.MinimumFractionalDigits = 3;
			FloatFormat.MaximumFractionalDigits = 3;
			return &FloatFormat;
		};

		const FNumberFormattingOptions* GetTimeFormat() const
		{
			static FNumberFormattingOptions FloatFormat;
			FloatFormat.MinimumIntegralDigits = 1;
			FloatFormat.MinimumFractionalDigits = 3;
			FloatFormat.MaximumFractionalDigits = 3;
			return &FloatFormat;
		};

		FText FormatSecondsAsTime(float InTimeSec) const
		{
			return FText::Format(LOCTEXT("TimeInSecondsFormat", "{0}s"), FText::AsNumber(InTimeSec, GetTimeFormat()));
		}

		FText FormatMillisecondsAsTime(float InTimeMS) const
		{
			return FText::Format(LOCTEXT("TimeInMillisecondsFormat", "{0}ms"), FText::AsNumber(InTimeMS, GetTimeFormat()));
		}

		FSlateIcon CreateIcon(FName InName)  const
		{
			return { GetStyleName(), InName };
		}

		const FSlateBrush& GetBrushEnsured(FName InName)  const
		{
			const ISlateStyle* AudioInsightsStyle = FSlateStyleRegistry::FindSlateStyle(GetStyleName());
			if (ensureMsgf(AudioInsightsStyle, TEXT("Missing slate style '%s'"), *GetStyleName().ToString()))
			{
				const FSlateBrush* Brush = AudioInsightsStyle->GetBrush(InName);
				if (ensureMsgf(Brush, TEXT("Missing brush '%s'"), *InName.ToString()))
				{
					return *Brush;
				}
			}

			if (const FSlateBrush* NoBrush = FAppStyle::GetBrush("NoBrush"))
			{
				return *NoBrush;
			}

			return *DefaultBrush;
		}

		FSlateStyle()
			: FSlateStyleSet(GetStyleName())
		{
			SetParentStyleName(FAppStyle::GetAppStyleSetName());

			const FString PluginsDir = FPaths::EnginePluginsDir();

			SetContentRoot(PluginsDir / TEXT("AudioInsights/Content"));
			SetCoreContentRoot(PluginsDir / TEXT("Slate"));

			/* Audio Insights */
			// Colors
			Set("AudioInsights.Analyzers.BackgroundColor", FLinearColor(0.0075f, 0.0075f, 0.0075f, 1.0f));

			// Icons
			const FVector2D Icon16(16.0f, 16.0f);
			const FVector2D Icon20(20.0f, 20.0f);
			const FVector2D Icon24(24.0f, 24.0f);
			const FVector2D Icon64(64.0f, 64.0f);

			Set("AudioInsights.Icon", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_insights_icon"), Icon16));
			Set("AudioInsights.Icon.Dashboard", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_dashboard"), Icon16));
			Set("AudioInsights.Icon.Event", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_event"), Icon16));
			Set("AudioInsights.Icon.Log", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_log"), Icon16));
			Set("AudioInsights.Icon.Sources", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_sources"), Icon16));
			Set("AudioInsights.Icon.Sources.Plots", new IMAGE_BRUSH(TEXT("Icons/audio_sources_plots"), Icon24));
			Set("AudioInsights.Icon.Submix", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_submix"), Icon16));
			Set("AudioInsights.Icon.VirtualLoop", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_virtualloop"), Icon16));
			Set("AudioInsights.Icon.Viewport", new IMAGE_BRUSH_SVG(TEXT("Icons/viewport"), Icon16));
			Set("AudioInsights.Icon.Open", new IMAGE_BRUSH_SVG(TEXT("Icons/open"), Icon20));
			Set("AudioInsights.Icon.ContentBrowser", new IMAGE_BRUSH_SVG(TEXT("Icons/content_browser"), Icon20));
			Set("AudioInsights.Icon.Start.Active", new IMAGE_BRUSH_SVG(TEXT("Icons/start_active"), Icon20));
			Set("AudioInsights.Icon.Start.Inactive", new IMAGE_BRUSH_SVG(TEXT("Icons/start_inactive"), Icon20));
			Set("AudioInsights.Icon.Record.Active", new IMAGE_BRUSH_SVG(TEXT("Icons/record_active"), Icon20));
			Set("AudioInsights.Icon.Record.Inactive", new IMAGE_BRUSH_SVG(TEXT("Icons/record_inactive"), Icon20));
			Set("AudioInsights.Icon.Stop.Active", new IMAGE_BRUSH_SVG(TEXT("Icons/stop_active"), Icon20));
			Set("AudioInsights.Icon.Stop.Inactive", new IMAGE_BRUSH_SVG(TEXT("Icons/stop_inactive"), Icon20));

			Set("AudioInsights.Thumbnail", new IMAGE_BRUSH_SVG(TEXT("Icons/audio_insights"), Icon16));


			/* Tree Dashboard */
			// Table style
			Set("TreeDashboard.TableViewRow", FTableRowStyle(FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row"))
				.SetOddRowBackgroundBrush(FSlateColorBrush(FStyleColors::Recessed))
				.SetEvenRowBackgroundBrush(FSlateColorBrush(FStyleColors::Background))
				.SetOddRowBackgroundHoveredBrush(FSlateColorBrush(FStyleColors::SelectHover))
				.SetEvenRowBackgroundHoveredBrush(FSlateColorBrush(FStyleColors::SelectHover))
			);


			/* Sound Dashboard */
			// Icons
			Set("AudioInsights.Icon.SoundDashboard.Browse",           new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/browse"),           Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Edit",             new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/edit"),             Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Filter",           new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/filter"),           Icon16));
			// @TODO UE-250399: Hide category pending to implement
			//Set("AudioInsights.Icon.SoundDashboard.Hide",             new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/hide"),             Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Info",             new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/info"),             Icon16));
			Set("AudioInsights.Icon.SoundDashboard.MetaSound",        new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/metasound"),        Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Mute",             new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/mute"),             Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Pin",              new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/pin"),              Icon16));
			Set("AudioInsights.Icon.SoundDashboard.ProceduralSource", new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/proceduralsource"), Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Reset",            new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/reset"),            Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Solo",             new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/solo"),             Icon16));
			Set("AudioInsights.Icon.SoundDashboard.SoundCue",         new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/soundcue"),         Icon16));
			Set("AudioInsights.Icon.SoundDashboard.SoundWave",        new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/soundwave"),        Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Tab",              new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/tab"),              Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Transparent",      new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/transparent"),      Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Visible",		  new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/visible"),			 Icon16));
			Set("AudioInsights.Icon.SoundDashboard.Invisible",		  new IMAGE_BRUSH_SVG(TEXT("Icons/SoundDashboard/invisible"),		 Icon16));
			
			// Category colors
			Set("SoundDashboard.MetaSoundColor", FLinearColor(0.008f, 0.76f, 0.078f));
			Set("SoundDashboard.SoundCueColor", FLinearColor(0.022f, 0.49f, 0.98f));
			Set("SoundDashboard.ProceduralSourceColor", FLinearColor(0.98f, 0.32f, 0.006f));
			Set("SoundDashboard.SoundWaveColor", FLinearColor(0.12f, 0.093f, 0.64f));
			Set("SoundDashboard.SoundCueTemplateColor", FLinearColor(0.98f, 0.01f, 0.01f));
			Set("SoundDashboard.PinnedColor", FLinearColor(0.9f, 0.9f, 0.9f));
			Set("SoundDashboard.HiddenColor", FLinearColor(0.4f, 0.4f, 0.4f));

			// Text colors
			Set("SoundDashboard.TimingOutTextColor", FLinearColor(1.0f, 1.0f, 1.0f, 0.25f));

			// Mute/Solo buttons style
			const FSlateRoundedBoxBrush RoundedWhiteAlphaBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.1f), 5.0f);

			Set("SoundDashboard.MuteSoloButton", FCheckBoxStyle(FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("TransparentCheckBox"))
				.SetPadding(FMargin(5.0f, 2.0f, 5.0f, 2.0f))
				.SetCheckedHoveredImage(RoundedWhiteAlphaBrush)
				.SetUncheckedHoveredImage(RoundedWhiteAlphaBrush)
			);

			FSlateStyleRegistry::RegisterSlateStyle(*this);
		}
	};
} // namespace UE::Audio::Insights
#undef LOCTEXT_NAMESPACE
