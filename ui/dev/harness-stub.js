/* DEV ONLY — LAYOUT TEST STUB. Static sample data, NOT real audio or devices. */
(function () {
  var q = new URLSearchParams(location.search);
  var status = q.get('status') || 'running';
  var running = status === 'running';

  var dev = function (id, name, kind, kindLabel, extra) {
    return Object.assign(
      {
        id: id, name: name, kind: kind, kindLabel: kindLabel, isVirtual: kind === 'virtual',
        isWindowsDefault: false, isCinemaxOutput: false, isCinemaxSource: false, hasProfile: false,
        headphoneLike: /headphones|earbuds|headset|handsfree/.test(kind), sampleRate: 48000, channels: 2,
      },
      extra || {},
    );
  };
  var devices = [
    dev('sample-1', 'Sample Speakers (layout test)', 'speakers', 'Built-in speakers', { hasProfile: true }),
    dev('sample-2', 'Sample Earbuds (layout test)', 'bluetooth-earbuds', 'Bluetooth earbuds', { isCinemaxOutput: running, hasProfile: true, sampleRate: 44100 }),
    dev('sample-3', 'Sample Earbuds Hands-Free (layout test)', 'bluetooth-handsfree', 'Bluetooth hands-free', { sampleRate: 16000, channels: 1 }),
    dev('sample-4', 'Sample Wired Headphones (layout test)', 'headphones', 'Wired headphones'),
    dev('sample-v', 'Sample Virtual Cable (layout test)', 'virtual', 'Virtual device', { isWindowsDefault: running, isCinemaxSource: running }),
  ];
  if (q.get('nodevices')) devices = [];
  if (status === 'no-virtual-device') devices = devices.filter(function (d) { return !d.isVirtual; });

  var bands = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000].map(function (f, i) {
    return { freq: f, gainDb: [3, 2.5, 1, -0.5, -1, 0, 1.5, 2, 1.5, 1][i], q: 1.41 };
  });
  var params = {
    preset: 'cinema', preampDb: -2, eqEnabled: true, eqBands: bands, bassDb: 1, midDb: 0, trebleDb: 1.5,
    bassEnabled: true, bassBoostDb: 4.5, bassFreq: 80, subBassDb: 2, bassHarmonics: 0.35, bassProtect: true,
    dialogueEnabled: true, dialogueAmount: 0.4, backgroundDb: -2,
    surroundEnabled: true, surroundMode: 'auto', surroundIntensity: 0.6, stereoWidth: 0.65, roomAmount: 0.25,
    dynamicsEnabled: true, compressorAmount: 0.3, loudnessEnabled: true, loudnessTargetLufs: -16, limiterCeilingDb: -1,
  };
  var presets = [
    ['neutral', 'NEUTRAL', 'Transparent pass-through with peak protection only'],
    ['cinema', 'CINEMA', 'Big, controlled low end with clear dialogue and a wide stage'],
    ['imax', 'IMAX-STYLE', 'Maximum width and scale for blockbuster moments'],
    ['theatre', 'THEATRE', 'Balanced, natural room sound'],
    ['deep-bass', 'DEEP BASS', 'Strong sub-bass, protected against distortion'],
    ['gaming', 'GAMING', 'Precise positioning and footstep clarity'],
    ['music', 'MUSIC', 'Faithful tonal balance with gentle width'],
    ['dialogue', 'DIALOGUE', 'Voices forward, background pulled back'],
    ['night', 'NIGHT MODE', 'Tamed peaks and lifted quiet details for low volume'],
    ['custom', 'CUSTOM', 'Your own settings for this device'],
  ].map(function (p) { return { id: p[0], name: p[1], tagline: p[2] }; });

  var out = devices.find(function (d) { return d.isCinemaxOutput; }) || null;
  var src = devices.find(function (d) { return d.isVirtual; }) || null;
  var messages = {
    'no-virtual-device': 'No virtual audio device found. Install VB-CABLE to process system audio.',
    'no-output-device': 'All playback devices are disconnected.',
    error: 'Render device was invalidated (AUDCLNT_E_DEVICE_INVALIDATED).',
    starting: 'Opening devices…',
    running: '',
    off: '',
  };
  var state = {
    version: '0.0.0-layout-test',
    engine: {
      enabled: status !== 'off', status: status, message: messages[status] || '', bypass: false,
      source: running ? src : null, output: running ? out : null,
      inputSampleRate: running ? 48000 : 0, outputSampleRate: running ? 44100 : 0,
      inputChannels: running ? 2 : 0, outputChannels: running ? 2 : 0,
      bufferMs: running ? 20 : 0, latencyMs: running ? 38 : 0,
    },
    devices: devices,
    params: params,
    settings: {
      launchOnStartup: true, minimizeToTray: true, startEngineOnLaunch: false, autoSwitchDevices: true,
      bufferMs: 20, compensateWindowsVolume: true, preferredVirtualDeviceId: '', highQualityResampling: true,
    },
    presets: presets,
  };

  // Static sample meter frame (fixed shape, no motion).
  var spectrum = [];
  for (var i = 0; i < 64; i++) spectrum.push(-14 - i * 0.62 - (i < 6 ? 0 : 0) - (i % 7) * 1.5);
  var waveform = [];
  for (var j = 0; j < 256; j++) waveform.push(0.35 * Math.sin(j / 9) * Math.cos(j / 41));
  var meters = {
    active: running && !q.get('quiet'), spectrum: spectrum, waveform: waveform,
    peakDb: [-4.2, -5.1], rmsDb: [-14.5, -15.2], inputPeakDb: -7.8, bass: 0.55,
    limiterGrDb: 1.2, compressorGrDb: 2.4, loudnessLufs: -16.3, clip: false, limiting: true,
    cpuPercent: 3.1, bufferFillMs: 14.2, driftPpm: 12, xruns: 0,
  };

  var listeners = new Map();
  var nextHandle = 1;
  function fire(id, payload) {
    var m = listeners.get(id);
    if (m) m.forEach(function (fn) { fn(payload); });
  }
  function clone(x) { return JSON.parse(JSON.stringify(x)); }
  function handle(call) {
    var a = call.params || [];
    var r = null;
    switch (call.name) {
      case 'getState': r = state; break;
      case 'setEnabled': state.engine.enabled = !!a[0]; state.engine.status = a[0] ? 'running' : 'off'; r = state; break;
      case 'setBypass': state.engine.bypass = !!a[0]; r = state; break;
      case 'setParams': Object.assign(params, a[0]); if (!a[0].preset) params.preset = 'custom'; r = params; break;
      case 'setEqBand': Object.assign(params.eqBands[a[0]], a[1]); params.preset = 'custom'; r = params; break;
      case 'applyPreset': params.preset = a[0]; r = params; break;
      case 'resetProfile': params.preset = 'neutral'; r = params; break;
      case 'setSettings': Object.assign(state.settings, a[0]); r = state.settings; break;
      case 'selectOutputDevice':
      case 'selectSourceDevice':
      case 'resetSettings':
      case 'restoreDefaultRouting': r = state; break;
      default: r = true;
    }
    setTimeout(function () { fire('__juce__complete', { promiseId: call.resultId, result: clone(r) }); }, 20);
  }

  window.__JUCE__ = {
    initialisationData: { __juce__functions: [] },
    backend: {
      addEventListener: function (id, fn) {
        var h = nextHandle++;
        if (!listeners.has(id)) listeners.set(id, new Map());
        listeners.get(id).set(h, fn);
        return [id, h];
      },
      removeEventListener: function (handle) {
        var m = listeners.get(handle[0]);
        if (m) m.delete(handle[1]);
      },
      emitEvent: function (id, payload) {
        if (id === '__juce__invoke') handle(payload);
      },
    },
  };
  setInterval(function () { fire('cinemaxMeters', clone(meters)); }, 100);

  document.addEventListener('DOMContentLoaded', function () {
    var b = document.createElement('div');
    b.textContent = 'LAYOUT TEST — NOT REAL AUDIO';
    b.style.cssText =
      'position:fixed;left:50%;top:6px;transform:translateX(-50%);z-index:9999;padding:4px 12px;border-radius:6px;' +
      'background:#ffe14d;color:#000;font:700 11px/1.4 Segoe UI,system-ui;letter-spacing:.12em;pointer-events:none';
    document.body.appendChild(b);
  });
})();
