/** CAD render batch exported from C++ engine. */
export interface Batch {
  topology: 'lines' | 'linestrip' | 'triangles';
  color: [number, number, number];
  layerName: string;
  vertices: [number, number][];
  breaks?: number[];
  lineWidth?: number;
  linePattern?: number[];
  space?: 'model' | 'paper' | 'unknown';
  layoutId?: number;
  viewportId?: number;
  drawOrder?: number;
  bounds?: Bounds;
}

/** Text entity rendered via Canvas fillText. */
export interface TextEntity {
  x: number;
  y: number;
  text: string;
  height: number;
  rotation?: number;
  color: [number, number, number];
  layerName?: string;
  widthFactor?: number;
  space?: 'model' | 'paper' | 'unknown';
  layoutId?: number;
  viewportId?: number;
  styleName?: string;
  styleIndex?: number;
  align?: number;
}

/** Layer definition. */
export interface Layer {
  name: string;
  color: [number, number, number];
  frozen: boolean;
  off?: boolean;
  locked: boolean;
  plotEnabled?: boolean;
  lineweight?: number;
  linetypeIndex?: number;
  plotStyleIndex?: number;
}

/** Axis-aligned bounding box. */
export interface Bounds {
  minX: number;
  maxX: number;
  minY: number;
  maxY: number;
  isEmpty?: boolean;
}

export interface ViewDefinition {
  id: string;
  name: string;
  type: 'layout' | 'model' | 'fallback';
  source?: string;
  paperMode?: boolean;
  layoutIndex?: number;
  viewportId?: number;
  bounds?: Bounds;
  presentationBounds?: Bounds;
  paperBounds?: Bounds;
  plotWindow?: Bounds;
  clipBounds?: Bounds;
}

export interface DiagnosticGap {
  code: string;
  category: string;
  message: string;
  count?: number;
}

export interface DrawDiagnostics {
  layouts?: number;
  viewports?: number;
  gaps?: DiagnosticGap[];
}

/** Full drawing data from render_export. */
export interface DrawData {
  batches: Batch[];
  texts: TextEntity[];
  layers: Layer[];
  bounds: Bounds;
  presentationBounds?: Bounds;
  rawBounds?: Bounds;
  views?: ViewDefinition[];
  activeViewId?: string;
  diagnostics?: DrawDiagnostics;
  entityCount: number;
  totalVertices: number;
}

/** Viewport state for Canvas rendering. */
export interface Viewport {
  centerX: number;
  centerY: number;
  zoom: number;
  canvasWidth: number;
  canvasHeight: number;
  dpr: number;
}

/** Application phase. */
export type AppPhase = 'landing' | 'parsing' | 'viewer' | 'error';

/** Measurement point. */
export type MeasurePoint = [number, number];

/** Completed measurement. */
export interface Measurement {
  type: 'dist' | 'area';
  points: MeasurePoint[];
  value: number;
}

/** Batch bounding box for frustum culling. */
export interface BatchBounds {
  minX: number;
  maxX: number;
  minY: number;
  maxY: number;
}

/** Recent file metadata for localStorage. */
export interface RecentFile {
  name: string;
  size: number;
  timestamp: number;
  cacheKey: string;
  entityCount: number;
  totalVertices: number;
}
