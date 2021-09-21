#include "AlsCameraComponent.h"

#include "AlsCameraAnimationInstance.h"
#include "AlsCharacter.h"
#include "DisplayDebugHelpers.h"
#include "DrawDebugHelpers.h"
#include "Camera/CameraTypes.h"
#include "Engine/Canvas.h"
#include "Utility/AlsCameraConstants.h"
#include "Utility/AlsMath.h"
#include "Utility/AlsUtility.h"

UAlsCameraComponent::UAlsCameraComponent()
{
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	bTickInEditor = false;
	bHiddenInGame = true;

	AnimClass = UAlsCameraAnimationInstance::StaticClass();
}

void UAlsCameraComponent::OnRegister()
{
	AlsCharacter = Cast<AAlsCharacter>(GetOwner());

	Super::OnRegister();
}

void UAlsCameraComponent::Activate(const bool bReset)
{
	if (!bReset && !ShouldActivate())
	{
		Super::Activate(bReset);
		return;
	}

	Super::Activate(bReset);

	TickCamera(0.0f, false);
}

void UAlsCameraComponent::BeginPlay()
{
	Super::BeginPlay();

	ensure(IsValid(SkeletalMesh) && IsValid(AlsCharacter));
}

void UAlsCameraComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	TickCamera(DeltaTime);
}

FVector UAlsCameraComponent::GetFirstPersonPivotLocation() const
{
	return AlsCharacter->GetMesh()->GetSocketLocation(FirstPersonSocket);
}

FTransform UAlsCameraComponent::GetThirdPersonPivotTransform() const
{
	return {
		GetComponentRotation(),
		(AlsCharacter->GetMesh()->GetSocketLocation(ThirdPersonFirstSocket) +
		 AlsCharacter->GetMesh()->GetSocketLocation(ThirdPersonSecondSocket)) * 0.5f
	};
}

void UAlsCameraComponent::GetViewInfo(FMinimalViewInfo& ViewInfo) const
{
	ViewInfo.Location = CameraLocation;
	ViewInfo.Rotation = CameraRotation;
	ViewInfo.FOV = CameraFov;
}

