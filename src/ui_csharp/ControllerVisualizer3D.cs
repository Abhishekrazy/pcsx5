using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Media3D;
using HelixToolkit.SharpDX.Core;
using HelixToolkit.Wpf.SharpDX;
using SharpDX;
using HxMesh = HelixToolkit.SharpDX.Core.MeshGeometry3D;
using MediaColor = System.Windows.Media.Color;
using Point3D = System.Windows.Media.Media3D.Point3D;
using Vector3D = System.Windows.Media.Media3D.Vector3D;

namespace Pcsx5Ui
{
    /// <summary>
    /// The 3D DualSense on the Input tab's Testing view, rendered by Helix
    /// Toolkit's Direct3D 11 viewport (ADR-004) with real PBR materials: base
    /// colour, normal map and roughness/metallic maps from the vendored
    /// Sketchfab model (assets/gamepad/dualsense3d, CC-BY-4.0 credit in that
    /// folder). Animated from the live pad state: the whole pad tilts with the
    /// gravity vector, L2/R2 hinge, buttons and D-pad arms depress and glow,
    /// sticks tilt, the touchpad depresses and shows touch trails, the player
    /// LEDs, the mute LED and the lightbar light. Pure presentation - it never
    /// owns pad state (Rule 11).
    ///
    /// Model space (OBJ exported with identity axes, same frame as the
    /// manifest): +X = player's right, -Y = the face (towards the viewer),
    /// +Z = the top edge.
    /// </summary>
    public class ControllerVisualizer3D : Grid
    {
        private readonly Viewport3DX _view = new Viewport3DX();
        private readonly GroupModel3D _root = new GroupModel3D();
        private readonly Transform3DGroup _rootXf = new Transform3DGroup();
        // One rotation taking the resting gravity vector to the current one: identity
        // at rest whatever the sensor's mounting or sign convention.
        private readonly AxisAngleRotation3D _tiltFree = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
        // Resting view: the pad lying flat seen from its bottom edge with the camera
        // just above the table (the user's reference photo).
        private readonly AxisAngleRotation3D _basePose = new AxisAngleRotation3D(new Vector3D(1, 0, 0), -78);
        private SharpDX.Vector3 _gRef, _gNow; private int _refSamples;
        private SharpDX.Vector3 _stillG; private int _stillSamples;   // re-level after 2 s at rest
        private bool _muteLocal, _mutePrev;
        private readonly Dictionary<string, Part> _parts = new Dictionary<string, Part>(StringComparer.Ordinal);
        private readonly Dictionary<string, TextureModel> _textures = new Dictionary<string, TextureModel>(StringComparer.Ordinal);
        private readonly List<PhongMaterial> _lightbar = new List<PhongMaterial>();
        private MediaColor _lightbarColor = MediaColor.FromRgb(0, 90, 255);
        private int _player = 1;
        private string _assetDir;
        private EnvironmentMap3D _env;

        private static readonly Color4 GlowOn = new Color4(0.12f, 0.35f, 0.41f, 1);   // press tint (emissive adds to the texture: keep it dim)
        private static readonly Color4 GlowDim = new Color4(0.09f, 0.24f, 0.28f, 1);  // for the black triggers
        private static readonly Color4 LedOn = new Color4(0.61f, 0.78f, 1f, 1);
        private static readonly Color4 MuteOn = new Color4(1f, 0.54f, 0.1f, 1);
        private static readonly Color4 Off = new Color4(0, 0, 0, 1);

        // Touch trails: per finger, a ring of small emissive dots on the touchpad
        // surface, the newest brightest. Touch space is 1920x1080 (INFERRED, the
        // DualSense's reported resolution); the pad's bounds come from the manifest.
        private const int TrailLen = 10;
        private readonly MeshGeometryModel3D[,] _trailDot = new MeshGeometryModel3D[2, TrailLen];
        private readonly PhongMaterial[,] _trailMat = new PhongMaterial[2, TrailLen];
        private readonly List<Point3D>[] _trailHist = { new List<Point3D>(), new List<Point3D>() };
        private Rect3D _padRect = new Rect3D(-0.41744, -0.318, 0.18186, 0.83488, 0, 0.45824);

