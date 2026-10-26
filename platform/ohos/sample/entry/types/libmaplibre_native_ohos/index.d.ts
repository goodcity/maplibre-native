import type { NodeContent } from '@kit.ArkUI';

export interface ResourceOptions {
  apiKey?: string;
  cachePath?: string;
  assetPath?: string;
}

export interface LatLngBounds {
  west: number;
  south: number;
  east: number;
  north: number;
}

export interface BoundOptions {
  bounds?: LatLngBounds;
  minZoom?: number;
  maxZoom?: number;
  minPitch?: number;
  maxPitch?: number;
}

export interface Coordinate {
  longitude: number;
  latitude: number;
}

export interface CameraPadding {
  top?: number;
  left?: number;
  bottom?: number;
  right?: number;
}

export interface ScreenPoint {
  x: number;
  y: number;
}

export interface CameraOptions {
  longitude?: number;
  latitude?: number;
  center?: Coordinate;
  centerAltitude?: number;
  padding?: CameraPadding;
  anchor?: ScreenPoint;
  zoom?: number;
  bearing?: number;
  pitch?: number;
  roll?: number;
  fov?: number;
}

export interface FrameRateRange {
  min: number;
  max: number;
  expected: number;
}

export interface SurfaceState {
  renderedFrames: number;
  submittedRevision: number;
  renderedRevision: number;
  lastRenderDurationUs: number;
  lastSubmitDurationUs: number;
  width: number;
  height: number;
  hasWindow: boolean;
  hasSurface: boolean;
  hasMap: boolean;
  needsRender: boolean;
  styleLoaded: boolean;
  mapLoaded: boolean;
  fullyLoaded: boolean;
  /** JSON fetch/parse completion, without source or renderer completion. */
  styleJsonLoaded: boolean;
  /** JSON, sprite and source-description completion; excludes renderer tiles. */
  styleResourcesLoaded: boolean;
  spritesLoaded: boolean;
  sourceCount: number;
  unloadedSourceCount: number;
  unloadedImageSourceCount: number;
  unloadedGeoJsonSourceCount: number;
  /** Unloaded Vector, Raster and RasterDEM source descriptions. */
  unloadedTileSourceCount: number;
  /** Last completed frame status since the latest style/surface reset. */
  hasRenderFrameStatus: boolean;
  lastFrameWasFull: boolean;
  lastFrameNeedsRepaint: boolean;
  renderedFrameRate: number;
  frameCallbackRate: number;
  backend?: string;
  surfaceVisible: boolean;
  lastSurfaceError?: string;
  lastMapLoadError?: string;
  lastRenderError?: string;
  lastStyleImageMissing?: string;
  lastGlyphsError?: string;
  lastSpritesError?: string;
}

export interface XComponentContext {
  execute(json: string): string;
  destroy: () => void;
  getCameraOptions: () => CameraOptions;
  getPixelRatio: () => number;
  getStyleAttributions: () => string[];
  getSurfaceState: () => SurfaceState;
  jumpTo: (options: CameraOptions) => void;
  reduceMemoryUse: () => void;
  renderFrame: () => void;
  setBounds: (options: BoundOptions) => void;
  setClientOptions: (name: string, version?: string) => void;
  setFrameRateRange: (range: FrameRateRange) => void;
  setPixelRatio: (pixelRatio: number) => void;
  setRenderingEnabled: (enabled: boolean) => void;
  setResourceOptions: (options: ResourceOptions) => void;
  setStyleUrl: (url: string) => void;
  setTileCacheEnabled: (enabled: boolean) => void;
}

export function createMap(nodeContent: NodeContent): XComponentContext;

export function sampleHttpRequests(): number[];
export function sampleResources(): number[];

/** Live C++ instances and acquired OH decoder wrappers in this native library.
 * Counts do not measure pixels, native/GPU bytes, or OH framework caches.
 * Fields are sampled separately. decoderReleaseFailures is cumulative.
 */
export interface NativeImageResourceCounts {
  imageSourceImpls: number;
  renderImageSources: number;
  rasterBuckets: number;
  rendererImpls: number;
  decoderImageSources: number;
  decoderOptions: number;
  decoderPixelmaps: number;
  decoderImageInfos: number;
  decoderReleaseFailures: number;
}

export function sampleImageResources(): NativeImageResourceCounts;

/** Diagnostic scan interval only. Counts are descriptors, not unique fences or owners.
 * Times are truncated to microseconds; no FD, name, pointer or path is returned.
 * directoryComplete describes enumeration only; use individual query/error fields.
 */
export interface FenceLifecycleEvent {
  sequence: number;
  controllerId: number;
  timeUs: number;
  /** 1 close entered; 2 clearMapState returned (including any backend reset);
   * 3 owned ArkUI node dispose returned/skipped. None means GPU completion.
   */
  phase: number;
  operationAttempted: boolean;
}
export interface NativeFenceResources {
  schema: number;
  entries: number;
  syncCandidates: number;
  legacyCandidates: number;
  confirmedSyncDescriptors: number;
  closed: number;
  linkErrors: number;
  duplicateErrors: number;
  changedAfterDuplicate: number;
  queryOk: number;
  unsupported: number;
  denied: number;
  queryErrors: number;
  active: number;
  signaled: number;
  errorStatus: number;
  invalidStatus: number;
  componentTotal: number;
  componentReads: number;
  componentReadErrors: number;
  componentTooLarge: number;
  childActive: number;
  childSignaled: number;
  childError: number;
  childInvalid: number;
  timestamps: number;
  beginUs: number;
  endUs: number;
  firstTimestampUs: number;
  lastTimestampUs: number;
  directoryAvailable: boolean;
  directoryComplete: boolean;
  clockAvailable: boolean;
  syncReadComplete: boolean;
  lifecycleRecording: boolean;
  lifecycleEpoch: number;
  lifecycleCapacity: number;
  lifecycleOverwritten: number;
  lifecycleClockFailures: number;
  lifecycleEvents: FenceLifecycleEvent[];
}
export function sampleFenceResources(): NativeFenceResources;
/** Default off. Enabling from off starts a new bounded epoch; reading never drains. */
export function setFenceDiagnosticRecording(enabled: boolean): void;

declare const maplibreNative: {
  sampleFenceResources: typeof sampleFenceResources;
  setFenceDiagnosticRecording: typeof setFenceDiagnosticRecording;
  sampleImageResources: typeof sampleImageResources;
  sampleHttpRequests: typeof sampleHttpRequests;
  sampleResources: typeof sampleResources;
  createMap: typeof createMap;
};

export default maplibreNative;
