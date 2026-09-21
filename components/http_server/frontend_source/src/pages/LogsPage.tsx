import { createSignal, For, onCleanup, onMount, Show, type Component } from 'solid-js';
import { TabShell } from '../components/layout/TabShell';
import { Button } from '../components/ui/Button';
import { PageHeader } from '../components/ui/PageHeader';
import { t } from '../i18n';
import { logLineClass, splitLogLines } from '../utils/logs';

const MAX_VISIBLE_LINES = 300;

export const LogsPage: Component = () => {
  const [lines, setLines] = createSignal<string[]>([]);
  const [status, setStatus] = createSignal<'connecting' | 'connected' | 'disconnected' | 'disabled'>(
    'connecting',
  );
  let socket: WebSocket | undefined;
  let statusTimer: ReturnType<typeof setTimeout> | undefined;
  let statusRequest: AbortController | undefined;
  let output: HTMLDivElement | undefined;
  let disposed = false;

  const closeSocket = () => {
    const previous = socket;
    socket = undefined;
    previous?.close();
  };

  const connect = () => {
    if (disposed) return;
    setStatus('connecting');
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const next = new WebSocket(`${protocol}//${window.location.host}/ws/logs`);
    socket = next;
    next.onopen = () => {
      if (!disposed && socket === next) setStatus('connected');
    };
    next.onmessage = (event: MessageEvent<string>) => {
      if (disposed || socket !== next) return;
      const followTail =
        !output || output.scrollTop + output.clientHeight >= output.scrollHeight - 32;
      setLines((current) => [...current, ...splitLogLines(event.data)].slice(-MAX_VISIBLE_LINES));
      if (followTail) {
        requestAnimationFrame(() => output?.scrollTo(0, output.scrollHeight));
      }
    };
    next.onclose = () => {
      if (disposed || socket !== next) return;
      socket = undefined;
      setStatus('disconnected');
    };
    next.onerror = () => next.close();
  };

  const pollStatus = async () => {
    if (disposed) return;
    const controller = new AbortController();
    statusRequest = controller;
    const timeout = setTimeout(() => controller.abort(), 5000);
    try {
      const response = await fetch('/api/logs/status', {
        cache: 'no-store', signal: controller.signal,
      });
      if (!response.ok) throw new Error('Log status unavailable');
      const result: { enabled: boolean } = await response.json();
      if (typeof result.enabled !== 'boolean') throw new Error('Invalid log status');
      if (disposed) return;
      if (!result.enabled) {
        closeSocket();
        setLines([]);
        setStatus('disabled');
      } else if (!socket) {
        connect();
      }
    } catch {
      if (!disposed) {
        closeSocket();
        setStatus('disconnected');
      }
    } finally {
      clearTimeout(timeout);
      if (!disposed) statusTimer = setTimeout(pollStatus, 2000);
    }
  };

  onMount(() => void pollStatus());
  onCleanup(() => {
    disposed = true;
    if (statusTimer) clearTimeout(statusTimer);
    statusRequest?.abort();
    closeSocket();
  });

  const statusLabel = () => {
    if (status() === 'disabled') return t('logsDisabled');
    if (status() === 'connected') return t('logsConnected');
    if (status() === 'connecting') return t('logsConnecting');
    return t('logsDisconnected');
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navLogs') as string}
        description={t('logsDesc') as string}
        actions={
          <>
            <span class="text-xs text-[var(--color-text-muted)]" role="status">
              {statusLabel()}
            </span>
            <Button size="sm" variant="secondary" onClick={() => setLines([])}>
              {t('logsClear')}
            </Button>
          </>
        }
      />
      <div
        ref={output}
        class="h-[min(65vh,640px)] overflow-auto bg-black/40 p-4 font-mono text-xs leading-5 text-[var(--color-text-secondary)]"
      >
        <Show when={lines().length > 0} fallback={
          <span>{status() === 'disabled' ? t('logsDisabledHint') : t('logsEmpty')}</span>
        }>
          <For each={lines()}>
            {(line) => <div class={`min-h-5 whitespace-pre-wrap break-all ${logLineClass(line)}`}>{line}</div>}
          </For>
        </Show>
      </div>
    </TabShell>
  );
};