        public bool IsLoaded3D { get; private set; }

        private sealed class Part
        {
            public GroupModel3D Visual;
            public Transform3DGroup Xf;   // composed here, pushed as a fresh MatrixTransform3D (Helix drifts when a transform is mutated in place)
            public TranslateTransform3D Press = new TranslateTransform3D();
            public AxisAngleRotation3D Hinge = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
            public AxisAngleRotation3D TiltX = new AxisAngleRotation3D(new Vector3D(1, 0, 0), 0);
            public AxisAngleRotation3D TiltZ = new AxisAngleRotation3D(new Vector3D(0, 0, 1), 0);
            public Point3D Pivot, Min, Max;
            public readonly List<PhongMaterial> Mats = new List<PhongMaterial>();   // press glow goes through their emissive colour
            public PhongMaterial Led;                                            // LEDs: a plain emissive material
        }

        public ControllerVisualizer3D()
        {
            Background = Brushes.Transparent;
            _view.EffectsManager = new DefaultEffectsManager();
            _view.Camera = new HelixToolkit.Wpf.SharpDX.PerspectiveCamera
            {
                Position = new Point3D(0, -3.6, 0.6),
                LookDirection = new Vector3D(0, 3.6, -0.6),
                UpDirection = new Vector3D(0, 0, 1),
                FieldOfView = 40,
                NearPlaneDistance = 0.1,
                FarPlaneDistance = 50,
            };
            _view.ShowViewCube = false;
            _view.ShowCoordinateSystem = false;
            _view.IsHitTestVisible = false;      // the pose is the pad's, not the mouse's
            _view.MSAA = MSAALevel.Eight;
            _view.FXAALevel = FXAALevel.High;
            _view.BackgroundColor = GroundColor();
            Loaded += (s, e) => SyncBackground();
            // Three-point studio: one key from above-front, one soft fill from the left,
            // a low ambient. Fewer, dimmer lights than before: the white shell was
            // blowing out and the glyphs washed away on the caps.
            _view.Items.Add(new AmbientLight3D { Color = MediaColor.FromRgb(0x24, 0x26, 0x2c) });   // low ambient: shadows stay dark, so the render keeps contrast
            _view.Items.Add(new DirectionalLight3D { Color = MediaColor.FromRgb(0xf0, 0xf2, 0xf6), Direction = new Vector3D(0.3, 0.9, -0.7) });
            _view.Items.Add(new DirectionalLight3D { Color = MediaColor.FromRgb(0x44, 0x4a, 0x56), Direction = new Vector3D(-0.7, 0.5, 0.2) });
            _rootXf.Children.Add(new RotateTransform3D(_tiltFree));
            _rootXf.Children.Add(new RotateTransform3D(_basePose));
            _root.Transform = new MatrixTransform3D(_rootXf.Value);
            _view.Items.Add(_root);
            Children.Add(_view);
        }

        /// <summary>The D3D surface cannot be transparent, so it paints the colour of
        /// the nearest ancestor panel with a solid brush - which is a theme token,
        /// so light/dark and the user's ground colour are followed. Cheap; called
        /// on load and every poll.</summary>
        private MediaColor _bg;
        private void SyncBackground()
        {
            MediaColor c = GroundColor(this);
            DependencyObject d = this;
            while ((d = System.Windows.Media.VisualTreeHelper.GetParent(d)) != null)
            {
                if (d is System.Windows.Controls.Border b && b.Background is SolidColorBrush sb && sb.Color.A == 255) { c = sb.Color; break; }
                if (d is Panel pn && pn.Background is SolidColorBrush pb && pb.Color.A == 255) { c = pb.Color; break; }
            }
            if (c != _bg) { _bg = c; _view.BackgroundColor = c; }
        }

        private static MediaColor GroundColor(FrameworkElement scope = null)
        {
            try { if (scope?.TryFindResource("ThemeGround") is SolidColorBrush sb) return sb.Color; } catch { }
            try { if (Application.Current?.Resources["ThemeGround"] is SolidColorBrush b) return b.Color; } catch { }
            return MediaColor.FromRgb(0x0b, 0x0d, 0x12);
        }

