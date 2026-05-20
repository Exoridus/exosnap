# ExoSnap Visual Direction

ExoSnap UI is a dark, technical recording instrument. It should feel precise, calm, and operational, not like a generic SaaS dashboard.

## Farbcharakter

- Base: very dark near-black with warm undertone (`bg0`..`bg4`)
- Structure: low-contrast warm borders (`line1`..`line3`) instead of shadows/glass
- Text: clear hierarchy from high contrast (`text0`) to low emphasis (`text3`)
- Accent: amber (`accent`) for action/state signal only
- Status colors: `ok`, `warn`, `err`, `info` for semantic system state

## Typography Character

- Default UI font: clean, neutral sans for control readability
- Operational metrics/states: mono/technical styling where values are scanned
- Headings are restrained: clarity over decorative display typography

## Surfaces / Borders

- Surfaces are flat and layered by luminance steps, not gradients or blur
- Panels use explicit 1px borders and compact radius
- Primary information areas remain dense but grid-aligned and readable

## Amber Accent Usage Rules

- Use amber for:
  - Primary CTA
  - Recording state signal
  - Active navigation indicator
  - Keyboard focus emphasis
- Do not use amber for:
  - General heading color
  - Decorative icon tinting
  - Random highlight accents

## UI Do / Don’t

- Do: keep visual hierarchy stable across all pages
- Do: prefer reusable theme tokens/QSS roles over inline per-widget coloring
- Do: preserve functional clarity first (states, blockers, runtime values)
- Don’t: reintroduce cool blue default-dark styling
- Don’t: add heavy shadow, glassmorphism, or ornamental effects
- Don’t: silently expand page scope during foundation passes

## Scope Guard

Alte Logo-/Brand-Ideationen werden in diesem Pfad nicht wieder geöffnet. Dieses Dokument ist eine UI-Leitplanke für die bestehende ExoSnap-Richtung, keine Logo- oder Brand-Neuentwicklung.
