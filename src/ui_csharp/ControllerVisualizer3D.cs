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
        private readonly Dictionary<string, Part> _parts = new Dictionary<string, Part>(StringComparer.Ordinal);
        private readonly List<EmissiveMaterial> _lightbar = new List<EmissiveMaterial>();
        private Color _lightbarColor = Color.FromRgb(0, 90, 255);
        private double _rollF, _pitchF;               // low-passed gravity tilt, degrees
        private bool _muteLocal, _mutePrev;           // mute toggled by the pad's own button (the reader has no LED state)
        private int _player = 1;
        private static readonly Color GlowOn = Color.FromRgb(0x1e, 0x5a, 0x68);   // press tint: emissive adds to the texture, so keep it dim (user: full accent was too bright)
        private static readonly Color GlowDim = Color.FromRgb(0x18, 0x3c, 0x48);   // for the black triggers: a tint, not a sticker
        private static readonly Color LedOn = Color.FromRgb(0x9c, 0xc8, 0xff);
        private static readonly Color MuteOn = Color.FromRgb(0xff, 0x8a, 0x1a);
        public bool IsLoaded3D { get; private set; }

        private sealed class Part
        {
            public ModelVisual3D Visual;
            public TranslateTransform3D Press = new TranslateTransform3D();
            public AxisAngleRotation3D Hinge = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
            public AxisAngleRotation3D TiltX = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
            public AxisAngleRotation3D TiltZ = new AxisAngleRotation3D(new Vector3D(0, 0, 1), 0);
            public Point3D Pivot;
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
                    group.Children.Add(new GeometryModel3D(geom, mat) { BackMaterial = mat });
                }
                foreach (var g in glassModels) group.Children.Add(g);

                var part = new Part { Glow = glowBrush };
                part.Pivot = PivotFor(name, mn, mx);
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
            _view.Children.Add(root);
            IsLoaded3D = _parts.Count > 0;
        }

        /// <summary>Partition a mesh's triangles into those lying entirely in
        /// front of <paramref name="yCut"/> (smaller Y = nearer the viewer) and
        /// the rest. Positions, normals and texture coordinates are carried over.</summary>
        private static void SplitFront(MeshGeometry3D src, double yCut, out MeshGeometry3D front, out MeshGeometry3D rest)
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
                bool isFront = src.Positions[a].Y < yCut && src.Positions[b].Y < yCut && src.Positions[c].Y < yCut;
                var dst = isFront ? front : rest; var map = isFront ? mapF : mapR;
                dst.TriangleIndices.Add(Remap(dst, map, a));
                dst.TriangleIndices.Add(Remap(dst, map, b));
                dst.TriangleIndices.Add(Remap(dst, map, c));
            }
            front.Freeze(); rest.Freeze();
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
            if (name == "l2" || name == "r2" || name == "l1" || name == "r1") return new Point3D(cx, cy, mx.Z);   // top edge
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
                _tiltX.Angle = 0; _tiltZ.Angle = 0; _rollF = _pitchF = 0; _muteLocal = _mutePrev = false;
                foreach (var p in _parts.Values) if (p.Glow != null) p.Glow.Color = Colors.Black;
                foreach (var p in _parts.Values) { p.Press.OffsetY = 0; p.Hinge.Angle = 0; p.TiltX.Angle = 0; p.TiltZ.Angle = 0; }
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
                double roll = Math.Atan2(ax, ay) * 180.0 / Math.PI;    // right side down = +
                double pitch = Math.Atan2(-az, ay) * 180.0 / Math.PI;  // top edge down = +
                const double k = 0.25;                                 // light low-pass against sensor noise
                _rollF += (roll - _rollF) * k;
                _pitchF += (pitch - _pitchF) * k;
            }
            // Signs flipped 2026-09-06 after the user's hand test read both axes
            // mirrored - consistent with the IMU frame being rotated 180 deg about
            // Y relative to the assumed one (X and Z both negated). INFERRED.
            _tiltX.Angle = Clamp(_pitchF, -85, 85);
            _tiltZ.Angle = -Clamp(_rollF, -85, 85);

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
            Hinge("l2", -s.L2 / 255.0 * 20);   // negative: the trigger's free end swings towards the pad's back
            Hinge("r2", -s.R2 / 255.0 * 20);
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
            if (p.Glow != null) p.Glow.Color = (click || Math.Abs(nx) > 0.2 || Math.Abs(ny) > 0.2) ? GlowOn : Colors.Black;
        }
    }
}
