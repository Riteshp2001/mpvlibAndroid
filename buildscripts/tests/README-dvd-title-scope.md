# DVD title and PGC scope regression

This regression compiles the full patched libdvdnav VM and libdvdread reader.
It uses an explicit local DVD09 ISO and a manifest containing its SHA-256 and
ISO file extent records. It does not download media or require an Android device.

Generate fixtures, then compile and run the full native test:

```sh
python3 tests/create-dvd-title-fixtures.py --image /path/to/DVD09.iso --manifest /path/to/DVD09-manifest.json --output /path/to/fixtures
python3 tests/test-libdvdnav-title-scope.py /path/to/patched-libdvdnav --dvdread-source /path/to/patched-libdvdread --fixtures /path/to/fixtures
```

Use `--cc` to select a compiler. MSVC requires a developer command prompt.
The source tree must include the producer libdvdnav patches in
`include/download-deps.sh`, and the ISO9660 libdvdread patch.
No official library version is changed.

## Coordinate contract

- A LINEAR layout follows authored next-PGC links and provides whole-title time.
  Commands, repeated PGCs, indefinite holds, and unequal angle durations are not
  represented as a fixed route. They remain playable through the VM.
- A current PGC has an independent selected-angle coordinate, duration and
  authored PTT table, including when a fixed whole-title route is unsupported.
  Its identity is title, VTS, PGC and angle; program is a cursor.
- Capture the raw position immediately after each block on the serialized VM
  owner. Store it with prefetched data. Reading the live VM at dequeue time does
  not describe the buffered block.
- The caller owns disc and presentation epoch validation. A seek may target the
  still-presented PGC after the VM has prefetched the next PGC. The candidate VM
  re-enters that PGC, verifies title/VTS/PGC/angle, proves a NAV floor landing,
  and commits only on success. Errors preserve the live VM and landing state.
- Time and authored chapter jumps may resume a read-ahead STOP while the title,
  VTS, angle and title VOB still belong to the presented scope. Chapter jumps
  execute the official PTT command on a candidate VM, without resetting to First
  Play. Failure preserves the live VM, started/stopped flags and pending data.
  These APIs do not permit a stale title seek after the live VM has entered a
  menu domain; presentation ownership across that transition remains separate.
- A successful authored chapter jump transfers the reader to the new program,
  including clearing the prior presentation's still and cell-time cursor. A
  rejected jump preserves both. Time seeking during an authored still remains
  prohibited. The continuous finite/infinite still fixtures exercise these
  contracts against the real VM and require new payload from chapter three.
- Existing public `dvdnav_get_current_time` and `dvdnav_time_search_floor` retain
  their original clock contract. The new scope API selects the internal angle
  clock mode of the same verified NAV algorithm.

## Evidence boundaries

The 17-second variants author IFO metadata over existing DVD09 VOB bytes; they
prove parser, VM traversal and mapping, not 17-second audiovisual playback.
The separate 60-second continuous-PTS fixtures retain the four original
15-second cells and coded payload; they verify actual NAV seek delivery.
The unequal-angle fixture limits the original NAV ranges to authored 5/7-second
angle members; it verifies the long-angle floor and selected-angle clock, not
seamless ILVU playback on a device.

The suite also covers command/loop rejection of fixed layouts, retained PGC
chapters, read failure rollback, stale angle scope, prefetch ahead of the
presented scope, and real NAV/payload delivery after STOP recovery. Decoder output,
audio, UI, and packaged native artifacts require
separate integration tests.

## Emitted block replay

Run the separate extent regression against the same patched source and fixtures:

```sh
python3 tests/test-libdvdnav-block-replay.py /path/to/patched-libdvdnav --dvdread-source /path/to/patched-libdvdread --fixtures /path/to/fixtures
```

This captures each emitted block's immutable VTS, menu/title VOB domain and VOB
sector. An independent read handle must reproduce earlier payload while the VM
is waiting for presentation and after it has stopped, including selected-angle
reads. The regression checks callback I/O and unchanged VM registers, hop state,
pending WAIT and current navigation file. No VM command or navigation seek is
used for byte replay. The producer applies the block-replay patch after its
stream-attributes patch; Media3's pinned archives use the same replay patch
without the mpv-specific stream-attributes patch.
