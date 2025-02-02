// Fill out your copyright notice in the Description page of Project Settings.

#include "StableDiffusionMoviePipeline.h"
#include "StableDiffusionToolsSettings.h"
#include "Engine/TextureRenderTarget2D.h"
#include "StableDiffusionSubsystem.h"
#include "MoviePipelineQueue.h"
#include "MoviePipeline.h"
#include "Async/Async.h"
#include "ImageUtils.h"
#include "EngineModule.h"
#include "IImageWrapperModule.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "MoviePipelineOutputSetting.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "MovieScene.h"
#include "RenderingThread.h"
#include "MoviePipelineImageQuantization.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "ImageWriteTask.h"
#include "ImageWriteQueue.h"
#include "StableDiffusionBlueprintLibrary.h"
#include "StableDiffusionToolsModule.h"
#include "Runtime/Launch/Resources/Version.h"



UStableDiffusionMoviePipeline::UStableDiffusionMoviePipeline() : UMoviePipelineDeferredPassBase()
{
	PassIdentifier = FMoviePipelinePassIdentifier("StableDiffusion");
}

void UStableDiffusionMoviePipeline::PostInitProperties()
{
	Super::PostInitProperties();

	auto Settings = GetMutableDefault<UStableDiffusionToolsSettings>();
	Settings->ReloadConfig(UStableDiffusionToolsSettings::StaticClass());

	if (!ImageGeneratorOverride) {
		if (Settings->GetGeneratorType() && Settings->GetGeneratorType() != UStableDiffusionBridge::StaticClass()) {
			ImageGeneratorOverride = Settings->GetGeneratorType();
		}
	}
	else {
		// We don't want to include the base bridge class as it has no implementation
		if (ImageGeneratorOverride->StaticClass() == UStableDiffusionBridge::StaticClass()) {
			ImageGeneratorOverride = nullptr;
		}
	}
}

#if WITH_EDITOR
FText UStableDiffusionMoviePipeline::GetFooterText(UMoviePipelineExecutorJob* InJob) const {
	return NSLOCTEXT(
		"MovieRenderPipeline",
		"DeferredBasePassSetting_FooterText_StableDiffusion",
		"Rendered frames are passed to the Stable Diffusion subsystem for processing");
}
#endif

void UStableDiffusionMoviePipeline::SetupForPipelineImpl(UMoviePipeline* InPipeline)
{
	Super::SetupForPipelineImpl(InPipeline);

	// Reset track containers
	LayerProcessorTracks.Reset();
	PromptTracks.Reset();
	OptionsTrack = nullptr;

	// Make sure model is loaded before we render
	auto SDSubsystem = GEditor->GetEditorSubsystem<UStableDiffusionSubsystem>();

	if (!SDSubsystem->GeneratorBridge || SDSubsystem->GeneratorBridge->StaticClass()->IsChildOf(ImageGeneratorOverride)) {
		SDSubsystem->CreateBridge(ImageGeneratorOverride);
	}

	auto Tracks = InPipeline->GetTargetSequence()->GetMovieScene()->GetMasterTracks();
	for (auto Track : Tracks) {
		if (auto MasterOptionsTrack = Cast<UStableDiffusionOptionsTrack>(Track)) {
			OptionsTrack = MasterOptionsTrack;
		} else if (auto PromptTrack = Cast<UStableDiffusionPromptMovieSceneTrack>(Track)){
			PromptTracks.Add(PromptTrack);
		}
		else if (auto LayerProcessorTrack = Cast<UStableDiffusionLayerProcessorTrack>(Track)) {
			LayerProcessorTracks.Add(LayerProcessorTrack);
		}
	}
}

void UStableDiffusionMoviePipeline::SetupImpl(const MoviePipeline::FMoviePipelineRenderPassInitSettings& InPassInitSettings)
{
	Super::SetupImpl(InPassInitSettings);
}

void UStableDiffusionMoviePipeline::TeardownForPipelineImpl(UMoviePipeline* InPipeline)
{
	PromptTracks.Reset();
}

void UStableDiffusionMoviePipeline::GatherOutputPassesImpl(TArray<FMoviePipelinePassIdentifier>& ExpectedRenderPasses) {
	Super::GatherOutputPassesImpl(ExpectedRenderPasses);
}


