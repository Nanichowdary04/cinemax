// ============================================================================
//  CINEMAX — UI <-> engine contract
//
//  This file is the single source of truth for the data exchanged between the
//  React frontend and the C++ host (JUCE WebBrowserComponent / WebView2).
//  The C++ side (app/src/WebBridge.cpp) serialises exactly these shapes.
//
//  Transport (JUCE native integration, see bridge.ts):
//    * Native functions: JS emits "__juce__invoke" {name, params, resultId};
//      C++ answers with event "__juce__complete" {promiseId, result}.
//    * Events from C++: window.__JUCE__.backend.addEventListener(id, fn).
//
//  Rule: the UI never invents audio or device data. If the bridge is not
//  available (e.g. opened in a normal browser) the UI shows a disconnected
//  state instead of placeholder numbers.
// ============================================================================

export type PresetId =
  | 'neutral'
  | 'cinema'
  | 'imax'
  | 'theatre'
  | 'deep-bass'
  | 'gaming'
  | 'music'
  | 'concert-hall'
  | 'dialogue'
  | 'night'
  | 'dolby-style'
  | 'custom';

export type IeqProfile = 'balanced' | 'warm' | 'detailed';

export type DeviceKind =
  | 'speakers'              // built-in / analog speakers
  | 'headphones'            // wired headphones (jack)
  | 'headset'               // wired headset
  | 'bluetooth-headphones'
  | 'bluetooth-earbuds'
  | 'bluetooth-speaker'
  | 'bluetooth-handsfree'   // narrow-band HFP endpoint (never auto-selected)
  | 'usb'
  | 'hdmi'
  | 'digital'               // S/PDIF etc.
  | 'line-out'
  | 'virtual'               // virtual cable used as CINEMAX source
  | 'unknown';

export interface DeviceInfo {
  id: string;               // Windows endpoint id
  name: string;             // e.g. "Headphones (TWS)"
  kind: DeviceKind;
  kindLabel: string;        // e.g. "Bluetooth earbuds"
  isVirtual: boolean;
  isWindowsDefault: boolean; // current Windows default playback device (multimedia role)
  isCinemaxOutput: boolean;  // CINEMAX is rendering processed audio to this device
  isCinemaxSource: boolean;  // CINEMAX captures system audio from this (virtual) device
  hasProfile: boolean;       // a saved per-device profile exists
  headphoneLike: boolean;    // spatial engine uses binaural (HRTF) mode for this device
  sampleRate: number;        // shared-mode mix format, 0 if unknown
  channels: number;          // shared-mode channel count, 0 if unknown
}

export type EngineStatus =
  | 'off'                 // engine disabled, Windows routing untouched
  | 'starting'
  | 'running'             // routed + processing
  | 'no-virtual-device'   // enabled but no virtual cable installed
  | 'no-output-device'    // enabled but no usable real output device
  | 'error'
  | 'passthrough';        // a Dolby-capable TV decodes the sound itself; CINEMAX rests

export interface EngineInfo {
  enabled: boolean;        // master ON/OFF requested by the user
  status: EngineStatus;
  message: string;         // human-readable detail for the status
  bypass: boolean;         // routed but DSP bypassed (A/B compare)
  source: DeviceInfo | null; // virtual capture device
  output: DeviceInfo | null; // real output device
  inputSampleRate: number;
  outputSampleRate: number;
  inputChannels: number;
  outputChannels: number;
  bufferMs: number;        // render buffer duration actually granted by Windows
  latencyMs: number;       // estimated latency added by CINEMAX
  detectedContent: DetectedContent; // content analyser verdict (after hysteresis)
  detectedVia: string;     // what decided it: an app/site name ("Netflix", "VLC", "YouTube Music") or "sound analysis"
}

/** What the user is listening to. 'manual' = they picked or edited a preset themselves. */
export type ListeningMode = 'manual' | 'music' | 'movies' | 'auto';
export type DetectedContent = 'unknown' | 'music' | 'movie' | 'game';

export interface Settings {
  listeningMode: ListeningMode;
  musicPreset: PresetId;           // preset used by Music mode
  moviePreset: PresetId;           // preset used by Movies mode
  gamePreset: PresetId;            // Auto: preset while a game is in front (picture + sound scenes)
  smartTone: boolean;              // automatic bass / treble per song or film
  earProfile: string;              // 'auto' | 'model' | a measured set key (headphone 3D sound)
  headTracking: boolean;           // webcam head tracking keeps the sound on the screen
  headTrackingFlip: boolean;       // mirrored camera picture: turn the other way
  launchOnStartup: boolean;
  minimizeToTray: boolean;
  startEngineOnLaunch: boolean;
  newDevicePreset: PresetId;       // sound for a device without a saved profile (never 'custom')
  autoSwitchDevices: boolean;
  bufferMs: number;              // requested: 10 | 20 | 40 | 80
  compensateWindowsVolume: boolean;
  preferredVirtualDeviceId: string; // '' = automatic
  highQualityResampling: boolean;
}

