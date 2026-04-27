import 'package:flutter/material.dart';
import 'src/cad_controller.dart';

void main() {
  runApp(const CadViewerApp());
}

class CadViewerApp extends StatelessWidget {
  const CadViewerApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'CAD Viewer',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark().copyWith(
        colorScheme: ColorScheme.dark(primary: Color(0xFF00FF88)),
        scaffoldBackgroundColor: Color(0xFF1E1E2E),
      ),
      home: const CadViewerPage(),
    );
  }
}

class CadViewerPage extends StatefulWidget {
  const CadViewerPage({super.key});

  @override
  State<CadViewerPage> createState() => _CadViewerPageState();
}

class _CadViewerPageState extends State<CadViewerPage> {
  final CadController _engine = CadController();

  @override
  void initState() {
    super.initState();
    _engine.initialize();
  }

  @override
  void dispose() {
    _engine.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('CAD Viewer'),
        actions: [
          IconButton(
            icon: const Icon(Icons.folder_open),
            tooltip: 'Open file',
            onPressed: _openFile,
          ),
          IconButton(
            icon: const Icon(Icons.fit_screen),
            tooltip: 'Fit to extents',
            onPressed: () => _engine.fitToExtents(),
          ),
        ],
      ),
      body: GestureDetector(
        onPanUpdate: (details) => _engine.pan(-details.delta.dx, details.delta.dy),
        onScaleUpdate: (details) {
          if (details.pointerCount == 1) return;
          _engine.zoom(details.scale, details.focalPoint.dx, details.focalPoint.dy);
        },
        child: CustomPaint(
          painter: _CadCanvasPainter(_engine),
          size: Size.infinite,
        ),
      ),
    );
  }

  void _openFile() {
    // File picker integration would go here.
    // For now, show a placeholder.
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('File picker integration pending')),
    );
  }
}

/// Placeholder painter — actual rendering would decode the command buffer
/// from the C++ engine and draw via Canvas API.
class _CadCanvasPainter extends CustomPainter {
  final CadController _engine;
  _CadCanvasPainter(this._engine);

  @override
  void paint(Canvas canvas, Size size) {
    _engine.resize(size.width.toInt(), size.height.toInt());
    final bg = Paint()..color = Color(0xFF1E1E2E);
    canvas.drawRect(Rect.fromLTWH(0, 0, size.width, size.height), bg);

    final ext = _engine.getExtents();
    if (ext == null) {
      final tp = TextPainter(
        text: TextSpan(text: 'No file loaded', style: TextStyle(color: Colors.white54, fontSize: 16)),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(canvas, Offset((size.width - tp.width) / 2, (size.height - tp.height) / 2));
    }
  }

  @override
  bool shouldRepaint(covariant _CadCanvasPainter old) => true;
}
