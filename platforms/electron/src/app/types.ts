/** CAD render batch exported from C++ engine. */
export interface Batch {
  topology: 'lines' | 'linestrip' | 'triangles';
  color: [number, number, number];
  layerName: string;
  vertices: number[]; // flat [x0,y0,x1,y1,...]
  breaks?: number[];
  lineWidth?: number;
  linePattern?: number[];
  space?: 'model' | 'paper' | 'unknown';
  layoutId?: number;
  viewportId?: number;
  drawOrder?: number;
  bounds?: Bounds;
  /** LOD tier: 0=standard, 1=low, 2=minimal */
  lodLevel?: number;
  /** EntityModifier bitmask (kModAlwaysDraw=1, kModScreenOriented=2, kModScreenSpaceSize=4, ...) */
  modifiers?: number;
  /** EntitySemantic value (0=Geometry, 1=Annotation, 2=Text, 3=Fill, 4=Structure, 5=Helper) */
  semantic?: number;
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

/** SceneNode type (mirrors C++ SceneNodeType). */
export type SceneNodeType =
  | 'modelSpace'
  | 'paperSpace'
  | 'layoutRoot'
  | 'blockDefinition'
  | 'blockInstance'
  | 'viewport'
  | 'entity';

/** Hierarchical scene tree node exported from C++ build_scene_tree(). */
export interface SceneNode {
  id: number;
  type: SceneNodeType;
  parentId: number;          // -1 = root
  visible: boolean;
  children?: number[];       // child node IDs
  entityIndices?: number[];  // entity vector indices (leaf nodes)
  blockDefId?: number;       // BlockInstance: referenced block definition node ID
  layoutIndex?: number;      // LayoutRoot: layout table index
  viewportIndex?: number;    // Viewport: viewport table index
  modifiers?: number;        // EntityModifier bitmask
  worldBounds?: Bounds;      // aggregated subtree world bounds
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
  usesNamedPlotStyles?: boolean;
  plotStyleTable?: string;
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
  entityTypeCounts?: Record<string, number>;
  sceneTree?: SceneNode[];
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
export type AppPhase = 'landing' | 'parsing' | 'viewer' | 'compare' | 'error';

export interface CompareResult {
  ours: DrawData | null;
  ourError: string | null;
  loading?: {
    ours?: boolean;
  };
  errors?: {
    ourParser?: string | null;
  };
  refInfo: {
    entityCount: number;
    ourEntityCount?: number;
    ourRenderTimeMs: number;
  };
}

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