void UAlsCameraComponent::TickCamera(float DeltaTime, bool bAllowLag)
{
	if (!IsValid(AlsCharacter) || !IsValid(GetAnimInstance()))
	{
		return;
	}

#if ENABLE_DRAW_DEBUG
	const auto bDisplayDebugCameraShapes{UAlsUtility::ShouldDisplayDebug(GetOwner(), UAlsCameraConstants::CameraShapesDisplayName())};
	const auto bDisplayDebugCameraTraces{UAlsUtility::ShouldDisplayDebug(GetOwner(), UAlsCameraConstants::CameraTracesDisplayName())};
#else
	const auto bDisplayDebugCameraShapes{false};
	const auto bDisplayDebugCameraTraces{false};
#endif

	// Calculate camera rotation. Use raw rotation locally and smooth rotation on remote clients.

	auto TargetCameraRotation{
		AlsCharacter->IsLocallyControlled()
			? AlsCharacter->GetViewRotation()
			: AlsCharacter->GetViewState().SmoothRotation
	};

	if (bAllowLag)
	{
		auto RotationLag{GetAnimInstance()->GetCurveValue(UAlsCameraConstants::RotationLagCurve())};

		if (bUseLagSubstepping && DeltaTime > MaxLagSubstepDeltaTime && RotationLag > 0.0f)
		{
			const auto InitialCameraRotation{CameraRotation};
			const auto SubstepRotationSpeed{(TargetCameraRotation - InitialCameraRotation).GetNormalized() * (1.f / DeltaTime)};

			auto PreviousSubstepTime{0.0f};

			for (auto SubstepNumber{1};; SubstepNumber++)
			{
				const auto SubstepTime{SubstepNumber * MaxLagSubstepDeltaTime};
				if (SubstepTime < DeltaTime - KINDA_SMALL_NUMBER)
				{
					CameraRotation = FMath::RInterpTo(CameraRotation, InitialCameraRotation + SubstepRotationSpeed * SubstepTime,
					                                  SubstepTime - PreviousSubstepTime, RotationLag);

					PreviousSubstepTime = SubstepTime;
					continue;
				}

				CameraRotation = FMath::RInterpTo(CameraRotation, TargetCameraRotation, DeltaTime - PreviousSubstepTime, RotationLag);
				break;
			}
		}
		else
		{
			CameraRotation = UAlsMath::ExponentialDecay(CameraRotation, TargetCameraRotation, DeltaTime, RotationLag);
		}
	}
	else
	{
		CameraRotation = TargetCameraRotation;
	}

	const auto FirstPersonOverride{UAlsMath::Clamp01(GetAnimInstance()->GetCurveValue(UAlsCameraConstants::FirstPersonOverrideCurve()))};
	if (FirstPersonOverride >= 1.0f)
	{
		PivotTargetLocation = GetThirdPersonPivotTransform().GetLocation();
		PivotLagLocation = PivotTargetLocation;
		PivotLocation = PivotTargetLocation;

		CameraLocation = GetFirstPersonPivotLocation();
		CameraFov = FirstPersonFov;
		return;
	}

	// Calculate pivot lag location. Get the target pivot transform and
	// interpolate using axis independent lag for maximum control.

	const auto PivotTargetTransform{GetThirdPersonPivotTransform()};

	PivotTargetLocation = PivotTargetTransform.GetLocation();

	const auto LagRotation{FRotator{0.0f, CameraRotation.Yaw, 0.0f}.Quaternion()};

#if ENABLE_DRAW_DEBUG
	if (bDisplayDebugCameraShapes)
	{
		UAlsUtility::DrawDebugSphereAlternative(GetWorld(), PivotTargetLocation, LagRotation.Rotator(), 16.0f, FLinearColor::Green);
	}
#endif

	if (bAllowLag)
	{
		const auto RelativeInitialPivotLagLocation{LagRotation.UnrotateVector(PivotLagLocation)};
		const auto RelativeTargetPivotLocation{LagRotation.UnrotateVector(PivotTargetLocation)};

		auto LocationLagX{GetAnimInstance()->GetCurveValue(UAlsCameraConstants::LocationLagXCurve())};
		auto LocationLagY{GetAnimInstance()->GetCurveValue(UAlsCameraConstants::LocationLagYCurve())};
		auto LocationLagZ{GetAnimInstance()->GetCurveValue(UAlsCameraConstants::LocationLagZCurve())};

		if (bUseLagSubstepping && DeltaTime > MaxLagSubstepDeltaTime &&
		    (LocationLagX > 0.0f || LocationLagY > 0.0f || LocationLagZ > 0.0f))
		{
			const auto SubstepMovementSpeed{(RelativeTargetPivotLocation - RelativeInitialPivotLagLocation) / DeltaTime};

			auto RelativePivotLagLocation{RelativeInitialPivotLagLocation};
			auto PreviousSubstepTime{0.0f};

			for (auto SubstepNumber{1};; SubstepNumber++)
			{
				const auto SubstepTime{SubstepNumber * MaxLagSubstepDeltaTime};
				if (SubstepTime < DeltaTime - KINDA_SMALL_NUMBER)
				{
					const auto SubstepRelativeTargetPivotLocation{RelativeInitialPivotLagLocation + SubstepMovementSpeed * SubstepTime};
					const auto SubstepDeltaTime{SubstepTime - PreviousSubstepTime};

					RelativePivotLagLocation.X = FMath::FInterpTo(RelativePivotLagLocation.X, SubstepRelativeTargetPivotLocation.X,
					                                              SubstepDeltaTime, LocationLagX);
					RelativePivotLagLocation.Y = FMath::FInterpTo(RelativePivotLagLocation.Y, SubstepRelativeTargetPivotLocation.Y,
					                                              SubstepDeltaTime, LocationLagY);
					RelativePivotLagLocation.Z = FMath::FInterpTo(RelativePivotLagLocation.Z, SubstepRelativeTargetPivotLocation.Z,
					                                              SubstepDeltaTime, LocationLagZ);

					PreviousSubstepTime = SubstepTime;
					continue;
				}

				const auto SubstepDeltaTime{DeltaTime - PreviousSubstepTime};

				RelativePivotLagLocation.X = FMath::FInterpTo(RelativePivotLagLocation.X, RelativeTargetPivotLocation.X,
				                                              SubstepDeltaTime, LocationLagX);
				RelativePivotLagLocation.Y = FMath::FInterpTo(RelativePivotLagLocation.Y, RelativeTargetPivotLocation.Y,
				                                              SubstepDeltaTime, LocationLagY);
				RelativePivotLagLocation.Z = FMath::FInterpTo(RelativePivotLagLocation.Z, RelativeTargetPivotLocation.Z,
				                                              SubstepDeltaTime, LocationLagZ);
				break;
			}

			PivotLagLocation = LagRotation.RotateVector(RelativePivotLagLocation);
		}
		else
		{
			PivotLagLocation = LagRotation.RotateVector({
				UAlsMath::ExponentialDecay(RelativeInitialPivotLagLocation.X, RelativeTargetPivotLocation.X, DeltaTime, LocationLagX),
				UAlsMath::ExponentialDecay(RelativeInitialPivotLagLocation.Y, RelativeTargetPivotLocation.Y, DeltaTime, LocationLagY),
				UAlsMath::ExponentialDecay(RelativeInitialPivotLagLocation.Z, RelativeTargetPivotLocation.Z, DeltaTime, LocationLagZ)
			});
		}
	}
	else
	{
		PivotLagLocation = PivotTargetLocation;
	}

#if ENABLE_DRAW_DEBUG
	if (bDisplayDebugCameraShapes)
	{
		DrawDebugLine(GetWorld(), PivotLagLocation, PivotTargetLocation,
		              FLinearColor{1.0f, 0.5f, 0.0f}.ToFColor(true),
		              false, 0.0f, 0, UAlsUtility::DrawLineThickness);

		UAlsUtility::DrawDebugSphereAlternative(GetWorld(), PivotLagLocation, LagRotation.Rotator(), 16.0f, {1.0f, 0.5f, 0.0f});
	}
#endif

	// Calculate pivot location. Get the pivot lag location and apply local offsets for further camera control.

	const auto PivotOffset{
		PivotTargetTransform.TransformVectorNoScale({
			GetAnimInstance()->GetCurveValue(UAlsCameraConstants::PivotOffsetXCurve()),
			GetAnimInstance()->GetCurveValue(UAlsCameraConstants::PivotOffsetYCurve()),
			GetAnimInstance()->GetCurveValue(UAlsCameraConstants::PivotOffsetZCurve())
		})
	};

	PivotLocation = PivotLagLocation + PivotOffset;

#if ENABLE_DRAW_DEBUG
	if (bDisplayDebugCameraShapes)
	{
		DrawDebugLine(GetWorld(), PivotLocation, PivotLagLocation,
		              FLinearColor{0.0f, 0.75f, 1.0f}.ToFColor(true),
		              false, 0.0f, 0, UAlsUtility::DrawLineThickness);

		UAlsUtility::DrawDebugSphereAlternative(GetWorld(), PivotLocation, LagRotation.Rotator(), 16.0f, {0.0f, 0.75f, 1.0f});
	}
#endif

	// Calculate result location. Get the pivot location and apply camera relative offsets.

	auto ResultLocation{
		PivotLocation + CameraRotation.RotateVector({
			GetAnimInstance()->GetCurveValue(UAlsCameraConstants::CameraOffsetXCurve()),
			GetAnimInstance()->GetCurveValue(UAlsCameraConstants::CameraOffsetYCurve()),
			GetAnimInstance()->GetCurveValue(UAlsCameraConstants::CameraOffsetZCurve())
		})
	};

	// Trace for an object between the camera and character to apply a corrective offset.

	static const FName MainTraceTag{FString::Format(TEXT("{0} (Main Trace)"), {ANSI_TO_TCHAR(__FUNCTION__)})};

	auto TraceStart{
		FMath::Lerp(
			AlsCharacter->GetMesh()->GetSocketLocation(bRightShoulder ? ThirdPersonTraceRightSocket : ThirdPersonTraceLeftSocket),
			PivotTargetLocation + PivotOffset + ThirdPersonTraceOverrideOffset,
			UAlsMath::Clamp01(GetAnimInstance()->GetCurveValue(UAlsCameraConstants::TraceOverrideCurve())))
	};

	const auto TraceChanel{UEngineTypes::ConvertToCollisionChannel(ThirdPersonTraceChannel)};
	const auto CollisionShape{FCollisionShape::MakeSphere(ThirdPersonTraceRadius)};

	FHitResult Hit;
	if (GetWorld()->SweepSingleByChannel(Hit, TraceStart, ResultLocation, FQuat::Identity, TraceChanel,
	                                     CollisionShape, {MainTraceTag, false, GetOwner()}))
	{
		if (!Hit.bStartPenetrating)
		{
			ResultLocation = Hit.Location;
		}
		else if (TryFindBlockingGeometryAdjustedLocation(TraceStart, bDisplayDebugCameraTraces))
		{
			static const FName AdjustedTraceTag{FString::Format(TEXT("{0} (Adjusted Trace)"), {ANSI_TO_TCHAR(__FUNCTION__)})};

			GetWorld()->SweepSingleByChannel(Hit, TraceStart, ResultLocation, FQuat::Identity, TraceChanel,
			                                 CollisionShape, {AdjustedTraceTag, false, GetOwner()});
			if (Hit.IsValidBlockingHit())
			{
				ResultLocation = Hit.Location;
			}
		}
	}

#if ENABLE_DRAW_DEBUG
	if (bDisplayDebugCameraTraces)
	{
		UAlsUtility::DrawDebugSweptSphere(GetWorld(), TraceStart, ResultLocation, ThirdPersonTraceRadius,
		                                  Hit.IsValidBlockingHit() ? FLinearColor::Red : FLinearColor::Green);
	}
#endif

	if (FirstPersonOverride <= 0.0f)
	{
		CameraLocation = ResultLocation;
		CameraFov = ThirdPersonFov;
	}
	else
	{
		CameraLocation = FMath::Lerp(ResultLocation, GetFirstPersonPivotLocation(), FirstPersonOverride);
		CameraFov = FMath::Lerp(ThirdPersonFov, FirstPersonFov, FirstPersonOverride);
	}
}

