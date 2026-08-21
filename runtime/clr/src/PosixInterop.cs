using System;
using System.Runtime.InteropServices;

namespace Lox;

/// <summary>
/// The two libc calls LoxRuntime needs a real file type and a real
/// processor-time reading for, neither of which a managed BCL call can
/// give faithfully on its own:
///
/// <list type="bullet">
/// <item><description><c>File.Exists</c>/<c>Directory.Exists</c> report
/// true for a symbolic link whose target is missing, and cannot tell a
/// character device or a named pipe from a regular file - the managed
/// layer never resolves the link and reads the real inode's type.
/// <c>statx(2)</c> does both in one syscall.</description></item>
/// <item><description><c>Process.TotalProcessorTime</c> reads
/// <c>/proc/self/stat</c>, whose clock-tick unit is 10&#160;ms on this
/// platform - far coarser than <c>std::clock()</c>'s microsecond steps.
/// <c>clock_gettime(CLOCK_PROCESS_CPUTIME_ID)</c> is the exact call
/// glibc's own <c>clock()</c> uses.</description></item>
/// </list>
///
/// <c>DllImport</c> adds no package reference, so this keeps
/// <c>LoxRuntime.csproj</c> dependency-free. Both calls are Linux-only;
/// every entry point here throws <see cref="PlatformNotSupportedException"/>
/// up front on any other OS rather than silently mis-answering.
/// </summary>
internal static class PosixInterop {
    private const int AtFdcwd = -100;

    // STATX_TYPE | STATX_SIZE | STATX_MTIME - the three fields LoxRuntime
    // reads back. Requesting exactly these (not STATX_ALL) keeps the
    // syscall to the fields this file actually promises.
    private const uint StatxMask = 0x00000001 | 0x00000200 | 0x00000040;

    private const ushort SFmt = 0xF000;
    private const ushort SIfdir = 0x4000;
    private const ushort SIfreg = 0x8000;

    // struct statx (linux/stat.h) is a fixed 256-byte, architecture-
    // independent layout by kernel ABI contract - unlike struct stat,
    // whose field widths vary by architecture and libc version. This is
    // exactly why the kernel added statx: a P/Invoke struct that matches
    // one architecture's struct stat silently misreads another's.
    [StructLayout(LayoutKind.Sequential)]
    private struct StatxTimestamp {
        public long TvSec;
        public uint TvNsec;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Size = 96)]
    private struct Reserved96 { }

    [StructLayout(LayoutKind.Sequential)]
    private struct Statx {
        public uint Mask;
        public uint Blksize;
        public ulong Attributes;
        public uint Nlink;
        public uint Uid;
        public uint Gid;
        public ushort Mode;
        public ushort Spare0;
        public ulong Ino;
        public ulong Size;
        public ulong Blocks;
        public ulong AttributesMask;
        public StatxTimestamp Atime;
        public StatxTimestamp Btime;
        public StatxTimestamp Ctime;
        public StatxTimestamp Mtime;
        public uint RdevMajor;
        public uint RdevMinor;
        public uint DevMajor;
        public uint DevMinor;
        public ulong MntId;
        public uint DioMemAlign;
        public uint DioOffsetAlign;
        public Reserved96 Spare3;
    }

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern int statx(int dirfd, string pathname, int flags, uint mask, out Statx statxbuf);

    [StructLayout(LayoutKind.Sequential)]
    private struct Timespec {
        public long TvSec;
        public long TvNsec;
    }

    [DllImport("libc", SetLastError = true)]
    private static extern int clock_gettime(int clockId, out Timespec tp);

    private const int ClockProcessCpuTimeId = 2;

    private static void RequireLinux(string surface) {
        if (!OperatingSystem.IsLinux()) {
            throw new PlatformNotSupportedException(
                $"LoxRuntime's {surface} needs a Linux-only syscall and has no fallback on this platform.");
        }
    }

    /// <summary>
    /// Resolves <paramref name="path"/> through every symbolic link and
    /// reports the real target's type - matching
    /// <c>std::filesystem::status</c>, which native's <c>os_api.cpp</c>
    /// uses. Returns false for anything the link chain does not resolve to
    /// (missing path, dangling symlink, or a permission error), the same
    /// "false on any error" contract as the <c>std::error_code</c>
    /// overloads native calls - never throws for those. <paramref
    /// name="isDir"/>/<paramref name="isFile"/> are false, and <paramref
    /// name="size"/>/<paramref name="mtimeSeconds"/> are 0, whenever this
    /// returns false.
    /// </summary>
    public static bool TryStat(string path, out bool isDir, out bool isFile, out ulong size, out double mtimeSeconds) {
        RequireLinux("exists()/is_dir()/is_file()/stat()");
        isDir = false;
        isFile = false;
        size = 0;
        mtimeSeconds = 0;
        if (statx(AtFdcwd, path, 0, StatxMask, out Statx buf) != 0) {
            return false;
        }
        ushort fileType = (ushort)(buf.Mode & SFmt);
        isDir = fileType == SIfdir;
        isFile = fileType == SIfreg;
        size = buf.Size;
        mtimeSeconds = buf.Mtime.TvSec + buf.Mtime.TvNsec / 1.0e9;
        return true;
    }

    /// <summary>
    /// This process's own user+system CPU time, in seconds, at
    /// <c>clock_gettime</c>'s own resolution (nanoseconds) - the same
    /// syscall glibc's <c>clock()</c> reads, so it matches
    /// <c>std::clock()</c>'s step size instead of
    /// <c>Process.TotalProcessorTime</c>'s 10&#160;ms <c>/proc</c> tick,
    /// and allocates nothing per call.
    /// </summary>
    public static double ProcessCpuTimeSeconds() {
        RequireLinux("clock()");
        if (clock_gettime(ClockProcessCpuTimeId, out Timespec ts) != 0) {
            throw new LoxError("clock() failed: clock_gettime(CLOCK_PROCESS_CPUTIME_ID) returned an error.");
        }
        return ts.TvSec + ts.TvNsec / 1.0e9;
    }
}
