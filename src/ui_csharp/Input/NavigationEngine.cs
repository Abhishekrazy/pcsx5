using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Pcsx5Ui.Input
{
    public class NavigationEngine
    {
        private Window _window;
        private Control _currentFocus;
        private List<Control> _navigableElements = new List<Control>();

        public event Action<Control> FocusChanged;

        public NavigationEngine(Window window)
        {
            _window = window;
        }

        public void RegisterElement(Control control)
        {
            if (!_navigableElements.Contains(control))
                _navigableElements.Add(control);
        }

        public void RegisterContainer(Panel panel)
        {
            foreach (var child in panel.Children)
            {
                if (child is Control c && c.Focusable)
                    RegisterElement(c);
                else if (child is Panel p)
                    RegisterContainer(p);
            }
        }

        public void SetFocus(Control control)
        {
            if (control != null && control.Visibility == Visibility.Visible && control.IsEnabled)
            {
                _currentFocus = control;
                control.Focus();
                FocusChanged?.Invoke(control);
            }
        }

        public Control GetCurrentFocus() => _currentFocus;

        public void Navigate(NavigationDirection direction)
        {
            if (_currentFocus == null)
            {
                if (_navigableElements.Count > 0)
                    SetFocus(_navigableElements[0]);
                return;
            }

            Control closest = null;
            double minDistance = double.MaxValue;
            Point currentPos = _currentFocus.TransformToAncestor(_window).Transform(new Point(_currentFocus.ActualWidth / 2, _currentFocus.ActualHeight / 2));

            foreach (var target in _navigableElements)
            {
                if (target == _currentFocus || target.Visibility != Visibility.Visible || !target.IsEnabled)
                    continue;

                // Ensure target is actually in the visual tree and descendant of the window
                if (!target.IsLoaded || !target.IsDescendantOf(_window))
                    continue;

                Point targetPos;
                try
                {
                    targetPos = target.TransformToAncestor(_window).Transform(new Point(target.ActualWidth / 2, target.ActualHeight / 2));
                }
                catch (InvalidOperationException)
                {
                    continue; // target is not in visual tree
                }

                double dx = targetPos.X - currentPos.X;
                double dy = targetPos.Y - currentPos.Y;
                
                bool validDirection = false;
                switch (direction)
                {
                    case NavigationDirection.Up: validDirection = dy < -10 && Math.Abs(dx) < Math.Abs(dy) * 2; break;
                    case NavigationDirection.Down: validDirection = dy > 10 && Math.Abs(dx) < Math.Abs(dy) * 2; break;
                    case NavigationDirection.Left: validDirection = dx < -10 && Math.Abs(dy) < Math.Abs(dx) * 2; break;
                    case NavigationDirection.Right: validDirection = dx > 10 && Math.Abs(dy) < Math.Abs(dx) * 2; break;
                }

                if (validDirection)
                {
                    double distance = Math.Sqrt(dx * dx + dy * dy);
                    if (distance < minDistance)
                    {
                        minDistance = distance;
                        closest = target;
                    }
                }
            }

            if (closest != null)
                SetFocus(closest);
        }
    }

    public enum NavigationDirection
    {
        Up, Down, Left, Right
    }
}
