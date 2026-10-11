import { createSignal, For, onCleanup, onMount, Show, type Component, type JSX } from 'solid-js';
import {
  airVoiceApi,
  audioUrl,
  type AirVoiceConfig,
  type AirVoiceRecording,
  type AirVoiceStatus,
  type AirVoiceTrack,
} from '../api/airvoice';
import {
  HISTORY_WINDOW_MS,
  LevelHistory,
  type HistoryPoint,
} from '../components/airvoice/LevelHistory';
import { LevelBar } from '../components/airvoice/LevelBar';
import { LogPanel } from '../components/airvoice/LogPanel';
import { RecordingAnalyzer } from '../components/airvoice/RecordingAnalyzer';
import { SettingsPanel } from '../components/airvoice/SettingsPanel';
import { fmtBytes, fmtDb, fmtDuration, fmtTime } from '../components/airvoice/format';
import { TabShell } from '../components/layout/TabShell';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { PageHeader } from '../components/ui/PageHeader';
import { Switch } from '../components/ui/Switch';
import { t, tf, type TKey } from '../i18n';
import { pushToast } from '../state/toast';

const LIVE_POLL_MS = 400;
const IDLE_POLL_MS = 2000;

const HINT_KEYS: Record<string, TKey> = {
  none: 'avHintNone',
  good: 'avHintGood',
  clipping: 'avHintClipping',
  too_quiet: 'avHintTooQuiet',
  low_snr: 'avHintLowSnr',
  airflow: 'avHintAirflow',
};

const STATE_KEYS: Record<string, TKey> = {
  idle: 'avStateIdle',
  monitor: 'avStateMonitor',
  recording: 'avStateRecording',
  saving: 'avStateSaving',
};

const Metric: Component<{ label: JSX.Element; value: JSX.Element; warn?: boolean }> = (props) => (
  <div class="flex flex-col gap-0.5 px-3 py-2 rounded-[var(--radius-sm)] bg-white/[0.02] border border-[var(--color-border-subtle)]/50">
    <span class="text-[0.68rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
      {props.label}
    </span>
    <span
      class={[
        'font-mono text-[0.9rem]',
        props.warn ? 'text-[var(--color-danger)]' : 'text-[var(--color-text-primary)]',
      ].join(' ')}
    >
      {props.value}
    </span>
  </div>
);

