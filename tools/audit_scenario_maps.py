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
    def unit(kind, x, y, owner=0, flags=0):
        units.append(struct.pack('<IHHHHHHBBBBIHHII',len(units)+1,x,y,kind,0,31,15,owner,100,100,100,0,0,flags,0,0))
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
    elif name=='prerequisite':
        unit(166,1008,1056); unit(156,1024,928); unit(156,672,704)
        unit(160,784,800); unit(164,784,960); unit(163,1008,800)
        unit(64,640,640)
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
    tags[b'SIDE']=bytes([2,0]+[7]*10)
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
    receipt=[make_map(name) for name in ['storm-allies','storm-clear','producer','prerequisite','combat','load']]
    (OUT/'map-receipt.json').write_text(json.dumps(receipt,indent=2))
    print(runtime())
