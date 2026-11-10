"""Make an explicit 3 kHz diagnostic fixture from an existing 6 kHz PCM capture."""
import argparse
import hashlib
import json
import struct
import wave
from pathlib import Path

import numpy as np


def encode(samples):
    predictor, step = 0, 127
    factors = (230, 230, 230, 230, 307, 409, 512, 614)
    nibbles = []
    for sample in samples:
        choices = []
        for nibble in range(16):
            delta = step * (2 * (nibble & 7) + 1) // 8
            value = max(-32768, min(32767, predictor + (-delta if nibble & 8 else delta)))
            choices.append(value)
        best = min(range(16), key=lambda nibble: abs(int(sample) - choices[nibble]))
        predictor = choices[best]
        step = max(127, min(24576, step * factors[best & 7] // 256))
        nibbles.append(best)
    assert len(nibbles) % 2 == 0
    return bytes((a << 4) | b for a, b in zip(nibbles[::2], nibbles[1::2]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('pcm', type=Path)
    parser.add_argument('reference', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    with wave.open(str(args.pcm), 'rb') as source:
        assert (source.getnchannels(), source.getsampwidth(), source.getframerate()) == (1, 2, 6000)
        samples = np.frombuffer(source.readframes(source.getnframes()), dtype='<i2')
    original = args.reference.read_bytes()
    assert original[:4] == b'WRP1'
    assert len(samples) == struct.unpack_from('<I', original, 4)[0] * 40
    assert encode(samples) == original[8:], 'PCM/encoder does not reproduce the original fixture'
    assert len(samples) % 80 == 0

    # Anti-alias before decimation; preserve cue duration/pitch, not high-band detail.
    position = np.arange(-31, 32, dtype=np.float64)
    kernel = 0.45 * np.sinc(0.45 * position) * np.hanning(63)
    kernel /= kernel.sum()
    assert abs(kernel.sum() - 1.0) < 1e-12
    low_response = abs(np.sum(kernel * np.exp(-2j * np.pi * 500 / 6000 * position)))
    high_response = abs(np.sum(kernel * np.exp(-2j * np.pi * 2000 / 6000 * position)))
    assert 0.98 < low_response < 1.02 and high_response < 0.01
    filtered = np.rint(np.convolve(samples.astype(np.float64), kernel, mode='same')[::2])
    assert np.max(filtered) <= 32767 and np.min(filtered) >= -32768, 'Filter would clip'
    reduced = filtered.astype('<i2')
    assert len(reduced) * 2 == len(samples)
    encoded = encode(reduced)
    payload = b'WRP1' + struct.pack('<I', len(reduced) // 40) + encoded
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output / 'speaker-replay.bin'
    assert not target.exists(), 'Do not overwrite a fixture from an earlier run'
    target.write_bytes(payload)
    with wave.open(str(args.output / 'reduced-bandwidth-reference.wav'), 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(3000)
        out.writeframes(reduced.tobytes())
    report = dict(
        status='diagnostic_fixture_not_full_quality_fix',
        original_pcm=str(args.pcm.resolve()), original_fixture_sha256=hashlib.sha256(original).hexdigest(),
        original_encoder_reproduction='byte_exact',
        original_rate=6000, reduced_rate=3000,
        original_packets=len(samples) // 40, reduced_packets=len(reduced) // 40,
        original_packets_per_second=150, reduced_packets_per_second=75,
        original_duration_seconds=len(samples) / 6000, reduced_duration_seconds=len(reduced) / 3000,
        anti_alias_filter='63 tap Hann-windowed sinc; cutoff1350Hz; decimate2',
        response_500hz=float(low_response), response_2000hz=float(high_response),
        format='same Yamaha ADPCM high nibble first',
        volume_changed=False, pitch_or_speed_changed=False,
        fixture_sha256=hashlib.sha256(payload).hexdigest(),
        hardware_accessed=False, production_source_changed=False,
    )
    (args.output / 'replay-input.json').write_text(json.dumps(report, indent=2) + '\n', encoding='ascii')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