export interface EqBand {
  freq: number;   // Hz, 20..20000
  gainDb: number; // -12..+12
  q: number;      // 0.3..8
}

export type SurroundMode = 'auto' | 'headphones' | 'speakers';

export interface DspParams {
  preset: PresetId;
  preampDb: number;            // -12..+6

  eqEnabled: boolean;
  eqBands: EqBand[];           // always 10 bands
  bassDb: number;              // tone control, low shelf 100 Hz, -12..+12
  midDb: number;               // tone control, peak 1 kHz, -12..+12
  trebleDb: number;            // tone control, high shelf 8 kHz, -12..+12

  bassEnabled: boolean;
  bassBoostDb: number;         // 0..12  (low shelf at bassFreq)
  bassFreq: number;            // 40..160 Hz
  subBassDb: number;           // -6..+9 (peak ~45 Hz)
  bassHarmonics: number;       // 0..1 psychoacoustic bass for small drivers
  bassProtect: boolean;        // dynamic LF limiter + device-aware high-pass

  dialogueEnabled: boolean;
  dialogueAmount: number;      // 0..1 voice clarity
  backgroundDb: number;        // -12..0 ambience/background level
  ieqEnabled: boolean;         // smart EQ (Dolby-style intelligent equalizer)
  ieqProfile: IeqProfile;      // target tonal balance
  ieqAmount: number;           // 0..1 how strongly it steers

  surroundEnabled: boolean;
  surroundMode: SurroundMode;
  surroundIntensity: number;   // 0..1 virtual surround / HRTF amount
  stereoWidth: number;         // 0..1  (0.5 = original width, 1 = widest)
  roomAmount: number;          // 0..1  theatre room ambience
  heightAmount: number;        // 0..1  3D height: overhead layer on virtual ceiling speakers
  roomSize: number;            // 0..1  venue: small theatre (RT60 ~0.45 s) .. concert hall (~2.1 s)
  immersive: boolean;          // Atmos-style 7.1.4 up-mix (real centre, surrounds, 4 overhead)
  volumeModeler: boolean;      // volume-aware tone: bass / top restored at low volume

  dynamicsEnabled: boolean;
  compressorAmount: number;    // 0..1  dynamic range reduction
  loudnessEnabled: boolean;    // slow loudness normalisation
  loudnessTargetLufs: number;  // -24..-10
  limiterCeilingDb: number;    // -6..-0.1 (limiter is always on)
}

export interface PresetInfo {
  id: PresetId;
  name: string;     // "CINEMA"
  tagline: string;  // one line description
}

export interface AppState {
  version: string;
  engine: EngineInfo;
  devices: DeviceInfo[];       // active playback devices (incl. virtual)
  params: DspParams;           // profile of the current output device
  settings: Settings;
  presets: PresetInfo[];
  vision: VisionState;         // CINEMAX Vision (picture)
  spatial: SpatialState;       // headphone 3D sound: real ears + head tracking
  tv: TvState;                 // HDMI / optical outputs (TV, soundbar, receiver)
}

// ---- Headphone 3D sound -----------------------------------------------------
export interface EarProfile {
  key: string;     // 'model' or a measured set key
  name: string;
  info: string;    // where the measurement comes from (and its licence)
}

export type HeadTrackingStatus =
  | 'off' | 'starting' | 'tracking' | 'no-face' | 'no-camera' | 'camera-busy' | 'camera-denied' | 'error';

export interface HeadTrackingState {
  status: HeadTrackingStatus;
  message: string;
  yaw: number;      // degrees, + = head turned right
  pitch: number;    // degrees, + = looking up
  fps: number;
}

export interface SpatialState {
  earProfiles: EarProfile[];       // head model first, then measured sets
  activeEarProfile: string;        // resolved key in use
  headTracking?: HeadTrackingState;
}

// ---- CINEMAX Vision (picture processing) ----------------------------------
export type PicturePresetId = 'cinema' | 'hdr' | 'vivid' | 'gaming' | 'work' | 'night' | 'custom';
export type Scene = 'none' | 'movie' | 'game' | 'work' | 'music';

