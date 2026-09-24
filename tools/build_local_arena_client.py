"""Rebuild local tournament client crash recovery and process ownership; never launches it."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def build(template, output, javac):
    template, output = Path(template).resolve(), Path(output).resolve()
    if output.exists():
        raise ValueError("output already exists")
    jar = template / "client-a/client.jar"
    sources = template / "_source/src/client"
    client = (sources / "Client.java").read_text()
    commands = (sources / "ClientCommands.java").read_text()
    old = "if(dir.list().length > 0)"
    if client.count(old) != 1:
        raise ValueError("unexpected client crash-log implementation")
    client = client.replace(old, "if(dir.isDirectory() && dir.list() != null && dir.list().length > 0)")
    start = commands.index("\t\twhile (WindowsCommandTools.IsWindowsProcessRunning(\"StarCraft.exe\"))")
    end = commands.index("\n\tpublic static void Client_KillExcessWindowsProccess", start)
    replacement = '''        try {
            Process process = new ProcessBuilder("powershell.exe", "-NoProfile", "-NonInteractive",
                "-WindowStyle", "Hidden", "-File", new File("stop-owned-starcraft.ps1").getAbsolutePath(),
                "-Runtime", new File(ClientSettings.Instance().ClientStarcraftDir).getCanonicalPath())
                .inheritIO().start();
            if (process.waitFor() != 0) throw new IOException("Owned StarCraft cleanup failed");
        } catch (Exception e) { throw new RuntimeException(e); }
    }
    '''
    commands = commands[:start] + replacement + commands[end:]
    launch = 'WindowsCommandTools.RunWindowsExeLocal(ClientSettings.Instance().ClientStarcraftDir, "injectory.x86.exe --launch StarCraft.exe --inject bwapi-data\\\\BWAPI.dll --set-flags SEM_NOGPFAULTERRORBOX", false, true);'
    if commands.count(launch) != 1:
        raise ValueError("unexpected launch implementation")
    commands = commands.replace(launch, '''try {
            Process process = new ProcessBuilder("powershell.exe", "-NoProfile", "-NonInteractive",
                "-WindowStyle", "Hidden", "-File", new File("start-owned-starcraft.ps1").getAbsolutePath(),
                "-Runtime", new File(ClientSettings.Instance().ClientStarcraftDir).getCanonicalPath())
                .inheritIO().start();
            if (process.waitFor() != 0) throw new IOException("Owned StarCraft launch failed");
        } catch (Exception e) { throw new RuntimeException(e); }''')
    if 'taskkill /T /F /IM StarCraft.exe' in commands:
        raise ValueError("unscoped cleanup remains")
    # The local template already disables cleanup of unrelated session processes.
    cleanup = commands[commands.index("public static void Client_KillExcessWindowsProccess"):]
    cleanup = cleanup[:cleanup.index("public static void Client_CleanStarcraftDirectory")]
    if "RunWindowsCommand" in cleanup or "KillWindowsProcess" in cleanup:
        raise ValueError("template still cleans unrelated processes")
    output.mkdir(parents=True)
    src, classes = output / "source/client", output / "classes"
    src.mkdir(parents=True)
    classes.mkdir()
    for name, content in (("Client.java", client), ("ClientCommands.java", commands)):
        (src / name).write_text(content)
    subprocess.run([str(javac), "--release", "8", "-classpath", str(jar), "-d", str(classes),
                    str(src / "Client.java"), str(src / "ClientCommands.java")], check=True)
    replacements = {p.relative_to(classes).as_posix(): p.read_bytes() for p in classes.rglob("*.class")}
    with zipfile.ZipFile(jar) as original, zipfile.ZipFile(output / "client.jar", "w", zipfile.ZIP_DEFLATED) as modified:
        for item in original.infolist():
            modified.writestr(item, replacements.pop(item.filename, original.read(item.filename)))
        for name, data in replacements.items():
            modified.writestr(name, data)
    root = Path(__file__).resolve().parents[1]
    helper = root / "scripts/stop-owned-starcraft.ps1"
    (output / helper.name).write_bytes(helper.read_bytes())
    launcher = root / "scripts/start-owned-starcraft.ps1"
    (output / launcher.name).write_bytes(launcher.read_bytes())
    inputs = [jar, sources / "Client.java", sources / "ClientCommands.java", helper, launcher, Path(__file__)]
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    (output / "build.json").write_text(json.dumps(dict(
        inputs={str(p.resolve()): digest(p) for p in inputs},
        outputs={str(p.relative_to(output)): digest(p) for p in output.rglob("*") if p.is_file()},
        changes=["missing crash log directory tolerated", "cleanup restricted to owned runtime process",
                 "launch parent and creation time recorded when BWAPI hides executable path"]), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--javac", default="javac")
    build(**vars(parser.parse_args()))
