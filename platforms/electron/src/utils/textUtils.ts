export interface RichTextRun {
  text: string;
  color?: [number, number, number];
  underline?: boolean;
  heightScale?: number;
}

export type RichTextLine = RichTextRun[];

const ACI_TABLE: [number, number, number][] = [
  [255,255,255],[255,0,0],[255,255,0],[0,255,0],[0,255,255],[0,0,255],
  [255,0,255],[255,255,255],[128,128,128],[192,192,192],
  [255,0,0],[255,127,127],[165,0,0],[165,82,82],[127,0,0],
  [127,63,63],[82,0,0],[82,41,41],[35,0,0],[35,18,18],
  [255,63,0],[255,159,127],[165,41,0],[165,103,82],[127,31,0],
  [127,79,63],[82,20,0],[82,51,41],[35,9,0],[35,22,18],
  [255,127,0],[255,191,127],[165,82,0],[165,124,82],[127,63,0],
  [127,95,63],[82,41,0],[82,62,41],[35,18,0],[35,27,18],
  [255,191,0],[255,223,127],[165,124,0],[165,145,82],[127,95,0],
  [127,111,63],[82,62,0],[82,72,41],[35,27,0],[35,31,18],
  [255,255,0],[255,255,127],[165,165,0],[165,165,82],[127,127,0],
  [127,127,63],[82,82,0],[82,82,41],[35,35,0],[35,35,18],
  [191,255,0],[223,255,127],[124,165,0],[145,165,82],[95,127,0],
  [111,127,63],[62,82,0],[72,82,41],[27,35,0],[31,35,18],
  [127,255,0],[191,255,127],[82,165,0],[124,165,82],[63,127,0],
  [95,127,63],[41,82,0],[62,82,41],[18,35,0],[27,35,18],
  [63,255,0],[159,255,127],[41,165,0],[103,165,82],[31,127,0],
  [79,127,63],[20,82,0],[51,82,41],[9,35,0],[22,35,18],
  [0,255,0],[127,255,127],[0,165,0],[82,165,82],[0,127,0],
  [63,127,63],[0,82,0],[41,82,41],[0,35,0],[18,35,18],
  [0,255,63],[127,255,159],[0,165,41],[82,165,103],[0,127,31],
  [63,127,79],[0,82,20],[41,82,51],[0,35,9],[18,35,22],
  [0,255,127],[127,255,191],[0,165,82],[82,165,124],[0,127,63],
  [63,127,95],[0,82,41],[41,82,62],[0,35,18],[18,35,27],
  [0,255,191],[127,255,223],[0,165,124],[82,165,145],[0,127,95],
  [63,127,111],[0,82,62],[41,82,72],[0,35,27],[18,35,31],
  [0,255,255],[127,255,255],[0,165,165],[82,165,165],[0,127,127],
  [63,127,127],[0,82,82],[41,82,82],[0,35,35],[18,35,35],
  [0,191,255],[127,223,255],[0,124,165],[82,145,165],[0,95,127],
  [63,111,127],[0,62,82],[41,72,82],[0,27,35],[18,31,35],
  [0,127,255],[127,191,255],[0,82,165],[82,124,165],[0,63,127],
  [63,95,127],[0,41,82],[41,62,82],[0,18,35],[18,27,35],
  [0,63,255],[127,159,255],[0,41,165],[82,103,165],[0,31,127],
  [63,79,127],[0,20,82],[41,51,82],[0,9,35],[18,22,35],
  [0,0,255],[127,127,255],[0,0,165],[82,82,165],[0,0,127],
  [63,63,127],[0,0,82],[41,41,82],[0,0,35],[18,18,35],
  [63,0,255],[159,127,255],[41,0,165],[103,82,165],[31,0,127],
  [79,63,127],[20,0,82],[51,41,82],[9,0,35],[22,18,35],
  [127,0,255],[191,127,255],[82,0,165],[124,82,165],[63,0,127],
  [95,63,127],[41,0,82],[62,41,82],[18,0,35],[27,18,35],
  [191,0,255],[223,127,255],[124,0,165],[145,82,165],[95,0,127],
  [111,63,127],[62,0,82],[72,41,82],[27,0,35],[31,18,35],
  [255,0,255],[255,127,255],[165,0,165],[165,82,165],[127,0,127],
  [127,63,127],[82,0,82],[82,41,82],[35,0,35],[35,18,35],
  [255,0,191],[255,127,223],[165,0,124],[165,82,145],[127,0,95],
  [127,63,111],[82,0,62],[82,41,72],[35,0,27],[35,18,31],
  [255,0,127],[255,127,191],[165,0,82],[165,82,124],[127,0,63],
  [127,63,95],[82,0,41],[82,41,62],[35,0,18],[35,18,27],
  [255,0,63],[255,127,159],[165,0,41],[165,82,103],[127,0,31],
  [127,63,79],[82,0,20],[82,41,51],[35,0,9],[35,18,22],
  [51,51,51],[91,91,91],[132,132,132],[173,173,173],[214,214,214],
  [255,255,255],
];

