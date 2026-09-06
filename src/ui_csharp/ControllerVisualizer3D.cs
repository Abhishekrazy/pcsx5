using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Media.Media3D;

namespace Pcsx5Ui
{
    /// <summary>
    /// The 3D DualSense on the Input tab's Testing view. Loads the vendored
    /// parts (assets/gamepad/dualsense3d, one OBJ per animatable part, CC-BY-4.0
    /// credit in that folder) into WPF's own Viewport3D and animates them from
    /// the live pad state: the whole pad tilts with the accelerometer's gravity vector, L2/R2
    /// hinge with the trigger axes, buttons and D-pad arms depress, the sticks
    /// tilt, the touchpad depresses on click, and the lightbar glows in the
    /// configured colour. Pure presentation - it never owns pad state (Rule 11).
    ///
    /// Model space (OBJ exported with identity axes, same frame as the
    /// manifest): +X = player's right, -Y = the face (towards the viewer),
    /// +Z = the top edge (triggers / touchpad).
    /// </summary>
    public class ControllerVisualizer3D : Grid
    {
        private readonly Viewport3D _view = new Viewport3D();
        private readonly Transform3DGroup _rootXf = new Transform3DGroup();
        private readonly AxisAngleRotation3D _tiltX = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
        private readonly AxisAngleRotation3D _tiltZ = new AxisAngleRotation3D(new Vector3D(0, 0, 1), 0);   // roll about the top-edge axis
        // Resting pose: the pad LYING on a table seen from its near edge, which is
        // how everyone's pad sits. Applied after the gravity tilt, so tilt stays in
        // the pad's own frame and the pose is only the viewpoint.
        // The classic lying-down product view: top edge (triggers, lightbar) nearest
        // and low, the face seen at a shallow angle. Yaw 180 turns the top edge to
        // the camera, then the near edge is dropped.
        private readonly AxisAngleRotation3D _baseYaw = new AxisAngleRotation3D(new Vector3D(0, 1, 0), 0);      // bottom edge (grips) nearest, as in the user's reference photo
        private readonly AxisAngleRotation3D _basePose = new AxisAngleRotation3D(new Vector3D(1, 0, 0), -78);   // camera just above the table: grips and bottom edge nearest, face at a shallow angle (the user's reference photo)
        // Level reference: the gravity vector captured when the first samples
        // arrive (the pad is resting when the popup opens). Tilt is measured from
        // it, so a sensor that is not mounted exactly parallel to the face still
        // reads dead level at rest.
        private Vector3D _gRef, _gNow; private int _refSamples;
        // One rotation taking the resting gravity vector to the current one: identity
        // at rest whatever the sensor's mounting or sign convention.
        private readonly AxisAngleRotation3D _tiltFree = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
        private readonly Dictionary<string, Part> _parts = new Dictionary<string, Part>(StringComparer.Ordinal);
        private readonly List<EmissiveMaterial> _lightbar = new List<EmissiveMaterial>();
        private Color _lightbarColor = Color.FromRgb(0, 90, 255);
        private bool _muteLocal, _mutePrev;           // mute toggled by the pad's own button (the reader has no LED state)
        private int _player = 1;
        // Touch trails: per finger, a ring of small emissive dots on the touchpad
        // surface, the newest brightest. Touch space is 1920x1080 (INFERRED, the
        // DualSense's reported resolution); the pad's bounds come from the manifest.
        private const int TrailLen = 10;
        private readonly TranslateTransform3D[,] _trailPos = new TranslateTransform3D[2, TrailLen];
        private readonly SolidColorBrush[,] _trailBrush = new SolidColorBrush[2, TrailLen];
        private readonly List<Point3D>[] _trailHist = { new List<Point3D>(), new List<Point3D>() };
        private Rect3D _padRect = new Rect3D(-0.41744, -0.318, 0.18186, 0.83488, 0, 0.45824);
        private static readonly Color GlowOn = Color.FromRgb(0x1e, 0x5a, 0x68);   // press tint: emissive adds to the texture, so keep it dim (user: full accent was too bright)
        private static readonly Color GlowDim = Color.FromRgb(0x18, 0x3c, 0x48);   // for the black triggers: a tint, not a sticker
        private static readonly Color LedOn = Color.FromRgb(0x9c, 0xc8, 0xff);
        private static readonly Color MuteOn = Color.FromRgb(0xff, 0x8a, 0x1a);
        private static readonly Material DarkInside = new DiffuseMaterial(new SolidColorBrush(Color.FromRgb(0x14, 0x16, 0x1b)));
        public bool IsLoaded3D { get; private set; }