export interface PictureParams {
  preset: PicturePresetId;
  brightness: number;      // -50..50 mid-tone lightness
  contrast: number;        // -50..50
  blackLevel: number;      // -20..20
  gamma: number;           // 1.8..2.6 (2.2 = unchanged)
  saturation: number;      // -50..50 %
  vibrance: number;        // 0..100
  temperatureK: number;    // 3500..10000 (6500 = unchanged)
  blueLight: number;       // 0..100
  gain: [number, number, number];    // -50..50 white balance (highlights)
  offset: [number, number, number];  // -20..20 white balance (shadows)
  sharpness: number;       // 0 = off, 1..5 hardware levels
  skinProtect: boolean;
}

export interface PictureAnalysis {
  shadowSteps: number;
  shadowStepsKept: number;
  highlightSteps: number;
  highlightStepsKept: number;
  white: [number, number, number];
  black: [number, number, number];
  gamutLimitedPercent: number;
  monotonic: boolean;
}

export interface DisplayView {
  key: string;
  name: string;
  adapter: string;
  internal: boolean;
  width: number;
  height: number;
  refreshHz: number;
  bitsPerColor: number;
  hdrSupported: boolean;
  hdrEnabled: boolean;
  wideColorSupported: boolean;
  wideColorEnabled: boolean;
  maxNits: number;
  minNits: number;
  sdrWhiteNits: number;
  gamutPercent: number;     // panel colour triangle area relative to sRGB
  ddcci: boolean;
  backlight: number;        // 0..100, -1 = not controllable
  baseBacklight: number;    // the user's own level (modes add their offset)
  backlightOffset: number;  // offset of the active picture mode (0 when off)
  backend: 'intel' | 'gamma' | 'none';
  hasLut: boolean;
  lutSize: number;
  hasSharpness: boolean;
  mode: 'auto' | 'manual';
  activePreset: PicturePresetId;
  hasCustom: boolean;
  params: PictureParams;
  processing: boolean;
  limitedByWindows: boolean;
  status: string;
  analysis: PictureAnalysis;
  hdmi: boolean;
  tv: TvPicture;
}

// ---- TV ---------------------------------------------------------------------
export type TvSpeakers = 'tv' | 'soundbar' | 'receiver';
export type TvRange = 'auto' | 'full' | 'limited';

export interface TvSetup {
  isTv: boolean;
  speakers: TvSpeakers;
  sizeInch: number;       // 24..120
  distanceM: number;      // 0.8..8
  lipSyncMs: number;      // 0..300
  letTvDecode: boolean;   // Dolby passthrough
}

export interface TvDolby {
  probed: boolean;
  dolbyDigital: boolean;
  dolbyDigitalPlus: boolean;
  atmos: boolean;
}

export interface TvAudioDevice {
  id: string;
  name: string;
  kind: DeviceKind;
  isOutput: boolean;
  setup: TvSetup;
  speakerAngle: number;   // degrees between the speakers, seen from the seat
  dolby: TvDolby;
}

export interface TvPicture {
  isTv: boolean;          // external HDMI display
  available: boolean;     // driver + TV accept HDMI signalling
  canRange: boolean;
  canContentType: boolean;
  canGameMode: boolean;
  range: TvRange;
  contentAuto: boolean;
  gameMode: boolean;
  sentContent: 'default' | 'off' | 'desktop' | 'film' | 'game';
  gameModeActive: boolean;
}

export interface TvState {
  devices: TvAudioDevice[];
}

export interface VisionSettings {
  enabled: boolean;
  autoPicture: boolean;
  linkAudio: boolean;
  browserVideoAsCinema: boolean;
  presetBacklight: boolean;       // picture modes move the backlight from the user's level
  defaultPreset: PicturePresetId;
}

export interface AppRule {
  exe: string;
  preset: PicturePresetId;
}

export interface VisionState {
  settings: VisionSettings;
  rules: AppRule[];
  presets: { id: PicturePresetId; name: string; tagline: string }[];
  displays: DisplayView[];
  scene: Scene;
  sceneApp: string;
  foregroundExe: string;
  comparing: boolean;
  nightLight: boolean;
  intelAvailable: boolean;
}