        /// <summary>Which player this pad is (1-4): lights the indicator LEDs in the
        /// DualSense pattern. INFERRED from the console's behaviour.</summary>
        public void SetPlayerIndex(int player) { _player = Math.Max(1, Math.Min(4, player)); }

        /// <summary>The lightbar colour (the shell's configured override).</summary>
        public MediaColor LightbarColor
        {
            get => _lightbarColor;
            set { _lightbarColor = value; var c = ToColor4(value); foreach (var m in _lightbar) m.EmissiveColor = c; }
        }

        private static Color4 ToColor4(MediaColor c) => new Color4(c.R / 255f, c.G / 255f, c.B / 255f, 1f);

        private TextureModel Tex(string name)
        {
            if (_textures.TryGetValue(name, out var t)) return t;
            string p = Path.Combine(_assetDir, "textures", name);
            if (!File.Exists(p)) return null;
            try { t = TextureModel.Create(p); } catch { t = null; }
            _textures[name] = t;
            return t;
        }

        /// <summary>The model's material for one texture tile: base colour and normal
        /// map (Helix's Phong path; its PBR path did not sample the albedo map for
        /// this model, so plastic gloss comes from a fixed specular instead of the
        /// roughness map). Each part gets its own instance so its press glow
        /// (emissive) is independent.</summary>
        private PhongMaterial TileMaterial(string tile, bool lightbar)
        {
            var m = new PhongMaterial
            {
                DiffuseMap = Tex(tile + "_baseColor.png"),
                NormalMap = Tex(tile + "_normal.png"),
                RenderDiffuseMap = true,
                RenderNormalMap = true,
                EnableAutoTangent = true,
                DiffuseColor = Color4.White,
                AmbientColor = new Color4(0.5f, 0.5f, 0.53f, 1),
                SpecularColor = new Color4(0.12f, 0.12f, 0.13f, 1),   // matte plastic: a soft, narrow highlight, never a hot spot over a glyph
                SpecularShininess = 60,
                ReflectiveColor = new Color4(0.05f, 0.05f, 0.055f, 1),  // a faint mirror of the environment
                RenderEnvironmentMap = true,
                EmissiveColor = Off,
                RenderShadowMap = false,
            };
            if (lightbar)
            {
                m.EmissiveMap = Tex("1011_emissive.png");
                m.RenderEmissiveMap = true;
                m.EmissiveColor = ToColor4(_lightbarColor);
                _lightbar.Add(m);
            }
            return m;
        }

