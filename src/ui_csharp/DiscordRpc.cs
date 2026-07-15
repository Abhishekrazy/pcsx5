using System;
using System.IO.Pipes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;

namespace Pcsx5Ui
{
    public class DiscordRpc
    {
        private const string DefaultAppId = "1323456789012345678";
        private NamedPipeClientStream _pipe;
        private string _appId;
        private bool _handshakeDone;
        private Task _pumpTask;
        private CancellationTokenSource _cts;

        public bool Connected => _pipe != null && _pipe.IsConnected && _handshakeDone;

        public void Start(string appId = DefaultAppId)
        {
            Stop();

            _appId = appId;
            _cts = new CancellationTokenSource();
            _pumpTask = Task.Run(() => ConnectionLoop(_cts.Token));
        }

        public void Stop()
        {
            if (_cts != null)
            {
                _cts.Cancel();
                _cts = null;
            }

            if (_pipe != null)
            {
                try { _pipe.Close(); } catch { }
                _pipe = null;
            }

            _handshakeDone = false;
        }

        private async Task ConnectionLoop(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                if (_pipe == null || !_pipe.IsConnected)
                {
                    _handshakeDone = false;
                    _pipe = await TryConnectPipeAsync(token);
                    if (_pipe != null)
                    {
                        if (await SendHandshakeAsync(token))
                        {
                            _handshakeDone = true;
                            await UpdatePresenceAsync("Idle", "Main Menu", "pcsx5_logo", "PCSX5 Emulator", "", "", token);
                        }
                        else
                        {
                            try { _pipe.Close(); } catch { }
                            _pipe = null;
                        }
                    }
                }

                try
                {
                    await Task.Delay(5000, token);
                }
                catch (TaskCanceledException)
                {
                    break;
                }
            }
        }

        private async Task<NamedPipeClientStream> TryConnectPipeAsync(CancellationToken token)
        {
            for (int i = 0; i < 10; i++)
            {
                if (token.IsCancellationRequested) return null;
                try
                {
                    string pipeName = $"discord-ipc-{i}";
                    var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
                    await pipe.ConnectAsync(100, token);
                    return pipe;
                }
                catch
                {
                    // Ignore connection failures and try next index
                }
            }
            return null;
        }

        private async Task<bool> SendHandshakeAsync(CancellationToken token)
        {
            if (_pipe == null || !_pipe.IsConnected) return false;

            string payload = $"{{\"v\":1,\"client_id\":\"{_appId}\"}}";
            return await SendFrameAsync(0, payload, token); // Opcode 0: Handshake
        }

        private async Task<bool> SendFrameAsync(int opcode, string payload, CancellationToken token)
        {
            if (_pipe == null || !_pipe.IsConnected) return false;

            try
            {
                byte[] payloadBytes = Encoding.UTF8.GetBytes(payload);
                byte[] header = new byte[8];

                // Opcode (4 bytes, little-endian)
                header[0] = (byte)(opcode & 0xFF);
                header[1] = (byte)((opcode >> 8) & 0xFF);
                header[2] = (byte)((opcode >> 16) & 0xFF);
                header[3] = (byte)((opcode >> 24) & 0xFF);

                // Length (4 bytes, little-endian)
                int len = payloadBytes.Length;
                header[4] = (byte)(len & 0xFF);
                header[5] = (byte)((len >> 8) & 0xFF);
                header[6] = (byte)((len >> 16) & 0xFF);
                header[7] = (byte)((len >> 24) & 0xFF);

                await _pipe.WriteAsync(header, 0, header.Length, token);
                await _pipe.WriteAsync(payloadBytes, 0, payloadBytes.Length, token);
                await _pipe.FlushAsync(token);
                return true;
            }
            catch
            {
                return false;
            }
        }

        public void UpdatePresence(string details, string state, string largeImageKey = "pcsx5_logo", string largeImageText = "PCSX5 Emulator", string smallImageKey = "", string smallImageText = "")
        {
            if (_pipe == null || !_pipe.IsConnected || !_handshakeDone) return;

            Task.Run(async () =>
            {
                try
                {
                    await UpdatePresenceAsync(details, state, largeImageKey, largeImageText, smallImageKey, smallImageText, CancellationToken.None);
                }
                catch { }
            });
        }

        private async Task UpdatePresenceAsync(string details, string state, string largeImageKey, string largeImageText, string smallImageKey, string smallImageText, CancellationToken token)
        {
            var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();

            var activityJson = new StringBuilder();
            activityJson.Append("{");
            activityJson.Append($"\"details\":\"{EscapeJson(details)}\",");
            activityJson.Append($"\"state\":\"{EscapeJson(state)}\",");
            activityJson.Append($"\"timestamps\":{{\"start\":{now}}},");
            activityJson.Append("\"assets\":{");
            activityJson.Append($"\"large_image\":\"{EscapeJson(largeImageKey)}\",");
            activityJson.Append($"\"large_text\":\"{EscapeJson(largeImageText)}\"");
            if (!string.IsNullOrEmpty(smallImageKey))
            {
                activityJson.Append($",\"small_image\":\"{EscapeJson(smallImageKey)}\",");
                activityJson.Append($"\"small_text\":\"{EscapeJson(smallImageText)}\"");
            }
            activityJson.Append("}");
            activityJson.Append("}");

            string nonce = Guid.NewGuid().ToString();
            string payload = $"{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{Process.GetCurrentProcess().Id},\"activity\":{activityJson.ToString()}}},\"nonce\":\"{nonce}\"}}";

            await SendFrameAsync(1, payload, token); // Opcode 1: Frame
        }

        private string EscapeJson(string str)
        {
            if (string.IsNullOrEmpty(str)) return "";
            return str.Replace("\\", "\\\\").Replace("\"", "\\\"");
        }
    }
}
