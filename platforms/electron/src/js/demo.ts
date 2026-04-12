/**
 * Simple demo application for the CAD viewer.
 *
 * Lets the user pick a local DXF file via an <input type="file">,
 * loads it into the <cad-canvas> viewer, and shows basic drawing info.
 */
import './CadCanvas'; // registers <cad-canvas>

const fileInput = document.getElementById('fileInput') as HTMLInputElement | null;
const statusSpan = document.getElementById('status') as HTMLSpanElement | null;
const cadCanvas = document.getElementById('viewer') as import('./CadCanvas').CadCanvas | null;

if (!fileInput || !statusSpan || !cadCanvas) {
  throw new Error(
    'Demo page is missing required elements: #fileInput, #status, or #viewer',
  );
}

function setStatus(text: string): void {
  statusSpan!.textContent = text;
}

fileInput.addEventListener('change', async () => {
  const file = fileInput.files?.[0];
  if (!file) return;

  setStatus(`Loading "${file.name}"...`);

  try {
    const buffer = await file.arrayBuffer();
    await cadCanvas.loadBuffer(new Uint8Array(buffer));

    const info = cadCanvas.controller.getDrawingInfo();
    setStatus(
      `${file.name} — ${info.entityCount} entities, ${info.layerCount} layers`,
    );
  } catch (err) {
    setStatus(`Error: ${err}`);
    console.error('[demo] Load failed:', err);
  }

  // Reset the input so the same file can be re-loaded
  fileInput.value = '';
});

setStatus('Ready — open a DXF file');