bool UAlsCameraComponent::TryFindBlockingGeometryAdjustedLocation(FVector& Location, const bool bDisplayDebugCameraTraces) const
{
	// Copied with modifications from ComponentEncroachesBlockingGeometry_WithAdjustment

	constexpr auto Epsilon{1.0f};

	const auto TraceChanel{UEngineTypes::ConvertToCollisionChannel(ThirdPersonTraceChannel)};
	const auto CollisionShape{FCollisionShape::MakeSphere(ThirdPersonTraceRadius + Epsilon)};

	static TArray<FOverlapResult> Overlaps;
	check(Overlaps.Num() <= 0)

	static const FName OverlapMultiTraceTag{FString::Format(TEXT("{0} (Overlap Multi)"), {ANSI_TO_TCHAR(__FUNCTION__)})};

	if (!GetWorld()->OverlapMultiByChannel(Overlaps, Location, FQuat::Identity, TraceChanel,
	                                       CollisionShape, {OverlapMultiTraceTag, false, GetOwner()}))
	{
		return false;
	}

	auto Adjustment{FVector::ZeroVector};
	auto bAnyValidBlock{false};

	FMTDResult MtdResult;

	for (const auto& Overlap : Overlaps)
	{
		if (!Overlap.Component.IsValid() || Overlap.Component->GetCollisionResponseToChannel(TraceChanel) != ECR_Block)
		{
			continue;
		}

		if (!Overlap.Component->ComputePenetration(MtdResult, CollisionShape, Location, FQuat::Identity))
		{
			Overlaps.Reset();
			return false;
		}

		if (!FMath::IsNearlyZero(MtdResult.Distance))
		{
			Adjustment += MtdResult.Direction * MtdResult.Distance;
			bAnyValidBlock = true;
		}
	}

	Overlaps.Reset();

	if (!bAnyValidBlock)
	{
		return false;
	}

	auto AdjustmentDirection{Adjustment};
	if (!AdjustmentDirection.Normalize())
	{
		return false;
	}

	if (UAlsMath::AngleBetweenSkipNormalization((GetOwner()->GetActorLocation() - Location).GetSafeNormal(),
	                                            AdjustmentDirection) > 90.0f + 1.0f)
	{
		return false;
	}

#if ENABLE_DRAW_DEBUG
	if (bDisplayDebugCameraTraces)
	{
		DrawDebugLine(GetWorld(), Location, Location + Adjustment,
		              FLinearColor{0.0f, 0.75f, 1.0f}.ToFColor(true),
		              false, 5.0f, 0, UAlsUtility::DrawLineThickness);
	}
#endif

	Location += Adjustment;

	static const FName FreeSpaceTraceTag{FString::Format(TEXT("{0} (Free Space Overlap)"), {ANSI_TO_TCHAR(__FUNCTION__)})};

	return !GetWorld()->OverlapBlockingTestByChannel(Location, FQuat::Identity, TraceChanel,
	                                                 FCollisionShape::MakeSphere(ThirdPersonTraceRadius),
	                                                 {FreeSpaceTraceTag, false, GetOwner()});
}

