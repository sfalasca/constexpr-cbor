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

  let toJson = null;
  let freeString = null;
  let moduleRef = null;

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

  async function handleFile(file) {
    clearError();
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

    jsonEl.textContent = json;
    resultMetaEl.textContent = `${file.name} — ${bytes.length.toLocaleString()} bytes decoded`;
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

  // `CborModule` is the global factory dist/cbor.js exports under
  // -sMODULARIZE=1 -sEXPORT_NAME=CborModule.
  CborModule()
    .then((Module) => {
      moduleRef = Module;
      toJson = Module.cwrap('cbor_to_json', 'number', ['number', 'number']);
      freeString = Module.cwrap('cbor_free_string', null, ['number']);
      setStatus('Decoder ready.');
      dropEl.querySelector('p:last-child').textContent = 'Decoder ready.';
    })
    .catch((e) => {
      showError(`Failed to load the WebAssembly decoder: ${e}`);
    });
})();
