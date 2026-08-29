#requires -Version 7.0

<#
.SYNOPSIS
    Bounded synthetic machine load for measurements that need a busy machine.

.DESCRIPTION
    Some checks are only meaningful under contention: whether capture keeps its
    cadence while the machine is saturated, whether a metric survives a loaded
    encoder. Generating that load naively leaks processes, and a leaked CPU
    burner is not a harmless leftover -- it silently invalidates every later
    measurement on the machine and can hold gigabytes.

    Three independent guarantees, so no single failure leaves a worker behind:

      1. Every worker carries its own wall-clock deadline and exits on its own.
      2. The caller's `finally` terminates the job.
      3. The workers live in a Win32 job object created with
         JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so the kernel kills the whole tree
         the moment this process goes away -- including a hard kill, a closed
         terminal, or a crash, where no `finally` runs.

    Workers are created suspended and assigned to the job before their first
    instruction. Assigning after launch would leave a race the everyday case
    loses: a launcher that spawns the real worker as a child (a Chocolatey shim
    for ffmpeg is one) can fork before the assignment lands, and the grandchild
    then belongs to no job. The shim also makes the launcher look idle -- it
    reports near-zero CPU while its child saturates a core -- which is why
    Start-SyntheticLoad verifies measured CPU instead of trusting that a started
    process is a working process.

    The job additionally caps total committed memory and process count, so a
    misbehaving worker cannot take the machine down with it.

.EXAMPLE
    Invoke-WithSyntheticLoad -MaxSeconds 360 -CpuWorkers 16 -ScriptBlock {
        pwsh ./probe.ps1 -Minutes 5
    }
#>

Set-StrictMode -Version Latest

# Module scope, so every function here fails loudly. A load generator that
# reports a problem as a warning and continues is the failure mode this module
# exists to remove: the caller would measure against load that is not running.
$ErrorActionPreference = 'Stop'