void UAlsCameraComponent::DisplayDebug(const UCanvas* Canvas, const FDebugDisplayInfo& DebugDisplay, float& VerticalPosition) const
{
	const auto Scale{FMath::Min(Canvas->SizeX / (1280.0f * Canvas->GetDPIScale()), Canvas->SizeY / (720.0f * Canvas->GetDPIScale()))};

	const auto RowOffset{12.0f * Scale};
	const auto ColumnOffset{200.0f * Scale};

	auto MaxVerticalPosition{VerticalPosition};
	auto HorizontalPosition{5.0f * Scale};

	if (!DebugDisplay.IsDisplayOn(UAlsCameraConstants::CameraCurvesDisplayName()) &&
	    !DebugDisplay.IsDisplayOn(UAlsCameraConstants::CameraShapesDisplayName()) &&
	    !DebugDisplay.IsDisplayOn(UAlsCameraConstants::CameraTracesDisplayName()))
	{
		VerticalPosition = MaxVerticalPosition;
		return;
	}

	const auto InitialVerticalPosition{VerticalPosition};

	static const auto CameraCurvesHeaderText{FText::AsCultureInvariant(TEXT("ALS.CameraCurves (Shift + 6)"))};

	if (DebugDisplay.IsDisplayOn(UAlsCameraConstants::CameraCurvesDisplayName()))
	{
		DisplayDebugHeader(Canvas, CameraCurvesHeaderText, FLinearColor::Green, Scale, HorizontalPosition, VerticalPosition);
		DisplayDebugCurves(Canvas, Scale, HorizontalPosition, VerticalPosition);

		MaxVerticalPosition = FMath::Max(MaxVerticalPosition, VerticalPosition + RowOffset);
		VerticalPosition = InitialVerticalPosition;
		HorizontalPosition += ColumnOffset;
	}
	else
	{
		DisplayDebugHeader(Canvas, CameraCurvesHeaderText, {0.0f, 0.333333f, 0.0f}, Scale, HorizontalPosition, VerticalPosition);

		VerticalPosition += RowOffset;
	}

	MaxVerticalPosition = FMath::Max(MaxVerticalPosition, VerticalPosition);

	static const auto ShapesHeaderText{FText::AsCultureInvariant(TEXT("ALS.CameraShapes (Shift + 7)"))};

	if (DebugDisplay.IsDisplayOn(UAlsCameraConstants::CameraShapesDisplayName()))
	{
		DisplayDebugHeader(Canvas, ShapesHeaderText, FLinearColor::Green, Scale, HorizontalPosition, VerticalPosition);
		DisplayDebugShapes(Canvas, Scale, HorizontalPosition, VerticalPosition);
	}
	else
	{
		DisplayDebugHeader(Canvas, ShapesHeaderText, {0.0f, 0.333333f, 0.0f}, Scale, HorizontalPosition, VerticalPosition);
	}

	VerticalPosition += RowOffset;
	MaxVerticalPosition = FMath::Max(MaxVerticalPosition, VerticalPosition);

	static const auto TracesHeaderText{FText::AsCultureInvariant(TEXT("ALS.CameraTraces (Shift + 8)"))};

	if (DebugDisplay.IsDisplayOn(UAlsCameraConstants::CameraTracesDisplayName()))
	{
		DisplayDebugHeader(Canvas, TracesHeaderText, FLinearColor::Green, Scale, HorizontalPosition, VerticalPosition);
		DisplayDebugTraces(Canvas, Scale, HorizontalPosition, VerticalPosition);
	}
	else
	{
		DisplayDebugHeader(Canvas, TracesHeaderText, {0.0f, 0.333333f, 0.0f}, Scale, HorizontalPosition, VerticalPosition);
	}

	VerticalPosition += RowOffset;
	MaxVerticalPosition = FMath::Max(MaxVerticalPosition, VerticalPosition);

	VerticalPosition = MaxVerticalPosition;
}

