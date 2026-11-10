"""Export an already captured, single continuous encoder stream for manual wire A/B."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import importlib.util

parser = argparse.ArgumentParser()
parser.add_argument('trace', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('speaker_analysis', Path(__file__).with_name('analyze-wiimote-speaker.py'))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)
records, files = analysis.read_records(args.trace)
sources = [r for r in records if r['stage'] == 1 and r['origin'] == 'parent']
assert sources and len({r['key'] for r in sources}) == 1
state = (0, 127)
encoded = bytearray()
for index, record in enumerate(sources):
    assert record['count'] == 40 and len(record['data']) == 116
    before = struct.unpack_from('<ii', record['data'])
    after = struct.unpack_from('<ii', record['data'], 8)
    assert record['flags'] == (0 if index == 0 else 1), 'Not a single reset/continuous stream'
    assert before == state, 'Encoder discontinuity'
    data = record['data'][-20:]
    _, state = analysis.decode(data, state)
    assert after == state
    encoded.extend(data)
_, devices = analysis.inspect(records)
device = devices[('parent', 0)]
assert device['matched'] == len(sources) and not device['unmatched'] and not device['state_mismatches']
payload = b'WRP1' + struct.pack('<I', len(sources)) + encoded
args.output.mkdir(parents=True, exist_ok=True)
target = args.output / 'speaker-replay.bin'
assert not target.exists(), 'Do not overwrite an existing comparison fixture'
target.write_bytes(payload)
report = dict(trace_files=files, packets=len(sources), samples=len(sources)*40,
              sample_rate=6000, sha256=hashlib.sha256(payload).hexdigest(),
              state_continuous=True, all_packets_matched_hid=True,
              note='Original cue pauses removed for controlled continuous replay. No microphone or new capture.')
(args.output / 'replay-input.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print(json.dumps(report, indent=2))
