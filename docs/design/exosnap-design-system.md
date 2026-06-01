# ExoSnap Design System (Qt Foundation)

## Scope
UX-SYSTEM-R1 ports shared visual primitives from the Claude v2 design system into the Qt Widgets theme layer. This slice updates tokens and common control primitives only; it does not redesign pages or change recording/capture behavior.

## Core tokens

### Color tokens (resolved)

| Role | Value | Qt token |
|---|---|---|
| App background | `#0E0C0A` | `${bg0}` |
| Elevated background | `#141210` | `${bg1}` |
| Surface/card | `#191714` | `${bg2}` |
| Control surface | `#211E1B` | `${bg3}` |
| Hover surface | `#272420` | `${bg4}` |
| Border soft | `#272421` | `${line1}` |
| Border default | `#322E2B` | `${line2}` |
| Border strong | `#4B4741` | `${line3}` |
| Primary text | `#EDE9E2` | `${text0}` |
| Secondary text | `#AEA8A0` | `${text1}` |
| Muted text | `#7A756E` | `${text2}` |
| Faint text | `#54504B` | `${text3}` |
| Amber emphasis | `#E9B361` | `${accent}` |
| Green ready/pass | `#78DA95` | `${ok}` |
| Red danger/error | `#F05B54` | `${err}` |
| Warning amber | `#C29653` | `${warn}` |

### Size and shape tokens

| Role | Value |
|---|---|
| Control radius (`kRadiusMd`) | `9px` |
| Card radius (`kRadiusLg`) | `12px` |
| Pill radius (`kRadiusSm`) | `7px` |
| Control height (`kControlHeight`) | `36px` |
| Primary CTA height (`kPrimaryCtaHeight`) | `44px` |

## Component states
- Hover: stronger border + hover surface.
- Focus: visible amber border for keyboard navigation.
- Disabled: reduced contrast, no active tints.
- Status semantics:
  - green for ready/pass/safe
  - amber for selected/emphasis/warning
  - red for stop/error/blocked

## Qt implementation notes
- Theme resources:
  - `apps/exosnap/ui/theme/exosnap_dark.qss`
  - `apps/exosnap/ui/theme/icons/chevron-down.svg`
  - `apps/exosnap/ui/theme/exosnap_theme.qrc`
- Token sources:
  - `apps/exosnap/ui/theme/ExoSnapPalette.h`
  - `apps/exosnap/ui/theme/ExoSnapMetrics.h`
- State wiring for themed primitives:
  - `apps/exosnap/ui/widgets/StatusPill.cpp`
  - `apps/exosnap/ui/chrome/GlobalRecordingBar.cpp`
  - `apps/exosnap/ui/dialogs/SourcePickerDialog.cpp`
  - `apps/exosnap/ui/widgets/CaptureTargetCard.cpp`

## Typography
- Sans role: Inter (bundled) retained for this slice.
- Mono role: JetBrains Mono (bundled).
- Hanken Grotesk from prototype is not bundled in app resources in this slice.