        private sealed class Part
        {
            public ModelVisual3D Visual;
            public TranslateTransform3D Press = new TranslateTransform3D();
            public AxisAngleRotation3D Hinge = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
            public AxisAngleRotation3D TiltX = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
            public AxisAngleRotation3D TiltZ = new AxisAngleRotation3D(new Vector3D(0, 0, 1), 0);
            public Point3D Pivot;
            public Point3D Min, Max;
            // Press highlight / LED light. The brush's colour is what is set:
            // EmissiveMaterial.Color only FILTERS the brush, so a black brush
            // stays black whatever the material's Color is (the first round's
            // 'no glow, no LEDs' bug).
            public SolidColorBrush Glow;
        }

        public ControllerVisualizer3D()
        {
            Background = Brushes.Transparent;
            _view.Camera = new PerspectiveCamera
            {
                Position = new Point3D(0, -3.6, 0.6),
                LookDirection = new Vector3D(0, 3.6, -0.6),
                UpDirection = new Vector3D(0, 0, 1),
                FieldOfView = 40,
            };
            var lights = new ModelVisual3D
            {
                Content = new Model3DGroup
                {
                    Children =
                    {
                        new AmbientLight(Color.FromRgb(0x50, 0x54, 0x5c)),
                        new DirectionalLight(Color.FromRgb(0xe8, 0xea, 0xf0), new Vector3D(0.35, 1.0, -0.6)),
                        new DirectionalLight(Color.FromRgb(0x50, 0x60, 0x70), new Vector3D(-0.6, 0.4, 0.3)),
                        // Key light from above-front: gives the specular term something to reflect.
                        new DirectionalLight(Color.FromRgb(0x90, 0x94, 0x9c), new Vector3D(0.2, 0.8, -1.0)),
                    }
                }
            };
            _view.Children.Add(lights);
            _rootXf.Children.Add(new RotateTransform3D(_tiltX));
            _rootXf.Children.Add(new RotateTransform3D(_tiltZ));
            _rootXf.Children.Add(new RotateTransform3D(_tiltFree));
            _rootXf.Children.Add(new RotateTransform3D(_baseYaw));
            _rootXf.Children.Add(new RotateTransform3D(_basePose));
            Children.Add(_view);
        }

        /// <summary>Which player this pad is (1-4): lights the indicator LEDs in the
        /// DualSense pattern. INFERRED from the console's behaviour, not verified
        /// against the pad's own LED state (the reader exposes none).</summary>
        public void SetPlayerIndex(int player) { _player = Math.Max(1, Math.Min(4, player)); }

        /// <summary>The lightbar colour (the shell's configured override).</summary>
        public Color LightbarColor
        {
            get => _lightbarColor;
            set { _lightbarColor = value; foreach (var e in _lightbar) e.Color = value; }
        }

