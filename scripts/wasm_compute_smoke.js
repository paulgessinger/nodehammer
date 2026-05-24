// Headless smoke for the compute-worker wasm module.
//
// Loads nodehammer-compute.js in node and drives nh_compute_build with a real
// semantic .nhb, asserting it returns NHR8 render bytes — i.e. the full C ABI
// path (heap staging, ccall marshalling, malloc'd return, out_len) that the
// browser worker uses, exercised without a browser.
//
// Usage: node wasm_compute_smoke.js <nodehammer-compute.js> <scene.nhb>

const path = require('path');
const fs = require('fs');

const modulePath = process.argv[2];
const scenePath = process.argv[3];
if (!modulePath || !scenePath) {
    console.error('usage: node wasm_compute_smoke.js <nodehammer-compute.js> <scene.nhb>');
    process.exit(2);
}

// The module's EM_JS reporters call postMessage when it exists; node's main
// thread has none, so define one to observe progress / capture errors.
let errorMsg = null;
let progressCount = 0;
globalThis.postMessage = (m) => {
    if (!m) return;
    if (m.nh === 'error') errorMsg = m.message;
    else if (m.nh === 'progress') progressCount++;
};

const NodehammerCompute = require(path.resolve(modulePath));

NodehammerCompute().then((mod) => {
    if (typeof mod._nh_compute_build !== 'function') {
        console.error('FAIL: _nh_compute_build is not exported');
        process.exit(1);
    }

    const scene = new Uint8Array(fs.readFileSync(scenePath));
    const scenePtr = mod._malloc(scene.length);
    mod.HEAPU8.set(scene, scenePtr);
    const lenPtr = mod._malloc(4);

    const dataPtr = mod.ccall(
        'nh_compute_build',
        'number',
        ['number', 'number', 'number', 'string', 'number', 'number', 'number', 'number', 'number'],
        [1, scenePtr, scene.length, '', 0, 0, 0, 0, lenPtr]
    );
    mod._free(scenePtr);

    if (!dataPtr) {
        console.error('FAIL: nh_compute_build returned null:', errorMsg || '(no message)');
        process.exit(1);
    }

    const len = mod.HEAPU32[lenPtr >> 2];
    mod._free(lenPtr);
    const out = Buffer.from(mod.HEAPU8.subarray(dataPtr, dataPtr + len));
    mod._free(dataPtr);

    // FlatBuffer file identifier sits at bytes [4,8).
    const ident = len >= 8 ? out.slice(4, 8).toString('latin1') : '';
    if (ident !== 'NHR8') {
        console.error(`FAIL: expected NHR8 identifier, got "${ident}" (len=${len})`);
        process.exit(1);
    }

    // Re-build at the same epoch WITHOUT resending bytes — exercises the cache.
    const lenPtr2 = mod._malloc(4);
    const dataPtr2 = mod.ccall(
        'nh_compute_build',
        'number',
        ['number', 'number', 'number', 'string', 'number', 'number', 'number', 'number', 'number'],
        [1, 0, 0, null, 0, 0, 0, 0, lenPtr2]
    );
    if (!dataPtr2) {
        console.error('FAIL: cached re-build returned null:', errorMsg || '(no message)');
        process.exit(1);
    }
    const len2 = mod.HEAPU32[lenPtr2 >> 2];
    mod._free(lenPtr2);
    mod._free(dataPtr2);

    console.log(
        `OK: nh_compute_build -> ${len} bytes (NHR8), cache re-build -> ${len2} bytes, ` +
        `${progressCount} progress msgs`
    );
    process.exit(0);
});
