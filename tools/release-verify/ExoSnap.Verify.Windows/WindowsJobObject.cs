using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.System.JobObjects;

namespace ExoSnap.Verify.Windows;

/// <summary>
/// A Windows job object that terminates every process assigned to it when the
/// job handle closes.
/// </summary>
/// <remarks>
/// A gate that is cancelled or times out must not leave an updater, a probe or a
/// media tool running: the next scenario would then measure a machine the
/// previous one still owns. Assignment happens immediately after the child is
/// started, so a grandchild spawned in the first instants of the child's life can
/// still escape; nothing in the supported Win32 surface closes that window for a
/// process started through <see cref="System.Diagnostics.Process"/>.
/// </remarks>
public sealed class WindowsJobObject : IDisposable
{
    private readonly SafeFileHandle handle;
    private bool disposed;

    private WindowsJobObject(SafeFileHandle handle) => this.handle = handle;

    /// <summary>
    /// Creates an unnamed job object with kill-on-close, or returns null when the
    /// platform refuses. A null result is never a product verdict: the caller
    /// keeps running and records that child containment is unavailable.
    /// </summary>
    public static WindowsJobObject? TryCreate()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        SafeFileHandle? job = null;
        try
        {
            job = PInvoke.CreateJobObject(null, null);
            if (job.IsInvalid)
            {
                job.Dispose();
                return null;
            }

            var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            var bytes = new byte[Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>()];
            MemoryMarshal.Write(bytes, in limits);

            if (!PInvoke.SetInformationJobObject(
                    job,
                    JOBOBJECTINFOCLASS.JobObjectExtendedLimitInformation,
                    bytes))
            {
                job.Dispose();
                return null;
            }

            var created = new WindowsJobObject(job);
            job = null;
            return created;
        }
        catch (DllNotFoundException)
        {
            job?.Dispose();
            return null;
        }
        catch (EntryPointNotFoundException)
        {
            job?.Dispose();
            return null;
        }
    }

    /// <summary>
    /// Assigns a running process to the job. Returns false when the process has
    /// already exited or already belongs to an incompatible job.
    /// </summary>
    public bool TryAssign(SafeHandle processHandle)
    {
        ArgumentNullException.ThrowIfNull(processHandle);
        ObjectDisposedException.ThrowIf(this.disposed, this);
        return PInvoke.AssignProcessToJobObject(this.handle, processHandle);
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        if (this.disposed)
        {
            return;
        }

        this.disposed = true;
        this.handle.Dispose();
    }
}
