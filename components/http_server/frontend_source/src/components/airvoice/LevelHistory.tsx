import { createEffect, onCleanup, onMount, type Component } from 'solid-js';
import { DB_FLOOR } from './format';

export type HistoryPoint = {
  t: number;
  inRms: number;
  outRms: number;
  noise: number;
  vad: boolean;
};

export const HISTORY_WINDOW_MS = 60_000;
const HEIGHT = 150;
const GRID_DB = [0, -20, -40, -60];

type Props = { points: HistoryPoint[] };

/** Rolling 60 s level chart: input, output and noise floor, VAD shaded. */
export const LevelHistory: Component<Props> = (props) => {
  let canvas: HTMLCanvasElement | undefined;
  let observer: ResizeObserver | undefined;

  const draw = (pts: HistoryPoint[] = props.points) => {
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const width = canvas.clientWidth;
    canvas.width = Math.max(1, Math.floor(width * dpr));
    canvas.height = Math.floor(HEIGHT * dpr);
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, width, HEIGHT);
    const now = Date.now();
    const x = (time: number) => width - ((now - time) / HISTORY_WINDOW_MS) * width;
    const y = (db: number) =>
      Math.max(0, Math.min(HEIGHT, (Math.max(db, DB_FLOOR) / DB_FLOOR) * HEIGHT));

    ctx.font = '10px ui-monospace, monospace';
    ctx.textBaseline = 'top';
    for (const db of GRID_DB) {
      ctx.strokeStyle = 'rgba(255,255,255,0.07)';
      ctx.beginPath();
      ctx.moveTo(0, y(db) + 0.5);
      ctx.lineTo(width, y(db) + 0.5);
      ctx.stroke();
      ctx.fillStyle = 'rgba(255,255,255,0.35)';
      ctx.fillText(`${db}`, 4, Math.min(HEIGHT - 12, y(db) + 2));
    }

    ctx.fillStyle = 'rgba(104,211,145,0.10)';
    for (let i = 1; i < pts.length; i++) {
      const cur = pts[i];
      const prev = pts[i - 1];
      if (cur && prev && cur.vad) {
        ctx.fillRect(x(prev.t), 0, Math.max(1, x(cur.t) - x(prev.t)), HEIGHT);
      }
    }
    const line = (pick: (p: HistoryPoint) => number, color: string, dash: number[] = []) => {
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.5;
      ctx.setLineDash(dash);
      ctx.beginPath();
      pts.forEach((p, i) => {
        const px = x(p.t);
        const py = y(pick(p));
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      });
      ctx.stroke();
      ctx.setLineDash([]);
    };
    line((p) => p.noise, 'rgba(154,160,176,0.8)', [4, 3]);
    line((p) => p.inRms, '#f6ad55');
    line((p) => p.outRms, '#68d391');
  };

  onMount(() => {
    observer = new ResizeObserver(() => draw());
    if (canvas) observer.observe(canvas);
  });
  onCleanup(() => observer?.disconnect());
  /* The parent replaces the array on every poll, so tracking it redraws. */
  createEffect(() => draw(props.points));

  return <canvas ref={canvas} class="w-full block" style={{ height: `${HEIGHT}px` }} />;
};