        /// <summary>Load all parts from the vendored asset folder. Safe to call
        /// with a missing folder: the view simply stays empty.</summary>
        public void Load(string assetDir)
        {
            IsLoaded3D = false;
            if (string.IsNullOrEmpty(assetDir)) return;
            string manifestPath = Path.Combine(assetDir, "manifest.json");
            if (!File.Exists(manifestPath)) return;

            var textures = new Dictionary<string, ImageBrush>(StringComparer.Ordinal);
            ImageBrush Tex(string name)
            {
                if (textures.TryGetValue(name, out var b)) return b;
                string p = Path.Combine(assetDir, "textures", name);
                if (!File.Exists(p)) return null;
                var bmp = new BitmapImage();
                bmp.BeginInit(); bmp.CacheOption = BitmapCacheOption.OnLoad; bmp.UriSource = new Uri(p); bmp.EndInit(); bmp.Freeze();
                b = new ImageBrush(bmp) { ViewportUnits = BrushMappingMode.Absolute, TileMode = TileMode.None };
                b.Freeze();
                textures[name] = b;
                return b;
            }

            var root = new ModelVisual3D { Transform = _rootXf };
            using var doc = JsonDocument.Parse(File.ReadAllText(manifestPath));
            // The body goes first: WPF blends translucent geometry in draw order, so
            // the glass caps must be drawn after what they sit on or they composite
            // over the empty background instead of the pad.
            var partsJson = new List<JsonElement>(doc.RootElement.GetProperty("parts").EnumerateArray());
            partsJson.Sort((x, y) => (x.GetProperty("name").GetString() == "body" ? 0 : 1).CompareTo(y.GetProperty("name").GetString() == "body" ? 0 : 1));
            foreach (var pj in partsJson)
            {
                string name = pj.GetProperty("name").GetString();
                bool isCap = name.StartsWith("btn_", StringComparison.Ordinal) || name.StartsWith("dpad_", StringComparison.Ordinal);
                string file = Path.Combine(assetDir, pj.GetProperty("file").GetString());
                if (!File.Exists(file)) continue;
                var a = ReadVec(pj.GetProperty("bbox_min"));
                var b2 = ReadVec(pj.GetProperty("bbox_max"));
                var mn = new Point3D(Math.Min(a.X, b2.X), Math.Min(a.Y, b2.Y), Math.Min(a.Z, b2.Z));
                var mx = new Point3D(Math.Max(a.X, b2.X), Math.Max(a.Y, b2.Y), Math.Max(a.Z, b2.Z));
                var meshes = ObjLoader.Load(file);

                var group = new Model3DGroup();
                var glassModels = new List<GeometryModel3D>();
                var glowBrush = new SolidColorBrush(Colors.Black);
                var glow = new EmissiveMaterial(glowBrush);
                // WPF blends in draw order: the translucent 1001 glass caps go last so
                // the opaque glyph pieces beneath them are already in the depth buffer.
                var ordered = new List<KeyValuePair<string, MeshGeometry3D>>(meshes);
                ordered.Sort((p1, p2) => (p1.Key == "1001" ? 1 : 0).CompareTo(p2.Key == "1001" ? 1 : 0));
                foreach (var kv in ordered)
                {
                    var mat = new MaterialGroup();
                    Brush diff = Tex(kv.Key + "_baseColor.png");
                    var dm = new DiffuseMaterial(diff ?? (Brush)new SolidColorBrush(Color.FromRgb(0x2a, 0x2c, 0x32)));
                    // The 1011 tile carries the lightbar emissive; tint it by the configured colour.
                    if (kv.Key == "1011" && (name == "touchpad" || name == "body"))
                    {
                        var em = Tex("1011_emissive.png");
                        if (em != null)
                        {
                            var e = new EmissiveMaterial(em) { Color = _lightbarColor };
                            mat.Children.Add(e);
                            _lightbar.Add(e);
                        }
                    }
                    mat.Children.Add(dm);
                    // Viewport3D has no roughness/normal maps; a specular term is the one
                    // material cue it can give. Body plastic: broad, soft highlight.
                    mat.Children.Add(new SpecularMaterial(new SolidColorBrush(Color.FromArgb(0x55, 0xff, 0xff, 0xff)), 28));
                    mat.Children.Add(glow);
                    var geom = kv.Value;
                    if (name == "l2" || name == "r2")
                    {
                        // The trigger island carries its internal lever (vertices down to
                        // y=-0.2, deep inside the shell), which swung up through the body
                        // when the trigger hinged. Nothing inside the shell is visible: drop it.
                        SplitFront(kv.Value, -0.03, out var lever, out var visible);
                        geom = visible;
                    }
                    else if (name == "stick_l" || name == "stick_r")
                    {
                        // The stick island includes its shaft down inside the shell; seen
                        // through the hole from the side it poked out below the body.
                        // Cut a little inside the shell so the rounded base survives; the
                        // dark well hides the short stub.
                        SplitFront(kv.Value, -0.25, out var above, out var shaft, anyVertex: true);
                        geom = above;
                    }
                    if (isCap && kv.Key == "1001" && diff != null)
                    {
                        // The model builds each cap the way the real part is made: a
                        // clear disc on top, walls, and a floor 0.03 below it that
                        // carries the printed glyph. Split the front disc off by depth
                        // and draw it last as smoked glass, so the glyph on the floor
                        // shows through it; everything else stays opaque.
                        SplitFront(kv.Value, mn.Y + 0.008, out var glassMesh, out var restMesh);
                        geom = restMesh;
                        if (glassMesh.TriangleIndices.Count > 0)
                        {
                            var glassBrush = diff.Clone(); glassBrush.Opacity = 0.97; glassBrush.Freeze();
                            var gmat = new MaterialGroup();
                            gmat.Children.Add(new DiffuseMaterial(glassBrush) { Color = Color.FromRgb(0x9a, 0x9e, 0xaa) });
                            // Glass: tight, bright highlight.
                            gmat.Children.Add(new SpecularMaterial(new SolidColorBrush(Color.FromArgb(0xc0, 0xff, 0xff, 0xff)), 90));
                            gmat.Children.Add(glow);
                            glassModels.Add(new GeometryModel3D(glassMesh, gmat) { BackMaterial = gmat });
                        }
                    }
                    // Thin inner pieces (glyphs, LEDs) can face either way: draw both sides.
                    // The body's inside face is dark plastic, so an opening shows the
                    // shell's interior rather than its texture or the void.
                    group.Children.Add(new GeometryModel3D(geom, mat) { BackMaterial = name == "body" ? DarkInside : mat });
                }
                foreach (var g in glassModels) group.Children.Add(g);
                if (name == "stick_l" || name == "stick_r")
                {
                    // The real stick is a ball under the cap; the split mesh has only the
                    // cap and a stub, so a tilted stick showed an empty socket. A dark
                    // sphere at the base rides with the stick and always fills the hole.
                    double cx = (mn.X + mx.X) / 2, cz = (mn.Z + mx.Z) / 2;
                    // Same rubber as the stick: the 1011 tile sampled at a plain dark texel,
                    // with the same plastic highlight the other parts get. Centred below the
                    // cap's underside so it never shows through the concave dish.
                    // The stick's neck and skirt are painted from the 1001 tile at (0.648, 0.922)
                    // (measured from its faces), so the ball samples that same texel.
                    var rubber = Tex("1001_baseColor.png");
                    var ballMat = new MaterialGroup();
                    ballMat.Children.Add(new DiffuseMaterial(rubber ?? (Brush)new SolidColorBrush(Color.FromRgb(0x2a, 0x2c, 0x36))));
                    ballMat.Children.Add(new SpecularMaterial(new SolidColorBrush(Color.FromArgb(0x55, 0xff, 0xff, 0xff)), 28));
                    // Extrude the stick's bottom rim (r=0.13 at its base plane) downward as a
                    // partial sphere: the rim circle continues on a sphere centred 0.05 above
                    // it, curving inward to a pole 0.09 below. Not a full ball - just the
                    // rounded underside the split mesh lost.
                    group.Children.Add(new GeometryModel3D(RimDome(new Point3D(cx, mx.Y, cz), 0.13, 0.05, 32, 10, new Point(0.648, 1 - 0.922)), ballMat) { BackMaterial = ballMat });
                }

                var part = new Part { Glow = glowBrush };
                part.Pivot = PivotFor(name, mn, mx);
                part.Min = mn; part.Max = mx;
                var xf = new Transform3DGroup();
                xf.Children.Add(new RotateTransform3D(part.Hinge, part.Pivot));
                xf.Children.Add(new RotateTransform3D(part.TiltX, part.Pivot));
                xf.Children.Add(new RotateTransform3D(part.TiltZ, part.Pivot));
                xf.Children.Add(part.Press);
                part.Visual = new ModelVisual3D { Content = group, Transform = xf };
                root.Children.Add(part.Visual);
                _parts[name] = part;
            }
            // Player-indicator LEDs: five dots under the touchpad's bottom edge. The
            // Sketchfab mesh has no separate LED geometry there, so they are small
            // quads on the face, positioned from the touchpad/body bounds (INFERRED).
            AddLed(root, "led_l2", -0.105); AddLed(root, "led_l1", -0.052);
            AddLed(root, "led_c", 0); AddLed(root, "led_r1", 0.052); AddLed(root, "led_r2", 0.105);
            // The body has no interior under the sticks (the hole shows the white back
            // shell when a stick tilts): a dark disc at each stick's base plays the well.
            AddWell(root, -0.3176, -0.0013); AddWell(root, 0.3176, -0.0013);
            if (_parts.TryGetValue("touchpad", out var tp)) _padRect = new Rect3D(tp.Min.X, tp.Min.Y - 0.006, tp.Min.Z, tp.Max.X - tp.Min.X, 0, tp.Max.Z - tp.Min.Z);
            for (int f = 0; f < 2; f++) for (int i = 0; i < TrailLen; i++) AddTrailDot(root, f, i);
            _view.Children.Add(root);
            IsLoaded3D = _parts.Count > 0;
        }

