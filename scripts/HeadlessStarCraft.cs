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
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr OpenFileMapping(uint access, bool inherit, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr MapViewOfFile(IntPtr mapping, uint access, uint high, uint low, UIntPtr size);
        [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr view);
        public sealed class ServerConnection {
            public uint ProcessId { get; set; }
            public bool Connected { get; set; }
        }
        public static ServerConnection[] ReadServerConnections(string name) {
            // Official BWAPI4.4 GameTable:8 rows of uint,bool,padding,uint.
            // Discovery is read-only; never claim a slot or attach a client.
            IntPtr mapping = OpenFileMapping(4, false, name);
            if (mapping == IntPtr.Zero) {
                int error = Marshal.GetLastWin32Error();
                if (error == 2) return new ServerConnection[0];
                throw new Win32Exception(error, "Cannot read BWAPI client discovery");
            }
            try {
                IntPtr view = MapViewOfFile(mapping, 4, 0, 0, (UIntPtr)96);
                if (view == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot map BWAPI client discovery");
                try {
                    var rows = new List<ServerConnection>();
                    for (int i = 0; i < 8; ++i) {
                        uint pid = unchecked((uint)Marshal.ReadInt32(view, i * 12));
                        if (pid != 0) rows.Add(new ServerConnection {
                            ProcessId = pid, Connected = Marshal.ReadByte(view, i * 12 + 4) != 0 });
                    }
                    return rows.ToArray();
                } finally { UnmapViewOfFile(view); }
            } finally { CloseHandle(mapping); }
        }
        [DllImport("kernel32.dll")] static extern IntPtr GetConsoleWindow();
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
        public bool Failed { get { return Completed && launchError != null; } }
        public string FailureReason { get { return Failed ? launchError.ToString() : null; } }
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
        void AppendDiagnostic(string directory, string name, string text) {
            // A launch owns its journal even when a runtime directory is reused.
            // Live readers must permit this writer, and this writer permits them.
            using (var stream = new FileStream(Path.Combine(directory, DesktopName + "-" + name),
                       FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
                writer.Write(text);
        }
        void AddModule(uint pid, IntPtr file, ulong address, string directory) {
            try {
                string path = ModulePath(file);
                if (!modules.ContainsKey(pid)) modules.Add(pid, new List<Module>());
                modules[pid].Add(new Module { start = address, size = ImageSize(path), path = path });
                AppendDiagnostic(directory, "native-modules.txt",
                    pid + ",0x" + address.ToString("x") + "," + path + Environment.NewLine);
            } finally {
                if (file != IntPtr.Zero) CloseHandle(file);
            }
        }
        void RecordException(DebugEvent e, string directory) {
            uint code = BitConverter.ToUInt32(e.data, 0);
            bool first = BitConverter.ToUInt32(e.data, 152) != 0;
            if (code != 0xc0000005 && code != 0xc0000409 && code != 0xc0000374 && first) return;
            ulong address = Pointer(e.data, 16);
            Module owner = new Module { path = "unknown" };
            if (modules.ContainsKey(e.pid)) foreach (var module in modules[e.pid])
                if (address >= module.start && address - module.start < module.size) owner = module;
            string prefix = DesktopName + "-" + e.pid + "-" + e.tid + "-" + (first ? "first" : "fatal") + "-" + dumps;
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
            ProcessInfo created = new ProcessInfo();
            try {
                Directory.CreateDirectory(diagnostics);
                desktop = CreateDesktop(DesktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
                if (desktop == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot create headless desktop");
                var startup = new StartupInfo { cb = (uint)Marshal.SizeOf(typeof(StartupInfo)), desktop = DesktopName,
                                               flags = 1, show = 0 };
                if (!CreateProcess(injector, new StringBuilder("\"" + injector + "\" " + arguments),
                    IntPtr.Zero, IntPtr.Zero, false, 1 | 0x08000000, IntPtr.Zero, runtime, ref startup, out created))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot launch headless injector");
                injectorId = created.pid;
                // The debugger records faults; it must not kill a run merely
                // because a supervising PowerShell session stops unexpectedly.
                if (!DebugSetProcessKillOnExit(false))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot disable debugger exit termination");
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
                    try {
                        if (e.code == 3) {
                            ++live;
                            processes[e.pid] = new IntPtr((long)Pointer(e.data, 8));
                            if (e.pid == injectorId) {
                                // Creator handles and debug-event handles have
                                // different owners. Keep creator handles alive
                                // until aliasing can be checked; Windows closes
                                // debug process/thread handles on exit continuation.
                                var debugThread = new IntPtr((long)Pointer(e.data, 16));
                                if (created.process != IntPtr.Zero && created.process != processes[e.pid])
                                    CloseHandle(created.process);
                                if (created.thread != IntPtr.Zero && created.thread != debugThread)
                                    CloseHandle(created.thread);
                                created.process = created.thread = IntPtr.Zero;
                            }
                            AddModule(e.pid, new IntPtr((long)Pointer(e.data, 0)), Pointer(e.data, 24), diagnostics);
                        } else if (e.code == 6) {
                            AddModule(e.pid, new IntPtr((long)Pointer(e.data, 0)), Pointer(e.data, 8), diagnostics);
                        } else if (e.code == 1) {
                            uint code = BitConverter.ToUInt32(e.data, 0);
                            if (code != 0x80000003 && code != 0x4000001f) {
                                status = 0x80010001;
                                RecordException(e, diagnostics);
                            }
                        } else if (e.code == 5) {
                            processes.Remove(e.pid);
                            --live;
                            AppendDiagnostic(diagnostics, "native-exits.txt",
                                e.pid + ",0x" + BitConverter.ToUInt32(e.data, 0).ToString("x8") + Environment.NewLine);
                        }
                    } finally {
                        // Diagnostic failure cannot leave the debuggee suspended
                        // at a received event or swallow a first-chance fault.
                        if (!ContinueDebugEvent(e.pid, e.tid, status))
                            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot continue native debug event");
                    }
                    if (live == 0) break;
                }
            } catch (Exception error) {
                launchError = error;
                try {
                    Directory.CreateDirectory(diagnostics);
                    File.WriteAllText(Path.Combine(diagnostics, DesktopName + "-monitor-error.txt"), error.ToString());
                } catch (Exception diagnosticError) {
                    // Preserve attribution through the monitor API even when
                    // the diagnostic directory itself cannot accept a report.
                    launchError = new AggregateException(error, diagnosticError);
                } finally { ready.Set(); }
            } finally {
                lock (monitors) monitors.Remove(this);
                if (desktop != IntPtr.Zero) CloseDesktop(desktop);
                done.Set();
            }
        }
        public static HeadlessStarCraft Launch(string injector, string arguments, string runtime, string diagnostics) {
            if (IntPtr.Size != 8) throw new InvalidOperationException("Headless crash monitor requires 64-bit PowerShell");
            // A private game desktop does not hide a console belonging to its
            // supervisor. Reject that launch before creating either process.
            if (GetConsoleWindow() != IntPtr.Zero)
                throw new InvalidOperationException("Headless supervisor has a console; launch it with Start-ConsoleFreeProcess before starting StarCraft");
            var launch = new HeadlessStarCraft { DesktopName = "Protodd-" + Guid.NewGuid().ToString("N") };
            lock (monitors) monitors.Add(launch);
            var thread = new Thread(() => launch.Run(injector, arguments, runtime, diagnostics));
            thread.IsBackground = true; thread.Start();
            uint id = launch.ProcessId;
            return launch;
        }
    }
}
