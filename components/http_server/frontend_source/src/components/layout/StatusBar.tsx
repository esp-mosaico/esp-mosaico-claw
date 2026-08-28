import { Show, type Component, type JSX } from 'solid-js';
import { t } from '../../i18n';
import { appStatus } from '../../state/config';

export const Logo: Component<{ class?: string }> = (props) => (
  <svg
    xmlns="http://www.w3.org/2000/svg"
    class={props.class ?? 'h-auto w-[6rem]'}
    viewBox="0 0 907 191.26"
  >
    <path d="M804.192 75.674V62.749h-52.59v69.066h53.675V118.89h-38.085v-15.293h33.349V90.67h-33.35V75.674Zm38.578-13.616c-15.984 0-26.738 7.893-26.738 20.424 0 24.47 36.802 16.872 36.802 29.896 0 4.243-3.848 6.61-10.261 6.61-7.301 0-17.365-4.045-24.47-10.36l-6.216 12.729c7.795 6.61 19.142 10.952 30.489 10.952 15.293 0 27.034-7.302 27.034-20.819.099-24.864-36.704-17.76-36.704-30.488 0-3.75 3.552-5.624 8.782-5.624 5.525 0 14.208 2.763 21.509 7.104l6.019-12.925c-6.907-4.539-16.576-7.499-26.246-7.499m37.592.691v69.066h15.59v-19.733h13.714c17.563 0 27.627-9.275 27.627-25.357 0-15.294-10.064-23.976-27.627-23.976zm15.59 12.925h12.925c8.682 0 13.616 3.848 13.616 11.544 0 7.893-4.934 11.84-13.616 11.84H895.95zm74.788 30.981V96.591h-25.455v10.064z" fill="currentColor" transform="translate(-577.247 -.281)scale(1.03374)" />
    <path d="M1048.795 131.925v-67.2h-9.408l-24.48 48.384-24.576-48.384h-9.504v67.2h7.296V76.533l24.384 48.288h4.608l24.48-48.288.096 55.392zm49.344-67.392c-19.68 0-35.232 14.784-35.232 33.792 0 18.912 15.552 33.984 35.232 33.984 19.584 0 35.136-15.072 35.136-33.984s-15.552-33.792-35.136-33.792m0 7.296c14.976 0 27.264 11.712 27.264 26.496s-12.288 26.592-27.264 26.592-27.456-11.808-27.456-26.592 12.48-26.496 27.456-26.496m69.12-7.392c-13.92 0-23.232 6.816-23.232 17.376 0 24.096 37.824 14.688 37.728 32.352 0 6.432-6.048 10.368-15.744 10.368-7.584 0-16.128-3.456-22.368-9.504l-3.456 6.912c6.336 6.048 15.936 10.272 25.728 10.272 14.4 0 24.288-7.2 24.288-18.432.096-24.384-37.728-15.36-37.728-32.64 0-5.664 5.664-8.928 14.304-8.928 5.376 0 12.48 1.632 18.528 5.664l3.264-7.2c-5.568-3.744-13.536-6.24-21.312-6.24m80.448 50.496 7.488 16.992h8.352l-30.048-67.2h-7.968l-30.144 67.2h8.16l7.488-16.992zm-3.264-7.392h-30.144l14.976-34.08zm37.344 24.384v-67.2h-7.68v67.2zm49.056-67.392c-19.488 0-35.04 14.88-35.04 33.696 0 19.008 15.36 34.08 34.752 34.08 9.312 0 18.336-4.128 24.96-10.368l-4.704-5.28c-5.28 4.992-12.48 8.064-19.872 8.064-15.168 0-27.36-11.808-27.36-26.496 0-14.784 12.192-26.496 27.36-26.496 7.392 0 14.688 3.168 19.872 8.448l4.608-5.856c-6.432-6.048-15.36-9.792-24.576-9.792m65.76 0c-19.68 0-35.232 14.784-35.232 33.792 0 18.912 15.552 33.984 35.232 33.984 19.584 0 35.136-15.072 35.136-33.984s-15.552-33.792-35.136-33.792m0 7.296c14.976 0 27.264 11.712 27.264 26.496s-12.288 26.592-27.264 26.592-27.456-11.808-27.456-26.592 12.48-26.496 27.456-26.496" fill="#ff4c01" transform="translate(-577.247 -.281)scale(1.03374)" />
    <path d="M86.412.084a4.64 4.64 0 0 0-2.383 1.258c-.84.84-1.36 2-1.36 3.27v85.42c0 2.54 2.07 4.609 4.61 4.609h71.42c1.27 0 2.432-.52 3.272-1.36a4.6 4.6 0 0 0 1.357-3.253c0-.306-.029-.611-.098-.897-.01-.13-.05-.25-.08-.38-.02-.02-.02-.049-.03-.079-.01-.11-.04-.21-.07-.31-.39-1.52-.81-3.032-1.27-4.532-.02-.05-.03-.12-.06-.18-6.44-21.48-18.14-40.688-33.58-56.128C117.02 16.392 103.95 7.21 89.5.56c-.14-.07-.27-.14-.41-.2a4.64 4.64 0 0 0-2.678-.277m76.916 89.944.002.013v-.02zM50.45 32.55c-.64 0-1.259.12-1.799.36-.03.02-.04.02-.07.03a83 83 0 0 0-24.47 16.95A82.4 82.4 0 0 0 2.25 88.66l-.13.49c-.05.28-.08.57-.08.88 0 2.54 2.07 4.61 4.61 4.61h43.8c1.28 0 2.44-.52 3.26-1.36.84-.84 1.36-1.99 1.36-3.26V37.173c0-1.28-.52-2.442-1.36-3.262a4.58 4.58 0 0 0-3.26-1.359m-45.83 75.89c-1.28 0-2.42.52-3.26 1.36S0 111.791 0 113.061c0 .15.02.32.03.47 2.04 33.83 24.4 62.19 55.05 73.02 8.63 3.05 17.91 4.711 27.6 4.711 22.88 0 43.57-9.262 58.57-24.262 13.83-13.84 22.8-32.55 24.09-53.36v-.038l.04-.541V113h.02a4.64 4.64 0 0 0-1.361-3.2 4.58 4.58 0 0 0-3.27-1.36z" fill="#e8362d" />
  </svg>
);

