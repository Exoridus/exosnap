#pragma once

// CanonicalMachine.h -- one healthy machine every Diagnostics capture starts from.
//
// The Diagnostics page describes the machine it is running on, and a harness run
// has none: the capability probe has not landed, no adapter has been enumerated,
// and the selected codecs therefore carry no support annotation. Every capture
// consequently opened with "3 things to fix before recording" -- a missing
// annotation and two unavailable codecs -- which is a statement about the fixture
// and not about the product.
//
// These functions are that missing machine, written down: a discrete NVIDIA part
// with NVENC and all three codecs, so a scenario only has to state the ONE thing
// it is about. A scenario that wants a defect adds it on top (MP4 + FLAC stays a
// real blocker); a scenario that wants a healthy page gets one.
//
// The capability set is built by the product's own CapabilityBuilder from a
// synthetic runtime snapshot, so the annotations a capture shows are the ones the
// real derivation produces rather than a second, hand-written truth.
//
// Pure and harness-only. Nothing in a shipping code path calls it.

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>
#include <capability/capability_set.h>
#include <capability/runtime_snapshot.h>

#include <vector>

namespace exosnap::visual {

// The probe result a real RTX 5070 Ti would produce: NVENC present, H.264, HEVC
// and AV1 all encodable, 4:4:4 on the two codecs that have it, B-frames and the
// lookahead/temporal-AQ pair advertised, and the WDDM driver version that renders
// as the vendor's own "581.29".
[[nodiscard]] capability::RuntimeCapabilitySnapshot CanonicalMachineRuntimeSnapshot();

// The effective capability set derived from that snapshot, marked probed so every
// consumer treats it as a completed hardware answer. This is what makes the
// selected video and audio codecs carry an annotation, which is what silences
// rec.003, rec.004 and the missing-annotation blocker.
[[nodiscard]] capability::CapabilitySet CanonicalMachineCapabilities();

// The enumerated adapter list, for DeviceAdapter::setAdaptersForTest. One
// discrete adapter: the Hardware capabilities row summarises it instead of
// reporting "Not scanned yet", and the Device page renders the same machine the
// Diagnostics page describes.
[[nodiscard]] std::vector<capability::AdapterInfo> CanonicalMachineAdapters();
[[nodiscard]] std::vector<capability::AdapterEncoderCapability> CanonicalMachineAdapterCapabilities();

} // namespace exosnap::visual
