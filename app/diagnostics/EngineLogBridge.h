#pragma once

namespace exosnap {

// Brings up the engine's logger and mirrors its records into AppLog.
//
// Without this the engine's logger stays uninitialised and every log() call returns
// early, so the capture path's decisions — tone-map vs native HDR, scRGB detection,
// foreign surface formats — are invisible to the user and to support.
//
// Call once after AppLog::init(). Safe to call again; the engine's initialize() resets.
void InitializeEngineLogging();

// Flushes and detaches the sink. Call before the application object goes away, so a
// late engine record cannot reach a half-destroyed AppLog.
void ShutdownEngineLogging();

} // namespace exosnap