void UAlsCameraComponent::DisplayDebugHeader(const UCanvas* Canvas, const FText& HeaderText, const FLinearColor& HeaderColor,
                                             const float Scale, const float HorizontalPosition, float& VerticalPosition)
{
	FCanvasTextItem Text{
		{HorizontalPosition, VerticalPosition},
		HeaderText,
		GEngine->GetMediumFont(),
		HeaderColor
	};

	Text.Scale = {Scale, Scale};
	Text.EnableShadow(FLinearColor::Black);

	Text.Draw(Canvas->Canvas);

	VerticalPosition += 15.0f * Scale;
}

void UAlsCameraComponent::DisplayDebugCurves(const UCanvas* Canvas, const float Scale,
                                             const float HorizontalPosition, float& VerticalPosition) const
{
	VerticalPosition += 4.0f * Scale;

	FCanvasTextItem Text{
		FVector2D::ZeroVector,
		FText::GetEmpty(),
		GEngine->GetMediumFont(),
		FLinearColor::White
	};

	Text.Scale = {Scale * 0.75f, Scale * 0.75f};
	Text.EnableShadow(FLinearColor::Black);

	const auto RowOffset{12.0f * Scale};
	const auto ColumnOffset{145.0f * Scale};

	static TArray<FName> CurveNames;
	check(CurveNames.Num() <= 0)

	GetAnimInstance()->GetAllCurveNames(CurveNames);

	CurveNames.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const auto& CurveName : CurveNames)
	{
		const auto CurveValue{GetAnimInstance()->GetCurveValue(CurveName)};

		Text.SetColor(FMath::Lerp(FLinearColor::Gray, FLinearColor::White, UAlsMath::Clamp01(FMath::Abs(CurveValue))));

		Text.Text = FText::AsCultureInvariant(FName::NameToDisplayString(CurveName.ToString(), false));
		Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

		Text.Text = FText::AsCultureInvariant(FString::Printf(TEXT("%.2f"), CurveValue));
		Text.Draw(Canvas->Canvas, {HorizontalPosition + ColumnOffset, VerticalPosition});

		VerticalPosition += RowOffset;
	}

	CurveNames.Reset();
}

