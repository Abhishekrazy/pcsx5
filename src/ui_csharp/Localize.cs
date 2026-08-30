using System;
using System.ComponentModel;
using System.Windows.Data;
using System.Windows.Markup;

namespace Pcsx5Ui
{
    /// <summary>
    /// Binding source that exposes <see cref="I18n"/> to XAML by key.
    ///
    /// A plain markup extension returning a string would be resolved once, while
    /// XAML is being parsed -- which happens before the configured language has
    /// been loaded, so every label would render as its raw key. Exposing the
    /// lookup as a bindable indexer instead means the text re-resolves whenever
    /// <see cref="Refresh"/> is raised, so strings are correct after load and
    /// also update when the user changes language without restarting.
    /// </summary>
    public sealed class I18nSource : INotifyPropertyChanged
    {
        public static I18nSource Instance { get; } = new I18nSource();

        private I18nSource() { }

        public string this[string key] => I18n.Tr(key);

        public event PropertyChangedEventHandler PropertyChanged;

        /// <summary>Re-evaluate every bound string. Called after I18n.Load.</summary>
        public void Refresh()
        {
            // "Item[]" is the WPF convention for "every indexer value changed".
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs("Item[]"));
        }
    }

    /// <summary>
    /// XAML markup extension for localized text: <c>Text="{loc:Tr Key=view.library}"</c>.
    /// Returns a one-way binding rather than a string so the value survives a
    /// language load or change.
    /// </summary>
    public sealed class TrExtension : MarkupExtension
    {
        public string Key { get; set; }

        public TrExtension() { }

        public TrExtension(string key)
        {
            Key = key;
        }

        public override object ProvideValue(IServiceProvider serviceProvider)
        {
            if (string.IsNullOrEmpty(Key)) return string.Empty;

            var binding = new Binding("[" + Key + "]")
            {
                Source = I18nSource.Instance,
                Mode = BindingMode.OneWay,
            };
            return binding.ProvideValue(serviceProvider);
        }
    }
}
