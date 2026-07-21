using System;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace Pcsx5Ui
{
    // Speaker output + mic input test for the DualSense's standard USB Audio
    // Class endpoints ("Wireless Controller" render/capture endpoints).
    // USB ONLY: over Bluetooth the controller exposes no audio endpoints, in
    // which case the Find methods return null and the callers surface a
    // "connect via USB" message.
    //
    // The render endpoint is 4-channel: channels 1-2 = speaker/headphone,
    // channels 3-4 = haptic actuators. The test tone writes stereo to ch 1-2
    // and keeps ch 3-4 silent.
    internal static class DualSenseAudio
    {
        private const string EndpointNameHint = "Wireless Controller";

        private static readonly object Gate = new object();
        private static WasapiCapture _capture;
        private static float _currentLevel; // RMS, 0..1

        // Latest mic RMS level (0..1), updated by the capture thread.
        internal static float CurrentLevel
        {
            get { lock (Gate) { return _currentLevel; } }
        }

        internal static bool IsMicMeterRunning
        {
            get { lock (Gate) { return _capture != null; } }
        }

        private static MMDevice FindEndpoint(DataFlow flow)
        {
            try
            {
                using (var enumerator = new MMDeviceEnumerator())
                {
                    foreach (var device in enumerator.EnumerateAudioEndPoints(flow, DeviceState.Active))
                    {
                        var friendly = device.FriendlyName ?? string.Empty;
                        var deviceName = string.Empty;
                        try { deviceName = device.DeviceFriendlyName ?? string.Empty; }
                        catch (Exception) { }

                        if (friendly.IndexOf(EndpointNameHint, StringComparison.OrdinalIgnoreCase) >= 0 ||
                            deviceName.IndexOf(EndpointNameHint, StringComparison.OrdinalIgnoreCase) >= 0)
                        {
                            return device;
                        }

                        device.Dispose();
                    }
                }
            }
            catch (Exception)
            {
            }

            return null;
        }

        // Plays a short 440 Hz sine through the DualSense render endpoint.
        // Returns null on success, or a user-facing error message (including
        // the USB-only note when the endpoint is missing).
        internal static string PlayTestTone(double seconds = 1.5, double frequency = 440.0)
        {
            MMDevice device = null;
            try
            {
                device = FindEndpoint(DataFlow.Render);
                if (device == null)
                {
                    return "DualSense audio endpoint not found. Speaker output works over USB only - connect the controller via USB.";
                }

                var mixFormat = device.AudioClient.MixFormat;
                var provider = new SineToneProvider(mixFormat.SampleRate, mixFormat.Channels, frequency, seconds);
                var output = new WasapiOut(device, AudioClientShareMode.Shared, false, 100);
                output.PlaybackStopped += (s, e) =>
                {
                    output.Dispose();
                    device.Dispose();
                };
                output.Init(new SampleToWaveProvider(provider));
                output.Play();
                return null;
            }
            catch (Exception ex)
            {
                device?.Dispose();
                return "Test tone failed: " + ex.Message;
            }
        }

        // Starts the mic level meter. Returns null on success or a user-facing
        // error message (USB-only note when the endpoint is missing).
        internal static string StartMicMeter()
        {
            lock (Gate)
            {
                if (_capture != null)
                {
                    return null; // already running
                }

                MMDevice device = null;
                try
                {
                    device = FindEndpoint(DataFlow.Capture);
                    if (device == null)
                    {
                        return "DualSense microphone not found. Mic input works over USB only - connect the controller via USB.";
                    }

                    _currentLevel = 0;
                    _capture = new WasapiCapture(device);
                    _capture.DataAvailable += OnCaptureData;
                    _capture.RecordingStopped += (s, e) => StopMicMeter();
                    _capture.StartRecording();
                    return null;
                }
                catch (Exception ex)
                {
                    device?.Dispose();
                    StopLocked();
                    return "Mic capture failed: " + ex.Message;
                }
            }
        }

        internal static void StopMicMeter()
        {
            lock (Gate)
            {
                StopLocked();
            }
        }

        private static void StopLocked()
        {
            if (_capture != null)
            {
                try
                {
                    _capture.DataAvailable -= OnCaptureData;
                    _capture.StopRecording();
                }
                catch (Exception)
                {
                }

                try { _capture.Dispose(); }
                catch (Exception) { }
                _capture = null;
            }

            _currentLevel = 0;
        }

        private static void OnCaptureData(object sender, WaveInEventArgs e)
        {
            var capture = _capture;
            if (capture == null || e.BytesRecorded <= 0)
            {
                return;
            }

            double sum = 0;
            var samples = 0;
            var format = capture.WaveFormat;
            if (format.Encoding == WaveFormatEncoding.IeeeFloat)
            {
                var waveBuffer = new WaveBuffer(e.Buffer);
                var count = e.BytesRecorded / 4;
                for (var i = 0; i < count; i++)
                {
                    var v = waveBuffer.FloatBuffer[i];
                    sum += v * v;
                }
                samples = count;
            }
            else if (format.BitsPerSample == 16)
            {
                var count = e.BytesRecorded / 2;
                for (var i = 0; i < count; i++)
                {
                    var v = (short)(e.Buffer[i * 2] | (e.Buffer[i * 2 + 1] << 8));
                    var n = v / 32768.0;
                    sum += n * n;
                }
                samples = count;
            }

            if (samples > 0)
            {
                lock (Gate)
                {
                    _currentLevel = (float)Math.Sqrt(sum / samples);
                }
            }
        }

        // 440 Hz sine in IEEE float at the endpoint's mix sample rate and
        // channel count. Stereo goes to channels 1-2; channels 3-4 (haptic
        // actuators on the 4-channel layout) stay silent.
        private sealed class SineToneProvider : ISampleProvider
        {
            private readonly int _sampleRate;
            private readonly int _totalFrames;
            private readonly double _frequency;
            private int _frame;

            internal SineToneProvider(int sampleRate, int channels, double frequency, double seconds)
            {
                _sampleRate = sampleRate;
                _frequency = frequency;
                _totalFrames = (int)(seconds * sampleRate);
                WaveFormat = WaveFormat.CreateIeeeFloatWaveFormat(sampleRate, channels);
            }

            public WaveFormat WaveFormat { get; }

            public int Read(float[] buffer, int offset, int count)
            {
                var channels = WaveFormat.Channels;
                var frames = count / channels;
                var framesToWrite = Math.Min(frames, _totalFrames - _frame);
                for (var f = 0; f < framesToWrite; f++)
                {
                    var sample = (float)(Math.Sin(2.0 * Math.PI * _frequency * (_frame + f) / _sampleRate) * 0.25);
                    var baseIndex = offset + f * channels;
                    for (var c = 0; c < channels; c++)
                    {
                        buffer[baseIndex + c] = c < 2 ? sample : 0.0f;
                    }
                }

                _frame += framesToWrite;
                return framesToWrite * channels;
            }
        }
    }
}
