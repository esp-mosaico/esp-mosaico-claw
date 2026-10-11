import type { Component } from 'solid-js';
import { t } from '../../i18n';
import { fmtDb, meterPct } from './format';

type LevelBarProps = {
  label: string;
  rms: number;
  peak: number;
  color: string;
  active: boolean;
};

/** Horizontal RMS bar with a peak tick, scaled from -80 to 0 dBFS. */
export const LevelBar: Component<LevelBarProps> = (props) => {
  const clipping = () => props.active && props.peak > -1;
  return (
    <div class="flex flex-col gap-1">
      <div class="flex items-baseline justify-between gap-2 text-[0.78rem]">
        <span class="text-[var(--color-text-secondary)]">{props.label}</span>
        <span class="font-mono text-[var(--color-text-primary)]">
          {props.active ? fmtDb(props.rms, 1) : '—'}
          <span class="text-[var(--color-text-muted)]">
            {' '}
            / {t('avPeak')} {props.active ? fmtDb(props.peak, 1) : '—'}
          </span>
        </span>
      </div>
      <div class="relative h-3 rounded-full bg-white/[0.06] overflow-hidden">
        <div
          class="absolute inset-y-0 left-0 rounded-full transition-[width] duration-150"
          style={{
            width: `${props.active ? meterPct(props.rms) : 0}%`,
            background: clipping() ? 'var(--color-danger)' : props.color,
          }}
        />
        <div
          class="absolute inset-y-0 w-0.5 bg-white/80 transition-[left] duration-150"
          style={{ left: `${props.active ? meterPct(props.peak) : 0}%` }}
        />
        {/* -20 dBFS (speech target) and -1 dBFS (clip) guides */}
        <div class="absolute inset-y-0 w-px bg-white/20" style={{ left: `${meterPct(-20)}%` }} />
        <div class="absolute inset-y-0 w-px bg-red-400/40" style={{ left: `${meterPct(-1)}%` }} />
      </div>
    </div>
  );
};
