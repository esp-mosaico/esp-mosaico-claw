export const DB_FLOOR = -80;

export function fmtDb(value: number | undefined, digits = 0): string {
  if (value === undefined || !Number.isFinite(value) || value <= -119) return '—';
  return `${value.toFixed(digits)} dB`;
}

export function fmtDuration(ms: number): string {
  const total = Math.max(0, Math.round(ms / 100)) / 10;
  const minutes = Math.floor(total / 60);
  const seconds = total - minutes * 60;
  return `${String(minutes).padStart(2, '0')}:${seconds.toFixed(1).padStart(4, '0')}`;
}

export function fmtBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}

export function fmtTime(epochSeconds: number): string {
  if (!epochSeconds || epochSeconds < 1_600_000_000) return '—';
  return new Date(epochSeconds * 1000).toLocaleString();
}

/** Map dBFS to a 0..100 meter position over [DB_FLOOR, 0]. */
export function meterPct(dbfs: number): number {
  if (!Number.isFinite(dbfs)) return 0;
  return Math.max(0, Math.min(100, ((dbfs - DB_FLOOR) / -DB_FLOOR) * 100));
}