void UStableDiffusionMoviePipeline::RenderSample_GameThreadImpl(const FMoviePipelineRenderPassMetrics& InSampleState)
{
	UMoviePipelineImagePassBase::RenderSample_GameThreadImpl(InSampleState);

	// Wait for a surface to be available to write to. This will stall the game thread while the RHI/Render Thread catch up.
	{
		// Wait for a all surfaces to be available to write to. This will stall the game thread while the RHI/Render Thread catch up.
		SCOPE_CYCLE_COUNTER(STAT_MoviePipeline_WaitForAvailableSurface);
		for (TPair<FIntPoint, TSharedPtr<FMoviePipelineSurfaceQueue, ESPMode::ThreadSafe>> SurfaceQueueIt : SurfaceQueues)
		{
			if (SurfaceQueueIt.Value.IsValid())
			{
				SurfaceQueueIt.Value->BlockUntilAnyAvailable();
			}
		}
	}

	// Main Render Pass
	{
		FMoviePipelineRenderPassMetrics InOutSampleState = InSampleState;
		FMoviePipelinePassIdentifier LayerPassIdentifier(PassIdentifier);
		LayerPassIdentifier.Name = PassIdentifier.Name;
		LayerPassIdentifier.CameraName = GetCameraName(0);

		// Rebuild world
		//GetPipeline()->GetWorld()->UpdateWorldComponents(true, false);

		// Setup render targets and drawing surfaces
		FStableDiffusionDeferredPassRenderStatePayload Payload;
		Payload.CameraIndex = 0;
		Payload.TileIndex = InOutSampleState.TileIndexes;
		TWeakObjectPtr<UTextureRenderTarget2D> ViewRenderTarget = GetOrCreateViewRenderTarget(InOutSampleState.BackbufferSize, (IViewCalcPayload*)(&Payload));
		check(ViewRenderTarget.IsValid());
		FRenderTarget* RenderTarget = ViewRenderTarget->GameThread_GetRenderTargetResource();
		FCanvas Canvas = FCanvas(RenderTarget, nullptr, GetPipeline()->GetWorld(), ERHIFeatureLevel::SM5, FCanvas::CDM_ImmediateDrawing, 1.0f);

#if WITH_EDITOR
		auto SDSubsystem = GEditor->GetEditorSubsystem<UStableDiffusionSubsystem>();
		if (SDSubsystem) {
			// Get input image from rendered data
			// TODO: Add float colour support to the generated images
			FStableDiffusionInput Input;
			Input.PreviewIterationRate = -1;
			Input.DebugPythonImages = DebugPythonImages;
			Input.Options.InSizeX = RenderTarget->GetSizeXY().X;
			Input.Options.InSizeY = RenderTarget->GetSizeXY().Y;
			Input.Options.OutSizeX = RenderTarget->GetSizeXY().X;
			Input.Options.OutSizeY = RenderTarget->GetSizeXY().Y;

			// Get frame time for curve evaluation
			auto EffectiveFrame = FFrameNumber(this->GetPipeline()->GetOutputState().EffectiveFrameNumber);
			auto TargetSequencer = this->GetPipeline()->GetTargetSequence();
			auto OriginalSeqFramerateRatio = TargetSequencer->GetMovieScene()->GetDisplayRate().AsDecimal() / this->GetPipeline()->GetPipelineMasterConfig()->GetEffectiveFrameRate(TargetSequencer).AsDecimal();

			// To evaluate curves we need to use the original sequence frame number. 
			// Frame number for curves includes subframes so we also mult by 1000 to get the subframe number
			auto FullFrameTime = EffectiveFrame * OriginalSeqFramerateRatio * 1000.0f;

			// Get image pipeline and global options from the options section
			TArray<UImagePipelineStageAsset*> Stages;
			if (OptionsTrack) {
				for (auto Section : OptionsTrack->Sections) {
					if (Section) {
						auto OptionSection = Cast<UStableDiffusionOptionsSection>(Section);
						if (OptionSection) {
							Stages = OptionSection->PipelineStages;
							
							// Evaluate curve values
							OptionSection->GetStrengthChannel().Evaluate(FullFrameTime, Input.Options.Strength);
							OptionSection->GetIterationsChannel().Evaluate(FullFrameTime, Input.Options.Iterations);
							OptionSection->GetSeedChannel().Evaluate(FullFrameTime, Input.Options.Seed);
						}
					}
				}
			}

			// Build combined prompt
			TArray<FString> AccumulatedPrompt;
			for (auto Track : PromptTracks) {
				for (auto Section : Track->Sections) {
					if (auto PromptSection = Cast<UStableDiffusionPromptMovieSceneSection>(Section)) {
						if (PromptSection->IsActive()) {
							FPrompt Prompt = PromptSection->Prompt;
							PromptSection->GetWeightChannel().Evaluate(FullFrameTime, Prompt.Weight);

							// Get frame range of the section
							auto SectionStartFrame = PromptSection->GetInclusiveStartFrame();
							auto SectionEndFrame = PromptSection->GetExclusiveEndFrame();
							if (SectionStartFrame < FullFrameTime && FullFrameTime < SectionEndFrame) {
								Input.Options.AddPrompt(Prompt);
							}
						}
					}
				}
			}

			// Create output objects
			UTexture2D* OutTexture = UTexture2D::CreateTransient(Input.Options.OutSizeX, Input.Options.OutSizeY);
			FStableDiffusionImageResult LastStageResult;

			// Generate new stable diffusion frame from pipeline stages
			for (size_t StageIdx = 0; StageIdx < Stages.Num(); ++StageIdx) {
				UImagePipelineStageAsset* PrevStage = (StageIdx) ? Stages[StageIdx - 1] : nullptr;
				UImagePipelineStageAsset* CurrentStage = StageIdx < Stages.Num() ? Stages[StageIdx] : nullptr;
				
				if (!CurrentStage)
					continue;

				// Init model at the start of each stage.
				// TODO: Cache last model and only re-init if model options have changed
				SDSubsystem->InitModel(CurrentStage->Model->Options, CurrentStage->Pipeline, CurrentStage->LORAAsset, CurrentStage->TextualInversionAsset, CurrentStage->Layers, false, AllowNSFW, PaddingMode);
				if (SDSubsystem->GetModelStatus().ModelStatus != EModelStatus::Loaded) {
					UE_LOG(LogTemp, Error, TEXT("Failed to load model. Check the output log for more information"));
					continue;
				}

				// Duplicate the input as we're going to need to modify it
				FStableDiffusionInput StageInput = Input;

				// Modify global input options from the current stage
				StageInput.OutputType = CurrentStage->OutputType;
				
				// TODO: Make these keyable parameters in the options track
				StageInput.Options.GuidanceScale = (CurrentStage->OverrideInputOptions.OverrideGuidanceScale) ? CurrentStage->OverrideInputOptions.GuidanceScale : StageInput.Options.GuidanceScale;
				StageInput.Options.LoraWeight = (CurrentStage->OverrideInputOptions.OverrideLoraWeight) ? CurrentStage->OverrideInputOptions.LoraWeight : StageInput.Options.LoraWeight;

				// Duplicate the layers so we can modify the options without modifying the original asset
				TArray<FLayerProcessorContext> CurrentStageLayers;
				for (auto& Layer : CurrentStage->Layers) {
					FLayerProcessorContext TargetLayer;
					TargetLayer.OutputType = Layer.OutputType;
					TargetLayer.LayerType = Layer.LayerType;
					TargetLayer.Role = Layer.Role;
					TargetLayer.Processor = Layer.Processor;
					TargetLayer.ProcessorOptions = (Layer.ProcessorOptions) ? DuplicateObject(Layer.ProcessorOptions, GetPipeline()) : Layer.Processor->AllocateLayerOptions();
					TargetLayer.LatentData = (TargetLayer.OutputType == EImageType::Latent && LastStageResult.Completed) ? LastStageResult.OutLatent : TArray<uint8>();
					CurrentStageLayers.Add(TargetLayer);
				}

				// Gather all layers and modify options based on animated parameters
				ApplyLayerOptions(CurrentStageLayers, StageIdx, FullFrameTime);
				StageInput.InputLayers = CurrentStageLayers;

				bool FirstView = true;
				// Start a new capture pass for each layer
				for (auto& Layer : StageInput.InputLayers) {
					if (Layer.Processor) {
						// Prepare rendering the layer
						TSharedPtr<FSceneViewFamilyContext> ViewFamily;
						Layer.Processor->BeginCaptureLayer(GetPipeline()->GetWorld(), FIntPoint(StageInput.Options.OutSizeX, StageInput.Options.OutSizeY), nullptr, Layer.ProcessorOptions);
						GetPipeline()->GetWorld()->SendAllEndOfFrameUpdates();
						FSceneView* View = BeginSDLayerPass(InOutSampleState, ViewFamily);
						FirstView = false;

						// Set up post processing material from layer processor
						View->FinalPostProcessSettings.AddBlendable(Layer.Processor->GetActivePostMaterial(), 1.0f);
						IBlendableInterface* BlendableInterface = Cast<IBlendableInterface>(Layer.Processor->GetActivePostMaterial());
						if (BlendableInterface) {
							ViewFamily->EngineShowFlags.SetPostProcessMaterial(true);
							BlendableInterface->OverrideBlendableSettings(*View, 1.f);
						}
						ViewFamily->EngineShowFlags.SetPostProcessing(true);
						View->FinalPostProcessSettings.bBufferVisualizationDumpRequired = true;

						// Render the layer
						GetRendererModule().BeginRenderingViewFamily(&Canvas, ViewFamily.Get());
						FlushRenderingCommands();

						if (!RenderTarget->ReadPixels(Layer.LayerPixels, FReadSurfaceDataFlags())) {
							UE_LOG(LogTemp, Error, TEXT("Failed to read pixels from render target"));
						}

						// Cleanup before move
						View->FinalPostProcessSettings.RemoveBlendable(Layer.Processor->PostMaterial);
						Layer.Processor->EndCaptureLayer(GetPipeline()->GetWorld());

						StageInput.ProcessedLayers.Add(MoveTemp(Layer));
					}
				} 

				// Make sure model is loaded before generating
				if (SDSubsystem->GetModelStatus().ModelStatus == EModelStatus::Loaded) {
					LastStageResult = SDSubsystem->GeneratorBridge->GenerateImageFromStartImage(StageInput, OutTexture, nullptr);
				}


			} // End of stage pipeline processing

			// Convert generated image to 16 bit for the exr pipeline
			// TODO: Check bit depth of movie pipeline and convert to that instead
			TUniquePtr<FImagePixelData> SDImageDataBuffer16bit;
			if(IsValid(LastStageResult.OutTexture)){
				UStableDiffusionBlueprintLibrary::UpdateTextureSync(OutTexture);
				TArray<FColor> Pixels = UStableDiffusionBlueprintLibrary::ReadPixels(OutTexture);

				// Convert 8bit BGRA FColors returned from SD to 16bit BGRA
				TUniquePtr<TImagePixelData<FColor>> SDImageDataBuffer8bit;
				SDImageDataBuffer8bit = MakeUnique<TImagePixelData<FColor>>(FIntPoint(LastStageResult.OutWidth, LastStageResult.OutHeight), TArray64<FColor>(MoveTemp(Pixels)));
				SDImageDataBuffer16bit = UE::MoviePipeline::QuantizeImagePixelDataToBitDepth(SDImageDataBuffer8bit.Get(), 16);
			}
			else {
				UE_LOG(LogTemp, Error, TEXT("Stable diffusion generator failed to return any pixel data on frame %d. Please add a model asset to the Options track or initialize the StableDiffusionSubsystem model."), EffectiveFrame.Value);

				// Insert blank frame
				TArray<FColor> EmptyPixels;
				EmptyPixels.InsertUninitialized(0, Input.Options.OutSizeX * Input.Options.OutSizeY);
				TUniquePtr<TImagePixelData<FColor>> SDImageDataBuffer8bit = MakeUnique<TImagePixelData<FColor>>(FIntPoint(Input.Options.OutSizeX, Input.Options.OutSizeY), TArray64<FColor>(MoveTemp(EmptyPixels)));
				SDImageDataBuffer16bit = UE::MoviePipeline::QuantizeImagePixelDataToBitDepth(SDImageDataBuffer8bit.Get(), 16);
			}

			// Render the result to the render target
			ENQUEUE_RENDER_COMMAND(UpdateMoviePipelineRenderTarget)([this, Buffer=MoveTemp(SDImageDataBuffer16bit), RenderTarget](FRHICommandListImmediate& RHICmdList) {
				int64 OutSize;
				const void* OutRawData = nullptr;
				Buffer->GetRawData(OutRawData, OutSize);
				RHICmdList.UpdateTexture2D(
					RenderTarget->GetRenderTargetTexture(),
					0,
					FUpdateTextureRegion2D(0, 0, 0, 0, RenderTarget->GetSizeXY().X, RenderTarget->GetSizeXY().Y),
					RenderTarget->GetSizeXY().X * sizeof(FFloat16Color),
					(uint8*)OutRawData
				);
				RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			});

			// Readback + Accumulate
			PostRendererSubmission(InSampleState, LayerPassIdentifier, GetOutputFileSortingOrder() + 1, Canvas);
		}
#endif
	}
}



