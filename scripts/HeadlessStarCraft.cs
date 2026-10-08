using System;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Protodd.Runtime {
    // Creates new owned runs on an unshown desktop. No desktop switching,
    // window enumeration, input automation or changes to existing processes.
    public sealed class HeadlessStarCraft {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct StartupInfo {
            public uint cb; public string reserved, desktop, title;
            public uint x, y, xSize, ySize, xChars, yChars, fill, flags;
            public ushort show, reservedBytes; public IntPtr reservedData, stdin, stdout, stderr;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct ProcessInfo { public IntPtr process, thread; public uint pid, tid; }
        [StructLayout(LayoutKind.Sequential)]
        struct DebugEvent {
            public uint code, pid, tid, padding;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 160)] public byte[] data;
        }
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateDesktop(string name, IntPtr device, IntPtr mode, uint flags, uint access, IntPtr security);
        [DllImport("user32.dll", SetLastError = true)] static extern bool CloseDesktop(IntPtr desktop);
        delegate bool WindowVisitor(IntPtr window, IntPtr argument);
        [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desktop, WindowVisitor visit, IntPtr argument);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
        [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateProcess(string app, StringBuilder command, IntPtr processSecurity,
            IntPtr threadSecurity, bool inherit, uint flags, IntPtr environment, string directory,
            ref StartupInfo startup, out ProcessInfo process);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool WaitForDebugEvent(out DebugEvent debugEvent, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool ContinueDebugEvent(uint pid, uint tid, uint status);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool DebugSetProcessKillOnExit(bool kill);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFinalPathNameByHandle(IntPtr file, StringBuilder path, uint size, uint flags);
        [DllImport("dbghelp.dll", SetLastError = true)]
        static extern bool MiniDumpWriteDump(IntPtr process, uint pid, IntPtr file, uint type,
            IntPtr exception, IntPtr streams, IntPtr callback);
        struct Module { public ulong start, size; public string path; }
        readonly ManualResetEvent ready = new ManualResetEvent(false);
        readonly ManualResetEvent done = new ManualResetEvent(false);
        public bool Completed { get { return done.WaitOne(0); } }
        readonly Dictionary<uint, IntPtr> processes = new Dictionary<uint, IntPtr>();
        readonly Dictionary<uint, List<Module>> modules = new Dictionary<uint, List<Module>>();
        Exception launchError; uint injectorId; int dumps;
        readonly ConcurrentQueue<uint> closeRequests = new ConcurrentQueue<uint>();
        static readonly List<HeadlessStarCraft> monitors = new List<HeadlessStarCraft>();
        public void RequestClose(uint pid) { closeRequests.Enqueue(pid); }
        public static void RequestOwnedClose(uint pid) {
            lock (monitors) foreach (var monitor in monitors) monitor.RequestClose(pid);
        }
        public uint ProcessId { get { ready.WaitOne(); if (launchError != null) throw launchError; return injectorId; } }
        public string DesktopName { get; private set; }
        static ulong Pointer(byte[] data, int offset) { return BitConverter.ToUInt64(data, offset); }
        static string ModulePath(IntPtr file) {
            if (file == IntPtr.Zero) return "unknown";
            var result = new StringBuilder(32768);
            return GetFinalPathNameByHandle(file, result, (uint)result.Capacity, 0) == 0
                ? "unknown" : result.ToString().Replace("\\\\?\\", "");
        }
        static ulong ImageSize(string path) {
            try {
                using (var stream = File.OpenRead(path)) using (var reader = new BinaryReader(stream)) {
                    stream.Position = 0x3c; int pe = reader.ReadInt32();
                    stream.Position = pe + 24 + 56; return reader.ReadUInt32();
                }
            } catch { return 0; }
        }
        void AddModule(uint pid, IntPtr file, ulong address, string directory) {
            string path = ModulePath(file);
            if (!modules.ContainsKey(pid)) modules.Add(pid, new List<Module>());
            modules[pid].Add(new Module { start = address, size = ImageSize(path), path = path });
            File.AppendAllText(Path.Combine(directory, "native-modules.txt"),
                pid + ",0x" + address.ToString("x") + "," + path + Environment.NewLine);
            if (file != IntPtr.Zero) CloseHandle(file);
        }
        void RecordException(DebugEvent e, string directory) {
            uint code = BitConverter.ToUInt32(e.data, 0);
            bool first = BitConverter.ToUInt32(e.data, 152) != 0;
            if (code != 0xc0000005 && code != 0xc0000409 && code != 0xc0000374 && first) return;
            ulong address = Pointer(e.data, 16);
            Module owner = new Module { path = "unknown" };
            if (modules.ContainsKey(e.pid)) foreach (var module in modules[e.pid])
                if (address >= module.start && address - module.start < module.size) owner = module;
            string prefix = e.pid + "-" + e.tid + "-" + (first ? "first" : "fatal") + "-" + dumps;
            string report = (first ? "FIRST_CHANCE: " : "EXCEPTION: ") + "0x" + code.ToString("x8") + Environment.NewLine +
                "process=" + e.pid + Environment.NewLine + "thread=" + e.tid + Environment.NewLine +
                "address=0x" + address.ToString("x") + Environment.NewLine + "module=" + owner.path + Environment.NewLine +
                "module_base=0x" + owner.start.ToString("x") + Environment.NewLine +
                "module_offset=0x" + (address - owner.start).ToString("x") + Environment.NewLine;
            if (code == 0xc0000005) report += "operation=" + Pointer(e.data, 32) + Environment.NewLine +
                "memory=0x" + Pointer(e.data, 40).ToString("x") + Environment.NewLine;
            if (processes.ContainsKey(e.pid) && (!first || dumps < 4)) {
                string dump = Path.Combine(directory, prefix + ".dmp");
                using (var stream = new FileStream(dump, FileMode.CreateNew, FileAccess.Write, FileShare.Read)) {
                    bool saved = MiniDumpWriteDump(processes[e.pid], e.pid, stream.SafeFileHandle.DangerousGetHandle(),
                        0x1060, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
                    report += "dump=" + dump + Environment.NewLine + "dump_saved=" + saved + Environment.NewLine;
                    if (!saved) report += "dump_error=" + Marshal.GetLastWin32Error() + Environment.NewLine;
                }
                ++dumps;
            }
            File.WriteAllText(Path.Combine(directory, prefix + ".txt"), report);
        }
        void Run(string injector, string arguments, string runtime, string diagnostics) {
            IntPtr desktop = IntPtr.Zero;
            try {
                Directory.CreateDirectory(diagnostics);
                desktop = CreateDesktop(DesktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
                if (desktop == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot create headless desktop");
                var startup = new StartupInfo { cb = (uint)Marshal.SizeOf(typeof(StartupInfo)), desktop = DesktopName,
                                               flags = 1, show = 0 };
                ProcessInfo process;
                if (!CreateProcess(injector, new StringBuilder("\"" + injector + "\" " + arguments),
                    IntPtr.Zero, IntPtr.Zero, false, 1 | 0x08000000, IntPtr.Zero, runtime, ref startup, out process))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot launch headless injector");
                injectorId = process.pid;
                // The debugger records faults; it must not kill a run merely
                // because a supervising PowerShell session stops unexpectedly.
                if (!DebugSetProcessKillOnExit(false))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot disable debugger exit termination");
                CloseHandle(process.thread); CloseHandle(process.process);
                ready.Set();
                int live = 0;
                while (true) {
                    uint closeId;
                    while (closeRequests.TryDequeue(out closeId)) {
                        // Only a still-owned process on this private desktop.
                        // No focus changes or input are sent to other apps.
                        if (!processes.ContainsKey(closeId)) continue;
                        EnumDesktopWindows(desktop, (window, argument) => {
                            uint windowPid; GetWindowThreadProcessId(window, out windowPid);
                            if (windowPid == closeId) PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
                            return true;
                        }, IntPtr.Zero);
                    }
                    DebugEvent e;
                    if (!WaitForDebugEvent(out e, 200)) {
                        int error = Marshal.GetLastWin32Error();
                        if (error == 121) continue;
                        throw new Win32Exception(error, "Native crash monitor failed");
                    }
                    uint status = 0x00010002;
                    if (e.code == 3) {
                        ++live;
                        processes[e.pid] = new IntPtr((long)Pointer(e.data, 8));
                        AddModule(e.pid, new IntPtr((long)Pointer(e.data, 0)), Pointer(e.data, 24), diagnostics);
                        CloseHandle(new IntPtr((long)Pointer(e.data, 16)));
                    } else if (e.code == 2) {
                        CloseHandle(new IntPtr((long)Pointer(e.data, 0)));
                    } else if (e.code == 6) {
                        AddModule(e.pid, new IntPtr((long)Pointer(e.data, 0)), Pointer(e.data, 8), diagnostics);
                    } else if (e.code == 1) {
                        uint code = BitConverter.ToUInt32(e.data, 0);
                        if (code != 0x80000003 && code != 0x4000001f) {
                            RecordException(e, diagnostics); status = 0x80010001;
                        }
                    } else if (e.code == 5) {
                        File.AppendAllText(Path.Combine(diagnostics, "native-exits.txt"),
                            e.pid + ",0x" + BitConverter.ToUInt32(e.data, 0).ToString("x8") + Environment.NewLine);
                        if (processes.ContainsKey(e.pid)) { CloseHandle(processes[e.pid]); processes.Remove(e.pid); }
                        --live;
                    }
                    ContinueDebugEvent(e.pid, e.tid, status);
                    if (live == 0) break;
                }
            } catch (Exception error) {
                launchError = error;
                Directory.CreateDirectory(diagnostics);
                File.WriteAllText(Path.Combine(diagnostics, "monitor-error.txt"), error.ToString());
                ready.Set();
            } finally {
                lock (monitors) monitors.Remove(this);
                if (desktop != IntPtr.Zero) CloseDesktop(desktop);
                done.Set();
            }
        }
        public static HeadlessStarCraft Launch(string injector, string arguments, string runtime, string diagnostics) {
            if (IntPtr.Size != 8) throw new InvalidOperationException("Headless crash monitor requires 64-bit PowerShell");
            var launch = new HeadlessStarCraft { DesktopName = "Protodd-" + Guid.NewGuid().ToString("N") };
            lock (monitors) monitors.Add(launch);
            var thread = new Thread(() => launch.Run(injector, arguments, runtime, diagnostics));
            thread.IsBackground = true; thread.Start();
            uint id = launch.ProcessId;
            return launch;
        }
    }
}
