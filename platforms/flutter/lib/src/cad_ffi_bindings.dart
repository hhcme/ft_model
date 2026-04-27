import 'dart:ffi';
import 'dart:io' show Platform;
import 'package:ffi/ffi.dart';

// ============================================================
// CAD Engine FFI Bindings — Dart wrappers for C FFI API
// ============================================================

final class CadEngine extends Struct {
  @Uint64()
  external int _handle;
}

typedef _CreateNative = Pointer CadEngine Function();
typedef _DestroyNative = Void Function(Pointer<CadEngine>);
typedef _InitNative = Int32 Function(Pointer<CadEngine>, Pointer<Void>, Int32, Int32);
typedef _LoadFileNative = Int32 Function(Pointer<CadEngine>, Pointer<Utf8>);
typedef _LoadBufferNative = Int32 Function(Pointer<CadEngine>, Pointer<Uint8>, UintPtr);
typedef _RenderNative = Void Function(Pointer<CadEngine>);
typedef _PanNative = Void Function(Pointer<CadEngine>, Float, Float);
typedef _ZoomNative = Void Function(Pointer<CadEngine>, Float, Float, Float);
typedef _FitNative = Void Function(Pointer<CadEngine>);
typedef _ResizeNative = Void Function(Pointer<CadEngine>, Int32, Int32);
typedef _ShutdownNative = Void Function(Pointer<CadEngine>);
typedef _GetCmdBufNative = Pointer<Uint8> Function(Pointer<CadEngine>);
typedef _GetCmdBufSizeNative = UintPtr Function(Pointer<CadEngine>);
typedef _GetExtentsNative = Void Function(Pointer<CadEngine>, Pointer<Float>, Pointer<Float>, Pointer<Float>, Pointer<Float>);

class CadEngineBindings {
  late final DynamicLibrary _lib;

  late final Pointer<CadEngine> Function() create;
  late final void Function(Pointer<CadEngine>) destroy;
  late final int Function(Pointer<CadEngine>, Pointer<Void>, int, int) initialize;
  late final int Function(Pointer<CadEngine>, Pointer<Utf8>) loadFile;
  late final int Function(Pointer<CadEngine>, Pointer<Uint8>, int) loadBuffer;
  late final void Function(Pointer<CadEngine>) renderFrame;
  late final void Function(Pointer<CadEngine>, double, double) pan;
  late final void Function(Pointer<CadEngine>, double, double, double) zoom;
  late final void Function(Pointer<CadEngine>) fitToExtents;
  late final void Function(Pointer<CadEngine>, int, int) resize;
  late final void Function(Pointer<CadEngine>) shutdown;
  late final Pointer<Uint8> Function(Pointer<CadEngine>) getCommandBuffer;
  late final int Function(Pointer<CadEngine>) getCommandBufferSize;
  late final void Function(Pointer<CadEngine>, Pointer<Float>, Pointer<Float>, Pointer<Float>, Pointer<Float>) getExtents;

  CadEngineBindings() {
    _lib = _loadLibrary();
    create = _lib.lookupFunction<Pointer<CadEngine> Function(), Pointer<CadEngine> Function()>('cad_engine_create');
    destroy = _lib.lookupFunction<Void Function(Pointer<CadEngine>), void Function(Pointer<CadEngine>)>('cad_engine_destroy');
    initialize = _lib.lookupFunction<_InitNative, int Function(Pointer<CadEngine>, Pointer<Void>, int, int)>('cad_engine_initialize');
    loadFile = _lib.lookupFunction<_LoadFileNative, int Function(Pointer<CadEngine>, Pointer<Utf8>)>('cad_engine_load_dxf');
    loadBuffer = _lib.lookupFunction<_LoadBufferNative, int Function(Pointer<CadEngine>, Pointer<Uint8>, int)>('cad_engine_load_dxf_buffer');
    renderFrame = _lib.lookupFunction<_RenderNative, void Function(Pointer<CadEngine>)>('cad_engine_render_frame');
    pan = _lib.lookupFunction<_PanNative, void Function(Pointer<CadEngine>, double, double)>('cad_engine_pan_camera');
    zoom = _lib.lookupFunction<_ZoomNative, void Function(Pointer<CadEngine>, double, double, double)>('cad_engine_zoom_camera');
    fitToExtents = _lib.lookupFunction<_FitNative, void Function(Pointer<CadEngine>)>('cad_engine_fit_to_extents');
    resize = _lib.lookupFunction<_ResizeNative, void Function(Pointer<CadEngine>, int, int)>('cad_engine_resize_viewport');
    shutdown = _lib.lookupFunction<_ShutdownNative, void Function(Pointer<CadEngine>)>('cad_engine_shutdown');
    getCommandBuffer = _lib.lookupFunction<_GetCmdBufNative, Pointer<Uint8> Function(Pointer<CadEngine>)>('cad_engine_get_command_buffer');
    getCommandBufferSize = _lib.lookupFunction<_GetCmdBufSizeNative, int Function(Pointer<CadEngine>)>('cad_engine_get_command_buffer_size');
    getExtents = _lib.lookupFunction<_GetExtentsNative, void Function(Pointer<CadEngine>, Pointer<Float>, Pointer<Float>, Pointer<Float>, Pointer<Float>)>('cad_engine_get_extents');
  }

  static DynamicLibrary _loadLibrary() {
    if (Platform.isAndroid) return DynamicLibrary.open('libcad_core.so');
    if (Platform.isIOS) return DynamicLibrary.process();
    if (Platform.isMacOS) return DynamicLibrary.open('libcad_core.dylib');
    if (Platform.isLinux) return DynamicLibrary.open('libcad_core.so');
    if (Platform.isWindows) return DynamicLibrary.open('cad_core.dll');
    throw UnsupportedError('Unsupported platform');
  }
}
