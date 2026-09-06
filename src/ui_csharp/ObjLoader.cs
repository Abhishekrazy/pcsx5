using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Media3D;

namespace Pcsx5Ui
{
    /// <summary>
    /// Minimal Wavefront OBJ reader for the vendored DualSense parts
    /// (assets/gamepad/dualsense3d): triangulated faces `f a/b/c`, one mesh per
    /// `usemtl` group so each material can carry its own texture. Nothing else
    /// of the OBJ spec is needed, so nothing else is implemented (no dependency
    /// on a glTF/OBJ library, per the technology policy).
    /// </summary>
    internal static class ObjLoader
    {
        /// <summary>Meshes keyed by material name. OBJ V is bottom-up; WPF
        /// texture space is top-down, so V is flipped here.</summary>
        public static Dictionary<string, MeshGeometry3D> Load(string path)
        {
            var pos = new List<Point3D>();
            var nrm = new List<Vector3D>();
            var uv  = new List<System.Windows.Point>();
            var meshes = new Dictionary<string, MeshGeometry3D>(StringComparer.Ordinal);
            MeshGeometry3D cur = null;
            var inv = CultureInfo.InvariantCulture;

            foreach (var raw in File.ReadLines(path))
            {
                if (raw.Length < 2) continue;
                char c0 = raw[0];
                if (c0 == 'v')
                {
                    var t = raw.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    if (raw[1] == ' ' && t.Length >= 4)
                        pos.Add(new Point3D(double.Parse(t[1], inv), double.Parse(t[2], inv), double.Parse(t[3], inv)));
                    else if (raw[1] == 'n' && t.Length >= 4)
                        nrm.Add(new Vector3D(double.Parse(t[1], inv), double.Parse(t[2], inv), double.Parse(t[3], inv)));
                    else if (raw[1] == 't' && t.Length >= 3)
                        uv.Add(new System.Windows.Point(double.Parse(t[1], inv), 1.0 - double.Parse(t[2], inv)));
                }
                else if (c0 == 'u' && raw.StartsWith("usemtl ", StringComparison.Ordinal))
                {
                    string m = raw.Substring(7).Trim();
                    if (!meshes.TryGetValue(m, out cur))
                    {
                        cur = new MeshGeometry3D();
                        meshes[m] = cur;
                    }
                }
                else if (c0 == 'f' && raw[1] == ' ')
                {
                    if (cur == null) { cur = new MeshGeometry3D(); meshes["default"] = cur; }
                    var t = raw.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    if (t.Length < 4) continue;
                    // triangulated export: exactly three corners
                    for (int i = 1; i <= 3; i++)
                    {
                        var idx = t[i].Split('/');
                        int vi = int.Parse(idx[0], inv) - 1;
                        int ti = idx.Length > 1 && idx[1].Length > 0 ? int.Parse(idx[1], inv) - 1 : -1;
                        int ni = idx.Length > 2 && idx[2].Length > 0 ? int.Parse(idx[2], inv) - 1 : -1;
                        cur.TriangleIndices.Add(cur.Positions.Count);
                        cur.Positions.Add(pos[vi]);
                        cur.TextureCoordinates.Add(ti >= 0 ? uv[ti] : new System.Windows.Point(0, 0));
                        cur.Normals.Add(ni >= 0 ? nrm[ni] : new Vector3D(0, -1, 0));
                    }
                }
            }
            foreach (var m in meshes.Values) m.Freeze();
            return meshes;
        }
    }
}
