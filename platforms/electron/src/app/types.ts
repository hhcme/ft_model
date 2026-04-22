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
  rectWidth?: number;
  rectHeight?: number;
  space?: 'model' | 'paper' | 'unknown';
  layoutId?: number;
  viewportId?: number;
  styleName?: string;
  styleIndex?: number;
  kind?: 'text' | 'mtext' | 'dimension';
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
  type: 'layout' | 'layoutViewport' | 'model' | 'fallback';
  source?: string;
  paperMode?: boolean;
  layoutIndex?: number;
  viewportId?: number;
  entitySpace?: 'model' | 'paper' | 'unknown';
  hasViewportEntity?: boolean;
  bounds?: Bounds;
  presentationBounds?: Bounds;
  paperBounds?: Bounds;
  plotWindow?: Bounds;
  clipBounds?: Bounds;
  modelBounds?: Bounds;
  viewHeight?: number;
  customScale?: number;
  twistAngle?: number;
  modelCoverage?: number;
  targetModelBounds?: Bounds;
  targetModelCoverage?: number;
  targetPlusCenterModelBounds?: Bounds;
  targetPlusCenterModelCoverage?: number;
  frozenLayerCount?: number;
  frozenLayers?: string[];
  paperCenter?: { x: number; y: number; z?: number };
  modelViewCenter?: { x: number; y: number; z?: number };
  modelViewTarget?: { x: number; y: number; z?: number };
  center?: { x: number; y: number; z?: number };
  limits?: Bounds;
  extents?: Bounds;
  insertionBase?: { x: number; y: number; z?: number };
  plotScale?: number;
  plotRotation?: number;
  paperUnits?: number;
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

export interface DrawingInfo {
  acadVersion?: string;
  headerVarsBytes?: number;
  extents?: Bounds;
  insertionBase?: { x: number; y: number; z?: number };
  textSize?: number;
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
  drawingInfo?: DrawingInfo;
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
