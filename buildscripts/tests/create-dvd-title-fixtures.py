#!/usr/bin/env python3
"""Generate real-VM title fixtures from the pinned four-cell DVD09 ISO/manifest.

No downloads. The manifest contains the source SHA256 and ISO file block/size
entries. The 17s variants change metadata only. The separate 60s variants retain
the original cell durations and coded payload while normalizing PS/NAV clocks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', type=Path, required=True)
parser.add_argument('--manifest', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
ROOT = args.output.resolve()
ROOT.mkdir(parents=True, exist_ok=True)
NATIVE = ROOT
source = args.image.resolve()
manifest = json.loads(args.manifest.read_text())
image = bytearray(source.read_bytes())
assert hashlib.sha256(image).hexdigest() == manifest['sha256']
files = {item['path']: item for item in manifest['files']}
vts = files['/VIDEO_TS/VTS_01_0.IFO;1']
base = vts['block'] * 2048
u16 = lambda data, offset: struct.unpack_from('>H', data, offset)[0]
u32 = lambda data, offset: struct.unpack_from('>I', data, offset)[0]
put16 = lambda data, offset, value: struct.pack_into('>H', data, offset, value)
put32 = lambda data, offset, value: struct.pack_into('>I', data, offset, value)
original = bytes(image[base:base+vts['size']])
pgcit = u32(original, 0xcc) * 2048
ptt = u32(original, 0xc8) * 2048
pgc = pgcit + u32(original, pgcit+12)
assert original[pgc+2:pgc+4] == bytes([4,4])
playback = pgc + u16(original, pgc+232)
position = pgc + u16(original, pgc+234)
replacement = bytearray(24+304*2)
put16(replacement, 0, 2)
put32(replacement, 4, len(replacement)-1)
replacement[8] = 0x81
put32(replacement, 12, 24)
put32(replacement, 20, 328)
for part, durations in enumerate(((2,3),(5,7))):
    start = 24+part*304
    replacement[start:start+236] = original[pgc:pgc+236]
    replacement[start+2:start+4] = bytes([2,2])
    duration = sum(durations)
    replacement[start+4:start+8] = bytes([0,0,(duration//10)*16+duration%10,0x40])
    put16(replacement, start+0x9c, 2 if part == 0 else 0)
    put16(replacement, start+0x9e, 0 if part == 0 else 1)
    put16(replacement, start+0xa0, 0)
    put16(replacement, start+228, 0)
    put16(replacement, start+230, 236)
    put16(replacement, start+232, 240)
    put16(replacement, start+234, 288)
    replacement[start+236:start+238] = bytes([1,2])
    for cell, seconds in enumerate(durations):
        src = playback+(part*2+cell)*24
        dest = start+240+cell*24
        replacement[dest:dest+24] = original[src:src+24]
        replacement[dest+4:dest+8] = bytes([0,0,seconds,0x40])
        src = position+(part*2+cell)*4
        replacement[start+288+cell*4:start+292+cell*4] = original[src:src+4]
vtsi = bytearray(original)
assert pgcit+len(replacement) <= len(vtsi)
vtsi[pgcit:pgcit+len(replacement)] = replacement
entries = ptt + u32(vtsi, ptt+8)
for i, (number, program) in enumerate(((1,1),(1,2),(2,1),(2,2))):
    put16(vtsi, entries+i*4, number)
    put16(vtsi, entries+i*4+2, program)
for name in ('/VIDEO_TS/VTS_01_0.IFO;1','/VIDEO_TS/VTS_01_0.BUP;1'):
    entry = files[name]
    assert entry['size'] == len(vtsi)
    image[entry['block']*2048:entry['block']*2048+len(vtsi)] = vtsi
output = NATIVE/'two-pgc-dvd09.iso'
output.write_bytes(image)
(NATIVE/'VTS_01_0.IFO').write_bytes(vtsi)
result = {'source':str(source),'sourceSha256':manifest['sha256'],
          'path':str(output),'sha256':hashlib.sha256(image).hexdigest(),
          'pgcitOffset':pgcit,'pttOffset':ptt,
          'note':'Original authored VOB bytes unchanged. Changed PGC layout and IFO cell durations only; this probes VM flow/IFO metadata, not video timing.'}
(NATIVE/'fixture.json').write_bytes((json.dumps(result,indent=2)+'\n').replace('\n','\r\n').encode())
print(json.dumps(result,indent=2))

image = (ROOT/'two-pgc-dvd09.iso').read_bytes()
FIX = ROOT
base = files['/VIDEO_TS/VTS_01_0.IFO;1']['block'] * 2048
size = files['/VIDEO_TS/VTS_01_0.IFO;1']['size']
original = image[base:base+size]
u16 = lambda b, p: struct.unpack_from('>H', b, p)[0]
u32 = lambda b, p: struct.unpack_from('>I', b, p)[0]
p16 = lambda b, p, v: struct.pack_into('>H', b, p, v)
p32 = lambda b, p, v: struct.pack_into('>I', b, p, v)
pgcit = u32(original, 0xcc) * 2048
ptt = u32(original, 0xc8) * 2048
entry = ptt + u32(original, ptt+8)
pgc1 = pgcit + u32(original, pgcit+12)
pgc2 = pgcit + u32(original, pgcit+20)
fixtures = []

def save(name, edit=None, angles=1, parts=4):
    data = bytearray(image)
    vtsi = bytearray(original)
    if edit:
        edit(vtsi)
    for name_in_iso in ('/VIDEO_TS/VTS_01_0.IFO;1', '/VIDEO_TS/VTS_01_0.BUP;1'):
        item = files[name_in_iso]
        start = item['block'] * 2048
        data[start:start+size] = vtsi
    for name_in_iso in ('/VIDEO_TS/VIDEO_TS.IFO;1', '/VIDEO_TS/VIDEO_TS.BUP;1'):
        item = files[name_in_iso]
        start = item['block'] * 2048
        tt = start + u32(data, start+0xc4) * 2048 + 8
        # Author the title declaration consistently with this two-PGC route.
        # Multi-PGC is not itself proof of branching; actual links decide.
        data[tt] = 0x40
        data[tt+1] = angles
        p16(data, tt+2, parts)
    path = FIX / (name + '.iso')
    path.write_bytes(data)
    fixtures.append({'name': name, 'path': str(path),
                     'sha256': hashlib.sha256(data).hexdigest()})

save('linear')
save('loop', lambda b: p16(b, pgc2+0x9c, 1))
save('unreachable-ptt', lambda b: p16(b, pgc1+0x9c, 0))
save('random', lambda b: b.__setitem__(pgc1+0xa2, 1))
save('infinite-still', lambda b: b.__setitem__(pgc2+0xa3, 255))
save('finite-still', lambda b: b.__setitem__(pgc2+0xa3, 3))
save('entry-program-two', lambda b: p16(b, entry+2, 2))
save('invalid-next', lambda b: p16(b, pgc2+0x9c, 3))

def conditional(b):
    first = bytearray(b[pgc1:pgc1+304])
    first.extend(bytes.fromhex('0000 0001 0000 000f 20a4000000000001'))
    p16(first, 228, 304)
    second = b[pgc2:pgc2+304]
    header = bytearray(b[pgcit:pgcit+24])
    p32(header, 20, 24+len(first))
    replacement = header+first+second
    p32(replacement, 4, len(replacement)-1)
    b[pgcit:pgcit+len(replacement)] = replacement
save('conditional-post', conditional)

def angle(b, equal):
    b[pgc2+2] = 1
    b[pgc2+240] = (b[pgc2+240] & 0x0f) | 0x50
    b[pgc2+264] = (b[pgc2+264] & 0x0f) | 0xd0
    if equal:
        b[pgc2+264+4:pgc2+264+8] = b[pgc2+240+4:pgc2+240+8]
    p32(b, ptt+4, 23)
save('equal-angle', lambda b: angle(b, True), angles=2, parts=3)
save('unequal-angle', lambda b: angle(b, False), angles=2, parts=3)
save('missing-ptt-program', lambda b: p32(b, ptt+4, 23), parts=3)

def two_titles(b):
    p16(b, pgc1+0x9c, 0)
    b[pgcit+16] = 0x82
    p16(b, ptt, 2)
    p32(b, ptt+4, 31)
    p32(b, ptt+8, 16)
    p32(b, ptt+12, 24)
    for i, (pgc, program) in enumerate(((1,1),(1,2),(2,1),(2,2))):
        p16(b, ptt+16+i*4, pgc)
        p16(b, ptt+18+i*4, program)
save('two-titles', two_titles, parts=2)
two_titles_path = FIX/'two-titles.iso'
two_titles_image = bytearray(two_titles_path.read_bytes())
for name in ('/VIDEO_TS/VIDEO_TS.IFO;1', '/VIDEO_TS/VIDEO_TS.BUP;1'):
    start = files[name]['block']*2048
    tt = start + u32(two_titles_image, start+0xc4)*2048
    p16(two_titles_image, tt, 2)
    p32(two_titles_image, tt+4, 31)
    two_titles_image[tt+20:tt+32] = two_titles_image[tt+8:tt+20]
    two_titles_image[tt+27] = 2
two_titles_path.write_bytes(two_titles_image)
fixtures[-1]['sha256'] = hashlib.sha256(two_titles_image).hexdigest()

# Separate timed transport fixture: preserve original 15s cell geometry and
# coded audio/video payload. Retimestamp PS pack/PES and NAV presentation clocks
# by 15s per cell, producing continuous PTS across the two PGCs. NAV addresses,
# VOBU links and cell-local elapsed clocks remain untouched.
def real_durations(b):
    for pgc in (pgc1, pgc2):
        b[pgc+4:pgc+8] = bytes([0,0,0x30,0x40])
        for cell in (0,1):
            pos = pgc+240+24*cell
            b[pos] &= ~2  # stc_discontinuity
            b[pos+4:pos+8] = bytes([0,0,0x15,0x40])
save('continuous-pts', real_durations)
continuous_path = FIX/'continuous-pts.iso'
continuous = bytearray(continuous_path.read_bytes())
vob = files['/VIDEO_TS/VTS_01_1.VOB;1']
vob_base = vob['block'] * 2048
patch_counts = {'packs': 0, 'pesTimestamps': 0, 'navPackets': 0}

def timestamp5(pos, offset):
    value = ((continuous[pos] & 14) << 29) | (continuous[pos+1] << 22)
    value |= ((continuous[pos+2] & 254) << 14) | (continuous[pos+3] << 7) | (continuous[pos+4] >> 1)
    value = (value + offset) & ((1 << 33) - 1)
    continuous[pos] = (continuous[pos] & 0xf1) | ((value >> 29) & 14)
    continuous[pos+1] = (value >> 22) & 255
    continuous[pos+2] = ((value >> 14) & 254) | 1
    continuous[pos+3] = (value >> 7) & 255
    continuous[pos+4] = ((value << 1) & 254) | 1
    patch_counts['pesTimestamps'] += 1

for i in range(4):
    playback = (pgc1 if i < 2 else pgc2)+240+(i%2)*24
    first, last = u32(original, playback+8), u32(original, playback+20)
    offset = i * 15 * 90000
    for sector in range(first, last+1):
        pos = vob_base + sector*2048
        end = pos+2048
        assert continuous[pos:pos+4] == bytes.fromhex('000001ba')
        pack = int.from_bytes(continuous[pos+4:pos+10], 'big')
        scr = (((pack >> 43) & 7) << 30) | (((pack >> 27) & 0x7fff) << 15) | ((pack >> 11) & 0x7fff)
        scr = (scr + offset) & ((1 << 33)-1)
        mask = (7 << 43) | (0x7fff << 27) | (0x7fff << 11)
        pack = (pack & ~mask) | (((scr >> 30) & 7) << 43) | (((scr >> 15) & 0x7fff) << 27) | ((scr & 0x7fff) << 11)
        continuous[pos+4:pos+10] = pack.to_bytes(6, 'big')
        patch_counts['packs'] += 1
        pos += 14 + (continuous[pos+13] & 7)
        while pos+6 <= end and continuous[pos:pos+3] == b'\0\0\1':
            stream = continuous[pos+3]
            length = u16(continuous, pos+4)
            assert pos+6+length <= end
            if stream == 0xbf:
                subtype = continuous[pos+6]
                payload = pos+7
                if subtype == 0:
                    for field in (12,16,20):
                        value = u32(continuous, payload+field)
                        if value:
                            p32(continuous, payload+field, (value+offset) & 0xffffffff)
                    patch_counts['navPackets'] += 1
                elif subtype == 1:
                    p32(continuous, payload, (u32(continuous, payload)+offset) & 0xffffffff)
            elif stream == 0xbd or 0xc0 <= stream <= 0xef:
                assert continuous[pos+6] & 0xc0 == 0x80
                flags = continuous[pos+7] & 0xc0
                if flags & 0x80:
                    timestamp5(pos+9, offset)
                if flags == 0xc0:
                    timestamp5(pos+14, offset)
            pos += 6+length
continuous_path.write_bytes(continuous)
fixtures[-1]['sha256'] = hashlib.sha256(continuous).hexdigest()
fixtures[-1]['retimestamped'] = patch_counts


BASE = ROOT/'continuous-pts.iso'
base_bytes = BASE.read_bytes()
files = manifest['files']
files = {f['path']: f for f in files}
u32 = lambda b, p: struct.unpack_from('>I', b, p)[0]
p16 = lambda b, p, v: struct.pack_into('>H', b, p, v)
p32 = lambda b, p, v: struct.pack_into('>I', b, p, v)
manifest = {'base':str(BASE),'baseSha256':hashlib.sha256(base_bytes).hexdigest(),'fixtures':[]}
for fixture, command_table in (
    ('continuous-commands', '0000 0001 0000 000f 20a4000000000001'),
    ('continuous-nop', '0001 0000 0000 000f 0000000000000000')):
    source = bytearray(base_bytes)
    for name in ('/VIDEO_TS/VTS_01_0.IFO;1', '/VIDEO_TS/VTS_01_0.BUP;1'):
        start = files[name]['block'] * 2048
        size = files[name]['size']
        vtsi = bytearray(source[start:start+size])
        table = u32(vtsi, 0xcc)*2048
        pgc1 = table + u32(vtsi, table+12)
        pgc2 = table + u32(vtsi, table+20)
        first = bytearray(vtsi[pgc1:pgc1+304])
        first.extend(bytes.fromhex(command_table))
        p16(first, 228, 304)
        second = vtsi[pgc2:pgc2+304]
        header = bytearray(vtsi[table:table+24])
        p32(header, 20, 24+len(first))
        replacement = header+first+second
        p32(replacement, 4, len(replacement)-1)
        vtsi[table:table+len(replacement)] = replacement
        source[start:start+size] = vtsi
    out = ROOT/f'{fixture}.iso'
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(source)
    vob_hashes = {}
    for name, info in files.items():
        if '.VOB;' in name:
            start = info['block']*2048; end = start+info['size']
            assert source[start:end] == base_bytes[start:end]
            vob_hashes[name] = hashlib.sha256(source[start:end]).hexdigest()
    manifest['fixtures'].append({'path':str(out),'sha256':hashlib.sha256(source).hexdigest(),
        'commandTable':command_table,'unchangedVobSha256':vob_hashes,
        'description':'60s continuous PS/NAV payload unchanged; PGC1 command added; current PGCs each 30s with PTT1/2 then PTT3/4.'})
(ROOT/'fixture-manifest.json').write_bytes((json.dumps(manifest,indent=2)+'\n').replace('\n','\r\n').encode())
print('continuous-commands fixture ready')

input_files = {f['path']: f for f in json.loads(args.manifest.read_text())['files']}
(ROOT/'title-vob-offset.txt').write_text(str(input_files['/VIDEO_TS/VTS_01_1.VOB;1']['block'] * 2048))

data = bytearray((ROOT/'unequal-angle.iso').read_bytes())
files = {f['path']:f for f in json.loads(args.manifest.read_text())['files']}
u32 = lambda b,p:struct.unpack_from('>I',b,p)[0]
p32 = lambda b,p,v:struct.pack_into('>I',b,p,v)
base = files['/VIDEO_TS/VTS_01_0.IFO;1']['block']*2048
table = base + u32(data,base+0xcc)*2048
pgc = table + u32(data,table+20)
admap = base + u32(data,base+0xe4)*2048
addresses = [u32(data,p) for p in range(admap+4,admap+u32(data,admap)+1,4)]
vob = files['/VIDEO_TS/VTS_01_1.VOB;1']['block']*2048
def elapsed(sector):
    t = data[vob+sector*2048+1031+28:vob+sector*2048+1031+32]
    bcd=lambda v:(v>>4)*10+(v&15)
    return bcd(t[0])*3600+bcd(t[1])*60+bcd(t[2])+bcd(t[3]&63)/25
for index,duration in enumerate((5,7)):
    cell=pgc+240+index*24
    first,last=u32(data,cell+8),u32(data,cell+20)
    navs=[s for s in addresses if first<=s<=last]
    retained=[s for s in navs if elapsed(s)<duration]
    next_sector=next(s for s in navs if elapsed(s)>=duration)
    for name in ('/VIDEO_TS/VTS_01_0.IFO;1','/VIDEO_TS/VTS_01_0.BUP;1'):
        at=files[name]['block']*2048+(cell-base)
        p32(data,at+16,retained[-1]);p32(data,at+20,next_sector-1)
out=ROOT/'unequal-angle-nav.iso';out.write_bytes(data)
print(out)

data=bytearray((ROOT/'unequal-angle-nav.iso').read_bytes())
files={f['path']:f for f in json.loads(args.manifest.read_text())['files']}
u32=lambda b,p:struct.unpack_from('>I',b,p)[0]
p16=lambda b,p,v:struct.pack_into('>H',b,p,v)
p32=lambda b,p,v:struct.pack_into('>I',b,p,v)
for name in ('/VIDEO_TS/VTS_01_0.IFO;1','/VIDEO_TS/VTS_01_0.BUP;1'):
    base=files[name]['block']*2048
    table=base+u32(data,base+0xcc)*2048
    first=table+u32(data,table+12); pgc=table+u32(data,table+20)
    header=bytearray(data[pgc:pgc+240]);header[2]=2;header[3]=3;header[236:238]=b'\x01\x03'
    p16(header,234,312)
    cells=data[pgc+240:pgc+288]+data[first+240:first+264]
    positions=data[pgc+288:pgc+296]+data[first+288:first+292]
    body=header+cells+positions
    data[pgc:pgc+len(body)]=body
    p32(data,table+4,pgc+len(body)-table-1)
    ptt=base+u32(data,base+0xc8)*2048;p32(data,ptt+4,27)
for name in ('/VIDEO_TS/VIDEO_TS.IFO;1','/VIDEO_TS/VIDEO_TS.BUP;1'):
    base=files[name]['block']*2048;tt=base+u32(data,base+0xc4)*2048+8;p16(data,tt+2,4)
out=ROOT/'unequal-angle-followed.iso';out.write_bytes(data);print(out)

# Authored post commands branch on presented stream selections. VOB is unchanged.
data = bytearray((ROOT/'continuous-nop.iso').read_bytes())
commands = bytes.fromhex(
    '7300000f00010000'  # gprm15 += 1, proving one execution after ACK.
    '20a4008100060001'  # if sprm1 (audio) == 6: LinkPGCN 1.
    '20a4008200410001'  # if sprm2 (subtitle) == visible logical1: LinkPGCN 1.
    '3001000000000000') # Otherwise Exit.
for name in ('/VIDEO_TS/VTS_01_0.IFO;1', '/VIDEO_TS/VTS_01_0.BUP;1'):
    base = files[name]['block']*2048
    table = base + u32(data, base+0xcc)*2048
    pgcs = [table+u32(data, table+12), table+u32(data, table+20)]
    attribute = bytes(data[base+0x204:base+0x20c])
    data[base+0x203] = 7
    for logical in range(7):
        at=base+0x204+logical*8
        data[at:at+8]=attribute
        data[at]=(data[at]&~0x0c)|4
        data[at+2:at+4]=b'ja' if logical==6 else b'en'
        for pgc in pgcs:
            p16(data, pgc+12+logical*2, 0x8000 if logical in (0,6) else 0)
    data[base+0x255]=2
    for logical in range(2):
        at=base+0x256+logical*6
        data[at:at+6]=bytes([1,0])+ (b'en' if logical==0 else b'ja') + bytes(2)
        for pgc in pgcs:
            p32(data, pgc+28+logical*4, 0x80000000)
    pgc=pgcs[1];at=pgc+304
    p16(data, pgc+228, 304)
    data[at:at+8]=struct.pack('>HHHH',0,4,0,8+len(commands)-1)
    data[at+8:at+8+len(commands)]=commands
    p32(data,table+4,at+8+len(commands)-table-1)
(ROOT/'stream-command-branch.iso').write_bytes(data)
print('stream-command-branch.iso: original VOB, sparse audio aliases and subtitle registers branch at PGC2 end')

# A cell command with no link still observes a presentation boundary once.
data=bytearray((ROOT/'continuous-nop.iso').read_bytes())
for name in ('/VIDEO_TS/VTS_01_0.IFO;1','/VIDEO_TS/VTS_01_0.BUP;1'):
    base=files[name]['block']*2048
    table=base+u32(data,base+0xcc)*2048
    first=table+u32(data,table+12);second=table+u32(data,table+20)
    pgc1=bytearray(data[first:first+304]);pgc1[243]=1
    p16(pgc1,228,304)
    command_table=struct.pack('>HHHH',1,0,1,23)+bytes(8)+bytes.fromhex('7300000e00010000')
    pgc2=data[second:second+304]
    header=bytearray(data[table:table+24]);p32(header,20,24+len(pgc1)+len(command_table))
    content=header+pgc1+command_table+pgc2;p32(content,4,len(content)-1)
    data[table:table+len(content)]=content
(ROOT/'cell-command-nop.iso').write_bytes(data)
print('cell-command-nop.iso: plain cell command increments gprm14 once')

# Device presentation tests need matching IFO, NAV and elementary-stream clocks.
# Change only the final PGC's authored hold, retaining the real sixty-second VOB.
for name, still_seconds in (('continuous-finite-still', 3),
                            ('continuous-infinite-still', 255)):
    data = bytearray((ROOT/'continuous-pts.iso').read_bytes())
    for file_name in ('/VIDEO_TS/VTS_01_0.IFO;1', '/VIDEO_TS/VTS_01_0.BUP;1'):
        base = files[file_name]['block'] * 2048
        table = base + u32(data, base + 0xcc) * 2048
        last_pgc = table + u32(data, table + 20)
        data[last_pgc + 0xa3] = still_seconds
    (ROOT/(name + '.iso')).write_bytes(data)
    print(f'{name}.iso: original continuous VOB, final PGC still {still_seconds}')