        /// <summary>Partition a mesh's triangles into those lying entirely in
        /// front of <paramref name="yCut"/> (smaller Y = nearer the viewer) and
        /// the rest. Positions, normals and texture coordinates are carried over.</summary>
        private static void SplitFront(MeshGeometry3D src, double yCut, out MeshGeometry3D front, out MeshGeometry3D rest, bool anyVertex = false)
        {
            front = new MeshGeometry3D(); rest = new MeshGeometry3D();
            bool hasN = src.Normals != null && src.Normals.Count == src.Positions.Count;
            bool hasT = src.TextureCoordinates != null && src.TextureCoordinates.Count == src.Positions.Count;
            var mapF = new Dictionary<int, int>(); var mapR = new Dictionary<int, int>();
            int Remap(MeshGeometry3D dst, Dictionary<int, int> map, int i)
            {
                if (map.TryGetValue(i, out int j)) return j;
                j = dst.Positions.Count;
                dst.Positions.Add(src.Positions[i]);
                if (hasN) dst.Normals.Add(src.Normals[i]);
                if (hasT) dst.TextureCoordinates.Add(src.TextureCoordinates[i]);
                map[i] = j;
                return j;
            }
            for (int t = 0; t + 2 < src.TriangleIndices.Count; t += 3)
            {
                int a = src.TriangleIndices[t], b = src.TriangleIndices[t + 1], c = src.TriangleIndices[t + 2];
                bool isFront = anyVertex
                    ? (src.Positions[a].Y < yCut || src.Positions[b].Y < yCut || src.Positions[c].Y < yCut)
                    : (src.Positions[a].Y < yCut && src.Positions[b].Y < yCut && src.Positions[c].Y < yCut);
                var dst = isFront ? front : rest; var map = isFront ? mapF : mapR;
                dst.TriangleIndices.Add(Remap(dst, map, a));
                dst.TriangleIndices.Add(Remap(dst, map, b));
                dst.TriangleIndices.Add(Remap(dst, map, c));
            }
            front.Freeze(); rest.Freeze();
        }

