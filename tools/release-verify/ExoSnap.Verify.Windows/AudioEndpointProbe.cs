using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.Media.Audio;
using Windows.Win32.System.Com;
using Windows.Win32.System.Variant;
using Windows.Win32.UI.Shell.PropertiesSystem;

namespace ExoSnap.Verify.Windows;

/// <summary>One active audio endpoint and the format the audio engine runs it at.</summary>
/// <param name="EndpointId">The stable Windows endpoint id, never the friendly name.</param>
/// <param name="DataFlow">Whether the endpoint renders or captures.</param>
/// <param name="SampleRateHz">The shared-mode sample rate, or null when it could not be read.</param>
/// <param name="Channels">The shared-mode channel count, or null when it could not be read.</param>
public sealed record AudioEndpointInfo(
    string EndpointId,
    AudioDataFlow DataFlow,
    uint? SampleRateHz,
    ushort? Channels);

/// <summary>Which direction an audio endpoint moves samples in.</summary>
public enum AudioDataFlow
{
    /// <summary>An output endpoint.</summary>
    Render,

    /// <summary>An input endpoint.</summary>
    Capture,
}

/// <summary>
/// Read-only enumeration of the active WASAPI endpoints.
/// </summary>
/// <remarks>
/// Endpoints are identified by their Windows endpoint id and never by their
/// friendly name: two identical devices share one name, and a name changes when a
/// driver is reinstalled, so a scenario keyed on a name is a scenario that
/// silently starts measuring a different device.
/// </remarks>
public static class AudioEndpointProbe
{
    /// <summary>
    /// The active render and capture endpoints, or null when the audio service
    /// could not be reached.
    /// </summary>
    public static ReadOnlyCollection<AudioEndpointInfo>? TryEnumerateActiveEndpoints()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        object? enumeratorObject = null;
        try
        {
            enumeratorObject = new MMDeviceEnumerator();
            if (enumeratorObject is not IMMDeviceEnumerator enumerator)
            {
                return null;
            }

            var endpoints = new List<AudioEndpointInfo>();
            Collect(enumerator, EDataFlow.eRender, AudioDataFlow.Render, endpoints);
            Collect(enumerator, EDataFlow.eCapture, AudioDataFlow.Capture, endpoints);
            return new ReadOnlyCollection<AudioEndpointInfo>(endpoints);
        }
        catch (COMException)
        {
            return null;
        }
        catch (InvalidCastException)
        {
            return null;
        }
        catch (PlatformNotSupportedException)
        {
            return null;
        }
        finally
        {
            if (enumeratorObject is not null)
            {
                Marshal.ReleaseComObject(enumeratorObject);
            }
        }
    }

    private static void Collect(
        IMMDeviceEnumerator enumerator,
        EDataFlow flow,
        AudioDataFlow direction,
        List<AudioEndpointInfo> into)
    {
        enumerator.EnumAudioEndpoints(flow, DEVICE_STATE.DEVICE_STATE_ACTIVE, out IMMDeviceCollection collection);
        if (collection is null)
        {
            return;
        }

        try
        {
            collection.GetCount(out uint count);
            for (uint index = 0; index < count; index++)
            {
                collection.Item(index, out IMMDevice device);
                if (device is null)
                {
                    continue;
                }

                try
                {
                    into.Add(Describe(device, direction));
                }
                finally
                {
                    Marshal.ReleaseComObject(device);
                }
            }
        }
        finally
        {
            Marshal.ReleaseComObject(collection);
        }
    }

    private static unsafe AudioEndpointInfo Describe(IMMDevice device, AudioDataFlow direction)
    {
        string endpointId;
        PWSTR id = default;
        try
        {
            device.GetId(&id);
            endpointId = id.ToString();
        }
        finally
        {
            if (id.Value is not null)
            {
                Marshal.FreeCoTaskMem((nint)id.Value);
            }
        }

        uint? sampleRate = null;
        ushort? channels = null;

        device.OpenPropertyStore(STGM.STGM_READ, out IPropertyStore store);
        if (store is not null)
        {
            try
            {
                var key = PInvoke.PKEY_AudioEngine_DeviceFormat;
                store.GetValue(&key, out var value);
                try
                {
                    // The device format arrives as a raw WAVEFORMATEX blob; a
                    // property store that holds anything else here is a driver
                    // reporting something this probe must not guess at.
                    if (value.Anonymous.Anonymous.vt == VARENUM.VT_BLOB)
                    {
                        var blob = value.Anonymous.Anonymous.Anonymous.blob;
                        if (blob.cbSize >= (uint)sizeof(WAVEFORMATEX) && blob.pBlobData is not null)
                        {
                            var format = *(WAVEFORMATEX*)blob.pBlobData;
                            sampleRate = format.nSamplesPerSec;
                            channels = format.nChannels;
                        }
                    }
                }
                finally
                {
                    PInvoke.PropVariantClear(ref value);
                }
            }
            catch (COMException)
            {
                // Left as null: an endpoint whose format cannot be read is
                // reported as unknown, never as a rate it does not have.
            }
            finally
            {
                Marshal.ReleaseComObject(store);
            }
        }

        return new AudioEndpointInfo(endpointId, direction, sampleRate, channels);
    }
}
