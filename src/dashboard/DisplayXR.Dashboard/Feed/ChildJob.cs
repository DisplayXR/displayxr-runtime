// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace DisplayXR.Dashboard.Feed;

/// <summary>
/// One Windows job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE for every
/// CLI child. Its handle closes when this process ends however it ends, and
/// the kernel then kills whatever is still in the job: no orphaned
/// <c>displayxr-cli status --watch</c> holding a DIAG slot after a crash.
/// </summary>
internal static class ChildJob
{
    private static readonly object Gate = new();
    private static IntPtr _job;
    private static bool _failed;

    public static void Add(Process p)
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            IntPtr job = Job();
            if (job == IntPtr.Zero) return;
            if (!AssignProcessToJobObject(job, p.Handle))
                DashboardLog.Warn($"AssignProcessToJobObject failed ({Marshal.GetLastWin32Error()}); the child is still killed on a normal exit");
        }
        catch (Exception ex)
        {
            DashboardLog.Warn($"job object: {ex.Message}");
        }
    }

    private static IntPtr Job()
    {
        lock (Gate)
        {
            if (_job != IntPtr.Zero || _failed) return _job;
            IntPtr h = CreateJobObjectW(IntPtr.Zero, null);
            if (h == IntPtr.Zero) { _failed = true; return IntPtr.Zero; }
            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            int size = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
            IntPtr buf = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(info, buf, false);
                if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, buf, (uint)size))
                {
                    DashboardLog.Warn($"SetInformationJobObject failed ({Marshal.GetLastWin32Error()})");
                    CloseHandle(h);
                    _failed = true;
                    return IntPtr.Zero;
                }
            }
            finally { Marshal.FreeHGlobal(buf); }
            _job = h; // deliberately never closed: the OS closes it at process exit, which is the point
            return _job;
        }
    }

    private const int JobObjectExtendedLimitInformation = 9;
    private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
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
    private struct IO_COUNTERS
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateJobObjectW(IntPtr attributes, string? name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint length);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr h);
}