        /// <summary>Load all parts from the vendored asset folder. Safe to call
        /// with a missing folder: the view simply stays empty.</summary>
        public void Load(string assetDir)
        {
            IsLoaded3D = false;
            if (string.IsNullOrEmpty(assetDir)) return;
            _assetDir = assetDir;
            string manifestPath = Path.Combine(assetDir, "manifest.json");
            if (!File.Exists(manifestPath)) return;
            _root.Children.Clear(); _parts.Clear(); _lightbar.Clear(); _textures.Clear();
            if (_env != null) { _view.Items.Remove(_env); _env = null; }
            // Image-based lighting for the gloss: a generated studio cubemap (soft
            // gradient plus two softboxes) reflected by the materials; the skybox
            // itself is not drawn, the background stays the shell's ground.
            var envTex = Tex("studio_env.dds");
            if (envTex != null) { _env = new EnvironmentMap3D { Texture = envTex, SkipRendering = true }; _view.Items.Add(_env); }

            using var doc = JsonDocument.Parse(File.ReadAllText(manifestPath));
            foreach (var pj in doc.RootElement.GetProperty("parts").EnumerateArray())
            {
                string name = pj.GetProperty("name").GetString();
                string file = Path.Combine(assetDir, pj.GetProperty("file").GetString());
                if (!File.Exists(file)) continue;
                var a = ReadVec(pj.GetProperty("bbox_min"));
                var b2 = ReadVec(pj.GetProperty("bbox_max"));
                var mn = new Point3D(Math.Min(a.X, b2.X), Math.Min(a.Y, b2.Y), Math.Min(a.Z, b2.Z));
                var mx = new Point3D(Math.Max(a.X, b2.X), Math.Max(a.Y, b2.Y), Math.Max(a.Z, b2.Z));
                var meshes = ObjLoader.Load(file);

                var part = new Part { Pivot = PivotFor(name, mn, mx), Min = mn, Max = mx };
                var group = new GroupModel3D();
                PhongMaterial mat1001 = null;
                foreach (var kv in meshes)
                {
                    var mat = TileMaterial(kv.Key, kv.Key == "1011" && (name == "touchpad" || name == "body"));
                    if (kv.Key == "1001") mat1001 = mat;
                    part.Mats.Add(mat);
                    var geom = kv.Value;
                    if (name == "l2" || name == "r2")
                    {
                        // The trigger island carries its internal lever (vertices down to
                        // y=-0.2, deep inside the shell), which swung up through the body
                        // when the trigger hinged. Nothing inside the shell is visible: drop it.
                        SplitFront(kv.Value, -0.03, out _, out var visible);
                        geom = visible;
                    }
                    else if (name == "stick_l" || name == "stick_r")
                    {
                        // The stick island includes its shaft down inside the shell; cut a
                        // little inside so the rounded base survives.
                        SplitFront(kv.Value, -0.25, out var above, out _, anyVertex: true);
                        geom = above;
                    }
                    var hx = ToHelix(geom);
                    HelixToolkit.Wpf.SharpDX.Material useMat = mat;
                    group.Children.Add(new MeshGeometryModel3D { Geometry = hx, Material = mat, CullMode = SharpDX.Direct3D11.CullMode.None });
                }
                if ((name == "stick_l" || name == "stick_r") && mat1001 != null)
                {
                    // Extrude the stick's bottom rim (r=0.13 at its base plane) downward as a
                    // partial sphere: the rim circle continues on a sphere centred 0.05 above
                    // it, curving inward to a pole 0.09 below. Same material as the stick,
                    // sampled at the texel its rim faces use (measured: 0.621, 0.924).
                    double cx = (mn.X + mx.X) / 2, cz = (mn.Z + mx.Z) / 2;
                    group.Children.Add(new MeshGeometryModel3D { Geometry = RimDome(new Point3D(cx, mx.Y, cz), 0.13, 0.05, 32, 10, new Vector2(0.621f, 1 - 0.924f)), Material = mat1001, CullMode = SharpDX.Direct3D11.CullMode.None });
                }

                var xf = new Transform3DGroup();
                xf.Children.Add(new RotateTransform3D(part.Hinge, part.Pivot));
                xf.Children.Add(new RotateTransform3D(part.TiltX, part.Pivot));
                xf.Children.Add(new RotateTransform3D(part.TiltZ, part.Pivot));
                xf.Children.Add(part.Press);
                part.Xf = xf;
                group.Transform = new MatrixTransform3D(xf.Value);
                part.Visual = group;
                _root.Children.Add(group);
                _parts[name] = part;
            }

            // Player-indicator LEDs: five dots under the touchpad's bottom edge. The
            // Sketchfab mesh has no separate LED geometry there, so they are small
            // quads on the face, positioned from the touchpad/body bounds (INFERRED).
            AddLed("led_l2", -0.105); AddLed("led_l1", -0.052);
            AddLed("led_c", 0); AddLed("led_r1", 0.052); AddLed("led_r2", 0.105);
            if (_parts.TryGetValue("touchpad", out var tp)) _padRect = new Rect3D(tp.Min.X, tp.Min.Y - 0.006, tp.Min.Z, tp.Max.X - tp.Min.X, 0, tp.Max.Z - tp.Min.Z);
            for (int f = 0; f < 2; f++) for (int i = 0; i < TrailLen; i++) AddTrailDot(f, i);
            IsLoaded3D = _parts.Count > 0;
        }

