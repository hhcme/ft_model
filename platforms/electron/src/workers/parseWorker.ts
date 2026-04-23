/** Web Worker for off-main-thread JSON parsing and gzip decompression. */

self.onmessage = async (e: MessageEvent) => {
  const { id, buffer, fileName } = e.data as {
    id: number; buffer: ArrayBuffer; fileName: string;
  };

  try {
    let text: string;
    const isGzip = fileName.toLowerCase().endsWith('.gz');

    if (isGzip) {
      const ds = new DecompressionStream('gzip');
      const writer = ds.writable.getWriter();
      writer.write(new Uint8Array(buffer));
      writer.close();
      const reader = ds.readable.getReader();
      const chunks: Uint8Array[] = [];
      let total = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        total += value.length;
      }
      const merged = new Uint8Array(total);
      let offset = 0;
      for (const chunk of chunks) {
        merged.set(chunk, offset);
        offset += chunk.length;
      }
      text = new TextDecoder().decode(merged);
    } else {
      text = new TextDecoder().decode(buffer);
    }

    const data = JSON.parse(text);
    self.postMessage({ id, data });
  } catch (err: any) {
    self.postMessage({ id, error: err.message || String(err) });
  }
};
