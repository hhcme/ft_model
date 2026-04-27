import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import 'cad_ffi_bindings.dart';

/// High-level Dart wrapper for the CAD engine.
class CadController {
  final CadEngineBindings _bindings;
  Pointer<CadEngine>? _engine;

  CadController() : _bindings = CadEngineBindings();

  bool get isInitialized => _engine != null;

  /// Initialize the engine. Call once before loading files.
  void initialize({int width = 800, int height = 600}) {
    _engine = _bindings.create();
    _bindings.initialize(_engine!, nullptr, width, height);
  }

  /// Load a CAD file by path.
  bool loadFile(String path) {
    if (_engine == null) return false;
    final cPath = path.toNativeUtf8();
    try {
      return _bindings.loadFile(_engine!, cPath) == 0;
    } finally {
      malloc.free(cPath);
    }
  }

  /// Load a CAD file from raw bytes (DXF or DWG).
  bool loadBuffer(Uint8List data) {
    if (_engine == null) return false;
    final buf = malloc<Uint8>(data.length);
    try {
      buf.asTypedList(data.length).setAll(0, data);
      return _bindings.loadBuffer(_engine!, buf, data.length) == 0;
    } finally {
      malloc.free(buf);
    }
  }

  /// Get the render command buffer (for Flutter Canvas rendering).
  Uint8List? getCommandBuffer() {
    if (_engine == null) return null;
    final ptr = _bindings.getCommandBuffer(_engine!);
    final size = _bindings.getCommandBufferSize(_engine!);
    if (ptr == nullptr || size == 0) return null;
    return ptr.asTypedList(size);
  }

  /// Pan the view by (dx, dy) in screen pixels.
  void pan(double dx, double dy) {
    if (_engine == null) return;
    _bindings.pan(_engine!, dx, dy);
  }

  /// Zoom by factor around pivot (px, py) in screen coords.
  void zoom(double factor, double px, double py) {
    if (_engine == null) return;
    _bindings.zoom(_engine!, factor, px, py);
  }

  /// Fit view to drawing extents.
  void fitToExtents() {
    if (_engine == null) return;
    _bindings.fitToExtents(_engine!);
  }

  /// Notify engine of canvas resize.
  void resize(int width, int height) {
    if (_engine == null) return;
    _bindings.resize(_engine!, width, height);
  }

  /// Get drawing extents as (minX, minY, maxX, maxY).
  (double, double, double, double)? getExtents() {
    if (_engine == null) return null;
    final pMinX = malloc<Float>(), pMinY = malloc<Float>();
    final pMaxX = malloc<Float>(), pMaxY = malloc<Float>();
    try {
      _bindings.getExtents(_engine!, pMinX, pMinY, pMaxX, pMaxY);
      return (pMinX.value, pMinY.value, pMaxX.value, pMaxY.value);
    } finally {
      malloc.free(pMinX);
      malloc.free(pMinY);
      malloc.free(pMaxX);
      malloc.free(pMaxY);
    }
  }

  /// Release engine resources.
  void dispose() {
    if (_engine != null) {
      _bindings.shutdown(_engine!);
      _bindings.destroy(_engine!);
      _engine = null;
    }
  }
}