export const StatusSummary: Component<{ class?: string; compact?: boolean }> = (props) => {
  const online = () => appStatus()?.wifi_connected === true;
  const loading = () => appStatus() === null;

  return (
    <div
      class={[
        'min-w-0 flex items-center gap-2 text-[0.8rem]',
        props.compact ? 'flex-nowrap overflow-hidden' : 'flex-wrap',
        props.class ?? '',
      ]
        .join(' ')
        .trim()}
    >
      <span
        class={[
          'inline-flex items-center gap-2 px-3 py-1 rounded-full border font-medium min-w-0',
          loading()
            ? 'border-[var(--color-border-subtle)] text-[var(--color-text-muted)] bg-white/[0.04]'
            : online()
              ? 'border-[rgba(104,211,145,0.2)] bg-[var(--color-green-dim)] text-[var(--color-green)]'
              : 'border-[var(--color-border-subtle)] bg-white/[0.04] text-[var(--color-text-muted)]',
        ].join(' ')}
      >
        <span
          class={[
            'w-1.5 h-1.5 rounded-full',
            online() ? 'bg-[var(--color-green)] pulse-dot' : 'bg-[var(--color-text-muted)]',
          ].join(' ')}
        />
        <span class={props.compact ? 'truncate' : ''}>
          {loading()
            ? t('statusLoading')
            : online()
              ? t('statusOnline')
              : appStatus()?.ap_active
                ? t('statusApActive')
                : t('statusOffline')}
        </span>
      </span>
      <Show when={appStatus()?.ip}>
        <span
          class={[
            'text-[var(--color-border-subtle)] select-none',
            props.compact ? 'shrink-0' : '',
          ].join(' ')}
        >
          ·
        </span>
        <span
          class={[
            'font-mono text-[0.78rem] text-[var(--color-text-secondary)]',
            props.compact ? 'truncate min-w-0' : '',
          ].join(' ')}
        >
          IP: {appStatus()!.ip}
        </span>
      </Show>
      <Show when={appStatus()?.storage_base_path}>
        <span
          class={[
            'text-[var(--color-border-subtle)] select-none',
            props.compact ? 'shrink-0' : '',
          ].join(' ')}
        >
          ·
        </span>
        <span
          class={[
            'font-mono text-[0.78rem] text-[var(--color-text-secondary)]',
            props.compact ? 'truncate min-w-0' : '',
          ].join(' ')}
        >
          Storage: {appStatus()!.storage_base_path}
        </span>
      </Show>
    </div>
  );
};

type StatusBarProps = {
  leadingSlot?: JSX.Element;
  slot?: () => any;
};

export const StatusBar: Component<StatusBarProps> = (props) => {
  return (
    <header class="flex items-center justify-between gap-3 h-14 px-4 sm:px-5 border-b border-[var(--color-border-subtle)] bg-[rgba(10,11,14,0.85)] backdrop-blur-md sticky top-0 z-40">
      <div class="flex items-center gap-3 sm:gap-4 min-w-0">
        {props.leadingSlot}
        <a href="/" class="flex items-center text-[var(--color-text-primary)] no-underline">
          <Logo />
        </a>
        <div class="hidden lg:flex">
          <StatusSummary />
        </div>
      </div>
      <div class="flex items-center gap-2">{props.slot?.()}</div>
    </header>
  );
};