void UAlsCameraComponent::DisplayDebugShapes(const UCanvas* Canvas, const float Scale,
                                             const float HorizontalPosition, float& VerticalPosition) const
{
	VerticalPosition += 4.0f * Scale;

	FCanvasTextItem Text{
		FVector2D::ZeroVector,
		FText::GetEmpty(),
		GEngine->GetMediumFont(),
		FLinearColor::White
	};

	Text.Scale = {Scale * 0.75f, Scale * 0.75f};
	Text.EnableShadow(FLinearColor::Black);

	const auto RowOffset{12.0f * Scale};
	const auto ColumnOffset{120.0f * Scale};

	static const auto PivotTargetLocationText{
		FText::AsCultureInvariant(FName::NameToDisplayString(GET_MEMBER_NAME_STRING_CHECKED(ThisClass, PivotTargetLocation), false))
	};

	auto Color{FLinearColor::Green};
	Text.SetColor(Color);

	Text.Text = PivotTargetLocationText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	Text.Text = FText::AsCultureInvariant(FString::Printf(TEXT("X: %.2f Y: %.2f Z: %.2f"),
	                                                      PivotTargetLocation.X, PivotTargetLocation.Y, PivotTargetLocation.Z));
	Text.Draw(Canvas->Canvas, {HorizontalPosition + ColumnOffset, VerticalPosition});

	VerticalPosition += RowOffset;

	static const auto PivotLagLocationText{
		FText::AsCultureInvariant(FName::NameToDisplayString(GET_MEMBER_NAME_STRING_CHECKED(ThisClass, PivotLagLocation), false))
	};

	Color = {1.0f, 0.5f, 0.0f};
	Text.SetColor(Color);

	Text.Text = PivotLagLocationText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	Text.Text = FText::AsCultureInvariant(FString::Printf(TEXT("X: %.2f Y: %.2f Z: %.2f"),
	                                                      PivotLagLocation.X, PivotLagLocation.Y, PivotLagLocation.Z));
	Text.Draw(Canvas->Canvas, {HorizontalPosition + ColumnOffset, VerticalPosition});

	VerticalPosition += RowOffset;

	static const auto PivotLocationText{
		FText::AsCultureInvariant(FName::NameToDisplayString(GET_MEMBER_NAME_STRING_CHECKED(ThisClass, PivotLocation), false))
	};

	Color = {0.0f, 0.75f, 1.0f};
	Text.SetColor(Color);

	Text.Text = PivotLocationText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	Text.Text = FText::AsCultureInvariant(FString::Printf(TEXT("X: %.2f Y: %.2f Z: %.2f"),
	                                                      PivotLocation.X, PivotLocation.Y, PivotLocation.Z));
	Text.Draw(Canvas->Canvas, {HorizontalPosition + ColumnOffset, VerticalPosition});

	VerticalPosition += RowOffset;

	static const auto CameraFovText{
		FText::AsCultureInvariant(FName::NameToDisplayString(GET_MEMBER_NAME_STRING_CHECKED(ThisClass, CameraFov), false))
	};

	Color = FLinearColor::White;
	Text.SetColor(Color);

	Text.Text = CameraFovText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	Text.Text = FText::AsCultureInvariant(FString::Printf(TEXT("%.2f"), CameraFov));
	Text.Draw(Canvas->Canvas, {HorizontalPosition + ColumnOffset, VerticalPosition});

	VerticalPosition += RowOffset;

	static const auto RightShoulderText{
		FText::AsCultureInvariant(FName::NameToDisplayString(GET_MEMBER_NAME_STRING_CHECKED(ThisClass, bRightShoulder), true))
	};

	Text.Text = RightShoulderText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	Text.Text = FText::AsCultureInvariant(FName::NameToDisplayString(ToString(bRightShoulder), false));
	Text.Draw(Canvas->Canvas, {HorizontalPosition + ColumnOffset, VerticalPosition});

	VerticalPosition += RowOffset;
}

