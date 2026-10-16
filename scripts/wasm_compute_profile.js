// Profile the same C ABI used by the browser compute worker, without a GPU.
// Usage: node scripts/wasm_compute_profile.js MODULE SCENE.nhb CONFIG.toml OUT.json [START END] [REPEATS]
// CONFIG must be flattened (nodehammer config flatten). SCENE must be uncompressed.
// Optional NH_CPU_PROFILE=/path/run.cpuprofile collects a V8 sampling profile.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const inspector = require('node:inspector');
const os = require('node:os');
const { performance } = require('node:perf_hooks');

async function main() {
    const [modulePath, scenePath, configPath, outputPath, startArg = '0', endArg = '90', repeatArg = '3'] = process.argv.slice(2);
    const start = Number(startArg), end = Number(endArg), repeats = Number(repeatArg);
    if (!outputPath || !Number.isFinite(start) || !Number.isFinite(end) ||
        !Number.isInteger(repeats) || repeats < 1) {
        throw new Error('usage: node wasm_compute_profile.js MODULE SCENE.nhb FLAT_CONFIG.toml OUT.json [START END] [REPEATS]');
    }
    const scene = fs.readFileSync(scenePath);
    const config = fs.readFileSync(configPath, 'utf8');
    const factory = require(path.resolve(modulePath));
    const runs = [];
    let timings = {}, errors = [], diagnostics = [];
    globalThis.nhComputeTiming = (stage, ms) => { timings[stage] = ms; };
    globalThis.nhComputeDiagnostic = (severity, code, message) => {
        diagnostics.push({ severity, code, message });
        if (severity >= 3) errors.push(`${code}: ${message}`);
    };
    globalThis.postMessage = m => {
        if (m?.nh === 'error') errors.push(m.message);
    };
    const session = new inspector.Session();
    const post = (method, params = {}) => new Promise((resolve, reject) => {
        session.post(method, params, (error, result) => error ? reject(error) : resolve(result));
    });
    if (process.env.NH_CPU_PROFILE) {
        session.connect();
        await post('Profiler.enable');
        await post('Profiler.start');
    }
    for (const wedge of [false, true]) {
        const initStart = performance.now();
        const mod = await factory();
        const initializationMs = performance.now() - initStart;
        let referenceHash;
        for (let i = 0; i <= repeats; ++i) {
            timings = {}; errors = []; diagnostics = [];
            const fresh = i === 0;
            const wallStart = performance.now();
            let input = 0, output = 0, lenPtr = 0;
            try {
                if (fresh) {
                    input = mod._malloc(scene.length);
                    if (!input) throw new Error('input allocation failed');
                    mod.HEAPU8.set(scene, input);
                }
                lenPtr = mod._malloc(4);
                if (!lenPtr) throw new Error('length allocation failed');
                const callStart = performance.now();
                output = mod.ccall('nh_compute_build', 'number',
                    ['number', 'number', 'number', 'string', 'number', 'number', 'number', 'number', 'number'],
                    [1, input, fresh ? scene.length : 0, fresh ? config : null, wedge ? 1 : 0, start, end, 0, lenPtr]);
                const callMs = performance.now() - callStart;
                if (!output || errors.length) throw new Error(errors.join('\n') || 'compute returned null');
                const bytes = mod.HEAPU32[lenPtr >>> 2];
                const result = Buffer.from(mod.HEAPU8.subarray(output, output + bytes));
                const wallMs = performance.now() - wallStart;
                if (result.subarray(4, 8).toString() !== 'NHR8') throw new Error('invalid render output');
                const sha256 = crypto.createHash('sha256').update(result).digest('hex');
                if (fresh) referenceHash = sha256;
                if (sha256 !== referenceHash) throw new Error('cached rebuild differs from fresh output');
                if (!('total' in timings)) throw new Error('rebuild module with NODEHAMMER_PROFILING=ON');
                const row = { wedge: wedge ? [start, end] : null, mode: fresh ? 'fresh' : 'cached',
                    iteration: i, initializationMs: fresh ? initializationMs : null,
                    callMs, wallMs, stagesMs: { ...timings }, diagnostics: [...diagnostics], outputBytes: bytes, sha256,
                    wasmHeapCapacityBytes: mod.HEAPU8.byteLength, processRssBytes: process.memoryUsage().rss };
                runs.push(row);
                console.error(`${wedge ? 'wedge' : 'uncut'} ${row.mode} ${i}: ${callMs.toFixed(1)} ms, ${bytes} bytes`);
            } finally {
                if (output) mod._free(output);
                if (input) mod._free(input);
                if (lenPtr) mod._free(lenPtr);
            }
        }
    }
    if (process.env.NH_CPU_PROFILE) {
        const { profile } = await post('Profiler.stop');
        fs.writeFileSync(process.env.NH_CPU_PROFILE, JSON.stringify(profile));
        session.disconnect();
    }
    fs.writeFileSync(outputPath, JSON.stringify({ schema: 1, node: process.version,
        platform: process.platform, arch: process.arch, cpu: os.cpus()[0]?.model,
        wasmSha256: crypto.createHash('sha256').update(fs.readFileSync(modulePath.replace(/\.js$/, '.wasm'))).digest('hex'),
        module: path.resolve(modulePath), scene: path.resolve(scenePath), config: path.resolve(configPath),
        sceneSha256: crypto.createHash('sha256').update(scene).digest('hex'),
        configSha256: crypto.createHash('sha256').update(config).digest('hex'),
        cpuProfile: process.env.NH_CPU_PROFILE || null, runs }, null, 2) + '\n');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