        private void AddTrailDot(ModelVisual3D root, int finger, int i)
        {
            double r = i == 0 ? 0.02 : 0.012;
            var m = new MeshGeometry3D();
            m.Positions.Add(new Point3D(-r, 0, -r)); m.Positions.Add(new Point3D(r, 0, -r));
            m.Positions.Add(new Point3D(r, 0, r)); m.Positions.Add(new Point3D(-r, 0, r));
            m.TriangleIndices.Add(0); m.TriangleIndices.Add(2); m.TriangleIndices.Add(1);
            m.TriangleIndices.Add(0); m.TriangleIndices.Add(3); m.TriangleIndices.Add(2);
            m.Freeze();
            var brush = new SolidColorBrush(Colors.Transparent);
            var mat = new MaterialGroup();
            mat.Children.Add(new DiffuseMaterial(brush));
            mat.Children.Add(new EmissiveMaterial(brush));
            var xf = new TranslateTransform3D(0, 10, 0);   // parked far behind until a touch arrives
            root.Children.Add(new ModelVisual3D { Content = new GeometryModel3D(m, mat) { BackMaterial = mat }, Transform = xf });
            _trailPos[finger, i] = xf; _trailBrush[finger, i] = brush;
        }

        private void UpdateTouch(int finger, CoreBridge.PadTouch t)
        {
            var hist = _trailHist[finger];
            if (t.Active != 0)
            {
                double u = Math.Max(0, Math.Min(1, t.X / 1920.0)), v = Math.Max(0, Math.Min(1, t.Y / 1080.0));
                var pos = new Point3D(_padRect.X + u * _padRect.SizeX, _padRect.Y, _padRect.Z + _padRect.SizeZ - v * _padRect.SizeZ);
                if (hist.Count == 0 || (hist[0] - pos).Length > 0.004) hist.Insert(0, pos);
                if (hist.Count > TrailLen) hist.RemoveRange(TrailLen, hist.Count - TrailLen);
            }
            else hist.Clear();
            for (int i = 0; i < TrailLen; i++)
            {
                if (i < hist.Count)
                {
                    var pp = hist[i];
                    _trailPos[finger, i].OffsetX = pp.X; _trailPos[finger, i].OffsetY = pp.Y; _trailPos[finger, i].OffsetZ = pp.Z;
                    byte a = (byte)(i == 0 ? 0xff : 0xc0 - i * 0x10);
                    _trailBrush[finger, i].Color = Color.FromArgb(a, 0x5f, 0xe3, 0xff);
                }
                else { _trailPos[finger, i].OffsetY = 10; _trailBrush[finger, i].Color = Colors.Transparent; }
            }
        }