if (-not ('ExoSnap.SyntheticLoad.Native' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace ExoSnap.SyntheticLoad {

[StructLayout(LayoutKind.Sequential)]
public struct JobBasicLimitInformation {
    public long PerProcessUserTimeLimit;
    public long PerJobUserTimeLimit;
    public uint LimitFlags;
    public UIntPtr MinimumWorkingSetSize;
    public UIntPtr MaximumWorkingSetSize;
    public uint ActiveProcessLimit;
    public UIntPtr Affinity;
    public uint PriorityClass;
    public uint SchedulingClass;
}

[StructLayout(LayoutKind.Sequential)]
public struct IoCounters {
    public ulong ReadOperationCount;
    public ulong WriteOperationCount;
    public ulong OtherOperationCount;
    public ulong ReadTransferCount;
    public ulong WriteTransferCount;
    public ulong OtherTransferCount;
}

[StructLayout(LayoutKind.Sequential)]
public struct JobExtendedLimitInformation {
    public JobBasicLimitInformation BasicLimitInformation;
    public IoCounters IoInfo;
    public UIntPtr ProcessMemoryLimit;
    public UIntPtr JobMemoryLimit;
    public UIntPtr PeakProcessMemoryUsed;
    public UIntPtr PeakJobMemoryUsed;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct StartupInfo {
    public int cb;
    public string lpReserved;
    public string lpDesktop;
    public string lpTitle;
    public int dwX;
    public int dwY;
    public int dwXSize;
    public int dwYSize;
    public int dwXCountChars;
    public int dwYCountChars;
    public int dwFillAttribute;
    public int dwFlags;
    public short wShowWindow;
    public short cbReserved2;
    public IntPtr lpReserved2;
    public IntPtr hStdInput;
    public IntPtr hStdOutput;
    public IntPtr hStdError;
}

[StructLayout(LayoutKind.Sequential)]
public struct ProcessInformation {
    public IntPtr hProcess;
    public IntPtr hThread;
    public int dwProcessId;
    public int dwThreadId;
}

public static class Native {
    public const uint LimitJobMemory = 0x00000200;
    public const uint LimitActiveProcess = 0x00000008;
    public const uint LimitKillOnJobClose = 0x00002000;
    public const int ExtendedLimitInformation = 9;
    public const uint CreateSuspended = 0x00000004;
    public const uint CreateNoWindow = 0x08000000;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateJobObjectW(IntPtr securityAttributes, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint length);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool TerminateJobObject(IntPtr job, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CreateProcessW(
        string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
        uint creationFlags, IntPtr environment, string currentDirectory,
        ref StartupInfo startupInfo, out ProcessInformation processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint ResumeThread(IntPtr thread);

    // Creates the process suspended, assigns it to the job, and only then lets it
    // run. Anything it spawns afterwards inherits the job, which is the whole
    // point: a launcher that forks the real worker cannot escape.
    public static int StartInJob(IntPtr job, string executable, string commandLine) {
        StartupInfo startup = new StartupInfo();
        startup.cb = Marshal.SizeOf(typeof(StartupInfo));
        ProcessInformation info;
        StringBuilder mutable = new StringBuilder(commandLine);
        if (!CreateProcessW(executable, mutable, IntPtr.Zero, IntPtr.Zero, false,
                            CreateSuspended | CreateNoWindow, IntPtr.Zero, null,
                            ref startup, out info)) {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),
                "could not create the load worker process");
        }
        try {
            if (!AssignProcessToJobObject(job, info.hProcess)) {
                int error = Marshal.GetLastWin32Error();
                TerminateJobObject(job, 1);
                throw new System.ComponentModel.Win32Exception(error,
                    "could not assign the load worker to the job object");
            }
            if (ResumeThread(info.hThread) == uint.MaxValue) {
                int error = Marshal.GetLastWin32Error();
                TerminateJobObject(job, 1);
                throw new System.ComponentModel.Win32Exception(error,
                    "could not resume the load worker");
            }
            return info.dwProcessId;
        }
        finally {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
    }

    public static IntPtr CreateBoundedJob(uint activeProcessLimit, ulong memoryLimitBytes) {
        IntPtr job = CreateJobObjectW(IntPtr.Zero, null);
        if (job == IntPtr.Zero) {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),
                "could not create the job object");
        }
        JobExtendedLimitInformation limits = new JobExtendedLimitInformation();
        limits.BasicLimitInformation.LimitFlags =
            LimitKillOnJobClose | LimitActiveProcess | LimitJobMemory;
        limits.BasicLimitInformation.ActiveProcessLimit = activeProcessLimit;
        limits.JobMemoryLimit = (UIntPtr)memoryLimitBytes;

        int size = Marshal.SizeOf(typeof(JobExtendedLimitInformation));
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try {
            Marshal.StructureToPtr(limits, buffer, false);
            if (!SetInformationJobObject(job, ExtendedLimitInformation, buffer, (uint)size)) {
                int error = Marshal.GetLastWin32Error();
                CloseHandle(job);
                throw new System.ComponentModel.Win32Exception(error,
                    "could not apply the job object limits");
            }
        }
        finally {
            Marshal.FreeHGlobal(buffer);
        }
        return job;
    }
}

}
'@
}

# How long the workers are given to ramp before their CPU consumption is checked.
# Long enough for a cold pwsh to reach its loop, short enough not to pad a run.
$script:RampSeconds = 3

# Each worker must have earned at least this fraction of one core over the ramp.
# The failure this catches is a worker that started and does nothing -- a wrong
# command line, a launcher shim, a worker killed by the job memory cap.
$script:MinimumRampUtilization = 0.25

function New-SyntheticLoadWorkerCommand {
    param(
        [Parameter(Mandatory)] [ValidateSet('Cpu', 'Disk')] [string] $Kind,
        [Parameter(Mandatory)] [int] $Seconds,
        [string] $ScratchFile
    )
    # The worker's own deadline: the first of the three guarantees, and the only
    # one that still holds if this module is used wrongly.
    $prologue = "`$deadline=[DateTime]::UtcNow.AddSeconds($Seconds);"
    if ($Kind -eq 'Cpu') {
        return $prologue +
        '$x=0.0; while([DateTime]::UtcNow -lt $deadline){ for($i=0;$i -lt 200000;$i++){ $x=$x+[math]::Sqrt($i) } }'
    }
    # Bounded rewrite of one fixed-size file: sustained write pressure that
    # cannot fill the volume however long the load runs.
    $quoted = $ScratchFile.Replace("'", "''")
    return $prologue +
    "`$path='$quoted'; " +
    '$block=New-Object byte[] 4194304; $rng=[Random]::new(); ' +
    'try { while([DateTime]::UtcNow -lt $deadline){ $rng.NextBytes($block); [IO.File]::WriteAllBytes($path,$block) } } ' +
    'finally { Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue }'
}

function Get-SyntheticLoadCpuSeconds {
    param([Parameter(Mandatory)] [int[]] $ProcessId)
    $total = 0.0
    foreach ($id in $ProcessId) {
        $process = Get-Process -Id $id -ErrorAction SilentlyContinue
        # TotalProcessorTime is only readable while the process lives; a dead
        # worker contributes what it had, not an error.
        if ($null -ne $process) {
            try { $total += $process.TotalProcessorTime.TotalSeconds } catch { }
        }
    }
    return $total
}

function Start-SyntheticLoad {
    <#
    .SYNOPSIS
        Starts bounded CPU and disk load and returns a handle to stop it.
    .DESCRIPTION
        Prefer Invoke-WithSyntheticLoad, which cannot forget the Stop. Use this
        directly only when the load must span several statements, and put the
        Stop in a `finally`.

        Throws if the workers do not actually consume CPU, so a measurement is
        never taken against load that exists only on paper.
    .PARAMETER MaxSeconds
        Hard wall-clock ceiling. Every worker exits by itself at this point even
        if nothing stops it.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [ValidateRange(1, 7200)] [int] $MaxSeconds,
        [ValidateRange(0, 256)] [int] $CpuWorkers = [Environment]::ProcessorCount,
        [ValidateRange(0, 16)] [int] $DiskWorkers = 0,
        [string] $ScratchDirectory = $env:TEMP,
        # Total committed memory for all workers together, 0 to size it from the
        # worker count. The workers are small; the cap exists so a runaway one
        # cannot take the machine with it.
        [ValidateRange(0, 65536)] [int] $MemoryLimitMb = 0,
        [switch] $SkipUtilizationCheck
    )

    if ($CpuWorkers -eq 0 -and $DiskWorkers -eq 0) {
        throw 'Start-SyntheticLoad was asked for no workers at all'
    }

    $workerCount = $CpuWorkers + $DiskWorkers
    if ($MemoryLimitMb -eq 0) { $MemoryLimitMb = 256 + (256 * $workerCount) }

    # The running pwsh, by absolute path. Resolving through PATH would risk a
    # launcher shim, which is exactly the class of process this module refuses
    # to depend on.
    $shell = (Get-Process -Id $PID).Path
    if ([string]::IsNullOrWhiteSpace($shell)) { throw 'could not resolve the running PowerShell executable' }

    $job = [ExoSnap.SyntheticLoad.Native]::CreateBoundedJob(
        [uint32]($workerCount + 8), [uint64]$MemoryLimitMb * 1MB)

    $load = [pscustomobject]@{
        JobHandle   = $job
        ProcessIds  = @()
        StartedUtc  = [DateTime]::UtcNow
        MaxSeconds  = $MaxSeconds
        WorkerCount = $workerCount
        Stopped     = $false
    }

    try {
        $specs = @()
        for ($i = 0; $i -lt $CpuWorkers; $i++) { $specs += @{ Kind = 'Cpu'; Index = $i } }
        for ($i = 0; $i -lt $DiskWorkers; $i++) { $specs += @{ Kind = 'Disk'; Index = $i } }

        foreach ($spec in $specs) {
            $scratch = Join-Path $ScratchDirectory ("exosnap-load-{0}-{1}.tmp" -f $PID, $spec.Index)
            $inner = New-SyntheticLoadWorkerCommand -Kind $spec.Kind -Seconds $MaxSeconds -ScratchFile $scratch
            $commandLine = '"{0}" -NoProfile -NonInteractive -Command "{1}"' -f $shell, $inner
            $load.ProcessIds += [ExoSnap.SyntheticLoad.Native]::StartInJob($job, $shell, $commandLine)
        }

        if (-not $SkipUtilizationCheck) {
            Start-Sleep -Seconds $script:RampSeconds
            $consumed = Get-SyntheticLoadCpuSeconds -ProcessId $load.ProcessIds
            $required = $workerCount * $script:RampSeconds * $script:MinimumRampUtilization
            if ($consumed -lt $required) {
                throw ("the load workers consumed {0:N1} CPU s in {1} s, below the {2:N1} s that " -f
                    $consumed, $script:RampSeconds, $required) +
                "$workerCount working workers would earn -- the load did not materialize"
            }
        }
    }
    catch {
        Stop-SyntheticLoad -Load $load | Out-Null
        throw
    }

    return $load
}