function aciColor(index: number): [number, number, number] | undefined {
  if (index <= 0 || index === 256 || index >= ACI_TABLE.length) return undefined;
  return ACI_TABLE[index];
}

interface MTextState {
  color?: [number, number, number];
  underline: boolean;
  heightScale: number;
}

function pushRun(
  lines: RichTextLine[],
  text: string,
  state: MTextState,
) {
  if (!text) return;
  const parts = text.split('\n');
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i].replace(/^\s*[.·]\s*/, '');
    if (part) {
      lines[lines.length - 1].push({
        text: part,
        color: state.color,
        underline: state.underline,
        heightScale: state.heightScale,
      });
    }
    if (i + 1 < parts.length) {
      lines.push([]);
    }
  }
}

/** Parse common MTEXT control codes into renderable runs. */
export function parseRichMText(raw: string): RichTextLine[] {
  const lines: RichTextLine[] = [[]];
  let buffer = '';
  let state: MTextState = { underline: false, heightScale: 1 };
  const stack: MTextState[] = [];

  const flush = () => {
    pushRun(lines, buffer, state);
    buffer = '';
  };

  for (let i = 0; i < raw.length; i++) {
    const ch = raw[i];
    if (ch === '{') {
      flush();
      stack.push({ ...state });
      continue;
    }
    if (ch === '}') {
      flush();
      state = stack.pop() ?? { underline: false, heightScale: 1 };
      continue;
    }
    if (ch !== '\\') {
      buffer += ch;
      continue;
    }

    const code = raw[i + 1] ?? '';
    if (code === 'P') {
      flush();
      lines.push([]);
      i += 1;
      continue;
    }
    if (code === 'L') {
      flush();
      state.underline = true;
      i += 1;
      continue;
    }
    if (code === 'l') {
      flush();
      state.underline = false;
      i += 1;
      continue;
    }
    if (code === 'C') {
      const end = raw.indexOf(';', i + 2);
      if (end !== -1) {
        flush();
        const aci = Number.parseInt(raw.slice(i + 2, end), 10);
        state.color = aciColor(aci);
        i = end;
        continue;
      }
    }
    if (code === 'H') {
      const end = raw.indexOf(';', i + 2);
      if (end !== -1) {
        flush();
        const spec = raw.slice(i + 2, end);
        const match = spec.match(/^-?\d+(?:\.\d+)?/);
        if (match) {
          const scale = Number.parseFloat(match[0]);
          if (Number.isFinite(scale) && scale > 0) {
            state.heightScale = Math.max(0.12, Math.min(8, scale));
          }
        }
        i = end;
        continue;
      }
    }
    if (code === 'f' || code === 'F' || code === 'W' ||
        code === 'Q' || code === 'T' || code === 'A' || code === 'p') {
      const end = raw.indexOf(';', i + 2);
      if (end !== -1) {
        i = end;
        continue;
      }
    }
    if (raw.slice(i, i + 3).toLowerCase() === '\\pi') {
      const end = raw.indexOf(';', i + 3);
      if (end !== -1) {
        i = end;
        continue;
      }
    }
    if (code === '~') {
      buffer += ' ';
      i += 1;
      continue;
    }
    if (code) {
      i += 1;
    }
  }
  flush();

  return lines
    .map((line) => line.filter((run) => run.text.trim().length > 0))
    .filter((line) => line.length > 0);
}

/** Strip MTEXT formatting codes for plain-text display. */
export function cleanMText(raw: string): string {
  let t = raw;
  t = t.replace(/\\P/g, '\n');
  t = t.replace(/\\[A-Za-z]+[^;\\{}]*;/g, '');
  t = t.replace(/\\pi-?\d+(?:\.\d+)?;/gi, '');
  t = t.replace(/\\[HWCQT][^;]*;/gi, '');
  t = t.replace(/\\[LlOoKk]/g, '');
  t = t.replace(/\{\\f[^;{}]*;/gi, '{');
  t = t.replace(/\{\\[^}]*\}/g, '');
  t = t.replace(/[{}]/g, '');
  return t
    .split('\n')
    .map((line) => line.replace(/^\s*[.·]\s*/, '').trim())
    .filter(Boolean)
    .join('\n');
}
