/** Client for the Air Voice (whisper capture) test API, see
 * components/http_server/http_server_air_voice_api.c. */

export type AirVoiceTrack = 'processed' | 'raw';
export type AirVoiceState = 'idle' | 'monitor' | 'recording' | 'saving';
export type AirVoicePlayState = 'idle' | 'playing' | 'paused' | 'finished' | 'error';
export type AirVoiceHint = 'none' | 'good' | 'clipping' | 'too_quiet' | 'low_snr' | 'airflow';

export type AirVoiceMeter = {
  in_rms_dbfs: number;
  in_peak_dbfs: number;
  out_rms_dbfs: number;
  out_peak_dbfs: number;
  noise_dbfs: number;
  speech_dbfs: number;
  snr_db: number;
  agc_gain_db: number;
  voicing: number;
  vad: boolean;
  airflow: boolean;
  airflow_frames: number;
  clipped: number;
  speech_frames: number;
  whisper_frames: number;
  voiced_frames: number;
};

export type AirVoiceStatus = {
  state: AirVoiceState;
  mic_open: boolean;
  mic_busy: boolean;
  ui_hold: boolean;
  web_lease: boolean;
  applied_pga_db: number;
  applied_adc_gain_db: number;
  record: {
    name: string;
    ms: number;
    bytes: number;
    dropped_bytes: number;
    error: string;
    last_name: string;
    last_duration_ms: number;
  };
  play: {
    state: AirVoicePlayState;
    name: string;
    track: AirVoiceTrack;
    position_ms: number;
    duration_ms: number;
    error: string;
  };
  meter: AirVoiceMeter;
  hint: AirVoiceHint;
  volume: number;
  storage_free_bytes: number;
  storage_total_bytes: number;
  log_seq: number;
};

export type AirVoiceConfig = {
  preset: string;
  pga_db: number;
  adc_gain_db: number;
  hpf_hz: number;
  ns_enable: boolean;
  ns_depth_db: number;
  eq_enable: boolean;
  eq_presence_db: number;
  agc_enable: boolean;
  agc_target_dbfs: number;
  agc_max_gain_db: number;
  manual_gain_db: number;
  limiter_enable: boolean;
  vad_threshold_db: number;
  gate_db: number;
  airflow_enable: boolean;
  airflow_depth_db: number;
  save_raw: boolean;
  max_record_s: number;
};

export type AirVoiceConfigResponse = { config: AirVoiceConfig; presets: string[] };

export type AirVoiceRecording = {
  name: string;
  duration_ms: number;
  bytes: number;
  raw_bytes: number;
  has_raw: boolean;
  has_meta: boolean;
  mtime: number;
};

export type AirVoiceLogLine = { seq: number; ts: number; level: string; text: string };

const BASE = '/api/airvoice/';

async function call<T>(path: string, init?: RequestInit, signal?: AbortSignal): Promise<T> {
  const response = await fetch(BASE + path, { cache: 'no-store', signal, ...init });
  const text = await response.text();
  let body: unknown = null;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    /* plain text */
  }
  if (!response.ok) {
    const message =
      body && typeof body === 'object' && 'error' in body && typeof body.error === 'string'
        ? body.error
        : text || `HTTP ${response.status}`;
    throw new Error(message);
  }
  return body as T;
}

function post<T>(path: string, payload?: unknown) {
  return call<T>(path, {
    method: 'POST',
    headers: payload === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: payload === undefined ? undefined : JSON.stringify(payload),
  });
}

export const airVoiceApi = {
  status: (monitor: boolean, signal?: AbortSignal) =>
    call<AirVoiceStatus>('status' + (monitor ? '?monitor=1' : ''), undefined, signal),
  config: () => call<AirVoiceConfigResponse>('config'),
  saveConfig: (patch: Partial<AirVoiceConfig>) => post<AirVoiceConfigResponse>('config', patch),
  applyPreset: (name: string) => post<AirVoiceConfigResponse>('preset', { name }),
  recordStart: () => post<{ ok: boolean }>('record/start'),
  recordStop: () => post<{ ok: boolean }>('record/stop'),
  play: (name: string, track: AirVoiceTrack) => post<{ ok: boolean }>('play', { name, track }),
  playStop: () => post<{ ok: boolean }>('play/stop'),
  playPause: () => post<{ ok: boolean }>('play/pause'),
  setVolume: (volume: number) => post<{ ok: boolean }>('volume', { volume }),
  recordings: () => call<{ items: AirVoiceRecording[]; total: number }>('recordings'),
  deleteRecording: (name: string) =>
    call<{ ok: boolean }>('recordings?name=' + encodeURIComponent(name), { method: 'DELETE' }),
  logs: (after: number, signal?: AbortSignal) =>
    call<{ lines: AirVoiceLogLine[]; last_seq: number; more: boolean }>(
      'logs?after=' + after,
      undefined,
      signal,
    ),
  clearLogs: () => post<{ ok: boolean }>('logs/clear'),
  meta: (name: string) => call<Record<string, unknown>>('meta?name=' + encodeURIComponent(name)),
};

export function audioUrl(name: string, track: AirVoiceTrack) {
  return `${BASE}audio?name=${encodeURIComponent(name)}&track=${track}`;
}
