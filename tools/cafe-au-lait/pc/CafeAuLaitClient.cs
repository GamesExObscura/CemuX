using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace CafeAuLait {
    public sealed partial class DownloadJob {
        public volatile string Status = "Not connected";
        public volatile string PairCode = "";
        public volatile string Error = "";
        public volatile string Account = "";
        public volatile bool Complete;
        public volatile bool Finished;
        public volatile bool Cancelled;
        public volatile int FilesDone;
        public volatile int FileCount;
        public long BytesDone;
        public string DirectoryPath { get; private set; }
        private TcpClient client;
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private readonly HashSet<string> roots = new HashSet<string>(StringComparer.Ordinal);
        private readonly HashSet<string> nativeLibraries = new HashSet<string>(StringComparer.Ordinal);
        private readonly HashSet<string> names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly object gate = new object();
        private bool legacyTest;

        public DownloadJob(string policyFile) {
            AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);
            AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);
            var json = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(policyFile));
            foreach (string group in new [] {"system_apps", "applets", "shared"}) {
                string high = group == "system_apps" ? "00050010" : group == "applets" ? "00050030" : "0005001b";
                foreach (object value in (IEnumerable)json[group]) {
                    string id = (string)value;
                    roots.Add("mlc01/sys/title/" + high + "/" + id);
                    if (group != "shared") roots.Add("mlc01/usr/save/" + high + "/" + id);
                }
            }
            foreach (object value in (IEnumerable)json["system_state"]) roots.Add("mlc01/" + (string)value);
            foreach (object value in (IEnumerable)json["native_libraries"]) nativeLibraries.Add("cafeLibs/" + (string)value);
        }
        public static bool PrivateAddress(IPAddress address) {
            byte[] b = address.GetAddressBytes();
            return b.Length == 4 && (b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
                   (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254));
        }
        public bool Allowed(string path, string account) {
            if (!System.Text.RegularExpressions.Regex.IsMatch(account, "^8[0-9a-f]{7}$") || account == "80000000") return false;
            if (path.Length == 0 || path.Length > 480 || path.StartsWith("/") || path.EndsWith("/")) return false;
            foreach (char c in path) if (c < 32 || c > 126 || "\\:<>\"|?*".IndexOf(c) >= 0) return false;
            foreach (string part in path.Split('/')) {
                if (part.Length == 0 || part == "." || part == ".." || part.EndsWith(".") || part.EndsWith(" ")) return false;
                string device = part.Split('.')[0].ToUpperInvariant();
                if (device == "CON" || device == "NUL" || device == "AUX" || device == "PRN" ||
                    System.Text.RegularExpressions.Regex.IsMatch(device, "^(COM|LPT)[1-9]$")) return false;
            }
            if (path == "otp.bin" || path == "seeprom.bin") return true;
            if (nativeLibraries.Contains(path)) return true;
            if (path.StartsWith("mlc01/usr/save/system/act/" + account + "/", StringComparison.Ordinal)) return true;
            foreach (string root in roots) if (path.StartsWith(root + "/", StringComparison.Ordinal)) return true;
            return false;
        }
        public void Start(string address, int port, string destination, bool trustedLanConsent) {
            StartInternal(address, port, destination, trustedLanConsent, false, false);
        }
        // Tests may bind loopback only. Production UI has no switch for this overload.
        public void StartLoopbackTest(int port, string destination) {
            StartInternal("127.0.0.1", port, destination, true, true, true);
        }
        public void StartFullLoopbackTest(int port, string destination) {
            StartInternal("127.0.0.1", port, destination, true, true, false);
        }
        private void StartInternal(string address, int port, string destination, bool consent, bool test, bool allowLegacy) {
            IPAddress ip;
            if (!consent || !IPAddress.TryParse(address, out ip) || (!PrivateAddress(ip) && !(test && IPAddress.IsLoopback(ip))))
                throw new InvalidOperationException("Approve trusted-LAN use and enter a private IPv4 address, not a hostname or Internet address.");
            if (port < 1 || port > 65535 || DirectoryPath != null) throw new InvalidOperationException("Invalid or reused transfer job.");
            destination = Path.GetFullPath(destination);
            if (destination.StartsWith("\\\\", StringComparison.Ordinal)) throw new IOException("Choose a local PC drive for private console data, not a network/device path.");
            if (Directory.Exists(destination) || File.Exists(destination)) throw new IOException("Destination must be a new private folder.");
            for (string p = Path.GetDirectoryName(destination); !String.IsNullOrEmpty(p); p = Path.GetDirectoryName(p))
                if (Directory.Exists(p) && (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0) throw new IOException("Linked destination rejected.");
            DirectoryPath = destination;
            legacyTest = allowLegacy;
            Task.Run(() => Run(ip, port));
        }
        public void Cancel() {
            cancellation.Cancel();
            lock (gate) { if (client != null) client.Close(); }
        }
        private void Check() { cancellation.Token.ThrowIfCancellationRequested(); }
        private byte[] Read(NetworkStream stream, int count) {
            byte[] data = new byte[count]; int offset = 0;
            while (offset < count) { Check(); int n = stream.Read(data, offset, count - offset); if (n <= 0) throw new EndOfStreamException("Wii U disconnected; this transfer is incomplete."); offset += n; }
            return data;
        }
        private ulong Number(NetworkStream stream, int count) {
            byte[] data = Read(stream, count); ulong result = 0; foreach (byte b in data) result = (result << 8) | b; return result;
        }
        private void Run(IPAddress ip, int port) {
            try {
                Status = "Connecting to Wii U...";
                var tcp = new TcpClient(AddressFamily.InterNetwork);
                lock (gate) { client = tcp; }
                Check(); var connect = tcp.ConnectAsync(ip, port);
                if (!connect.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException("Could not connect. Open Cafe au Lait on Wii U and press A first.");
                connect.GetAwaiter().GetResult();
                using (tcp) using (NetworkStream stream = tcp.GetStream()) {
                    stream.ReadTimeout = 180000; stream.WriteTimeout = 15000;
                    byte[] nonce = new byte[32]; using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(nonce);
                    PairCode = BitConverter.ToString(nonce, 0, 8).Replace("-", "");
                    byte[] hello = Encoding.ASCII.GetBytes("CALPAIR1"); stream.Write(hello, 0, hello.Length); stream.Write(nonce, 0, nonce.Length);
                    Status = "Compare code " + PairCode + " on Wii U, then press A there to approve.";
                    string protocol = Encoding.ASCII.GetString(Read(stream, 8));
                    while (protocol == "CALSCAN2") {
                        ulong scanned = Number(stream, 8), bytes = Number(stream, 8);
                        if (scanned > 250000 || bytes > 1099511627776UL) throw new IOException("Invalid full MLC scan progress bounds.");
                        Status = "Wii U is inventorying the full MLC: " + scanned + " entries, " + (bytes / 1048576) + " MB. Nothing is filtered.";
                        protocol = Encoding.ASCII.GetString(Read(stream, 8));
                    }
                    if (protocol == "CALDUMP2") { ReceiveFull(stream); return; }
                    if (protocol != "CALDUMP1" || !legacyTest) throw new IOException("Update the Wii U SD app: a new FULL MLC export is required. Old partial transfers are not accepted.");
                    Account = Encoding.ASCII.GetString(Read(stream, 8));
                    if (!System.Text.RegularExpressions.Regex.IsMatch(Account, "^8[0-9a-f]{7}$") || Account == "80000000") throw new IOException("Invalid original console account ID.");
                    ulong count = Number(stream, 4); if (count == 0 || count > 20000) throw new IOException("Invalid snapshot file count.");
                    FileCount = (int)count;
                    Directory.CreateDirectory(DirectoryPath);
                    var security = new DirectorySecurity(); security.SetAccessRuleProtection(true, false);
                    foreach (var sid in new [] {WindowsIdentity.GetCurrent().User, new SecurityIdentifier("S-1-5-18")})
                        security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
                    Directory.SetAccessControl(DirectoryPath, security);
                    File.WriteAllText(Path.Combine(DirectoryPath, "TRANSFER-INCOMPLETE.txt"), "Do not import without DUMP-COMPLETE.txt.");
                    byte[] buffer = new byte[32768]; long total = 0;
                    for (int index = 0; index < FileCount; ++index) {
                        Check(); ulong pathLength = Number(stream, 4), size = Number(stream, 8);
                        if (pathLength < 1 || pathLength > 480 || size > 256UL * 1024 * 1024 || (ulong)total + size > 2UL * 1024 * 1024 * 1024) throw new IOException("Snapshot limits exceeded.");
                        byte[] nameBytes = Read(stream, (int)pathLength);
                        foreach (byte b in nameBytes) if (b < 32 || b > 126) throw new IOException("Invalid filename encoding.");
                        string relative = Encoding.ASCII.GetString(nameBytes);
                        if (!Allowed(relative, Account) || !names.Add(relative)) throw new IOException("Unapproved or duplicate file path. Transfer stopped.");
                        if ((relative == "otp.bin" && size != 1024) || (relative == "seeprom.bin" && size != 512)) throw new IOException("Invalid console identity length.");
                        string path = Path.GetFullPath(Path.Combine(DirectoryPath, relative.Replace('/', Path.DirectorySeparatorChar)));
                        if (!path.StartsWith(DirectoryPath.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new IOException("Path escaped private transfer folder.");
                        if ((ulong)new DriveInfo(Path.GetPathRoot(path)).AvailableFreeSpace < size + 64UL * 1024 * 1024) throw new IOException("Not enough PC disk space.");
                        Directory.CreateDirectory(Path.GetDirectoryName(path));
                        using (var sha = SHA256.Create()) using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                            ulong remaining = size;
                            while (remaining > 0) {
                                Check(); int need = (int)Math.Min((ulong)buffer.Length, remaining);
                                int n = stream.Read(buffer, 0, need); if (n <= 0) throw new EndOfStreamException("Wii U disconnected; partial data will not be imported.");
                                output.Write(buffer, 0, n); sha.TransformBlock(buffer, 0, n, null, 0); remaining -= (ulong)n; Interlocked.Add(ref BytesDone, n);
                            }
                            sha.TransformFinalBlock(new byte[0], 0, 0); byte[] expected = Read(stream, 32);
                            int difference = 0; for (int i = 0; i < 32; ++i) difference |= sha.Hash[i] ^ expected[i];
                            if (difference != 0) throw new IOException("SHA-256 mismatch; transfer is incomplete and will not be imported.");
                            output.Flush(true);
                        }
                        total += (long)size; FilesDone = index + 1; Status = "Verified file " + FilesDone + " / " + FileCount;
                    }
                    if (Encoding.ASCII.GetString(Read(stream, 8)) != "CALDONE1") throw new IOException("Final completion marker missing.");
                    if (!names.Contains("otp.bin") || !names.Contains("seeprom.bin") || !names.Contains("mlc01/usr/save/system/act/" + Account + "/account.dat")) throw new IOException("Snapshot is missing identity/account files.");
                    foreach (string required in nativeLibraries) if (!names.Contains(required)) throw new IOException("Snapshot is missing required native libraries. Use the updated Wii U app; no incomplete import is permitted.");
                    Check();
                    var info = new Dictionary<string, object> { {"schema", 1}, {"tool", "dumpling-minimal-system"}, {"transport", "Cafe au Lait trusted LAN private v1"}, {"account", Account}, {"game_files_copied", false}, {"game_saves_copied", false} };
                    File.WriteAllText(Path.Combine(DirectoryPath, "dump-info.json"), new JavaScriptSerializer().Serialize(info));
                    File.WriteAllText(Path.Combine(DirectoryPath, "DUMP-COMPLETE.txt"), "Cafe au Lait: all received files verified. Run structural validation before import.\r\n");
                    Complete = true; Status = "Transfer verified. Ready for private Cemu import.";
                }
            } catch (Exception ex) {
                Cancelled = cancellation.IsCancellationRequested;
                Error = Cancelled ? "Transfer cancelled. Existing games, saves and Cemu are unchanged." : ex.GetBaseException().Message;
                Status = Error;
            } finally { lock (gate) { if (client != null) client.Close(); } Finished = true; }
        }
    }
}