export const AirVoicePage: Component = () => {
  const [status, setStatus] = createSignal<AirVoiceStatus | null>(null);
  const [error, setError] = createSignal<string | null>(null);
  const [live, setLive] = createSignal(true);
  const [history, setHistory] = createSignal<HistoryPoint[]>([]);
  const [config, setConfig] = createSignal<AirVoiceConfig | null>(null);
  const [presets, setPresets] = createSignal<string[]>([]);
  const [recordings, setRecordings] = createSignal<AirVoiceRecording[]>([]);
  const [analyzing, setAnalyzing] = createSignal<string | null>(null);
  const [listening, setListening] = createSignal<{
    name: string;
    track: AirVoiceTrack;
    url: string | null;
  } | null>(null);
  const [busy, setBusy] = createSignal(false);
  const [volumeDraft, setVolumeDraft] = createSignal<number | null>(null);

  let timer: ReturnType<typeof setTimeout> | undefined;
  let controller: AbortController | undefined;
  let lastState = '';
  let disposed = false;

  const loadRecordings = async () => {
    try {
      const res = await airVoiceApi.recordings();
      setRecordings(res.items);
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  /* Download the whole WAV, then play it from memory: an <audio> element
   * streaming from the device throttles its reads and would hold the
   * device's single HTTP worker for the whole playback. */
  const listen = async (name: string, track: AirVoiceTrack) => {
    const previous = listening();
    if (previous?.url) URL.revokeObjectURL(previous.url);
    setListening({ name, track, url: null });
    try {
      const response = await fetch(audioUrl(name, track), { cache: 'no-store' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const url = URL.createObjectURL(await response.blob());
      if (listening()?.name === name && listening()?.track === track) {
        setListening({ name, track, url });
      } else {
        URL.revokeObjectURL(url);
      }
    } catch (err) {
      pushToast((err as Error).message, 'error');
      setListening(null);
    }
  };

  const stopListening = () => {
    const current = listening();
    if (current?.url) URL.revokeObjectURL(current.url);
    setListening(null);
  };

  const loadConfig = async () => {
    try {
      const res = await airVoiceApi.config();
      setConfig(res.config);
      setPresets(res.presets);
    } catch (err) {
      setError((err as Error).message);
    }
  };

  const pushHistory = (st: AirVoiceStatus) => {
    const now = Date.now();
    const point: HistoryPoint = {
      t: now,
      inRms: st.meter.in_rms_dbfs,
      outRms: st.meter.out_rms_dbfs,
      noise: st.meter.noise_dbfs,
      vad: st.meter.vad,
    };
    setHistory((prev) => prev.filter((p) => now - p.t <= HISTORY_WINDOW_MS).concat(point));
  };

  const poll = async () => {
    if (disposed) return;
    const visible = document.visibilityState === 'visible';
    if (visible) {
      controller = new AbortController();
      try {
        const st = await airVoiceApi.status(live(), controller.signal);
        setStatus(st);
        setError(null);
        if (st.mic_open) pushHistory(st);
        const settled = st.state === 'monitor' || st.state === 'idle';
        if (settled && (lastState === 'recording' || lastState === 'saving')) {
          void loadRecordings();
        }
        lastState = st.state;
      } catch (err) {
        if ((err as Error).name !== 'AbortError') {
          setError((err as Error).message);
        }
      }
    }
    timer = setTimeout(() => void poll(), live() && visible ? LIVE_POLL_MS : IDLE_POLL_MS);
  };

  onMount(() => {
    void poll();
    void loadConfig();
    void loadRecordings();
  });
  onCleanup(() => {
    disposed = true;
    stopListening();
    if (timer) clearTimeout(timer);
    controller?.abort();
  });

  const run = async (action: () => Promise<unknown>, after?: () => void) => {
    setBusy(true);
    try {
      await action();
      after?.();
    } catch (err) {
      pushToast((err as Error).message, 'error', 5000);
    } finally {
      setBusy(false);
    }
  };

  const recording = () => status()?.state === 'recording';
  const saving = () => status()?.state === 'saving';
  const toggleRecord = () =>
    void run(() => (recording() ? airVoiceApi.recordStop() : airVoiceApi.recordStart()));

  const deleteRecording = (name: string) => {
    if (!window.confirm(tf('avDeleteConfirm', { name }))) return;
    void run(
      () => airVoiceApi.deleteRecording(name),
      () => {
        pushToast(t('avDeleted') as string, 'success');
        if (analyzing() === name) setAnalyzing(null);
        if (listening()?.name === name) stopListening();
        void loadRecordings();
      },
    );
  };

  const commitVolume = (value: number) =>
    void run(
      () => airVoiceApi.setVolume(value),
      () => setVolumeDraft(null),
    );

  const meter = () => status()?.meter;
  const whisperPct = () => {
    const m = meter();
    if (!m) return null;
    const classified = m.whisper_frames + m.voiced_frames;
    return classified > 0 ? Math.round((m.whisper_frames * 100) / classified) : null;
  };
  const airflowPct = () => {
    const m = meter();
    if (!m || m.speech_frames === 0) return null;
    return Math.round((m.airflow_frames * 100) / m.speech_frames);
  };
  const playing = () => {
    const p = status()?.play.state;
    return p === 'playing' || p === 'paused';
  };

  return (
    <TabShell>
      <PageHeader
        title={t('avTitle') as string}
        description={t('avDesc') as string}
        actions={
          <>
            <Switch
              label={t('avLive') as string}
              hint={t('avLiveHint') as string}
              checked={live()}
              onChange={setLive}
            />
            <Button size="sm" variant="secondary" onClick={() => void loadRecordings()}>
              {t('avRefresh')}
            </Button>
          </>
        }
      />
      <Show when={error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tf('avServiceOffline', { error: error() ?? '' })} />
        </div>
      </Show>
      <Show when={status()?.mic_busy}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={t('avMicBusy') as string} />
        </div>
      </Show>

      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('avLive') as string}>
          <div class="flex flex-col gap-4 pt-2">
            <div class="flex flex-wrap items-center gap-4">
              <button
                type="button"
                disabled={busy() || saving() || !status()}
                onClick={toggleRecord}
                class={[
                  'inline-flex items-center gap-2 rounded-full px-6 py-3 text-[0.95rem] font-semibold transition disabled:opacity-50',
                  recording()
                    ? 'bg-white text-[var(--color-accent)] animate-pulse'
                    : 'bg-[var(--color-accent)] text-white hover:opacity-90',
                ].join(' ')}
              >
                <span
                  class={[
                    'inline-block w-3 h-3',
                    recording() ? 'bg-[var(--color-accent)]' : 'bg-white rounded-full',
                  ].join(' ')}
                />
                {recording()
                  ? `${t('avStop')} ${fmtDuration(status()?.record.ms ?? 0)}`
                  : t('avRecord')}
              </button>
              <div class="flex flex-col min-w-0">
                <span class="text-[0.95rem] font-semibold text-[var(--color-text-primary)]">
                  {t(STATE_KEYS[status()?.state ?? 'idle'] ?? 'avStateIdle')}
                  <Show when={recording()}>
                    <span class="font-mono text-[0.8rem] text-[var(--color-text-muted)]">
                      {' '}
                      {status()?.record.name} · {fmtBytes(status()?.record.bytes ?? 0)}
                    </span>
                  </Show>
                </span>
                <span
                  class={[
                    'text-[0.82rem]',
                    status()?.hint === 'good'
                      ? 'text-[var(--color-green)]'
                      : status()?.hint === 'none'
                        ? 'text-[var(--color-text-muted)]'
                        : 'text-[var(--color-orange)]',
                  ].join(' ')}
                >
                  {t(HINT_KEYS[status()?.hint ?? 'none'] ?? 'avHintNone')}
                </span>
              </div>
              <div class="flex-1" />
              <label class="flex items-center gap-2 text-[0.8rem] text-[var(--color-text-secondary)] min-w-[220px]">
                <span class="whitespace-nowrap">{t('avVolume')}</span>
                <input
                  type="range"
                  min={0}
                  max={100}
                  step={5}
                  class="!p-0 !border-0 !bg-transparent accent-[var(--color-accent)]"
                  value={volumeDraft() ?? Math.max(0, status()?.volume ?? 0)}
                  onInput={(e) => setVolumeDraft(Number(e.currentTarget.value))}
                  onChange={(e) => commitVolume(Number(e.currentTarget.value))}
                />
                <span class="font-mono w-9 text-right">
                  {volumeDraft() ?? status()?.volume ?? '—'}
                </span>
              </label>
            </div>

            <div class="grid gap-3 sm:grid-cols-2">
              <LevelBar
                label={t('avInput') as string}
                rms={meter()?.in_rms_dbfs ?? -120}
                peak={meter()?.in_peak_dbfs ?? -120}
                color="#f6ad55"
                active={!!status()?.mic_open}
              />
              <LevelBar
                label={t('avOutput') as string}
                rms={meter()?.out_rms_dbfs ?? -120}
                peak={meter()?.out_peak_dbfs ?? -120}
                color="#68d391"
                active={!!status()?.mic_open}
              />
            </div>

            <div class="grid gap-2 grid-cols-2 sm:grid-cols-6">
              <Metric label={t('avNoise')} value={fmtDb(meter()?.noise_dbfs, 1)} />
              <Metric label={t('avSpeech')} value={fmtDb(meter()?.speech_dbfs, 1)} />
              <Metric
                label={t('avSnr')}
                value={meter()?.speech_frames ? fmtDb(meter()?.snr_db, 1) : '—'}
                warn={!!meter()?.speech_frames && (meter()?.snr_db ?? 0) < 8}
              />
              <Metric label={t('avAgc')} value={fmtDb(meter()?.agc_gain_db, 1)} />
              <Metric
                label={t('avPhonation')}
                value={
                  <span class="flex items-center gap-2">
                    <span
                      class={[
                        'inline-block w-2.5 h-2.5 rounded-full',
                        meter()?.vad ? 'bg-[var(--color-green)]' : 'bg-white/10',
                      ].join(' ')}
                      title={t('avVad') as string}
                    />
                    {whisperPct() === null ? '—' : tf('avWhisperPct', { pct: whisperPct() ?? 0 })}
                  </span>
                }
              />
              <Metric
                label={t('avAirflow')}
                value={
                  <span class="flex items-center gap-2">
                    <span
                      class={[
                        'inline-block w-2.5 h-2.5 rounded-full',
                        meter()?.airflow ? 'bg-[var(--color-orange)]' : 'bg-white/10',
                      ].join(' ')}
                    />
                    {airflowPct() === null ? '—' : tf('avAirflowPct', { pct: airflowPct() ?? 0 })}
                  </span>
                }
                warn={(airflowPct() ?? 0) > 50}
              />
              <Metric
                label={t('avCodecGain')}
                value={
                  status()?.mic_open
                    ? `${status()?.applied_pga_db}+${status()?.applied_adc_gain_db} dB`
                    : '—'
                }
              />
              <Metric
                label={t('avVoicing')}
                value={meter() ? (meter()?.voicing ?? 0).toFixed(2) : '—'}
              />
              <Metric
                label={t('avClipped')}
                value={String(meter()?.clipped ?? 0)}
                warn={(meter()?.clipped ?? 0) > 0}
              />
              <Metric
                label={t('avDropped')}
                value={String(status()?.record.dropped_bytes ?? 0)}
                warn={(status()?.record.dropped_bytes ?? 0) > 0}
              />
              <Metric label={t('avStorage')} value={fmtBytes(status()?.storage_free_bytes ?? 0)} />
            </div>

            <div class="flex flex-col gap-1">
              <span class="text-[0.75rem] text-[var(--color-text-muted)]">
                {t('avHistory')} · <span class="text-[#f6ad55]">{t('avInput')}</span> ·{' '}
                <span class="text-[#68d391]">{t('avOutput')}</span> · <span>{t('avNoise')}</span>
              </span>
              <LevelHistory points={history()} />
            </div>

            <Show when={playing() || status()?.play.state === 'error'}>
              <div class="flex flex-wrap items-center gap-3 px-3 py-2 rounded-[var(--radius-sm)] bg-white/[0.03]">
                <span class="text-[0.82rem] text-[var(--color-text-secondary)]">
                  {t('avDevicePlayback')}:{' '}
                  <span class="font-mono text-[var(--color-text-primary)]">
                    {status()?.play.name} ({status()?.play.track})
                  </span>
                </span>
                <Show
                  when={status()?.play.state !== 'error'}
                  fallback={
                    <span class="text-[0.82rem] text-[var(--color-danger)]">
                      {tf('avPlayError', { error: status()?.play.error ?? '' })}
                    </span>
                  }
                >
                  <span class="font-mono text-[0.8rem]">
                    {fmtDuration(status()?.play.position_ms ?? 0)} /{' '}
                    {fmtDuration(status()?.play.duration_ms ?? 0)}
                  </span>
                  <Button
                    size="xs"
                    variant="secondary"
                    onClick={() => void run(airVoiceApi.playPause)}
                  >
                    {t('avPause')}
                  </Button>
                  <Button
                    size="xs"
                    variant="secondary"
                    onClick={() => void run(airVoiceApi.playStop)}
                  >
                    {t('avStop')}
                  </Button>
                </Show>
              </div>
            </Show>
          </div>
        </StaticConfigBlock>

        <StaticConfigBlock title={`${t('avRecordings')} (${recordings().length})`}>
          <Show
            when={recordings().length > 0}
            fallback={
              <span class="block py-3 text-[0.85rem] text-[var(--color-text-muted)]">
                {t('avEmpty')}
              </span>
            }
          >
            <div class="-mx-5 overflow-x-auto">
              <table class="w-full text-[0.82rem]">
                <thead>
                  <tr class="text-left text-[0.7rem] uppercase tracking-wider text-[var(--color-text-muted)]">
                    <th class="px-5 py-2 font-semibold">{t('avColName')}</th>
                    <th class="px-2 py-2 font-semibold">{t('avColTime')}</th>
                    <th class="px-2 py-2 font-semibold">{t('avColDuration')}</th>
                    <th class="px-2 py-2 font-semibold">{t('avColSize')}</th>
                    <th class="px-5 py-2" />
                  </tr>
                </thead>
                <tbody>
                  <For each={recordings()}>
                    {(rec) => (
                      <>
                        <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.015]">
                          <td class="px-5 py-2 font-mono text-[var(--color-text-primary)]">
                            {rec.name}
                            <Show when={rec.has_raw}>
                              <span class="ml-2 text-[0.65rem] px-1.5 py-0.5 rounded bg-white/5 text-[var(--color-text-muted)]">
                                RAW
                              </span>
                            </Show>
                          </td>
                          <td class="px-2 py-2 text-[var(--color-text-secondary)] whitespace-nowrap">
                            {fmtTime(rec.mtime)}
                          </td>
                          <td class="px-2 py-2 font-mono">{fmtDuration(rec.duration_ms)}</td>
                          <td class="px-2 py-2 font-mono text-[var(--color-text-secondary)]">
                            {fmtBytes(rec.bytes + rec.raw_bytes)}
                          </td>
                          <td class="px-5 py-2">
                            <div class="flex flex-wrap justify-end gap-1">
                              <Button
                                size="xs"
                                variant="secondary"
                                onClick={() => void listen(rec.name, 'processed')}
                              >
                                ▶ {t('avProcessed')}
                              </Button>
                              <Show when={rec.has_raw}>
                                <Button
                                  size="xs"
                                  variant="secondary"
                                  onClick={() => void listen(rec.name, 'raw')}
                                >
                                  ▶ {t('avRaw')}
                                </Button>
                              </Show>
                              <Button
                                size="xs"
                                variant="ghost"
                                disabled={busy() || recording()}
                                title={t('avPlayDevice') as string}
                                onClick={() =>
                                  void run(() => airVoiceApi.play(rec.name, 'processed'))
                                }
                              >
                                🔊 {t('avPlayDevice')}
                              </Button>
                              <Button
                                size="xs"
                                variant="ghost"
                                onClick={() =>
                                  setAnalyzing(analyzing() === rec.name ? null : rec.name)
                                }
                              >
                                {t('avAnalyze')}
                              </Button>
                              <a
                                class="inline-flex items-center px-2 py-1 text-[0.75rem] text-[var(--color-text-secondary)] hover:text-[var(--color-text-primary)]"
                                href={audioUrl(rec.name, 'processed')}
                                download={`${rec.name}.wav`}
                              >
                                {t('avDownload')}
                              </a>
                              <Button
                                size="xs"
                                variant="danger-ghost"
                                disabled={busy()}
                                onClick={() => deleteRecording(rec.name)}
                              >
                                {t('avDelete')}
                              </Button>
                            </div>
                          </td>
                        </tr>
                        <Show when={listening()?.name === rec.name}>
                          <tr>
                            <td colSpan={5} class="px-5 pb-3">
                              <div class="flex items-center gap-3">
                                <span class="text-[0.75rem] text-[var(--color-text-muted)] w-20">
                                  {listening()?.track === 'raw' ? t('avRaw') : t('avProcessed')}
                                </span>
                                <Show
                                  when={listening()?.url}
                                  fallback={
                                    <span class="flex-1 text-[0.8rem] text-[var(--color-text-muted)]">
                                      {t('statusLoading')}
                                    </span>
                                  }
                                >
                                  {(url) => (
                                    <audio controls autoplay class="h-8 flex-1" src={url()} />
                                  )}
                                </Show>
                                <Button size="xs" variant="ghost" onClick={stopListening}>
                                  {t('avClose')}
                                </Button>
                              </div>
                            </td>
                          </tr>
                        </Show>
                        <Show when={analyzing() === rec.name}>
                          <tr>
                            <td colSpan={5} class="p-0">
                              <RecordingAnalyzer
                                name={rec.name}
                                hasRaw={rec.has_raw}
                                onClose={() => setAnalyzing(null)}
                              />
                            </td>
                          </tr>
                        </Show>
                      </>
                    )}
                  </For>
                </tbody>
              </table>
            </div>
          </Show>
        </StaticConfigBlock>

        <CollapsibleConfigBlock title={t('avSettings') as string} defaultOpen>
          <SettingsPanel
            config={config()}
            presets={presets()}
            onSaved={(cfg, names) => {
              setConfig(cfg);
              setPresets(names);
            }}
          />
        </CollapsibleConfigBlock>

        <CollapsibleConfigBlock title={t('avLogs') as string} defaultOpen>
          <LogPanel />
        </CollapsibleConfigBlock>
      </div>
    </TabShell>
  );
};
