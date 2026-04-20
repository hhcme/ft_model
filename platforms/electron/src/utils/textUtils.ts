/** Strip MTEXT formatting codes for plain-text display. */
export function cleanMText(raw: string): string {
  let t = raw;
  t = t.replace(/\\P/g, '\n');
  t = t.replace(/\{\\[^}]*\}/g, '');
  t = t.replace(/[{}]/g, '');
  return t;
}