void UAlsCameraComponent::DisplayDebugTraces(const UCanvas* Canvas, const float Scale,
                                             const float HorizontalPosition, float& VerticalPosition) const
{
	VerticalPosition += 4.0f * Scale;

	FCanvasTextItem Text{
		FVector2D::ZeroVector,
		FText::GetEmpty(),
		GEngine->GetMediumFont(),
		FLinearColor::White
	};

	Text.Scale = {Scale * 0.75f, Scale * 0.75f};
	Text.EnableShadow(FLinearColor::Black);

	const auto RowOffset{12.0f * Scale};

	static const auto BlockingGeometryAdjustmentText{FText::AsCultureInvariant(TEXT("Blocking Geometry Adjustment"))};

	Text.SetColor({0.0f, 0.75f, 1.0f});

	Text.Text = BlockingGeometryAdjustmentText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	VerticalPosition += RowOffset;

	static const auto CameraTraceNoHitText{FText::AsCultureInvariant(TEXT("Camera Trace (No Hit)"))};

	Text.SetColor(FLinearColor::Green);

	Text.Text = CameraTraceNoHitText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	VerticalPosition += RowOffset;

	static const auto CameraTraceBlockingHitText{FText::AsCultureInvariant(TEXT("Camera Trace (Blocking Hit)"))};

	Text.SetColor(FLinearColor::Red);

	Text.Text = CameraTraceBlockingHitText;
	Text.Draw(Canvas->Canvas, {HorizontalPosition, VerticalPosition});

	VerticalPosition += RowOffset;
}