        private static HxMesh ToHelix(System.Windows.Media.Media3D.MeshGeometry3D m)
        {
            var h = new HxMesh
            {
                Positions = new Vector3Collection(m.Positions.Count),
                Normals = new Vector3Collection(m.Positions.Count),
                TextureCoordinates = new Vector2Collection(m.Positions.Count),
                TriangleIndices = new IntCollection(m.TriangleIndices.Count),
            };
            bool hasN = m.Normals != null && m.Normals.Count == m.Positions.Count;
            bool hasT = m.TextureCoordinates != null && m.TextureCoordinates.Count == m.Positions.Count;
            for (int i = 0; i < m.Positions.Count; i++)
            {
                var p = m.Positions[i];
                h.Positions.Add(new Vector3((float)p.X, (float)p.Y, (float)p.Z));
                if (hasN) { var n = m.Normals[i]; h.Normals.Add(new Vector3((float)n.X, (float)n.Y, (float)n.Z)); }
                if (hasT) { var t = m.TextureCoordinates[i]; h.TextureCoordinates.Add(new Vector2((float)t.X, (float)t.Y)); }
            }
            if (!hasT) h.TextureCoordinates = null;
            foreach (var i in m.TriangleIndices) h.TriangleIndices.Add(i);
            // The decimated export carries flat per-face normals, which shade as facets.
            // Weld vertices by position and average the face normals across them, so
            // the shell shades smoothly (the normal map supplies the fine detail).
            {
                var weld = new Dictionary<(int, int, int), int>();
                var group = new int[h.Positions.Count];
                var acc = new List<Vector3>();
                for (int i = 0; i < h.Positions.Count; i++)
                {
                    var p = h.Positions[i];
                    var key = ((int)Math.Round(p.X * 4000), (int)Math.Round(p.Y * 4000), (int)Math.Round(p.Z * 4000));
                    if (!weld.TryGetValue(key, out int g)) { g = acc.Count; weld[key] = g; acc.Add(Vector3.Zero); }
                    group[i] = g;
                }
                for (int t = 0; t + 2 < h.TriangleIndices.Count; t += 3)
                {
                    int i0 = h.TriangleIndices[t], i1 = h.TriangleIndices[t + 1], i2 = h.TriangleIndices[t + 2];
                    var n = Vector3.Cross(h.Positions[i1] - h.Positions[i0], h.Positions[i2] - h.Positions[i0]);   // area-weighted
                    acc[group[i0]] += n; acc[group[i1]] += n; acc[group[i2]] += n;
                }
                h.Normals = new Vector3Collection(h.Positions.Count);
                for (int i = 0; i < h.Positions.Count; i++) { var v = acc[group[i]]; if (v.LengthSquared() > 0) v.Normalize(); h.Normals.Add(v); }
            }
            return h;
        }

        /// <summary>Partition a mesh's triangles by depth: "front" = nearer the viewer
        /// than <paramref name="yCut"/> (all vertices, or any vertex when
        /// <paramref name="anyVertex"/>), "rest" = the others.</summary>
        private static void SplitFront(System.Windows.Media.Media3D.MeshGeometry3D src, double yCut,
            out System.Windows.Media.Media3D.MeshGeometry3D front, out System.Windows.Media.Media3D.MeshGeometry3D rest, bool anyVertex = false)
        {
            front = new System.Windows.Media.Media3D.MeshGeometry3D(); rest = new System.Windows.Media.Media3D.MeshGeometry3D();
            bool hasN = src.Normals != null && src.Normals.Count == src.Positions.Count;
            bool hasT = src.TextureCoordinates != null && src.TextureCoordinates.Count == src.Positions.Count;
            var mapF = new Dictionary<int, int>(); var mapR = new Dictionary<int, int>();
            int Remap(System.Windows.Media.Media3D.MeshGeometry3D dst, Dictionary<int, int> map, int i)
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
        }

