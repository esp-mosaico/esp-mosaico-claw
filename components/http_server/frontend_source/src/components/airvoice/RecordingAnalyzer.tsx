import { createSignal, For, onCleanup, onMount, Show, type Component } from 'solid-js';
import { audioUrl, type AirVoiceTrack } from '../../api/airvoice';
import { t } from '../../i18n';
import { Button } from '../ui/Button';
import { analyze, drawSpectrogram, drawWaveform, parseWav, type TrackAnalysis } from './analysis';
import { fmtDb, fmtDuration } from './format';

type Props = {
  name: string;
  hasRaw: boolean;
  onClose: () => void;
};

type Loaded = { track: AirVoiceTrack; result: TrackAnalysis; url: string };

async function load(name: string, tracks: AirVoiceTrack[]): Promise<Loaded[]> {
  const out: Loaded[] = [];
  for (const track of tracks) {
    const response = await fetch(audioUrl(name, track), { cache: 'no-store' });
    if (!response.ok) throw new Error(`${track}: HTTP ${response.status}`);
    const buffer = await response.arrayBuffer();
    const { sampleRate, samples } = parseWav(buffer);
    /* Play from memory: streaming <audio> from the device's single-threaded
     * HTTP server would stall every other request while the media element
     * throttles its reads. */
    const url = URL.createObjectURL(new Blob([buffer], { type: 'audio/wav' }));
    out.push({ track, result: analyze(sampleRate, samples), url });
  }
  return out;
}

const Stat: Component<{ label: string; value: string; warn?: boolean }> = (props) => (
  <div class="flex flex-col px-3 py-1.5 rounded-[var(--radius-sm)] bg-white/[0.03]">
    <span class="text-[0.68rem] uppercase tracking-wider text-[var(--color-text-muted)]">
      {props.label}
    </span>
    <span
      class={[
        'font-mono text-[0.85rem]',
        props.warn ? 'text-[var(--color-danger)]' : 'text-[var(--color-text-primary)]',
      ].join(' ')}
    >
      {props.value}
    </span>
  </div>
);

const TrackView: Component<{ name: string; item: Loaded }> = (props) => {
  const r = () => props.item.result;
  return (
    <div class="flex flex-col gap-2">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <span class="text-[0.85rem] font-semibold text-[var(--color-text-primary)]">
          {props.item.track === 'raw' ? t('avRaw') : t('avProcessed')} ·{' '}
          {fmtDuration((r().samples.length / r().sampleRate) * 1000)}
        </span>
        <audio controls preload="auto" class="h-8 max-w-full" src={props.item.url} />
      </div>
      <div class="grid grid-cols-3 gap-1.5">
        <Stat label={t('avStatRms') as string} value={fmtDb(r().rmsDb, 1)} />
        <Stat
          label={t('avStatPeak') as string}
          value={fmtDb(r().peakDb, 1)}
          warn={r().peakDb > -1}
        />
        <Stat
          label={t('avStatClip') as string}
          value={`${r().clipPct.toFixed(2)}%`}
          warn={r().clipPct > 0.01}
        />
        <Stat label={t('avStatNoise') as string} value={fmtDb(r().noiseDb, 1)} />
        <Stat label={t('avStatSpeech') as string} value={fmtDb(r().speechDb, 1)} />
        <Stat label={t('avStatSnr') as string} value={fmtDb(r().speechDb - r().noiseDb, 1)} />
      </div>
      <canvas
        class="w-full block rounded-[var(--radius-sm)]"
        style={{ height: '80px' }}
        ref={(el) => requestAnimationFrame(() => drawWaveform(el, r().samples, 80))}
        title={t('avWaveform') as string}
      />
      <canvas
        class="w-full block rounded-[var(--radius-sm)]"
        style={{ height: '160px' }}
        ref={(el) => requestAnimationFrame(() => drawSpectrogram(el, r().samples, 160))}
        title={t('avSpectrogram') as string}
      />
    </div>
  );
};

export const RecordingAnalyzer: Component<Props> = (props) => {
  /* Plain signals, not createResource: a resource read here would suspend the
   * page-level <Suspense> and blank the whole page while the WAVs download. */
  const [items, setItems] = createSignal<Loaded[] | null>(null);
  const [error, setError] = createSignal<string | null>(null);
  let disposed = false;
  onMount(() => {
    load(props.name, props.hasRaw ? ['processed', 'raw'] : ['processed'])
      .then((loaded) => {
        if (disposed) loaded.forEach((item) => URL.revokeObjectURL(item.url));
        else setItems(loaded);
      })
      .catch((err: unknown) => setError(String(err)));
  });
  onCleanup(() => {
    disposed = true;
    items()?.forEach((item) => URL.revokeObjectURL(item.url));
  });
  return (
    <div class="flex flex-col gap-4 px-5 py-4 bg-black/20 border-y border-[var(--color-border-subtle)]">
      <div class="flex items-center justify-between gap-2">
        <span class="text-[0.9rem] font-semibold text-[var(--color-text-primary)] font-mono truncate">
          {t('avAnalysis')} · {props.name}
        </span>
        <Button size="xs" variant="ghost" onClick={props.onClose}>
          {t('avClose')}
        </Button>
      </div>
      <Show when={!items() && !error()}>
        <span class="text-[0.82rem] text-[var(--color-text-muted)]">{t('avAnalyzing')}</span>
      </Show>
      <Show when={error()}>
        <span class="text-[0.82rem] text-[var(--color-danger)]">{error()}</span>
      </Show>
      <Show when={items()}>
        {(loaded) => (
          <div class="grid gap-5 lg:grid-cols-2">
            <For each={loaded()}>{(item) => <TrackView name={props.name} item={item} />}</For>
          </div>
        )}
      </Show>
    </div>
  );
};