FSceneView* UStableDiffusionMoviePipeline::BeginSDLayerPass(FMoviePipelineRenderPassMetrics& InOutSampleState, TSharedPtr<FSceneViewFamilyContext>& ViewFamily)
{
	// Get new view state for our stencil render
	FStableDiffusionDeferredPassRenderStatePayload RenderState;
	RenderState.CameraIndex = 0;
	RenderState.TileIndex = InOutSampleState.TileIndexes;
	RenderState.SceneViewIndex = 0;
	ViewFamily = CalculateViewFamily(InOutSampleState, &RenderState);

	//ViewFamily->bResolveScene = true;
	//ViewFamily->bIsMultipleViewFamily = true;
	ViewFamily->EngineShowFlags.PostProcessing = 1;
	ViewFamily->EngineShowFlags.SetPostProcessMaterial(true);
	ViewFamily->EngineShowFlags.SetPostProcessing(true);
	
	FSceneView* View = const_cast<FSceneView*>(ViewFamily->Views[0]);
	View->FinalPostProcessSettings.bBufferVisualizationDumpRequired = true;
	return View;
}


void UStableDiffusionMoviePipeline::BeginExportImpl(){
	if (!bUpscale) {
		return;
	}

	UMoviePipelineOutputSetting* OutputSettings = GetPipeline()->GetPipelineMasterConfig()->FindSetting<UMoviePipelineOutputSetting>();
	check(OutputSettings);

	auto SDSubsystem = GEditor->GetEditorSubsystem<UStableDiffusionSubsystem>();
	check(SDSubsystem);

	// Free up loaded model so we have enough VRAM to upsample
	SDSubsystem->ReleaseModel();

	auto OutputData = GetPipeline()->GetOutputDataParams();
	for (auto shot : OutputData.ShotData) {
		for (auto renderpass : shot.RenderPassData) {
			// We want to persist the upsampler model so we don't have to keep reloading it every frame
			SDSubsystem->GeneratorBridge->StartUpsample();

			for (auto file : renderpass.Value.FilePaths) {
				// Reload image from disk
				UTexture2D* Image = FImageUtils::ImportFileAsTexture2D(file);
				if (!IsValid(Image)) {
					continue;
				}
				UStableDiffusionBlueprintLibrary::UpdateTextureSync(Image);

				// Read half-float pixels from source texture
				FFloat16Color* MipData = reinterpret_cast<FFloat16Color*>(Image->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_ONLY));
				TArrayView64<FFloat16Color> SourceColors(MipData, Image->GetSizeX() * Image->GetSizeY());

				// Convert pixels from FFloat16Color to FColor
				TArray<FColor> QuantitizedPixelData;
				QuantitizedPixelData.InsertUninitialized(0, SourceColors.Num());
				for (int idx = 0; idx < SourceColors.Num(); ++idx) {
					QuantitizedPixelData[idx] = SourceColors[idx].GetFloats().ToFColor(true);
				}

				// Unlock source texture since we've converted the pixel data
				Image->GetPlatformData()->Mips[0].BulkData.Unlock();
				
				// Build our upsample parameters
				FStableDiffusionImageResult UpsampleInput;
				UpsampleInput.OutWidth = Image->GetSizeX();
				UpsampleInput.OutHeight = Image->GetSizeY();
				UpsampleInput.Upsampled = false;
				UpsampleInput.Completed = false;
				UpsampleInput.OutTexture = UStableDiffusionBlueprintLibrary::ColorBufferToTexture(QuantitizedPixelData, FIntPoint(Image->GetSizeX(), Image->GetSizeY()), nullptr, true);
				UStableDiffusionBlueprintLibrary::UpdateTextureSync(UpsampleInput.OutTexture);
				
				// Create a destination texture that is 4x times larger than the input to hold the upsample result
				// TODO: Allow for arbitary resize factors 
				UTexture2D* UpsampledTexture = UTexture2D::CreateTransient(UpsampleInput.OutTexture->GetSizeX() * 4, UpsampleInput.OutTexture->GetSizeY() * 4);
				FStableDiffusionImageResult UpsampleResult = SDSubsystem->GeneratorBridge->UpsampleImage(UpsampleInput, UpsampledTexture);

				if (IsValid(UpsampleResult.OutTexture)) {
					UStableDiffusionBlueprintLibrary::UpdateTextureSync(UpsampleResult.OutTexture);

					// Build an export task that will async write the upsampled image to disk
					TUniquePtr<FImageWriteTask> ExportTask = MakeUnique<FImageWriteTask>();
					ExportTask->Format = EImageFormat::EXR;
					ExportTask->CompressionQuality = (int32)EImageCompressionQuality::Default;
					FString OutputName = FString::Printf(TEXT("%s%s"), *UpscaledFramePrefix, *FPaths::GetBaseFilename(file));
					FString OutputDirectory = OutputSettings->OutputDirectory.Path;
					FString OutputPath = FPaths::Combine(OutputDirectory, OutputName);
					FString OutputPathResolved;

					TMap<FString, FString> FormatOverrides{ {TEXT("ext"), *FPaths::GetExtension(file)} };
					FMoviePipelineFormatArgs OutArgs;
					GetPipeline()->ResolveFilenameFormatArguments(OutputPath, FormatOverrides, OutputPathResolved, OutArgs);
					ExportTask->Filename = OutputPathResolved;

					// Convert RGBA pixels back to FloatRGBA
					TArray<FColor> SrcPixels = UStableDiffusionBlueprintLibrary::ReadPixels(UpsampleResult.OutTexture);
					TArray64<FFloat16Color> ConvertedSrcPixels;
					ConvertedSrcPixels.InsertUninitialized(0, SrcPixels.Num());
					for (size_t idx = 0; idx < SrcPixels.Num(); ++idx) {
						ConvertedSrcPixels[idx] = FFloat16Color(SrcPixels[idx]);
					}
					TUniquePtr<TImagePixelData<FFloat16Color>> UpscaledPixelData = MakeUnique<TImagePixelData<FFloat16Color>>(
						FIntPoint(UpsampleResult.OutWidth, UpsampleResult.OutHeight),
						MoveTemp(ConvertedSrcPixels)
						);
					ExportTask->PixelData = MoveTemp(UpscaledPixelData);
					
					// Enqueue image write
					GetPipeline()->ImageWriteQueue->Enqueue(MoveTemp(ExportTask));
				}
			}

			SDSubsystem->GeneratorBridge->StopUpsample();
		}
	}
}

