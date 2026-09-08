using System.Collections.ObjectModel;
using System.Globalization;
using System.Runtime.InteropServices;
using Windows.Win32;
using Windows.Win32.Graphics.Direct3D;
using Windows.Win32.Graphics.Direct3D11;
using Windows.Win32.Graphics.Dxgi;
using Windows.Win32.Graphics.Dxgi.Common;

namespace ExoSnap.Verify.Windows;

/// <summary>One display output as DXGI describes it.</summary>
/// <param name="DeviceName">The GDI device name, stable across a session.</param>
/// <param name="AttachedToDesktop">Whether the output currently forms part of the desktop.</param>
/// <param name="BitsPerColor">Bits per colour channel the output is scanning out.</param>
/// <param name="IsHdr">Whether the output is presenting in an HDR colour space right now.</param>
public sealed record DisplayOutputInfo(string DeviceName, bool AttachedToDesktop, uint BitsPerColor, bool IsHdr);

/// <summary>One graphics adapter as DXGI describes it.</summary>
/// <param name="Description">The adapter name reported by the driver.</param>
/// <param name="VendorId">The PCI vendor id.</param>
/// <param name="Vendor">The vendor name derived from the vendor id, or the raw id.</param>
/// <param name="IsSoftware">Whether this is a Microsoft software adapter rather than hardware.</param>
/// <param name="Outputs">The outputs attached to this adapter.</param>
public sealed record GraphicsAdapterInfo(
    string Description,
    uint VendorId,
    string Vendor,
    bool IsSoftware,
    ReadOnlyCollection<DisplayOutputInfo> Outputs);

/// <summary>
/// Read-only DXGI and Direct3D 11 enumeration.
/// </summary>
/// <remarks>
/// HDR is reported as the colour space an output is presenting in at this
/// instant, not as a capability the panel might have. A scenario that requires
/// HDR needs the desktop to actually be in HDR, so a panel that could be switched
/// but has not been correctly fails to satisfy the requirement.
/// </remarks>
public static class GraphicsProbe
{
    private const uint VendorNvidia = 0x10DE;
    private const uint VendorAmd = 0x1002;
    private const uint VendorIntel = 0x8086;
    private const uint VendorMicrosoft = 0x1414;

    // DXGI's sentinel for "no adapter or output at this index".
    private const int DxgiErrorNotFound = unchecked((int)0x887A0002);

    /// <summary>
    /// Enumerates the adapters and their outputs, or returns null when DXGI is
    /// not reachable at all.
    /// </summary>
    public static ReadOnlyCollection<GraphicsAdapterInfo>? TryEnumerateAdapters()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        IDXGIFactory1? factory = null;
        try
        {
            if (PInvoke.CreateDXGIFactory1(out factory).Failed || factory is null)
            {
                return null;
            }

            var adapters = new List<GraphicsAdapterInfo>();
            for (uint index = 0; ; index++)
            {
                var hr = factory.EnumAdapters1(index, out IDXGIAdapter1 adapter);
                if (hr.Value == DxgiErrorNotFound || hr.Failed || adapter is null)
                {
                    break;
                }

                try
                {
                    adapters.Add(Describe(adapter));
                }
                finally
                {
                    Marshal.ReleaseComObject(adapter);
                }
            }

            return new ReadOnlyCollection<GraphicsAdapterInfo>(adapters);
        }
        catch (COMException)
        {
            return null;
        }
        catch (DllNotFoundException)
        {
            return null;
        }
        finally
        {
            if (factory is not null)
            {
                Marshal.ReleaseComObject(factory);
            }
        }
    }

    /// <summary>
    /// Whether a hardware Direct3D 11 device can be created, or null when the
    /// question could not be answered. The device is released immediately; this
    /// is a capability question, not a rendering path.
    /// </summary>
    public static bool? TryCreateHardwareDevice()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        ID3D11Device? device = null;
        ID3D11DeviceContext? context = null;
        try
        {
            var hr = PInvoke.D3D11CreateDevice(
                null,
                D3D_DRIVER_TYPE.D3D_DRIVER_TYPE_HARDWARE,
                default,
                D3D11_CREATE_DEVICE_FLAG.D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                null,
                PInvoke.D3D11_SDK_VERSION,
                out device,
                out _,
                out context);
            return hr.Succeeded;
        }
        catch (COMException)
        {
            return null;
        }
        catch (DllNotFoundException)
        {
            return null;
        }
        finally
        {
            if (context is not null)
            {
                Marshal.ReleaseComObject(context);
            }

            if (device is not null)
            {
                Marshal.ReleaseComObject(device);
            }
        }
    }

    private static GraphicsAdapterInfo Describe(IDXGIAdapter1 adapter)
    {
        var desc = adapter.GetDesc1();
        var outputs = new List<DisplayOutputInfo>();

        for (uint index = 0; ; index++)
        {
            var hr = adapter.EnumOutputs(index, out IDXGIOutput output);
            if (hr.Value == DxgiErrorNotFound || hr.Failed || output is null)
            {
                break;
            }

            try
            {
                // IDXGIOutput6 carries the colour space, and it exists only from
                // Windows 10 onwards. An output whose colour space cannot be
                // read is left out entirely rather than reported as SDR.
                if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 10240) && output is IDXGIOutput6 output6)
                {
                    var outputDesc = output6.GetDesc1();
                    outputs.Add(new DisplayOutputInfo(
                        outputDesc.DeviceName.ToString(),
                        outputDesc.AttachedToDesktop,
                        outputDesc.BitsPerColor,
                        outputDesc.ColorSpace == DXGI_COLOR_SPACE_TYPE.DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
                }
            }
            finally
            {
                Marshal.ReleaseComObject(output);
            }
        }

        var vendorId = desc.VendorId;
        return new GraphicsAdapterInfo(
            desc.Description.ToString(),
            vendorId,
            VendorName(vendorId),
            vendorId == VendorMicrosoft,
            new ReadOnlyCollection<DisplayOutputInfo>(outputs));
    }

    private static string VendorName(uint vendorId) => vendorId switch
    {
        VendorNvidia => "NVIDIA",
        VendorAmd => "AMD",
        VendorIntel => "Intel",
        VendorMicrosoft => "Microsoft",
        _ => "0x" + vendorId.ToString("X4", CultureInfo.InvariantCulture),
    };
}
