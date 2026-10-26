// Wires the drop zone / file picker to the Emscripten-compiled
// constexpr-cbor decoder (dist/cbor.js, loaded as a global `CborModule`
// factory via -sMODULARIZE=1 -sEXPORT_NAME=CborModule). No framework, no
// build step for this file itself -- it's the whole viewer's logic.
(() => {
  const dropEl = document.getElementById('drop');
  const fileInputEl = document.getElementById('file-input');
  const statusEl = document.getElementById('status');
  const errorEl = document.getElementById('error');
  const resultEl = document.getElementById('result');
  const resultMetaEl = document.getElementById('result-meta');
  const jsonEl = document.getElementById('json');
  const downloadBtn = document.getElementById('download-btn');
  const encodeErrorEl = document.getElementById('encode-error');

  let toJson = null;
  let freeString = null;
  let fromJson = null;
  let freeBytes = null;
  let moduleRef = null;
  let currentFileName = 'decoded.cbor';

  function setStatus(text) {
    statusEl.textContent = text;
  }

  function showError(text) {
    errorEl.textContent = text;
    errorEl.style.display = 'block';
    resultEl.style.display = 'none';
  }

  function clearError() {
    errorEl.style.display = 'none';
  }

  function showEncodeError(text) {
    encodeErrorEl.textContent = text;
    encodeErrorEl.style.display = 'block';
  }

  function clearEncodeError() {
    encodeErrorEl.style.display = 'none';
  }

  function decode(bytes) {
    const Module = moduleRef;
    const ptr = Module._malloc(bytes.length);
    try {
      Module.HEAPU8.set(bytes, ptr);
      const resultPtr = toJson(ptr, bytes.length);
      if (resultPtr === 0) {
        return null;
      }
      const json = Module.UTF8ToString(resultPtr);
      freeString(resultPtr);
      return json;
    } finally {
      Module._free(ptr);
    }
  }

  // Encodes the given JSON text (following the same conventions decode()
  // produces, documented in README.md) to CBOR bytes, or returns null if
  // the text isn't well-formed JSON / doesn't match one of those
  // conventions -- the encode side of the same "refuse, don't crash"
  // contract the decoder follows.
  function encode(jsonText) {
    const Module = moduleRef;
    const textBytes = new TextEncoder().encode(jsonText);
    const textPtr = Module._malloc(textBytes.length || 1);
    const outLenPtr = Module._malloc(4);
    try {
      Module.HEAPU8.set(textBytes, textPtr);
      const resultPtr = fromJson(textPtr, textBytes.length, outLenPtr);
      if (resultPtr === 0) {
        return null;
      }
      const outLen = Module.HEAPU32[outLenPtr >> 2];
      const bytes = Module.HEAPU8.slice(resultPtr, resultPtr + outLen);
      freeBytes(resultPtr);
      return bytes;
    } finally {
      Module._free(textPtr);
      Module._free(outLenPtr);
    }
  }

  function downloadFileName() {
    const base = currentFileName.replace(/\.cbor$/i, '');
    return `${base}-edited.cbor`;
  }

  function handleDownload() {
    clearEncodeError();
    if (!fromJson) {
      showEncodeError('The encoder has not finished loading yet. Try again in a moment.');
      return;
    }

    let bytes;
    try {
      bytes = encode(jsonEl.value);
    } catch (e) {
      showEncodeError(`Encoder crashed unexpectedly: ${e}`);
      return;
    }

    if (bytes === null) {
      showEncodeError(
        'This text is not valid JSON, or uses a shape this viewer doesn’t ' +
          'recognize (see the README’s conventions table for byte strings, ' +
          'bignums, tags, and simple values).'
      );
      return;
    }

    const blob = new Blob([bytes], {type: 'application/cbor'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = downloadFileName();
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    setStatus(`Downloaded ${downloadFileName()} (${bytes.length.toLocaleString()} bytes).`);
  }

  async function handleFile(file) {
    clearError();
    clearEncodeError();
    resultEl.style.display = 'none';
    setStatus(`Reading ${file.name} (${file.size.toLocaleString()} bytes)...`);

    const buf = await file.arrayBuffer();
    const bytes = new Uint8Array(buf);

    if (!toJson) {
      showError('The decoder has not finished loading yet. Try again in a moment.');
      return;
    }

    let json;
    try {
      json = decode(bytes);
    } catch (e) {
      showError(`Decoder crashed unexpectedly: ${e}`);
      return;
    }

    if (json === null) {
      showError(
        `"${file.name}" was refused: it is not well-formed CBOR, or exceeds a ` +
          'built-in safety limit (e.g. nesting depth). This is the library’s ' +
          'own "refuse, don’t crash" contract, not a viewer bug.'
      );
      setStatus(`Failed to decode ${file.name}.`);
      return;
    }

    currentFileName = file.name;
    jsonEl.value = json;
    resultMetaEl.textContent = `${file.name} — ${bytes.length.toLocaleString()} bytes decoded, editable below`;
    resultEl.style.display = 'block';
    setStatus(`Decoded ${file.name}.`);
  }

  dropEl.addEventListener('click', () => fileInputEl.click());
  fileInputEl.addEventListener('change', () => {
    if (fileInputEl.files && fileInputEl.files[0]) {
      handleFile(fileInputEl.files[0]);
    }
  });
  dropEl.addEventListener('dragover', (e) => {
    e.preventDefault();
    dropEl.classList.add('dragover');
  });
  dropEl.addEventListener('dragleave', () => dropEl.classList.remove('dragover'));
  dropEl.addEventListener('drop', (e) => {
    e.preventDefault();
    dropEl.classList.remove('dragover');
    const file = e.dataTransfer.files && e.dataTransfer.files[0];
    if (file) handleFile(file);
  });
  downloadBtn.addEventListener('click', handleDownload);
  jsonEl.addEventListener('input', clearEncodeError);

  // `CborModule` is the global factory dist/cbor.js exports under
  // -sMODULARIZE=1 -sEXPORT_NAME=CborModule.
  CborModule()
    .then((Module) => {
      moduleRef = Module;
      toJson = Module.cwrap('cbor_to_json', 'number', ['number', 'number']);
      freeString = Module.cwrap('cbor_free_string', null, ['number']);
      fromJson = Module.cwrap('cbor_from_json', 'number', ['number', 'number', 'number']);
      freeBytes = Module.cwrap('cbor_free_bytes', null, ['number']);
      setStatus('Decoder ready.');
      dropEl.querySelector('p:last-of-type').textContent = 'Decoder ready.';
    })
    .catch((e) => {
      showError(`Failed to load the WebAssembly decoder: ${e}`);
    });
})();