void UStableDiffusionMoviePipeline::ApplyLayerOptions(TArray<FLayerProcessorContext>& Layers, size_t StageIndex, FFrameTime FrameTime) {
	for (auto Track : LayerProcessorTracks) {
		for (auto Section : Track->Sections) {
			if (auto LayerProcessorSection = Cast<UStableDiffusionLayerProcessorSection>(Section)) {

				// Get frame range of the section
				bool InRange = true;
				InRange &= (LayerProcessorSection->HasStartFrame()) ? LayerProcessorSection->GetInclusiveStartFrame() < FrameTime : true;
				InRange &= (LayerProcessorSection->HasEndFrame()) ? LayerProcessorSection->GetExclusiveEndFrame() > FrameTime : true;
				
				if (InRange && 
					LayerProcessorSection->IsActive() && 
					LayerProcessorSection->LayerIndex < Layers.Num() && 
					LayerProcessorSection->ImagePipelineStageIndex == StageIndex
				)
				{
					auto ScalarParams = LayerProcessorSection->GetScalarParameterNamesAndCurves();
					auto& ExistingLayerContext = Layers[LayerProcessorSection->LayerIndex];

					ULayerProcessorOptions* LayerOptions = (LayerProcessorSection->LayerProcessorOptionOverride) ? LayerProcessorSection->LayerProcessorOptionOverride : ExistingLayerContext.ProcessorOptions;
					if (IsValid(LayerOptions)) {
						// Iterate over all properties in the processor options class
						for (TFieldIterator<FProperty> PropertyIt(LayerOptions->GetClass()); PropertyIt; ++PropertyIt)
						{
							if (FProperty* Prop = *PropertyIt) {
								if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Prop)) {
									// Match the property against the available parameter curves in this section
									auto Param = ScalarParams.FindByPredicate([&Prop](const FScalarParameterNameAndCurve& ParamNameAndCurve) {
										return ParamNameAndCurve.ParameterName == Prop->GetFName();
										});

									// Evaluate the parameter at the current time and set the value in the layer options object
									if (Param) {
										float ParamValAtTime;
										Param->ParameterCurve.Evaluate(FrameTime, ParamValAtTime);
										Prop->SetValue_InContainer(LayerOptions, &ParamValAtTime);
									}
								}
							}
						}
					}

					ExistingLayerContext.ProcessorOptions = LayerOptions;
				}
			}
		}
	}
}