"""Compare existing file audio; never records a microphone or accesses HID."""
import argparse
import json
from pathlib import Path
import wave
import numpy as np
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument('recording', type=Path)
parser.add_argument('reference', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

def read(path):
    with wave.open(str(path), 'rb') as wav:
        assert wav.getsampwidth() == 2
        rate, channels = wav.getframerate(), wav.getnchannels()
        data = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2').astype(float)
    return data.reshape(-1, channels).mean(axis=1) / 32768, rate

recorded, rate = read(args.recording)
reference, ref_rate = read(args.reference)
assert ref_rate == 6000 and rate == 48000
n = np.arange(-64, 65)
h = 2*(2800/rate)*np.sinc(2*(2800/rate)*n)*np.hanning(len(n))
h /= h.sum()
rec6 = np.convolve(recorded, h, mode='same')[::8]
rec6 -= rec6.mean()
comparisons = []
for multiplier in (0.5, 0.75, 1.0, 1.25, 1.5, 2.0):
    ref = np.interp(np.arange(0, len(reference), multiplier), np.arange(len(reference)), reference)
    ref -= ref.mean()
    if len(ref) > len(rec6):
        continue
    size = 1 << (len(rec6)+len(ref)-1).bit_length()
    corr = np.fft.irfft(np.fft.rfft(rec6, size)*np.conj(np.fft.rfft(ref, size)), size)[:len(rec6)-len(ref)+1]
    sums = np.r_[0, np.cumsum(rec6*rec6)]
    energy = sums[len(ref):]-sums[:-len(ref)]
    normalized = corr / np.sqrt(np.maximum(energy*(ref@ref), 1e-30))
    best = int(np.argmax(np.abs(normalized)))
    comparisons.append(dict(playback_rate_multiplier=multiplier, correlation=float(normalized[best]), start_seconds=best/6000))

window, hop = 4096, 240
frames = np.lib.stride_tricks.sliding_window_view(recorded, window)[::hop]
power = np.abs(np.fft.rfft(frames*np.hanning(window), axis=1))**2
freq = np.fft.rfftfreq(window, 1/rate)
times = (np.arange(len(frames))*hop+window/2)/rate
psd = power.mean(axis=0)
bands = [(20,100),(100,300),(300,600),(600,1500),(1500,3000),(3000,8000)]
total = psd[(freq>=20)&(freq<8000)].sum()
band_power = [dict(low=lo, high=hi, fraction=float(psd[(freq>=lo)&(freq<hi)].sum()/total)) for lo,hi in bands]
peaks = []
for i in np.argsort(psd)[::-1]:
    if not 40 <= freq[i] < 8000 or any(abs(freq[i]-p['hz']) < 35 for p in peaks):
        continue
    peaks.append(dict(hz=float(freq[i]), relative_db=float(10*np.log10(psd[i]/psd.max()))))
    if len(peaks) == 12:
        break

envelopes = []
for lo,hi in bands:
    envelope = np.sqrt(power[:,(freq>=lo)&(freq<hi)].sum(axis=1))
    centered = envelope-envelope.mean()
    spectrum = np.abs(np.fft.rfft(centered*np.hanning(len(centered))))**2
    modulation = np.fft.rfftfreq(len(centered), hop/rate)
    selection = np.flatnonzero((modulation>=2)&(modulation<=20))
    peak = selection[np.argmax(spectrum[selection])]
    envelopes.append(dict(low=lo, high=hi, strongest_2_to_20hz_modulation=float(modulation[peak]), modulation_depth=float(envelope.std()/max(envelope.mean(),1e-12))))

report = dict(recording_seconds=len(recorded)/rate, reference_seconds=len(reference)/ref_rate,
              recording_peak=float(np.abs(recorded).max()), recording_rms=float(np.sqrt(np.mean(recorded**2))),
              clipped_recording_samples=int(np.sum(np.abs(recorded)>=32767/32768)),
              rate_correlations=comparisons, band_power=band_power, spectral_peaks=peaks,
              band_modulation=envelopes,
              limitation='Phone/microphone/room processing differs from software reference. Correlation/peaks alone do not identify hardware root cause. No direct auditory claim.')
(args.output/'recording-analysis.json').write_text(json.dumps(report,indent=2)+'\n')

image = Image.new('RGB',(1200,720),'white')
draw = ImageDraw.Draw(image)
draw.text((40,12),'Actual Wiimote recording: waveform, spectrum and spectrogram (diagnostic only)',fill='black')
for x in range(1120):
    part=recorded[int(x*len(recorded)/1120):max(int((x+1)*len(recorded)/1120),int(x*len(recorded)/1120)+1)]
    draw.line((40+x,125-float(part.max())*100,40+x,125-float(part.min())*100),fill='#145a67')
draw.line((40,125,1160,125),fill='#bbbbbb')
draw.text((40,215),'Average spectrum: 0 to 8 kHz; full vertical range = 70 dB',fill='black')
selected = np.flatnonzero(freq<=8000)
db = 10*np.log10(np.maximum(psd,1e-30)/psd.max())
points = [(40+float(freq[i])*1120/8000,335-float(np.clip(db[i],-70,0))*100/70) for i in selected]
draw.line(points,fill='#145a67',width=2)
draw.text((40,450),'Spectrogram: 0 to 4 kHz, left to right is time',fill='black')
bins = np.flatnonzero(freq<=4000)
dbspec=10*np.log10(np.maximum(power[:,bins],1e-30)/power.max())
values=np.uint8(np.clip((dbspec+65)*255/65,0,255)).T[::-1]
colors=np.stack((values,np.uint8(values.astype(float)*0.65),255-values),axis=2)
spec=Image.fromarray(colors).resize((1120,210))
image.paste(spec,(40,475))
draw.text((40,698),'No EQ/filter changes applied to Cemu. All analysis stays in local files.',fill='black')
image.save(args.output/'recording-analysis.png')
print(json.dumps(report,indent=2))
