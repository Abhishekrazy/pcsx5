using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace Pcsx5Ui
{
    /// <summary>
    /// Direction-aware focus movement for pad navigation: from the focused
    /// element, "down" goes to the element visually below it, the way a TV
    /// remote moves across a grid - not to the next entry in a hand-written
    /// list. Pure geometry over the elements' on-screen bounds; it owns no
    /// state and never touches emulator state (Rule 11).
    /// </summary>
    internal static class SpatialNav
    {
        public enum Dir { Up, Down, Left, Right }

        /// <summary>Pick the best element in <paramref name="dir"/> from
        /// <paramref name="from"/>. Returns null when nothing lies that way.
        /// With no origin, returns the top-left-most candidate.</summary>
        public static FrameworkElement Find(IEnumerable<FrameworkElement> candidates, FrameworkElement from, Dir dir)
        {
            var root = Application.Current?.MainWindow as Visual;
            if (root == null) return null;

            Rect origin = Rect.Empty;
            if (from != null) origin = Bounds(from, root);

            FrameworkElement best = null;
            double bestScore = double.MaxValue;
            foreach (var c in candidates)
            {
                if (c == null || ReferenceEquals(c, from) || !c.IsVisible || !c.IsEnabled) continue;
                var r = Bounds(c, root);
                if (r.IsEmpty || r.Width <= 0 || r.Height <= 0) continue;

                double score;
                if (origin.IsEmpty)
                {
                    score = r.Top * 4 + r.Left;
                }
                else
                {
                    double ocx = origin.Left + origin.Width / 2, ocy = origin.Top + origin.Height / 2;
                    double ccx = r.Left + r.Width / 2, ccy = r.Top + r.Height / 2;
                    double forward, sideways;
                    switch (dir)
                    {
                        case Dir.Down:  forward = r.Top - origin.Bottom;  sideways = Math.Abs(ccx - ocx); break;
                        case Dir.Up:    forward = origin.Top - r.Bottom;  sideways = Math.Abs(ccx - ocx); break;
                        case Dir.Right: forward = r.Left - origin.Right;  sideways = Math.Abs(ccy - ocy); break;
                        default:        forward = origin.Left - r.Right;  sideways = Math.Abs(ccy - ocy); break;
                    }
                    // Must be ahead (a little overlap tolerated for rows that touch).
                    if (forward < -Math.Min(origin.Height, origin.Width) * 0.5) continue;
                    // Also require the centre to be ahead of the origin's centre.
                    double centreAhead = dir switch
                    {
                        Dir.Down => ccy - ocy,
                        Dir.Up => ocy - ccy,
                        Dir.Right => ccx - ocx,
                        _ => ocx - ccx,
                    };
                    if (centreAhead <= 1) continue;

                    // Prefer things that overlap the origin's lane; off-lane
                    // candidates pay for their sideways offset.
                    bool overlaps = dir == Dir.Down || dir == Dir.Up
                        ? (r.Left < origin.Right && r.Right > origin.Left)
                        : (r.Top < origin.Bottom && r.Bottom > origin.Top);
                    score = Math.Max(0, forward) + (overlaps ? sideways * 0.5 : sideways * 3 + 200);
                }
                if (score < bestScore) { bestScore = score; best = c; }
            }
            return best;
        }

        private static Rect Bounds(FrameworkElement e, Visual root)
        {
            try
            {
                if (!e.IsDescendantOf(root)) return Rect.Empty;
                return e.TransformToAncestor(root).TransformBounds(new Rect(0, 0, e.ActualWidth, e.ActualHeight));
            }
            catch { return Rect.Empty; }
        }
    }
}