        /// <summary>A spherical cap hanging below a rim circle of radius <paramref name="rimR"/>
        /// at <paramref name="rim"/>; the sphere's centre sits <paramref name="centreAbove"/>
        /// above it (towards the cap, -Y) and the pole points into the shell (+Y).</summary>
        private static HxMesh RimDome(Point3D rim, double rimR, double centreAbove, int slices, int stacks, Vector2 uv)
        {
            var m = new HxMesh { Positions = new Vector3Collection(), Normals = new Vector3Collection(), TextureCoordinates = new Vector2Collection(), TriangleIndices = new IntCollection() };
            double R = Math.Sqrt(rimR * rimR + centreAbove * centreAbove);
            var c = new Vector3((float)rim.X, (float)(rim.Y - centreAbove), (float)rim.Z);
            double phi0 = Math.Atan2(rimR, centreAbove);
            for (int i = 0; i <= stacks; i++)
            {
                double phi = phi0 * (1.0 - (double)i / stacks);
                for (int j = 0; j <= slices; j++)
                {
                    double th = 2 * Math.PI * j / slices;
                    var n = new Vector3((float)(Math.Sin(phi) * Math.Cos(th)), (float)Math.Cos(phi), (float)(Math.Sin(phi) * Math.Sin(th)));
                    m.Positions.Add(c + n * (float)R); m.Normals.Add(n); m.TextureCoordinates.Add(uv);
                }
            }
            for (int i = 0; i < stacks; i++)
                for (int j = 0; j < slices; j++)
                {
                    int a = i * (slices + 1) + j, b = a + slices + 1;
                    m.TriangleIndices.Add(a); m.TriangleIndices.Add(b); m.TriangleIndices.Add(a + 1);
                    m.TriangleIndices.Add(a + 1); m.TriangleIndices.Add(b); m.TriangleIndices.Add(b + 1);
                }
            return m;
        }

        private static HxMesh Quad(double hw, double hh)
        {
            var m = new HxMesh { Positions = new Vector3Collection(), Normals = new Vector3Collection(), TextureCoordinates = new Vector2Collection(), TriangleIndices = new IntCollection() };
            var n = new Vector3(0, -1, 0);
            m.Positions.Add(new Vector3((float)-hw, 0, (float)-hh)); m.Positions.Add(new Vector3((float)hw, 0, (float)-hh));
            m.Positions.Add(new Vector3((float)hw, 0, (float)hh)); m.Positions.Add(new Vector3((float)-hw, 0, (float)hh));
            for (int i = 0; i < 4; i++) { m.Normals.Add(n); m.TextureCoordinates.Add(Vector2.Zero); }
            m.TriangleIndices.AddRange(new[] { 0, 2, 1, 0, 3, 2 });
            return m;
        }

        private void AddLed(string name, double x)
        {
            const double y = -0.306, z = 0.165;
            var mat = new PhongMaterial { DiffuseColor = new Color4(0.23f, 0.24f, 0.27f, 1), EmissiveColor = Off, SpecularColor = Color4.Black };
            var part = new Part { Led = mat, Pivot = new Point3D(x, y, z) };
            var group = new GroupModel3D { Transform = new TranslateTransform3D(x, y, z) };
            group.Children.Add(new MeshGeometryModel3D { Geometry = Quad(0.011, 0.007), Material = mat, CullMode = SharpDX.Direct3D11.CullMode.None });
            part.Visual = group;
            _root.Children.Add(group);
            _parts[name] = part;
        }

