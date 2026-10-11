/** Browser-side analysis of 16-bit PCM WAV recordings: level statistics,
 * waveform envelope and an STFT spectrogram. */

export type TrackAnalysis = {
  sampleRate: number;
  samples: Float32Array;
  rmsDb: number;
  peakDb: number;
  clipPct: number;
  noiseDb: number;
  speechDb: number;
};

const FRAME_MS = 20;
const CLIP_LEVEL = 32700 / 32768;
const FFT_SIZE = 512;
const HOP = 256;

function db(power: number) {
  return power > 1e-12 ? 10 * Math.log10(power) : -120;
}

export function parseWav(buffer: ArrayBuffer): { sampleRate: number; samples: Float32Array } {
  const view = new DataView(buffer);
  const tag = (offset: number) => String.fromCharCode(...new Uint8Array(buffer, offset, 4));
  if (buffer.byteLength < 44 || tag(0) !== 'RIFF' || tag(8) !== 'WAVE') {
    throw new Error('not a WAV file');
  }
  let offset = 12;
  let sampleRate = 16000;
  let bits = 16;
  let channels = 1;
  while (offset + 8 <= buffer.byteLength) {
    const id = tag(offset);
    const size = view.getUint32(offset + 4, true);
    const body = offset + 8;
    if (id === 'fmt ') {
      channels = view.getUint16(body + 2, true);
      sampleRate = view.getUint32(body + 4, true);
      bits = view.getUint16(body + 14, true);
    } else if (id === 'data') {
      if (bits !== 16) throw new Error(`unsupported ${bits}-bit WAV`);
      const end = Math.min(buffer.byteLength, body + size);
      const count = Math.floor((end - body) / 2 / channels);
      const samples = new Float32Array(count);
      for (let i = 0; i < count; i++) {
        samples[i] = view.getInt16(body + i * 2 * channels, true) / 32768;
      }
      return { sampleRate, samples };
    }
    offset = body + size + (size & 1);
  }
  throw new Error('WAV data chunk not found');
}

function percentile(sorted: number[], p: number) {
  if (sorted.length === 0) return -120;
  const idx = Math.min(sorted.length - 1, Math.max(0, Math.round((p / 100) * (sorted.length - 1))));
  return sorted[idx] ?? -120;
}

export function analyze(sampleRate: number, samples: Float32Array): TrackAnalysis {
  let sum = 0;
  let peak = 0;
  let clipped = 0;
  for (let i = 0; i < samples.length; i++) {
    const v = samples[i] ?? 0;
    const a = Math.abs(v);
    sum += v * v;
    if (a > peak) peak = a;
    if (a >= CLIP_LEVEL) clipped++;
  }
  const frame = Math.max(1, Math.round((sampleRate * FRAME_MS) / 1000));
  const frameDb: number[] = [];
  for (let start = 0; start + frame <= samples.length; start += frame) {
    let acc = 0;
    for (let i = start; i < start + frame; i++) {
      const v = samples[i] ?? 0;
      acc += v * v;
    }
    frameDb.push(db(acc / frame));
  }
  frameDb.sort((a, b) => a - b);
  return {
    sampleRate,
    samples,
    rmsDb: db(samples.length ? sum / samples.length : 0),
    peakDb: peak > 0 ? 20 * Math.log10(peak) : -120,
    clipPct: samples.length ? (clipped * 100) / samples.length : 0,
    noiseDb: percentile(frameDb, 10),
    speechDb: percentile(frameDb, 90),
  };
}

/* ---------------- drawing ---------------- */

const COLORMAP: [number, number, number, number][] = [
  [0.0, 0, 0, 4],
  [0.25, 59, 15, 112],
  [0.5, 140, 41, 129],
  [0.75, 222, 73, 104],
  [1.0, 252, 253, 191],
];

function colormap(v: number): [number, number, number] {
  const x = Math.max(0, Math.min(1, v));
  for (let i = 1; i < COLORMAP.length; i++) {
    const hi = COLORMAP[i];
    const lo = COLORMAP[i - 1];
    if (!hi || !lo) break;
    const [p1, r1, g1, b1] = hi;
    if (x <= p1) {
      const [p0, r0, g0, b0] = lo;
      const t = (x - p0) / (p1 - p0);
      return [r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t];
    }
  }
  return [252, 253, 191];
}

function prepareCanvas(canvas: HTMLCanvasElement, height: number) {
  const dpr = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  canvas.width = Math.max(1, Math.floor(width * dpr));
  canvas.height = Math.floor(height * dpr);
  return { width: canvas.width, height: canvas.height, ctx: canvas.getContext('2d') };
}

