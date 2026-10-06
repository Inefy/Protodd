"""Generate isolated UMS engine fixtures from the bundled BWAPI test terrain.

Requires mpyq and dclimplode in the local validation environment. No playing
runtime, trained model or tournament package is modified.
"""
from pathlib import Path
import struct
import hashlib
import json
import shutil
import zipfile
import mpyq
import dclimplode

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/audit-validation-20260926'

def chk_source():
    path = ROOT / 'build/_deps/bwapi-src/bwapi/TestAIModule/maps/TestModule/ProtossTest.scm'
    archive = mpyq.MPQArchive(str(path), listfile=False)
    block = archive.block_table[archive.get_hash_table_entry('staredit\\scenario.chk').block_table_index]
    archive.file.seek(block.offset)
    data = archive.file.read(block.archived_size)
    key = archive._hash('scenario.chk', 'TABLE')
    count = (block.size + 4095) // 4096
    offsets = struct.unpack('<' + str(count+1) + 'I', archive._decrypt(data[:4*(count+1)], key-1))
    parts = []
    for i in range(count):
        part = data[offsets[i]:offsets[i+1]]
        part = archive._decrypt(part, key+i) + part[len(part)//4*4:]
        if len(part) < min(4096, block.size-i*4096):
            assert part[0] == 8
            part = dclimplode.decompressobj().decompress(part[1:])
        parts.append(part)
    data = b''.join(parts)
    assert len(data) == block.size
    tags = {}
    at = 0
    while at < len(data):
        tag, length = struct.unpack_from('<4sI', data, at)
        tags[tag] = bytearray(data[at+8:at+8+length]); at += 8+length
    return archive, tags, path

def mpq_write(path, chk, archive):
    def encrypt(data, key):
        seed1, seed2 = key, 0xEEEEEEEE
        result = bytearray()
        for (plain,) in struct.iter_unpack('<I', data):
            seed2 = (seed2 + archive.encryption_table[0x400+(seed1 & 255)]) & 0xffffffff
            result += struct.pack('<I', plain ^ ((seed1+seed2) & 0xffffffff))
            seed1 = (((~seed1 << 21)+0x11111111) | (seed1 >> 11)) & 0xffffffff
            seed2 = (plain+seed2+(seed2 << 5)+3) & 0xffffffff
        return result
    files = [('staredit\\scenario.chk', chk), ('(listfile)', b'staredit\\scenario.chk\r\n')]
    body, blocks = bytearray(), bytearray()
    hashes = [b'\xff'*16 for _ in range(16)]
    for index, (name, data) in enumerate(files):
        blocks += struct.pack('<IIII', 32+len(body), len(data), len(data), 0x81000000)
        body += data
        slot = archive._hash(name, 'TABLE_OFFSET') % 16
        while hashes[slot] != b'\xff'*16: slot = (slot+1) % 16
        hashes[slot] = struct.pack('<IIHHI', archive._hash(name, 'HASH_A'), archive._hash(name, 'HASH_B'), 0, 0, index)
    hash_at = 32+len(body); block_at = hash_at+256
    header = struct.pack('<4sIIHHIIII', b'MPQ\x1a', 32, block_at+len(blocks), 0, 3, hash_at, block_at, 16, len(files))
    path.write_bytes(header+body+encrypt(b''.join(hashes), archive._hash('(hash table)', 'TABLE'))+encrypt(blocks, archive._hash('(block table)', 'TABLE')))
    assert mpyq.MPQArchive(str(path), listfile=False).read_file('staredit\\scenario.chk') == chk

def make_map(name):
    archive, tags, source = chk_source()
    units = []
    def unit(kind, x, y, owner=0, flags=0, hp=100, shields=100, resources=0,
             valid_state=31, valid_relation=15, energy=100):
        units.append(struct.pack('<IHHHHHHBBBBIHHII',len(units)+1,x,y,kind,0,
                                 valid_state,valid_relation,owner,hp,shields,energy,
                                 resources,0,flags,0,0))
    if not name.startswith('racebot-'):
        unit(214, 512, 512); unit(214, 3500, 3500, 1)
        unit(154, 512, 528); unit(131, 3504, 3504, 1)
    if name.startswith('storm'):
        unit(67,800,1000)
        for i in range(4): unit(43,1000+8*i,1000,1)
        if name=='storm-allies':
            unit(72,1000,1000)
            for i in range(20): unit(64, 976+(i%5)*16, 970+(i//5)*16)
    elif name=='producer':
        unit(166,1008,1056) # powered; BWAPI assigns reverse discovery IDs here
        unit(166,400,1056)  # lower-ID, unpowered
        unit(156,1024,928)
    elif name=='producer-pair':
        # The second Gateway is discovered first and receives the lower unit
        # ID. The live fixture starts it, then tests the finishing queue slot
        # against the other idle Gateway.
        unit(160,1008,1056)
        unit(160,800,1056)
        unit(156,1024,928)
        unit(156,832,928)
    elif name=='cannon-blocker':
        # A compact powered home base with a legal Probe producer. Neutral
        # mineral fields tessellate the entire powered Cannon search region so
        # the stock engine and bridge must report a real placement failure.
        unit(156,640,640)
        unit(160,800,640)
        unit(166,544,672)
        unit(64,704,736)
        footprints=[(16,16,4,3),(20,20,2,2),(25,20,4,3),(17,21,3,2),(22,23,1,1)]
        for tile_y in range(10,31):
            for tile_x in range(10,30,2):
                overlaps=any(tile_x < left+width and left < tile_x+2 and
                             tile_y < top+height and top < tile_y+1
                             for left,top,width,height in footprints)
                if not overlaps:
                    unit(176,tile_x*32,tile_y*32,owner=11,resources=1500)
    elif name=='resource-overlap':
        # Independent powered producers let the audit overlap native macro,
        # detector, model-path, upgrade and ammunition spending in one frame.
        unit(156,640,640); unit(156,832,640)
        unit(160,736,544); unit(160,736,704)
        unit(164,640,736)
        unit(155,864,544); unit(159,864,704)
        unit(83,1008,800) # Reaver starts without Scarabs for maintenance.
        unit(64,640,544) # Probe builder for the supply Pylon.
    elif name=='build-cancel':
        # A visible remote mineral site makes the first Nexus command an
        # engine Build order while the Probe is still travelling. The audit
        # rejects the first Stop through the adapter seam, then observes the
        # stock engine accept a retry and clear the old order.
        unit(64,640,544)
        unit(84,2050,2000) # Reveal the remote Nexus footprint before issue.
        for x,y in [(1984,1952),(2024,1952),(2064,1952),(2104,1952),
                    (2004,2032),(2044,2032),(2084,2032)]:
            unit(176,x,y,owner=11,resources=1500)
    elif name=='worker-issuer':
        # An idle Probe and visible nearby Zergling isolate worker defense.
        unit(64,640,544)
        unit(37,704,576,owner=1)
    elif name=='worker-local-defense':
        # Two simultaneous mineral-line attacks exercise per-base coverage in
        # the stock engine. The main has a two-Zealot screen; the natural does
        # not, so only its local Probes should receive accepted attack orders.
        unit(65,560,512); unit(65,590,512)
        unit(37,660,512,owner=1); unit(37,700,512,owner=1)
        unit(154,2050,2000) # Protoss Nexus at the remote mineral cluster.
        for x,y in [(1984,1952),(2024,1952),(2064,1952),(2104,1952),
                    (2004,2032),(2044,2032),(2084,2032)]:
            unit(176,x,y,owner=11,resources=1500)
        unit(37,2160,2000,owner=1); unit(37,2200,2000,owner=1)
        for i in range(16):
            unit(64,480+(i%5)*20,640+(i//5)*32)
            unit(64,1990+(i%5)*24,2200+(i//5)*30)
    elif name=='worker-mining':
        # Match the source UMS mineral field's validity and zero shield/energy
        # fields so BWAPI preserves its initial resource count.
        unit(64,640,544)
        unit(176,704,576,owner=11,hp=100,shields=0,resources=1500,
             valid_state=16,valid_relation=18,energy=0)
    elif name in ('racebot-worker-defense','racebot-combat'):
        # RaceBot fixtures select Terran for player 0 and place a visible
        # Zergling close enough to the home CC to exercise defense issuers.
        unit(106,512,512) # Terran Command Center
        unit(7,640,544) # Terran SCV
        unit(131,3504,3504,owner=1) # Zerg Hatchery
        unit(37,704,576,owner=1) # Visible Zergling threat
        if name=='racebot-combat':
            for i in range(4): unit(0,800+16*i,544) # Terran Marines
    elif name=='command-suppression':
        unit(65,800,800) # Zealot pursues the visible low-health Marine.
        unit(0,960,800,owner=1,hp=1)
    elif name=='observer-safety':
        unit(84,800,800) # BWAPI unit-type enum value for Protoss_Observer.
        unit(8,864,800,owner=1) # Terran Wraith supplies real air-weapon pressure.
    elif name=='pylon-loss':
        unit(160,1008,1056)
        unit(164,928,992)
        unit(156,1024,928)
        unit(156,512,640) # Home Pylon preserves enough supply for the seeded Zealot.
        unit(64,640,640)
        for i in range(8): unit(65,944+(i%4)*32,1056+(i//4)*32)
        for i in range(12): unit(37,1200+(i%4)*16,928+(i//4)*16,1)
    elif name=='prerequisite':
        unit(166,1008,1056); unit(156,1024,928); unit(156,672,704)
        unit(160,784,800); unit(164,784,960); unit(163,1008,800)
        unit(64,640,640)
    elif name=='power-recovery':
        # The completed production and technology structures start without
        # either covering Pylon. The stock engine must expose the lost power,
        # and the bot must restore it through its ordinary macro path.
        unit(166,1008,1056); unit(160,784,800); unit(164,784,960)
        unit(163,1008,800); unit(64,640,640)
    elif name=='combat':
        unit(65,800,1000)
        unit(0,880,1000,1,16) # invincible close Marine
        unit(0,1200,1000,1)
        unit(84,1160,1100) # far-target visibility; not near the cloaked fixture
        unit(154,2512,2512)
        unit(61,2600,2500,1,1)
    elif name=='load':
        for i in range(80): unit(65,800+(i%10)*36,1000+(i//10)*36)
        for i in range(12): unit(66,660+(i%3)*44,1000+(i//3)*44)
        for i in range(4): unit(67,730,1030+i*40)
        for i in range(8): unit(64,500+(i%4)*28,720+(i//4)*28)
        for i in range(20): unit(156,320+(i%4)*96,1500+(i//4)*96)
        for i in range(100): unit(37,1200+(i%10)*24,1000+(i//10)*24,1)
        for i in range(20): unit(38,1480+(i%4)*32,1000+(i//4)*32,1)
        for i in range(8): unit(43,1300+(i%4)*48,1300+(i//4)*48,1)
    tags[b'UNIT']=b''.join(units)
    tags[b'OWNR']=tags[b'IOWN']=bytes([6,5]+[0]*10)
    side=bytearray([2,0]+[7]*10)
    if name.startswith('racebot-'): side[0]=1
    tags[b'SIDE']=bytes(side)
    # Separate forces and disable the template's random player assignment,
    # shared vision, allied victory and alliance flags.
    tags[b'FORC']=bytes([0,1,2,2,2,2,2,2])+bytes(12)
    tags[b'PUNI']=bytes([1])*5700
    for tag,count in [(b'UNIS',228),(b'UNIx',228),(b'UPGS',46),(b'UPGx',61),(b'TECS',24),(b'TECx',44)]:
        tags[tag][:count]=bytes([1])*count
    for tag,count in [(b'PTEC',24),(b'PTEx',44)]:
        researched=bytearray(count)
        if name.startswith('storm') or name=='load': researched[19]=1
        tags[tag]=bytes([1])*(12*count)+bytes(12*count)+bytes([1])*count+researched+bytes([1])*(12*count)
    for tag,count in [(b'UPGR',46),(b'PUPx',61)]:
        levels=bytearray(count)
        if name=='prerequisite': levels[13]=1
        tags[tag]=bytes([3])*(12*count)+bytes(12*count)+bytes([3])*count+levels+bytes([1])*(12*count)
    trigger=bytearray(2400); trigger[15]=22 # Always
    struct.pack_into('<IIIIIIHBBB3x', trigger, 320, 0,0,0,0,0,5000,2,26,7,0)
    trigger[2372]=1
    tags[b'TRIG']=trigger
    data=b''.join(tag+struct.pack('<I',len(value))+value for tag,value in tags.items())
    path=OUT/'maps'/f'{name}.scx'; path.parent.mkdir(parents=True,exist_ok=True)
    mpq_write(path,data,archive)
    return {'map':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'template':str(source),'template_sha256':hashlib.sha256(source.read_bytes()).hexdigest()}

def runtime():
    target=OUT/'runtime'; target.mkdir(parents=True,exist_ok=True)
    for path in (ROOT/'build/match-runtime-a').iterdir():
        if path.is_file() and path.suffix.lower() in ('.exe','.dll','.mpq','.snp','.ini'):
            if not (target/path.name).exists(): shutil.copy2(path,target/path.name)
    with zipfile.ZipFile(ROOT/'build/pvz-available-composition-20260926/reference-a/server/required/Required_BWAPI_440.zip') as z:
        z.extractall(target)
    for suffix in ('mpc','spc'): shutil.copy2(target/f'characters/default.{suffix}',target/f'characters/AstraBot.{suffix}')
    shutil.copytree(OUT/'maps',target/'maps/audit',dirs_exist_ok=True)
    (target/'bwapi-data/logs').mkdir(exist_ok=True)
    return target

if __name__=='__main__':
    OUT.mkdir(parents=True,exist_ok=True)
    receipt=[make_map(name) for name in ['storm-allies','storm-clear','producer','producer-pair','cannon-blocker','resource-overlap','build-cancel','worker-issuer','worker-local-defense','worker-mining','racebot-worker-defense','racebot-combat','command-suppression','observer-safety','pylon-loss','prerequisite','power-recovery','combat','load']]
    (OUT/'map-receipt.json').write_text(json.dumps(receipt,indent=2))
    print(runtime())