        private void AddTrailDot(int finger, int i)
        {
            double r = i == 0 ? 0.02 : 0.012;
            var mat = new PhongMaterial { DiffuseColor = new Color4(0, 0, 0, 0), EmissiveColor = new Color4(0, 0, 0, 0), SpecularColor = Color4.Black };
            var dot = new MeshGeometryModel3D { Geometry = Quad(r, r), Material = mat, Transform = new TranslateTransform3D(0, 10, 0), IsTransparent = true, CullMode = SharpDX.Direct3D11.CullMode.None };   // parked far behind until a touch arrives
            _root.Children.Add(dot);
            _trailDot[finger, i] = dot; _trailMat[finger, i] = mat;
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
                    _trailDot[finger, i].Transform = new TranslateTransform3D(pp.X, pp.Y, pp.Z);
                    float a = i == 0 ? 1f : (0.75f - i * 0.06f);
                    var c = new Color4(0.37f, 0.89f, 1f, a);
                    _trailMat[finger, i].DiffuseColor = c; _trailMat[finger, i].EmissiveColor = c;
                }
                else _trailDot[finger, i].Transform = new TranslateTransform3D(0, 10, 0);
            }
        }

        private static Point3D ReadVec(JsonElement e)
            => new Point3D(e[0].GetDouble(), e[1].GetDouble(), e[2].GetDouble());

        // Where each part rotates about: triggers hinge at their top edge on the
        // body side, sticks tilt about their base, everything else about its centre.
        private static Point3D PivotFor(string name, Point3D mn, Point3D mx)
        {
            double cx = (mn.X + mx.X) / 2, cy = (mn.Y + mx.Y) / 2, cz = (mn.Z + mx.Z) / 2;
            if (name == "l2" || name == "r2") return new Point3D(cx, 0.0, mx.Z - 0.005);
            if (name == "l1" || name == "r1") return new Point3D(cx, cy, mx.Z);
            if (name == "stick_l" || name == "stick_r") return new Point3D(cx, mx.Y, cz);
            return new Point3D(cx, cy, cz);
        }

        private static bool Bit(uint v, uint m) => (v & m) != 0;

        /// <summary>Drive every part from the live state. Call once per poll.</summary>
        internal void Update(ref CoreBridge.PadState s, bool have)
        {
            if (!IsLoaded3D) return;
            SyncBackground();
            if (!have)
            {
                _tiltFree.Angle = 0; _muteLocal = _mutePrev = false; _refSamples = 0; _stillSamples = 0;
                foreach (var p in _parts.Values) { SetGlow(p, Off); p.Press.OffsetY = 0; p.Hinge.Angle = 0; p.TiltX.Angle = 0; p.TiltZ.Angle = 0; }
                UpdateTouch(0, default); UpdateTouch(1, default);
                _root.Transform = new MatrixTransform3D(_rootXf.Value);
                foreach (var p in _parts.Values) Apply(p);
                return;
            }

            // Whole pad: the rotation from the resting gravity vector (captured when the
            // first samples arrive; the pad is on the table then) to the current one.
            // Sensor -> model axes (INFERRED; the user read both horizontal axes
            // mirrored under the first mapping, i.e. 180 deg about the face normal).
            double ax = s.AccelX, ay = s.AccelY, az = s.AccelZ;
            double mag = Math.Sqrt(ax * ax + ay * ay + az * az);
            if (mag > 1000)   // raw counts; 1 g is ~8192 (INFERRED), anything smaller is not a gravity reading
            {
                var g = new SharpDX.Vector3((float)(-ax / mag), (float)(-ay / mag), (float)(az / mag));
                if (_refSamples < 20)
                {
                    _gRef = _refSamples == 0 ? g : _gRef + (g - _gRef) / (_refSamples + 1);
                    _gNow = g;
                    _refSamples++;
                }
                else _gNow = _gNow + (g - _gNow) * 0.25f;   // light low-pass against sensor noise
                // The popup is usually opened with the pad in hand, so the first reference
                // is the in-hand pose. Whenever the pad has been still for ~2 s (gravity
                // within 1.5 deg of its running value for 120 polls) that resting vector
                // becomes the new level, so a pad set down reads flat again.
                if ((g - _stillG).Length() < 0.026f) { if (++_stillSamples == 120) _gRef = _stillG; }
                else { _stillG = g; _stillSamples = 0; }
                var a = _gRef; var b = _gNow;
                if (a.Length() > 0.5f && b.Length() > 0.5f)
                {
                    a.Normalize(); b.Normalize();
                    var axis = SharpDX.Vector3.Cross(a, b);
                    double dot = Math.Max(-1, Math.Min(1, SharpDX.Vector3.Dot(a, b)));
                    double angle = Math.Acos(dot) * 180.0 / Math.PI;
                    if (axis.Length() > 1e-4f) { axis.Normalize(); _tiltFree.Axis = new Vector3D(axis.X, axis.Y, axis.Z); _tiltFree.Angle = angle; }
                    else _tiltFree.Angle = dot < 0 ? 180 : 0;
                }
            }

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
            Press("btn_share",    Bit(s.Buttons, 0x1), press * 0.6);
            Press("btn_options",  Bit(s.Buttons, 0x8), press * 0.6);

            // Bumpers click; triggers hinge with the analog axis about the top-edge pivot.
            Hinge("l1", Bit(s.Buttons, 0x400) ? 6 : 0);
            Hinge("r1", Bit(s.Buttons, 0x800) ? 6 : 0);
            Hinge("l2", -s.L2 / 255.0 * 22);   // negative about +X here: the free end swings into the body (user-verified pivot)
            Hinge("r2", -s.R2 / 255.0 * 22);
            GlowTint("l1", Bit(s.Buttons, 0x400)); GlowTint("r1", Bit(s.Buttons, 0x800));
            GlowTint("l2", s.L2 > 30); GlowTint("r2", s.R2 > 30);

            // Indicator LEDs (player pattern) and the mute LED.
            SetLed("led_c",  _player == 1 || _player == 3);
            SetLed("led_l1", _player >= 2);
            SetLed("led_r1", _player >= 2);
            SetLed("led_l2", _player == 4);
            SetLed("led_r2", _player == 4);
            // Mute: depresses on its own bit (0x00200000); the LED follows a local
            // toggle (lit on first press, dark on the next) OR MicMuted, because the
            // reader exposes no LED state.
            bool muteDown = Bit(s.Buttons, 0x00200000);
            if (muteDown && !_mutePrev) _muteLocal = !_muteLocal;
            _mutePrev = muteDown;
            if (_parts.TryGetValue("btn_mute", out var mute))
            {
                mute.Press.OffsetY = muteDown ? press * 0.6 : 0;
                SetGlow(mute, (_muteLocal || s.MicMuted != 0) ? MuteOn : (muteDown ? GlowOn : Off));
            }

            // Sticks tilt about their base; 128 is centre.
            Stick("stick_l", s.Lx, s.Ly, Bit(s.Buttons, 0x2), press);
            Stick("stick_r", s.Rx, s.Ry, Bit(s.Buttons, 0x4), press);
            UpdateTouch(0, s.Touch0);
            UpdateTouch(1, s.Touch1);
            _root.Transform = new MatrixTransform3D(_rootXf.Value);
            foreach (var p in _parts.Values) Apply(p);
        }

        private static void Apply(Part p)
        {
            if (p.Visual != null && p.Xf != null) p.Visual.Transform = new MatrixTransform3D(p.Xf.Value);
        }

        private static void SetGlow(Part p, Color4 c)
        {
            foreach (var m in p.Mats) m.EmissiveColor = c;
            if (p.Led != null) p.Led.EmissiveColor = c;
        }

        private void Press(string name, bool on, double depth)
        {
            if (!_parts.TryGetValue(name, out var p)) return;
            p.Press.OffsetY = on ? depth : 0;
            SetGlow(p, on ? GlowOn : Off);
        }

        private void GlowTint(string name, bool on)
        {
            if (_parts.TryGetValue(name, out var p)) SetGlow(p, on ? GlowDim : Off);
        }

        private void SetLed(string name, bool on)
        {
            if (_parts.TryGetValue(name, out var p)) SetGlow(p, on ? LedOn : Off);
        }

        private void Hinge(string name, double deg)
        {
            if (_parts.TryGetValue(name, out var p)) p.Hinge.Angle = deg;
        }

        private void Stick(string name, byte x, byte y, bool click, double depth)
        {
            if (!_parts.TryGetValue(name, out var p)) return;
            double nx = (x - 128) / 127.0, ny = (y - 128) / 127.0;
            p.TiltZ.Angle = Math.Max(-22, Math.Min(22, nx * 22));
            p.TiltX.Angle = Math.Max(-22, Math.Min(22, ny * 22));
            p.Press.OffsetY = click ? depth : 0;
            SetGlow(p, click ? GlowOn : Off);   // deflection is visible on its own
        }
    }
}
