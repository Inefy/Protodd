"""Build Local PC networking and immutable per-game telemetry archives into the manager."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def build(template, output, javac):
    template, output = Path(template).resolve(), Path(output).resolve()
    if output.exists():
        raise ValueError('output already exists')
    source_root = template / '_source/src'
    settings_path = source_root / 'objects/BWAPISettings.java'
    server_path = source_root / 'server/ServerClientThread.java'
    settings = settings_path.read_text()
    if 'Local PC' not in settings:
        raise ValueError('source must explicitly select Local PC networking')
    server = server_path.read_text()
    original = 'dm.write(ServerSettings.Instance().ServerBotDir + dm.botName + "\\\\write");'
    if server.count(original) != 1:
        raise ValueError('unexpected write-directory receiver')
    replacement = '''if (lastInstructionSent == null || !dm.botName.matches("[A-Za-z0-9_-]+"))
                        throw new java.io.IOException("Invalid archived game identity");
                    String expectedBot = lastInstructionSent.isHost ?
                        lastInstructionSent.hostBot.getName() : lastInstructionSent.awayBot.getName();
                    if (!expectedBot.equals(dm.botName)) throw new java.io.IOException("Bot identity mismatch");
                    java.nio.file.Path archive = java.nio.file.Paths.get(ServerSettings.Instance().ServerReplayDir,
                        "bot-write", "game-" + lastInstructionSent.game_id, dm.botName, "received");
                    java.nio.file.Files.createDirectories(archive);
                    java.nio.file.Path raw = archive.resolve("received.zip");
                    if (java.nio.file.Files.exists(raw)) {
                        if (!java.util.Arrays.equals(java.nio.file.Files.readAllBytes(raw), dm.getRawData()))
                            throw new java.io.IOException("Conflicting game telemetry retransmission");
                    } else {
                        java.nio.file.Files.write(raw, dm.getRawData(), java.nio.file.StandardOpenOption.CREATE_NEW);
                        dm.write(archive.toString());
                    }
                    // Evaluation campaigns retain evidence but never send prior
                    // writes back into a subsequent game.
                    if (ServerSettings.Instance().EnableBotFileIO) {
                        dm.write(ServerSettings.Instance().ServerBotDir + dm.botName + "\\\\write");
                    }'''
    server = server.replace(original, replacement)
    output.mkdir(parents=True)
    sources, classes = output / 'source', output / 'classes'
    classes.mkdir()
    for name, content in [('objects/BWAPISettings.java', settings), ('server/ServerClientThread.java', server)]:
        target = sources / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    jar = template / 'server/server.jar'
    subprocess.run([str(javac), '--release', '8', '-classpath', str(jar), '-d', str(classes),
                    str(sources / 'objects/BWAPISettings.java'), str(sources / 'server/ServerClientThread.java')], check=True)
    replacements = {p.relative_to(classes).as_posix(): p.read_bytes() for p in classes.rglob('*.class')}
    with zipfile.ZipFile(jar) as original_jar, zipfile.ZipFile(output / 'server.jar', 'w', zipfile.ZIP_DEFLATED) as modified:
        for item in original_jar.infolist():
            modified.writestr(item, replacements.pop(item.filename, original_jar.read(item.filename)))
        for name, data in replacements.items():
            modified.writestr(name, data)
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    (output / 'build.json').write_text(json.dumps(dict(
        inputs={str(p.resolve()): digest(p) for p in (jar, settings_path, server_path, Path(__file__))},
        outputs={str(p.relative_to(output)): digest(p) for p in output.rglob('*') if p.is_file()},
        changes=['Local PC', 'per-game telemetry preserved', 'evaluation write data never redispatched']), indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--template', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--javac', default='javac')
    build(**vars(parser.parse_args()))