function Stop-SyntheticLoad {
    <#
    .SYNOPSIS
        Terminates the load and returns what it actually consumed.
    .DESCRIPTION
        Safe to call more than once and safe to call on a partially started load.
        Terminating the job kills every worker and everything a worker spawned;
        closing the handle would do it too, and both happen here.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)] [AllowNull()] $Load)

    if ($null -eq $Load -or $Load.Stopped) { return $null }

    # Read the consumption before terminating: a dead process reports nothing.
    $cpuSeconds = Get-SyntheticLoadCpuSeconds -ProcessId $Load.ProcessIds
    $wallSeconds = ([DateTime]::UtcNow - $Load.StartedUtc).TotalSeconds

    if ($Load.JobHandle -ne [IntPtr]::Zero) {
        [void][ExoSnap.SyntheticLoad.Native]::TerminateJobObject($Load.JobHandle, 1)
        [void][ExoSnap.SyntheticLoad.Native]::CloseHandle($Load.JobHandle)
        $Load.JobHandle = [IntPtr]::Zero
    }
    $Load.Stopped = $true

    $survivors = @($Load.ProcessIds | Where-Object { $null -ne (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
    if ($survivors.Count -gt 0) {
        # Reported, not swallowed: a survivor means the job contract failed, and
        # a machine with a leftover burner invalidates every later measurement.
        Write-Warning ("synthetic load left {0} process(es) running: {1}" -f
            $survivors.Count, ($survivors -join ', '))
    }

    return [pscustomobject]@{
        WallSeconds = [math]::Round($wallSeconds, 2)
        CpuSeconds  = [math]::Round($cpuSeconds, 2)
        CoresBusy   = if ($wallSeconds -gt 0) { [math]::Round($cpuSeconds / $wallSeconds, 2) } else { 0 }
        Workers     = $Load.WorkerCount
        Survivors   = $survivors
    }
}

function Invoke-WithSyntheticLoad {
    <#
    .SYNOPSIS
        Runs a script block while the machine is loaded, and stops the load after.
    .DESCRIPTION
        The safe form: the load cannot outlive the script block, whether it
        returns, throws, or the whole process is killed.

        Emits the measured utilization so a caller can state what the machine was
        actually doing rather than what it asked for.
    .EXAMPLE
        Invoke-WithSyntheticLoad -MaxSeconds 360 -CpuWorkers 16 -ScriptBlock {
            pwsh ./drift-probe.ps1 -Label load -Minutes 5
        }
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [ValidateRange(1, 7200)] [int] $MaxSeconds,
        [Parameter(Mandatory)] [scriptblock] $ScriptBlock,
        [ValidateRange(0, 256)] [int] $CpuWorkers = [Environment]::ProcessorCount,
        [ValidateRange(0, 16)] [int] $DiskWorkers = 0,
        [string] $ScratchDirectory = $env:TEMP,
        [ValidateRange(0, 65536)] [int] $MemoryLimitMb = 0,
        [switch] $SkipUtilizationCheck
    )

    $load = Start-SyntheticLoad -MaxSeconds $MaxSeconds -CpuWorkers $CpuWorkers `
        -DiskWorkers $DiskWorkers -ScratchDirectory $ScratchDirectory `
        -MemoryLimitMb $MemoryLimitMb -SkipUtilizationCheck:$SkipUtilizationCheck
    if ($null -eq $load) { throw 'the synthetic load did not start' }
    try {
        & $ScriptBlock
    }
    finally {
        $report = Stop-SyntheticLoad -Load $load
        if ($null -ne $report) {
            Write-Verbose ("synthetic load: {0} workers, {1} cores busy over {2} s" -f
                $report.Workers, $report.CoresBusy, $report.WallSeconds)
        }
    }
}

function ConvertTo-SyntheticLoadLiteral {
    param([Parameter(Mandatory)] [AllowEmptyString()] [string] $Value)
    return "'" + $Value.Replace("'", "''") + "'"
}

function Invoke-WithContainedProcess {
    <#
    .SYNOPSIS
        Runs an external tool under the same containment as the load workers,
        for as long as the script block needs it and no longer.
    .DESCRIPTION
        For the helper processes a measurement needs running alongside it: a tone
        on the render endpoint, a competing encoder, a traffic generator.

        The tool is not started directly. A small PowerShell worker owns it,
        holds the wall-clock deadline and kills the whole child tree when the
        deadline passes -- which restores the self-deadline guarantee for a tool
        that has no duration argument of its own, and contains launcher shims
        that fork the real binary as a child.
    .EXAMPLE
        Invoke-WithContainedProcess -FilePath ffplay -MaxSeconds 400 -ArgumentList @(
            '-nodisp', '-autoexit', '-f', 'lavfi', '-i', 'sine=frequency=440'
        ) -ScriptBlock { pwsh ./probe.ps1 }
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $FilePath,
        [Parameter(Mandatory)] [scriptblock] $ScriptBlock,
        [string[]] $ArgumentList = @(),
        [Parameter(Mandatory)] [ValidateRange(1, 7200)] [int] $MaxSeconds,
        [ValidateRange(128, 65536)] [int] $MemoryLimitMb = 2048
    )

    $resolved = (Get-Command -Name $FilePath -CommandType Application -ErrorAction Stop |
            Select-Object -First 1).Source
    $shell = (Get-Process -Id $PID).Path

    $argumentLiteral = if ($ArgumentList.Count -eq 0) { '@()' }
    else { '@(' + (($ArgumentList | ForEach-Object { ConvertTo-SyntheticLoadLiteral $_ }) -join ',') + ')' }

    # Kill($true) takes the child tree with it: the resolved path may still be a
    # launcher that forks the real binary.
    $inner = "`$child=Start-Process -FilePath $(ConvertTo-SyntheticLoadLiteral $resolved) " +
    "-ArgumentList $argumentLiteral -PassThru -WindowStyle Hidden; " +
    "if(-not `$child.WaitForExit($($MaxSeconds * 1000))){ try { `$child.Kill(`$true) } catch { } }"

    $job = [ExoSnap.SyntheticLoad.Native]::CreateBoundedJob([uint32]16, [uint64]$MemoryLimitMb * 1MB)
    $commandLine = '"{0}" -NoProfile -NonInteractive -Command "{1}"' -f $shell, $inner
    try {
        [void][ExoSnap.SyntheticLoad.Native]::StartInJob($job, $shell, $commandLine)
        & $ScriptBlock
    }
    finally {
        [void][ExoSnap.SyntheticLoad.Native]::TerminateJobObject($job, 1)
        [void][ExoSnap.SyntheticLoad.Native]::CloseHandle($job)
    }
}

Export-ModuleMember -Function Start-SyntheticLoad, Stop-SyntheticLoad, Invoke-WithSyntheticLoad,
    Invoke-WithContainedProcess
