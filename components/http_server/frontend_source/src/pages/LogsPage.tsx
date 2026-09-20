import { createSignal, For, onCleanup, onMount, Show, type Component } from 'solid-js';
import { TabShell } from '../components/layout/TabShell';
import { Button } from '../components/ui/Button';
import { PageHeader } from '../components/ui/PageHeader';
import { t } from '../i18n';

const MAX_VISIBLE_LINES = 300;

export const LogsPage: Component = () => {
  const [lines, setLines] = createSignal<string[]>([]);
  const [status, setStatus] = createSignal<'connecting' | 'connected' | 'disconnected'>(
    'connecting',
  );
  let socket: WebSocket | undefined;
  let reconnectTimer: ReturnType<typeof setTimeout> | undefined;
  let output: HTMLDivElement | undefined;
  let disposed = false;

  const connect = () => {
    if (disposed) return;
    setStatus('connecting');
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const next = new WebSocket(`${protocol}//${window.location.host}/ws/logs`);
    socket = next;
    next.onopen = () => setStatus('connected');
    next.onmessage = (event: MessageEvent<string>) => {
      const followTail =
        !output || output.scrollTop + output.clientHeight >= output.scrollHeight - 32;
      setLines((current) => [...current, event.data].slice(-MAX_VISIBLE_LINES));
      if (followTail) {
        requestAnimationFrame(() => output?.scrollTo(0, output.scrollHeight));
      }
    };
    next.onclose = () => {
      if (disposed) return;
      setStatus('disconnected');
      reconnectTimer = setTimeout(connect, 2000);
    };
    next.onerror = () => next.close();
  };

  onMount(connect);
  onCleanup(() => {
    disposed = true;
    if (reconnectTimer) clearTimeout(reconnectTimer);
    socket?.close();
  });

  const statusLabel = () => {
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
        <Show when={lines().length > 0} fallback={<span>{t('logsEmpty')}</span>}>
          <For each={lines()}>
            {(line) => <div class="whitespace-pre-wrap break-all">{line}</div>}
          </For>
        </Show>
      </div>
    </TabShell>
  );
};
