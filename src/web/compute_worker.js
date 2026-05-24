// compute_worker.js — Web Worker host for the headless nodehammer compute module.
//
// Runs tessellation + the wedge cut off the main thread. The viewer (main
// thread) posts build requests; this worker owns a *separate* wasm instance
// (nodehammer-compute.js, its own heap) and replies with NHR8 render bytes.
// Nothing is shared: scene bytes come in copied, render bytes go back
// transferred. No SharedArrayBuffer → no COOP/COEP headers required.
//
// Must be created as a classic worker (`new Worker('compute_worker.js')`) so
// importScripts is available.
//
// Protocol (main -> worker):
//   { nh:'build', epoch, sceneBytes?:ArrayBuffer, configToml?:string,
//     wedge?:{ startDeg, endDeg, margin } }
//   sceneBytes/configToml may be omitted when `epoch` matches the last scene
//   already shipped to the wasm cache (e.g. re-aiming the wedge).
//
// Protocol (worker -> main):
//   { nh:'progress', phase, processed, total }   (emitted from wasm via EM_JS)
//   { nh:'error', message }                       (emitted from wasm via EM_JS)
//   { nh:'result', epoch, buffer:ArrayBuffer }    (NHR8 render bytes, transferred)

// The MODULARIZEd loader defines the global NodehammerCompute factory.
importScripts('nodehammer-compute.js');

let modulePromise = null;
function getModule() {
    if (!modulePromise) {
        modulePromise = NodehammerCompute();
    }
    return modulePromise;
}

// Epoch whose scene bytes are already resident in the wasm cache. Lets re-aim
// requests skip re-sending (and re-deserializing) the scene.
let loadedEpoch = -1;

self.onmessage = async function (e) {
    const msg = e.data || {};
    if (msg.nh !== 'build') {
        return;
    }

    let mod;
    try {
        mod = await getModule();
    } catch (err) {
        self.postMessage({ nh: 'error', message: 'compute: module load failed: ' + err });
        return;
    }

    const epoch = msg.epoch >>> 0;
    const wedge = msg.wedge || null;

    // Stage scene bytes into the wasm heap only when this epoch isn't cached.
    let scenePtr = 0;
    let sceneLen = 0;
    const sendBytes = !!msg.sceneBytes && epoch !== loadedEpoch;
    if (sendBytes) {
        const bytes = new Uint8Array(msg.sceneBytes);
        sceneLen = bytes.length;
        scenePtr = mod._malloc(sceneLen);
        mod.HEAPU8.set(bytes, scenePtr);
    }

    const lenPtr = mod._malloc(4);
    let dataPtr = 0;
    try {
        dataPtr = mod.ccall(
            'nh_compute_build',
            'number',
            ['number', 'number', 'number', 'string', 'number', 'number', 'number', 'number', 'number'],
            [
                epoch,
                scenePtr,
                sceneLen,
                sendBytes ? (msg.configToml || '') : null,
                wedge ? 1 : 0,
                wedge ? wedge.startDeg : 0,
                wedge ? wedge.endDeg : 0,
                wedge ? (wedge.margin != null ? wedge.margin : 2.0) : 0,
                lenPtr,
            ]
        );
    } finally {
        if (scenePtr) {
            mod._free(scenePtr);
        }
    }

    if (!dataPtr) {
        // The module already posted an { nh:'error' } message via EM_JS.
        mod._free(lenPtr);
        return;
    }

    // Re-read the heap views after the build (growth may have moved them).
    const len = mod.HEAPU32[lenPtr >> 2];
    mod._free(lenPtr);

    // Copy out of the wasm heap into a standalone buffer we can transfer, then
    // free the wasm-side copy.
    const out = new Uint8Array(len);
    out.set(mod.HEAPU8.subarray(dataPtr, dataPtr + len));
    mod._free(dataPtr);

    loadedEpoch = epoch;
    self.postMessage({ nh: 'result', epoch: epoch, buffer: out.buffer }, [out.buffer]);
};
