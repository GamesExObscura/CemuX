"""Compare opt-in emulator speaker samples with host HID submissions, not microphone audio."""
import argparse
from collections import defaultdict, deque
import json
import math
from pathlib import Path
import struct
import wave

HEADER = struct.Struct('<IHBBQQII')
FACTORS = (230, 230, 230, 230, 307, 409, 512, 614)


def decode(data, state):
    predictor, step = state
    samples = []
    for byte in data:
        for nibble in (byte >> 4, byte & 15):
            # Independent reference decode; C/C++ division truncates toward zero.
            delta = step * (2 * (nibble & 7) + 1) // 8
            predictor = min(32767, max(-32768, predictor + (-delta if nibble & 8 else delta)))
            step = min(24576, max(127, step * FACTORS[nibble & 7] // 256))
            samples.append(predictor)
    return samples, (predictor, step)


def parse_records(data, origin):
    records = []
    offset = 0
    while offset + HEADER.size <= len(data):
        magic, version, stage, flags, time, key, count, length = HEADER.unpack_from(data, offset)
        if magic != 0x314b5053 or version != 1 or stage not in (1, 2, 3, 4, 5):
            raise ValueError(f'Invalid record in {origin} at {offset}')
        offset += HEADER.size
        if length > len(data) - offset:
            raise ValueError(f'Truncated record in {origin} at {offset}')
        records.append(dict(stage=stage, flags=flags, time=time, key=key, count=count,
                            data=bytes(data[offset:offset + length]), origin=origin))
        offset += length
    if offset != len(data):
        raise ValueError(f'Trailing partial header in {origin}')
    return records


def read_records(root):
    records = []
    files = []
    for path in sorted(root.rglob('wiimote-speaker-*.spk')):
        records.extend(parse_records(path.read_bytes(), str(path.parent.relative_to(root))))
        files.append(str(path))
    return sorted(records, key=lambda r: r['time']), files


def quality(expected, actual):
    if len(expected) != len(actual):
        raise ValueError('Unequal reference/output sample lengths')
    count = len(expected)
    signal = sum(x * x for x in expected)
    error = sum((x - y) ** 2 for x, y in zip(expected, actual))
    return dict(samples=count, peak=max(map(abs, expected), default=0),
                source_rms=math.sqrt(signal / count) if count else 0,
                error_rms=math.sqrt(error / count) if count else 0,
                snr_db=10 * math.log10(signal / error) if signal and error else None,
                clipped_source_samples=sum(x in (-32768, 32767) for x in expected))


def inspect(records):
    sources = defaultdict(lambda: dict(pcm=[], decoded=[], state_mismatches=0, calls=0, first_flags=0))
    devices = defaultdict(lambda: dict(queued=deque(), expected=[], decoded=[], audio=[], gaps=[],
                                      latencies=[], writes=[], rejections=0, writes_failed=0,
                                      matched=0, unmatched=0, state_mismatches=0, state=(0, 127), last=None,
                                      last_start=None, start_gaps=[], audio_writes=[], clusters=[]))
    latest_source = {}
    for record in records:
        stage, data, key, origin = record['stage'], record['data'], record['key'], record['origin']
        if stage == 1:
            count = record['count']
            if len(data) != 16 + count * 2 + (count + 1) // 2:
                raise ValueError('Bad source payload length')
            predictor, step, after_predictor, after_step = struct.unpack_from('<iiii', data)
            pcm = list(struct.unpack_from('>' + 'h' * count, data, 16))
            encoded = data[16 + count * 2:]
            decoded, after = decode(encoded, (predictor, step))
            stream = sources[(origin, key)]
            stream['calls'] += 1
            stream['first_flags'] += record['flags'] == 0
            stream['state_mismatches'] += count % 2 == 0 and after != (after_predictor, after_step)
            stream['pcm'].extend(pcm)
            stream['decoded'].extend(decoded[:count])
            latest_source[(origin, encoded)] = dict(pcm=pcm, state=(predictor, step), key=key)
        elif stage == 2:
            device = devices[(origin, key)]
            if record['flags']:
                device['queued'].append((record['time'], data, latest_source.get((origin, data))))
            else:
                device['rejections'] += 1
        elif stage == 3:
            device = devices[(origin, key)]
            device['writes'].append(record['count'])
            if not record['flags']:
                device['writes_failed'] += 1
                continue
            if len(data) < 2:
                continue
            if data[0] == 0x14 and data[1] & 4:
                device['state'] = (0, 127)
            if data[0] != 0x18:
                continue
            length = data[1] >> 3
            if not 0 < length <= 20 or len(data) < length + 2:
                raise ValueError('Malformed HID speaker packet')
            encoded = data[2:length + 2]
            submitted = record['time'] - record['count']
            device['audio_writes'].append(record['count'])
            if device['last_start'] is not None:
                device['start_gaps'].append(submitted - device['last_start'])
            if device['last_start'] is None or submitted - device['last_start'] >= 100000:
                device['clusters'].append(dict(first_us=submitted, last_us=submitted, samples=0, last_samples=length*2))
            cluster = device['clusters'][-1]
            cluster['last_us'] = submitted
            cluster['samples'] += length*2
            cluster['last_samples'] = length*2
            device['last_start'] = submitted
            match = None
            if device['queued'] and device['queued'][0][1] == encoded:
                match = device['queued'].popleft()
            else:
                device['unmatched'] += 1
            before = device['state']
            decoded, device['state'] = decode(encoded, before)
            device['audio'].extend(decoded)
            if match:
                device['latencies'].append(record['time'] - match[0])
                if match[2] and len(match[2]['pcm']) == len(decoded):
                    device['matched'] += 1
                    device['state_mismatches'] += before != match[2]['state']
                    device['expected'].extend(match[2]['pcm'])
                    device['decoded'].extend(decoded)
            if device['last'] is not None:
                device['gaps'].append(record['time'] - device['last'])
            device['last'] = record['time']
    return sources, devices


def write_wave(path, samples):
    with wave.open(str(path), 'wb') as output:
        output.setparams((1, 2, 6000, len(samples), 'NONE', 'not compressed'))
        output.writeframes(struct.pack('<' + 'h' * len(samples), *samples))


def report(root, output):
    records, files = read_records(root)
    if not files:
        raise ValueError('No .spk files yet. Close the whole private Cemu normally to flush the in-memory trace.')
    sources, devices = inspect(records)
    output.mkdir(parents=True, exist_ok=True)
    timing = [struct.unpack('<QQQ', r['data']) for r in records if r['stage'] == 4]
    probes = [r for r in records if r['stage'] == 5]
    result = dict(trace_files=files, source_streams=[], devices=[],
                  discovery_probes=dict(records=len(probes), failed=sum(not r['flags'] for r in probes),
                      max_write_ms=max((r['count'] for r in probes), default=0)/1000,
                      total_write_ms=sum(r['count'] for r in probes)/1000),
                  writer_timing=dict(records=len(timing), **{
                      name: dict(max_ms=max((r[index] for r in timing), default=0)/1000,
                                 median_ms=sorted(r[index] for r in timing)[len(timing)//2]/1000 if timing else 0)
                      for index, name in enumerate(('previous_wait', 'registry_lock', 'selection'))}),
                  stage_counts={str(stage):sum(r['stage']==stage for r in records) for stage in (1,2,3,4,5)},
                  hid_trace_missing=not any(r['stage']==3 for r in records),
                  caveat='HID submission success is not proof of over-the-air reception. Gaps can include normal pauses between cues.')
    for number, ((origin, key), stream) in enumerate(sources.items()):
        name = f'source-{number}'
        write_wave(output / f'{name}-pcm.wav', stream['pcm'])
        write_wave(output / f'{name}-codec-loopback.wav', stream['decoded'])
        result['source_streams'].append(dict(origin=origin, state_address=hex(key), calls=stream['calls'],
            first_flags=stream['first_flags'], encoder_state_mismatches=stream['state_mismatches'],
            **quality(stream['pcm'], stream['decoded']), pcm_file=f'{name}-pcm.wav', codec_file=f'{name}-codec-loopback.wav'))
    for number, ((origin, key), device) in enumerate(devices.items()):
        if not device['audio'] and not device['rejections'] and not device['queued']:
            continue
        name = f'device-{number}-hid-decoded.wav'
        if device['audio']: write_wave(output / name, device['audio'])
        active_gaps = [gap for gap in device['gaps'] if gap < 100000]
        start_gaps = [gap for gap in device['start_gaps'] if gap < 100000]
        audio_writes = sorted(device['audio_writes'])
        result['devices'].append(dict(origin=origin, device=key, matching_source_packets=device['matched'],
            unmatched_hid_packets=device['unmatched'], queued_without_matching_hid=len(device['queued']),
            rejected_before_queue=device['rejections'], failed_hid_writes=device['writes_failed'],
            decoder_state_mismatches=device['state_mismatches'],
            minimum_gap_ms=min(active_gaps, default=0) / 1000, maximum_gap_under_100ms=max(active_gaps, default=0) / 1000,
            minimum_submission_gap_ms=min(start_gaps, default=0) / 1000,
            maximum_submission_gap_under_100ms=max(start_gaps, default=0) / 1000,
            median_audio_write_ms=audio_writes[len(audio_writes)//2] / 1000 if audio_writes else 0,
            audio_clusters=[dict(samples=c['samples'], sample_duration_ms=c['samples']/6,
                                submission_span_ms=(c['last_us']-c['first_us'])/1000,
                                paced_duration_ms=(c['last_us']-c['first_us'])/1000+c['last_samples']/6)
                            for c in device['clusters']],
            gaps_under_3ms=sum(gap < 3000 for gap in active_gaps), maximum_queue_latency_ms=max(device['latencies'], default=0) / 1000,
            maximum_write_ms=max(device['writes'], default=0) / 1000, **quality(device['expected'], device['decoded']),
            hid_file=name if device['audio'] else None))
    (output / 'report.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def self_test():
    records = []
    state = (0, 127)
    for packet in range(12):
        encoded = bytes((i * 17 + packet * 3) & 255 for i in range(20))
        before = state
        pcm, state = decode(encoded, state)
        source = struct.pack('<iiii', *before, *state) + struct.pack('>' + 'h' * 40, *pcm) + encoded
        records.append(dict(stage=1, flags=packet != 0, time=packet * 6667, key=99, count=40, data=source, origin='parent'))
        records.append(dict(stage=2, flags=packet != 4, time=packet * 6667 + 100, key=0, count=0, data=encoded, origin='parent'))
        if packet != 4:
            records.append(dict(stage=3, flags=1, time=packet * 6667 + 300, key=0, count=50,
                                data=bytes((0x18, 160)) + encoded, origin='parent'))
    raw = bytearray()
    for r in sorted(records, key=lambda r: r['time']):
        raw.extend(HEADER.pack(0x314b5053, 1, r['stage'], r['flags'], r['time'], r['key'], r['count'], len(r['data'])))
        raw.extend(r['data'])
    sources, devices = inspect(parse_records(raw, 'parent'))
    stream = sources[('parent', 99)]
    assert quality(stream['pcm'], stream['decoded'])['error_rms'] == 0
    assert stream['state_mismatches'] == 0
    device = devices[('parent', 0)]
    assert device['rejections'] == 1 and device['matched'] == 11
    assert device['state_mismatches'] > 0 and quality(device['expected'], device['decoded'])['error_rms'] > 0
    assert device['writes_failed'] == 0 and len(device['queued']) == 0
    assert max(device['gaps']) > 13000
    records.append(dict(stage=4, flags=1, time=999999, key=0, count=0,
                        data=struct.pack('<QQQ', 24, 5000, 100), origin='parent'))
    records.append(dict(stage=5, flags=0, time=999999, key=99, count=22000,
                        data=bytes((0x15,0)), origin='parent'))
    _, timed_devices = inspect(records)
    assert timed_devices[('parent', 0)]['writes'] == device['writes']
    print('PASS synthetic source/codec/HID correlation and deliberate dropped-packet detection')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.root and args.output:
        print(json.dumps(report(args.root, args.output), indent=2))
    else:
        parser.error('Provide ROOT --output DIRECTORY, or --self-test')
