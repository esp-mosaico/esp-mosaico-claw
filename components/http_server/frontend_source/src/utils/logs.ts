/** A log frame may contain several physical lines and terminal color escapes. */
export function splitLogLines(text: string): string[] {
  const lines = text.replace(/\x1b\[[0-9;]*m/g, '').split(/\r?\n/);
  if (lines.at(-1) === '') lines.pop();
  return lines;
}

export function logLineClass(line: string): string {
  if (/^I(?:\s|$)/.test(line)) return 'text-green-500';
  if (/^W(?:\s|$)/.test(line)) return 'text-yellow-500';
  if (/^E(?:\s|$)/.test(line)) return 'text-red-500';
  return '';
}
