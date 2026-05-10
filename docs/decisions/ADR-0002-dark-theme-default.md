# ADR-0002: Dark Theme by Default

## Status
Accepted

## Decision
Use dark mode as the default application theme.

## Rationale
The product is recording-oriented, frequently used beside games or dark desktop environments, and WinUI 3 supports native dark theme resources without requiring custom styling work.

## Consequences
- App requests dark theme by default
- Light/system-following theme may be added later
- Do not create a custom visual-design subproject for the MVP