        /// <summary>A spherical cap hanging below a rim circle of radius <paramref name="rimR"/>
        /// at <paramref name="rim"/> (the rim is on the sphere; the sphere's centre sits
        /// <paramref name="centreAbove"/> above it, towards the cap).</summary>
        private static MeshGeometry3D RimDome(Point3D rim, double rimR, double centreAbove, int slices, int stacks, Point uv)
        {
            var m = new MeshGeometry3D();
            double R = Math.Sqrt(rimR * rimR + centreAbove * centreAbove);
            var c = new Point3D(rim.X, rim.Y - centreAbove, rim.Z);   // -Y is towards the cap
            double phi0 = Math.Atan2(rimR, centreAbove);               // polar angle of the rim, measured from +Y (down into the shell)
            for (int i = 0; i <= stacks; i++)
            {
                double phi = phi0 * (1.0 - (double)i / stacks);        // rim -> pole at +Y (down into the shell)
                for (int j = 0; j <= slices; j++)
                {
                    double th = 2 * Math.PI * j / slices;
                    var n = new Vector3D(Math.Sin(phi) * Math.Cos(th), Math.Cos(phi), Math.Sin(phi) * Math.Sin(th));
                    m.Positions.Add(c + n * R); m.Normals.Add(n); m.TextureCoordinates.Add(uv);
                }
            }
            for (int i = 0; i < stacks; i++)
                for (int j = 0; j < slices; j++)
                {
                    int a = i * (slices + 1) + j, b = a + slices + 1;
                    m.TriangleIndices.Add(a); m.TriangleIndices.Add(b); m.TriangleIndices.Add(a + 1);
                    m.TriangleIndices.Add(a + 1); m.TriangleIndices.Add(b); m.TriangleIndices.Add(b + 1);
                }
            m.Freeze();
            return m;
        }

        private static MeshGeometry3D Sphere(Point3D c, double r, int slices, int stacks, Point uv)
        {
            var m = new MeshGeometry3D();
            for (int i = 0; i <= stacks; i++)
            {
                double phi = Math.PI * i / stacks;
                for (int j = 0; j <= slices; j++)
                {
                    double th = 2 * Math.PI * j / slices;
                    var n = new Vector3D(Math.Sin(phi) * Math.Cos(th), Math.Cos(phi), Math.Sin(phi) * Math.Sin(th));
                    m.Positions.Add(c + n * r); m.Normals.Add(n); m.TextureCoordinates.Add(uv);
                }
            }
            for (int i = 0; i < stacks; i++)
                for (int j = 0; j < slices; j++)
                {
                    int a = i * (slices + 1) + j, b = a + slices + 1;
                    m.TriangleIndices.Add(a); m.TriangleIndices.Add(b); m.TriangleIndices.Add(a + 1);
                    m.TriangleIndices.Add(a + 1); m.TriangleIndices.Add(b); m.TriangleIndices.Add(b + 1);
                }
            m.Freeze();
            return m;
        }

        private void AddWell(ModelVisual3D root, double x, double z)
        {
            const double y = -0.272, r = 0.148; const int n = 32;
            var m = new MeshGeometry3D();
            m.Positions.Add(new Point3D(x, y, z));
            for (int i = 0; i < n; i++)
            {
                double a = i * 2 * Math.PI / n;
                m.Positions.Add(new Point3D(x + r * Math.Cos(a), y, z + r * Math.Sin(a)));
            }
            for (int i = 1; i <= n; i++) { m.TriangleIndices.Add(0); m.TriangleIndices.Add(i); m.TriangleIndices.Add(i % n + 1); }
            m.Freeze();
            var mat = new DiffuseMaterial(new SolidColorBrush(Color.FromRgb(0x14, 0x16, 0x1b)));
            root.Children.Add(new ModelVisual3D { Content = new GeometryModel3D(m, mat) { BackMaterial = mat } });
        }

        private void AddLed(ModelVisual3D root, string name, double x)
        {
            const double y = -0.306, z = 0.165, hw = 0.011, hh = 0.007;
            var m = new MeshGeometry3D();
            m.Positions.Add(new Point3D(x - hw, y, z - hh)); m.Positions.Add(new Point3D(x + hw, y, z - hh));
            m.Positions.Add(new Point3D(x + hw, y, z + hh)); m.Positions.Add(new Point3D(x - hw, y, z + hh));
            m.TriangleIndices.Add(0); m.TriangleIndices.Add(2); m.TriangleIndices.Add(1);
            m.TriangleIndices.Add(0); m.TriangleIndices.Add(3); m.TriangleIndices.Add(2);
            m.Freeze();
            var glowBrush = new SolidColorBrush(Colors.Black);
            var glow = new EmissiveMaterial(glowBrush);
            var mat = new MaterialGroup();
            mat.Children.Add(new DiffuseMaterial(new SolidColorBrush(Color.FromRgb(0x3a, 0x3c, 0x44))));
            mat.Children.Add(glow);
            var part = new Part { Glow = glowBrush, Pivot = new Point3D(x, y, z) };
            part.Visual = new ModelVisual3D { Content = new GeometryModel3D(m, mat) { BackMaterial = mat } };
            root.Children.Add(part.Visual);
            _parts[name] = part;
        }