export interface Meters {
  active: boolean;               // audio is currently flowing through the engine
  spectrum: number[];            // 64 log-spaced bands 20 Hz..20 kHz, dBFS (-100..0)
  waveform: number[];            // 256 points, mono mix of output, -1..1
  peakDb: [number, number];      // output sample peak L/R since previous frame, dBFS
  rmsDb: [number, number];       // output RMS L/R, dBFS
  inputPeakDb: number;           // pre-processing peak (after volume compensation)
  bass: number;                  // 0..1 low-frequency activity
  limiterGrDb: number;           // limiter gain reduction, positive dB
  compressorGrDb: number;        // compressor gain reduction, positive dB
  loudnessLufs: number;          // short-term loudness of output, -70 if silent
  clip: boolean;                 // a full-scale sample was seen since previous frame (source audio
                                 // arriving already clipped, or output at 0 dBFS); the limiter prevents output clipping
  limiting: boolean;             // peak protection engaged since previous frame
  cpuPercent: number;            // DSP time as % of real-time budget
  bufferFillMs: number;          // capture FIFO fill
  driftPpm: number;              // clock-drift correction currently applied
  xruns: number;                 // total underruns/overruns since start
  movieProbability: number;      // 0..1 content analyser: how film-like the input is
  contentConfidence: number;     // 0..1 how much real audio has been analysed
  smartBassDb: number;           // smart tone bass gain currently applied (0 when off)
  smartTrebleDb: number;         // smart tone treble gain currently applied
}

export interface Notice {
  level: 'info' | 'warning' | 'error';
  message: string;
}

// ---- Native function names (all return Promises) --------------------------
export interface NativeApi {
  getState(): Promise<AppState>;
  setEnabled(on: boolean): Promise<AppState>;
  setBypass(on: boolean): Promise<AppState>;
  setParams(partial: Partial<DspParams>): Promise<DspParams>;   // sets preset 'custom' unless partial.preset given
  setEqBand(index: number, band: Partial<EqBand>): Promise<DspParams>;
  applyPreset(id: PresetId): Promise<DspParams>;
  resetProfile(): Promise<DspParams>;                         // neutral profile for current output device
  setListeningMode(mode: ListeningMode): Promise<AppState>;   // Music / Movies / Auto (applies the preset)
  selectOutputDevice(id: string): Promise<AppState>;
  selectSourceDevice(id: string): Promise<AppState>;          // '' = automatic
  setSettings(partial: Partial<Settings>): Promise<Settings>;
  resetSettings(): Promise<AppState>;
  restoreDefaultRouting(): Promise<AppState>;                 // engine off + restore Windows default output
  openWindowsSoundSettings(): Promise<boolean>;
  openExternalUrl(url: string): Promise<boolean>;             // whitelisted in C++ (VB-CABLE page only)
  setVisualizerActive(on: boolean): Promise<boolean>;
  startEarTest(key: string): Promise<boolean>;                // ear-fit test sound through one ear profile
  stopEarTest(): Promise<boolean>;
  getHeadTracking(): Promise<HeadTrackingState>;              // live status (poll while shown)
  setTvSetup(deviceId: string, partial: Partial<TvSetup>): Promise<AppState>;
  visionSetTv(displayKey: string, partial: Partial<Pick<TvPicture, 'range' | 'contentAuto' | 'gameMode'>>): Promise<AppState>;
  // CINEMAX Vision
  visionRefresh(): Promise<AppState>;
  visionSetEnabled(on: boolean): Promise<AppState>;
  visionSetSettings(partial: Partial<VisionSettings>): Promise<AppState>;
  visionSelectPreset(displayKey: string, id: PicturePresetId): Promise<AppState>;
  visionSetAuto(displayKey: string): Promise<AppState>;
  visionSetParams(displayKey: string, partial: Partial<PictureParams>): Promise<AppState>;
  visionSetBacklight(displayKey: string, percent: number): Promise<boolean>;
  visionResetDisplay(displayKey: string): Promise<AppState>;
  visionSaveProfiles(): Promise<boolean>;
  visionSetCompare(on: boolean): Promise<boolean>;
  visionRestoreOriginal(): Promise<AppState>;
  visionSetHdr(displayKey: string, on: boolean): Promise<AppState>;
  visionAddRule(exe: string, id: PicturePresetId): Promise<AppState>;
  visionRemoveRule(exe: string): Promise<AppState>;
  openWindowsDisplaySettings(nightLight: boolean): Promise<boolean>;
}

export const VB_CABLE_URL = 'https://vb-audio.com/Cable/';

// ---- Event ids emitted by C++ ---------------------------------------------
export const EVENT_STATE = 'cinemaxState';     // payload: AppState
export const EVENT_METERS = 'cinemaxMeters';   // payload: Meters (~30 Hz while visualiser active)
export const EVENT_NOTICE = 'cinemaxNotice';   // payload: Notice
