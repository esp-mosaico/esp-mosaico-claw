import {
  createEffect,
  createSignal,
  For,
  onCleanup,
  onMount,
  Show,
  type Component,
} from 'solid-js';
import { airVoiceApi, type AirVoiceLogLine } from '../../api/airvoice';
import { t } from '../../i18n';
import { pushToast } from '../../state/toast';
import { Button } from '../ui/Button';
import { Switch } from '../ui/Switch';

const POLL_MS = 1000;
const MAX_LINES = 1000;

const LEVEL_CLASS: Record<string, string> = {
  E: 'text-[var(--color-danger)]',
  W: 'text-[var(--color-orange)]',
  I: 'text-[var(--color-text-secondary)]',
  D: 'text-[var(--color-text-muted)]',
};

function fmtTs(ms: number) {
  const s = ms / 1000;
  return s.toFixed(3).padStart(10, ' ');
}

export const LogPanel: Component = () => {
  const [lines, setLines] = createSignal<AirVoiceLogLine[]>([]);
  const [paused, setPaused] = createSignal(false);
  const [warnOnly, setWarnOnly] = createSignal(false);
  let lastSeq = 0;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let controller: AbortController | undefined;
  let box: HTMLDivElement | undefined;
  let stickToBottom = true;

  const poll = async () => {
    if (!paused() && document.visibilityState === 'visible') {
      controller = new AbortController();
      try {
        let more = true;
        while (more) {
          const res = await airVoiceApi.logs(lastSeq, controller.signal);
          if (res.lines.length) {
            lastSeq = res.last_seq;
            setLines((prev) => {
              const next = prev.concat(res.lines);
              return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next;
            });
          }
          more = res.more;
        }
      } catch {
        /* device busy or offline: retry on the next tick */
      }
    }
    timer = setTimeout(() => void poll(), POLL_MS);
  };

  onMount(() => void poll());
  onCleanup(() => {
    if (timer) clearTimeout(timer);
    controller?.abort();
  });

  const visible = () =>
    warnOnly() ? lines().filter((l) => l.level === 'E' || l.level === 'W') : lines();

  createEffect(() => {
    void visible().length;
    if (box && stickToBottom) {
      queueMicrotask(() => box && (box.scrollTop = box.scrollHeight));
    }
  });

  const clear = async () => {
    try {
      await airVoiceApi.clearLogs();
      setLines([]);
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  const download = () => {
    const text = lines()
      .map((l) => `${fmtTs(l.ts)} ${l.level} ${l.text}`)
      .join('\n');
    const blob = new Blob([text + '\n'], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `air_voice_log_${new Date().toISOString().replace(/[:.]/g, '-')}.txt`;
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <div class="flex flex-col gap-2 pt-2">
      <div class="flex flex-wrap items-center gap-2">
        <Button size="xs" variant="secondary" onClick={() => setPaused(!paused())}>
          {paused() ? t('avLogsResume') : t('avLogsPause')}
        </Button>
        <Button size="xs" variant="secondary" onClick={() => void clear()}>
          {t('avLogsClear')}
        </Button>
        <Button size="xs" variant="secondary" onClick={download}>
          {t('avLogsDownload')}
        </Button>
        <div class="flex-1" />
        <Switch label={t('avLogsWarnOnly') as string} checked={warnOnly()} onChange={setWarnOnly} />
      </div>
      <div
        ref={box}
        class="h-72 overflow-auto rounded-[var(--radius-sm)] bg-black/40 border border-[var(--color-border-subtle)] px-3 py-2 font-mono text-[0.72rem] leading-5"
        onScroll={(e) => {
          const el = e.currentTarget;
          stickToBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
        }}
      >
        <Show
          when={visible().length > 0}
          fallback={<span class="text-[var(--color-text-muted)]">{t('avLogsEmpty')}</span>}
        >
          <For each={visible()}>
            {(line) => (
              <div class="whitespace-pre-wrap break-all">
                <span class="text-[var(--color-text-muted)]">{fmtTs(line.ts)} </span>
                <span class={LEVEL_CLASS[line.level] ?? ''}>
                  {line.level} {line.text}
                </span>
              </div>
            )}
          </For>
        </Show>
      </div>
    </div>
  );
};