        private static Point3D ReadVec(JsonElement e)
            => new Point3D(e[0].GetDouble(), e[1].GetDouble(), e[2].GetDouble());

        // Where each part rotates about: triggers hinge on their top edge,
        // sticks tilt about their base, everything else about its centre.
        private static Point3D PivotFor(string name, Point3D mn, Point3D mx)
        {
            double cx = (mn.X + mx.X) / 2, cy = (mn.Y + mx.Y) / 2, cz = (mn.Z + mx.Z) / 2;
            // Trigger hinge: at the trigger's TOP edge on the body side (y ~ 0, the
            // shell's mid-plane), so the free end below swings inward on press.
            if (name == "l2" || name == "r2") return new Point3D(cx, 0.0, mx.Z - 0.005);
            if (name == "l1" || name == "r1") return new Point3D(cx, cy, mx.Z);   // top edge
            if (name == "stick_l" || name == "stick_r") return new Point3D(cx, mx.Y, cz);                      // base (body side)
            return new Point3D(cx, cy, cz);
        }

        private static bool Bit(uint v, uint m) => (v & m) != 0;
        private static double Clamp(double v, double lo, double hi) => v < lo ? lo : (v > hi ? hi : v);

        /// <summary>Drive every part from the live state. Call once per poll.</summary>
        internal void Update(ref CoreBridge.PadState s, bool have)
        {
            if (!IsLoaded3D) return;
            if (!have)
            {
                _tiltX.Angle = 0; _tiltZ.Angle = 0; _tiltFree.Angle = 0; _muteLocal = _mutePrev = false; _refSamples = 0;
                foreach (var p in _parts.Values) if (p.Glow != null) p.Glow.Color = Colors.Black;
                foreach (var p in _parts.Values) { p.Press.OffsetY = 0; p.Hinge.Angle = 0; p.TiltX.Angle = 0; p.TiltZ.Angle = 0; }
                UpdateTouch(0, default); UpdateTouch(1, default);
                return;
            }

            // Whole pad: absolute tilt from the gravity vector. The core now hands
            // over the real accelerometer (the DualSenseWindows accel/gyro field
            // swap is undone in dualsense_ds5w.cpp). Sensor frame, INFERRED from
            // the Linux hid-playstation / SDL convention: +X = player's right,
            // +Y = up out of the face (so +1 g at rest face-up), +Z = towards the
            // player. Model frame: +Z is the top edge, so sensor Z is model -Z.
            // Gravity is an absolute reference: no integration, so no drift and
            // nothing to settle. Yaw is unobservable from gravity and left at 0.
            double ax = s.AccelX, ay = s.AccelY, az = s.AccelZ;
            double mag = Math.Sqrt(ax * ax + ay * ay + az * az);
            if (mag > 1000)   // raw counts; 1 g is ~8192 (INFERRED), anything smaller is not a gravity reading
            {
                // Sensor -> model axes (INFERRED; the user read both horizontal axes
                // mirrored under the first mapping, i.e. 180 deg about the face normal).
                var g = new Vector3D(-ax / mag, -ay / mag, az / mag);
                if (_refSamples < 20)
                {
                    // Average the first ~0.3 s as "level"; the pad is on the table now.
                    _gRef = _refSamples == 0 ? g : _gRef + (g - _gRef) / (_refSamples + 1);
                    _gNow = g;
                    _refSamples++;
                }
                else
                {
                    const double k = 0.25;   // light low-pass against sensor noise
                    _gNow = _gNow + (g - _gNow) * k;
                }
                var a = _gRef; var b = _gNow;
                if (a.Length > 0.5 && b.Length > 0.5)
                {
                    a.Normalize(); b.Normalize();
                    var axis = Vector3D.CrossProduct(a, b);
                    double dot = Math.Max(-1, Math.Min(1, Vector3D.DotProduct(a, b)));
                    double angle = Math.Acos(dot) * 180.0 / Math.PI;
                    if (axis.Length > 1e-4) { axis.Normalize(); _tiltFree.Axis = axis; _tiltFree.Angle = angle; }
                    else _tiltFree.Angle = dot < 0 ? 180 : 0;
                }
            }
            _tiltX.Angle = 0; _tiltZ.Angle = 0;

            // Face buttons / D-pad / system buttons depress into the face (+Y).
            const double press = 0.014;
            Press("btn_triangle", Bit(s.Buttons, 0x1000), press);
            Press("btn_circle",   Bit(s.Buttons, 0x2000), press);
            Press("btn_cross",    Bit(s.Buttons, 0x4000), press);
            Press("btn_square",   Bit(s.Buttons, 0x8000), press);
            Press("dpad_up",      Bit(s.Buttons, 0x10),   press);
            Press("dpad_right",   Bit(s.Buttons, 0x20),   press);
            Press("dpad_down",    Bit(s.Buttons, 0x40),   press);
            Press("dpad_left",    Bit(s.Buttons, 0x80),   press);
            Press("btn_ps",       Bit(s.Buttons, 0x10000), press * 0.7);
            Press("touchpad",     Bit(s.Buttons, 0x100000), press * 0.6);

            // Shoulders click, triggers hinge with the analog axis.
            Hinge("l1", Bit(s.Buttons, 0x400) ? 6 : 0);
            Hinge("r1", Bit(s.Buttons, 0x800) ? 6 : 0);
            Hinge("l2", -s.L2 / 255.0 * 22);   // negative about +X here: the free end swings into the body (user-verified pivot, direction corrected)
            Hinge("r2", -s.R2 / 255.0 * 22);
            // Share / Options: their own parts now (round 6 export).
            Press("btn_share",   Bit(s.Buttons, 0x1), press * 0.6);
            Press("btn_options", Bit(s.Buttons, 0x8), press * 0.6);
            Glow("l1", Bit(s.Buttons, 0x400)); Glow("r1", Bit(s.Buttons, 0x800));
            GlowTint("l2", s.L2 > 24); GlowTint("r2", s.R2 > 24);

            // Indicator LEDs (player pattern) and the mute LED (from MicMuted).
            SetLed("led_c",  _player == 1 || _player == 3);
            SetLed("led_l1", _player >= 2);
            SetLed("led_r1", _player >= 2);
            SetLed("led_l2", _player == 4);
            SetLed("led_r2", _player == 4);
            // Mute: the button depresses on its own bit (0x00200000), and the LED
            // follows a local toggle because the reader exposes no LED state -
            // it lights on the first press, goes dark on the next.
            bool muteDown = Bit(s.Buttons, 0x00200000);
            if (muteDown && !_mutePrev) _muteLocal = !_muteLocal;
            _mutePrev = muteDown;
            if (_parts.TryGetValue("btn_mute", out var mute))
            {
                mute.Press.OffsetY = muteDown ? press * 0.6 : 0;
                if (mute.Glow != null) mute.Glow.Color = (_muteLocal || s.MicMuted != 0) ? MuteOn : (muteDown ? GlowOn : Colors.Black);
            }

            // Sticks tilt about their base; 128 is centre.
            Stick("stick_l", s.Lx, s.Ly, Bit(s.Buttons, 0x2), press);
            Stick("stick_r", s.Rx, s.Ry, Bit(s.Buttons, 0x4), press);
            UpdateTouch(0, s.Touch0);
            UpdateTouch(1, s.Touch1);
        }

