using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

// Compiled as a Windows application: Java can start this boundary without
// allocating a console, and its PowerShell child uses CREATE_NO_WINDOW.
internal static class ConsoleFreeOwnedStarCraft {
    [DllImport("kernel32.dll")] static extern IntPtr GetConsoleWindow();
    static string Quote(string value) {
        var quoted = new StringBuilder("\"");
        int slashes = 0;
        foreach (char ch in value) {
            if (ch == '\\') { ++slashes; continue; }
            quoted.Append('\\', ch == '"' ? slashes * 2 + 1 : slashes);
            quoted.Append(ch);
            slashes = 0;
        }
        quoted.Append('\\', slashes * 2);
        return quoted.Append('"').ToString();
    }

    static string PowerShell() {
        foreach (string directory in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';')) {
            if (String.IsNullOrWhiteSpace(directory)) continue;
            string candidate = Path.Combine(directory.Trim().Trim('"'), "pwsh.exe");
            if (File.Exists(candidate)) return Path.GetFullPath(candidate);
        }
        throw new FileNotFoundException("Console-free StarCraft client requires PowerShell 7 (pwsh.exe) on PATH");
    }

    [STAThread]
    static int Main(string[] args) {
        try {
            if (GetConsoleWindow() != IntPtr.Zero)
                throw new InvalidOperationException("Owned client launcher must have no console");
            if (args.Length != 2 || (args[0] != "start" && args[0] != "stop"))
                throw new ArgumentException("Expected start|stop and the owned runtime directory");
            string runtime = Path.GetFullPath(args[1]);
            if (!Directory.Exists(runtime)) throw new DirectoryNotFoundException(runtime);
            string helper = Path.Combine(AppDomain.CurrentDomain.BaseDirectory,
                args[0] + "-owned-starcraft.ps1");
            if (!File.Exists(helper)) throw new FileNotFoundException("Missing owned launch helper", helper);
            var start = new ProcessStartInfo {
                FileName = PowerShell(), WorkingDirectory = runtime,
                UseShellExecute = false, CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardOutput = true, RedirectStandardError = true,
                Arguments = "-NoLogo -NoProfile -NonInteractive -File " + Quote(helper) +
                    " -Runtime " + Quote(runtime) + (args[0] == "start" ? " -MonitorOwnedLock" : "")
            };
            string log = Path.Combine(runtime, "headless-client-" + args[0] + "-" +
                Guid.NewGuid().ToString("N") + ".log");
            using (var output = new StreamWriter(log, false, new UTF8Encoding(false)))
            using (var process = new Process { StartInfo = start }) {
                object gate = new object();
                DataReceivedEventHandler record = (sender, e) => {
                    if (e.Data == null) return;
                    lock (gate) { output.WriteLine(e.Data); output.Flush(); }
                };
                process.OutputDataReceived += record;
                process.ErrorDataReceived += record;
                if (!process.Start()) throw new InvalidOperationException("Owned helper did not start");
                process.BeginOutputReadLine(); process.BeginErrorReadLine();
                process.WaitForExit();
                return process.ExitCode;
            }
        } catch (Exception error) {
            // No message box or visible error window, including failure paths.
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