export function drawWaveform(canvas: HTMLCanvasElement, samples: Float32Array, height: number) {
  const { width, height: h, ctx } = prepareCanvas(canvas, height);
  if (!ctx) return;
  ctx.fillStyle = '#0e1015';
  ctx.fillRect(0, 0, width, h);
  ctx.strokeStyle = 'rgba(255,255,255,0.12)';
  ctx.beginPath();
  ctx.moveTo(0, h / 2);
  ctx.lineTo(width, h / 2);
  ctx.stroke();
  const per = samples.length / width;
  ctx.fillStyle = '#f6ad55';
  for (let x = 0; x < width; x++) {
    const start = Math.floor(x * per);
    const end = Math.min(samples.length, Math.floor((x + 1) * per) + 1);
    let lo = 0;
    let hi = 0;
    let clip = false;
    for (let i = start; i < end; i++) {
      const v = samples[i] ?? 0;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
      if (Math.abs(v) >= CLIP_LEVEL) clip = true;
    }
    ctx.fillStyle = clip ? '#f87171' : '#f6ad55';
    const top = ((1 - hi) / 2) * h;
    const bottom = ((1 - lo) / 2) * h;
    ctx.fillRect(x, top, 1, Math.max(1, bottom - top));
  }
}

function fftInPlace(re: Float64Array, im: Float64Array) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      const tr = re[i] ?? 0;
      re[i] = re[j] ?? 0;
      re[j] = tr;
      const ti = im[i] ?? 0;
      im[i] = im[j] ?? 0;
      im[j] = ti;
    }
  }
  for (let size = 2; size <= n; size <<= 1) {
    const step = (-2 * Math.PI) / size;
    const half = size / 2;
    for (let start = 0; start < n; start += size) {
      for (let k = 0; k < half; k++) {
        const wr = Math.cos(step * k);
        const wi = Math.sin(step * k);
        const a = start + k;
        const b = a + half;
        const br = re[b] ?? 0;
        const bi = im[b] ?? 0;
        const ar = re[a] ?? 0;
        const ai = im[a] ?? 0;
        const tr = br * wr - bi * wi;
        const ti = br * wi + bi * wr;
        re[b] = ar - tr;
        im[b] = ai - ti;
        re[a] = ar + tr;
        im[a] = ai + ti;
      }
    }
  }
}

/** Spectrogram over -110..-20 dBFS per bin; columns are max-pooled frames. */
export function drawSpectrogram(canvas: HTMLCanvasElement, samples: Float32Array, height: number) {
  const { width, height: h, ctx } = prepareCanvas(canvas, height);
  if (!ctx) return;
  const frames = Math.max(1, Math.floor((samples.length - FFT_SIZE) / HOP) + 1);
  const columns = Math.min(width, frames);
  const bins = FFT_SIZE / 2;
  const window = new Float64Array(FFT_SIZE);
  for (let i = 0; i < FFT_SIZE; i++) window[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / FFT_SIZE);
  const norm = FFT_SIZE * FFT_SIZE * 0.375 * 0.25;
  const grid = new Float32Array(columns * bins).fill(-140);
  const re = new Float64Array(FFT_SIZE);
  const im = new Float64Array(FFT_SIZE);
  const stride = Math.max(1, Math.floor(frames / (columns * 4)));
  for (let f = 0; f < frames; f += stride) {
    const col = Math.min(columns - 1, Math.floor((f / frames) * columns));
    const start = f * HOP;
    for (let i = 0; i < FFT_SIZE; i++) {
      re[i] = (samples[start + i] ?? 0) * (window[i] ?? 0);
      im[i] = 0;
    }
    fftInPlace(re, im);
    for (let k = 0; k < bins; k++) {
      const r = re[k] ?? 0;
      const i = im[k] ?? 0;
      const value = db((r * r + i * i) / norm);
      const idx = col * bins + k;
      if (value > (grid[idx] ?? -140)) grid[idx] = value;
    }
  }
  const image = ctx.createImageData(columns, bins);
  for (let c = 0; c < columns; c++) {
    for (let k = 0; k < bins; k++) {
      const [r, g, b] = colormap(((grid[c * bins + k] ?? -140) + 110) / 90);
      const p = ((bins - 1 - k) * columns + c) * 4;
      image.data[p] = r;
      image.data[p + 1] = g;
      image.data[p + 2] = b;
      image.data[p + 3] = 255;
    }
  }
  const off = document.createElement('canvas');
  off.width = columns;
  off.height = bins;
  off.getContext('2d')?.putImageData(image, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(off, 0, 0, width, h);
}