        private void Press(string name, bool on, double depth)
        {
            if (!_parts.TryGetValue(name, out var p)) return;
            p.Press.OffsetY = on ? depth : 0;
            if (p.Glow != null) p.Glow.Color = on ? GlowOn : Colors.Black;
        }

        private void GlowTint(string name, bool on)
        {
            if (_parts.TryGetValue(name, out var p) && p.Glow != null) p.Glow.Color = on ? GlowDim : Colors.Black;
        }

        private void Glow(string name, bool on)
        {
            if (_parts.TryGetValue(name, out var p) && p.Glow != null) p.Glow.Color = on ? GlowOn : Colors.Black;
        }

        private void SetLed(string name, bool on)
        {
            if (_parts.TryGetValue(name, out var p) && p.Glow != null) p.Glow.Color = on ? LedOn : Colors.Black;
        }

        private void Hinge(string name, double deg)
        {
            if (_parts.TryGetValue(name, out var p)) p.Hinge.Angle = deg;
        }

        private void Stick(string name, byte x, byte y, bool click, double depth)
        {
            if (!_parts.TryGetValue(name, out var p)) return;
            double nx = (x - 128) / 127.0, ny = (y - 128) / 127.0;
            p.TiltZ.Angle = Clamp(nx * 22, -22, 22);
            p.TiltX.Angle = Clamp(ny * 22, -22, 22);
            p.Press.OffsetY = click ? depth : 0;
            if (p.Glow != null) p.Glow.Color = click ? GlowOn : Colors.Black;   // deflection is visible on its own
        }
    }
}
