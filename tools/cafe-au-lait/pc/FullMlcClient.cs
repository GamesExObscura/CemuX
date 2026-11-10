using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace CafeAuLait {
    public sealed partial class DownloadJob {
        private static readonly UTF8Encoding FullUtf8 = new UTF8Encoding(false, true);
        public long TotalBytes;
        public bool AllowedFull(string path, bool directory) {
            if (String.IsNullOrEmpty(path) || path.StartsWith("/") || path.EndsWith("/")) return false;
            try { if (FullUtf8.GetByteCount(path) > 480) return false; } catch (EncoderFallbackException) { return false; }
            foreach (char c in path) if (c < 32 || c == 127 || "\\:<>\"|?*".IndexOf(c) >= 0) return false;
            foreach (string part in path.Split('/')) {
                if (part.Length == 0 || part == "." || part == ".." || part.EndsWith(".") || part.EndsWith(" ")) return false;
                string device = part.Split('.')[0].ToUpperInvariant();
                if (device == "CON" || device == "NUL" || device == "AUX" || device == "PRN" || System.Text.RegularExpressions.Regex.IsMatch(device, "^(COM|LPT)[1-9]$")) return false;
            }
            if (path == "mlc01") return directory;
            if (path.StartsWith("mlc01/", StringComparison.Ordinal)) return true;
            return !directory && (path == "otp.bin" || path == "seeprom.bin" || nativeLibraries.Contains(path));
        }
        private static void HashBytes(HashAlgorithm hash, byte[] data) { hash.TransformBlock(data, 0, data.Length, null, 0); }
        private byte[] FullRead(NetworkStream stream, int size, HashAlgorithm hash) { byte[] data = Read(stream, size); HashBytes(hash, data); return data; }
        private ulong FullNumber(NetworkStream stream, int size, HashAlgorithm hash) {
            byte[] data = FullRead(stream, size, hash); ulong value = 0; foreach (byte b in data) value = (value << 8) | b; return value;
        }
        private static bool SameHash(byte[] a, byte[] b) { int result = a.Length ^ b.Length; for (int i = 0; i < a.Length && i < b.Length; ++i) result |= a[i] ^ b[i]; return result == 0; }
        private static string Hex(byte[] value) { return BitConverter.ToString(value).Replace("-", ""); }
        private string FullPath(string relative) {
            string path = Path.GetFullPath(Path.Combine(DirectoryPath, relative.Replace('/', Path.DirectorySeparatorChar)));
            if (!path.StartsWith(DirectoryPath.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new IOException("Full MLC path escaped the new private folder.");
            for (string p = path; p != null && p.Length >= DirectoryPath.Length; p = Path.GetDirectoryName(p))
                if ((Directory.Exists(p) || File.Exists(p)) && (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0) throw new IOException("Linked destination rejected.");
            return path;
        }
        private void ReceiveFull(NetworkStream stream) {
            using (var inventoryHash = SHA256.Create()) {
                HashBytes(inventoryHash, Encoding.ASCII.GetBytes("CALDUMP2"));
                Account = Encoding.ASCII.GetString(FullRead(stream, 8, inventoryHash));
                if (!System.Text.RegularExpressions.Regex.IsMatch(Account, "^8[0-9a-f]{7}$") || Account == "80000000") throw new IOException("Invalid selected account ID.");
                ulong count = FullNumber(stream, 4, inventoryHash), expectedTotal = FullNumber(stream, 8, inventoryHash);
                if (count == 0 || count > 250000 || expectedTotal > 1099511627776UL) throw new IOException("Full MLC inventory limits exceeded.");
                FileCount = (int)count; TotalBytes = (long)expectedTotal;
                if ((ulong)new DriveInfo(Path.GetPathRoot(DirectoryPath)).AvailableFreeSpace < expectedTotal * 2 + 536870912UL) throw new IOException("Not enough PC space for the full received MLC AND its private import. Choose a larger storage folder.");
                Directory.CreateDirectory(DirectoryPath);
                var security = new DirectorySecurity(); security.SetAccessRuleProtection(true, false);
                foreach (var sid in new [] { WindowsIdentity.GetCurrent().User, new SecurityIdentifier("S-1-5-18") })
                    security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
                Directory.SetAccessControl(DirectoryPath, security);
                File.WriteAllText(Path.Combine(DirectoryPath, "TRANSFER-INCOMPLETE.txt"), "Unusable unless DUMP-COMPLETE.txt and a verified full MLC manifest exist.");
                var directories = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var json = new JavaScriptSerializer();
                string manifestPath = Path.Combine(DirectoryPath, "full-mlc-manifest.jsonl");
                byte[] buffer = new byte[32768]; ulong total = 0, metadata = 0;
                int fileCount = 0, directoryCount = 0;
                using (var manifest = new StreamWriter(new FileStream(manifestPath, FileMode.CreateNew, FileAccess.Write, FileShare.None), new UTF8Encoding(false))) {
                    manifest.WriteLine(json.Serialize(new { schema = 2, type = "header", scope = "full_mlc", account = Account, entries = count, bytes = expectedTotal }));
                    for (int index = 0; index < FileCount; ++index) {
                        Check(); ulong kind = FullNumber(stream, 1, inventoryHash), pathLength = FullNumber(stream, 4, inventoryHash), size = FullNumber(stream, 8, inventoryHash);
                        if (kind > 1 || pathLength < 1 || pathLength > 480 || size > 4294967295UL || total + size > expectedTotal || (kind == 1 && size != 0)) throw new IOException("Invalid full MLC entry bounds.");
                        metadata += pathLength + 64; if (metadata > 67108864UL) throw new IOException("Full MLC metadata safety limit exceeded.");
                        string relative = FullUtf8.GetString(FullRead(stream, (int)pathLength, inventoryHash));
                        bool directory = kind == 1;
                        if (!AllowedFull(relative, directory) || !names.Add(relative)) throw new IOException("Unsafe, case-colliding or duplicate full MLC path.");
                        if (relative.StartsWith("mlc01/", StringComparison.Ordinal)) {
                            string parent = relative.Substring(0, relative.LastIndexOf('/'));
                            if (!directories.Contains(parent)) throw new IOException("Full MLC manifest lacks an explicitly declared parent directory.");
                        }
                        if ((relative == "otp.bin" && size != 1024) || (relative == "seeprom.bin" && size != 512)) throw new IOException("Invalid console identity size.");
                        string path = FullPath(relative);
                        byte[] digest;
                        using (var sha = SHA256.Create()) {
                            if (directory) {
                                if (File.Exists(path)) throw new IOException("File/directory collision.");
                                Directory.CreateDirectory(path); directories.Add(relative); ++directoryCount;
                            } else {
                                if ((ulong)new DriveInfo(Path.GetPathRoot(path)).AvailableFreeSpace < size + 67108864UL) throw new IOException("Not enough PC space; full MLC transfer stopped.");
                                Directory.CreateDirectory(Path.GetDirectoryName(path));
                                using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                                    ulong remaining = size;
                                    while (remaining > 0) {
                                        Check(); int n = stream.Read(buffer, 0, (int)Math.Min((ulong)buffer.Length, remaining));
                                        if (n <= 0) throw new EndOfStreamException("Disconnected; full MLC remains incomplete.");
                                        output.Write(buffer, 0, n); sha.TransformBlock(buffer, 0, n, null, 0); remaining -= (ulong)n; Interlocked.Add(ref BytesDone, n);
                                    }
                                    output.Flush(true);
                                }
                                ++fileCount;
                            }
                            sha.TransformFinalBlock(new byte[0], 0, 0); digest = sha.Hash;
                            if (!SameHash(digest, FullRead(stream, 32, inventoryHash))) throw new IOException("SHA-256 mismatch; no complete full MLC marker will be written.");
                        }
                        manifest.WriteLine(json.Serialize(new { type = directory ? "directory" : "file", path = relative, size = size, sha256 = Hex(digest) }));
                        total += size; FilesDone = index + 1; Status = "Full MLC: verified " + FilesDone + " / " + FileCount + " entries";
                    }
                    if (total != expectedTotal || !directories.Contains("mlc01")) throw new IOException("Full MLC total/root mismatch.");
                    if (Encoding.ASCII.GetString(Read(stream, 8)) != "CALDONE2") throw new IOException("Full MLC completion marker missing.");
                    inventoryHash.TransformFinalBlock(new byte[0], 0, 0);
                    if (!SameHash(inventoryHash.Hash, Read(stream, 32))) throw new IOException("Full inventory checksum mismatch.");
                    manifest.Flush();
                }
                if (!names.Contains("otp.bin") || !names.Contains("seeprom.bin") || !names.Contains("mlc01/usr/save/system/act/" + Account + "/account.dat")) throw new IOException("Full MLC is missing the selected account or online identity files.");
                foreach (string required in nativeLibraries) if (!names.Contains(required)) throw new IOException("Required selected native library is absent.");
                string manifestHash; using (var sha = SHA256.Create()) using (var input = File.OpenRead(manifestPath)) manifestHash = Hex(sha.ComputeHash(input));
                Check();
                var info = new Dictionary<string, object> { {"schema", 2}, {"tool", "cafe-au-lait-full-mlc"}, {"full_mlc", true}, {"transport", "trusted LAN v2"}, {"account", Account}, {"entries", FileCount}, {"files", fileCount}, {"directories", directoryCount}, {"bytes", total}, {"manifest_sha256", manifestHash}, {"inventory_sha256", Hex(inventoryHash.Hash)}, {"game_files_copied", true}, {"game_saves_copied", true} };
                File.WriteAllText(Path.Combine(DirectoryPath, "dump-info.json"), json.Serialize(info), new UTF8Encoding(false));
                File.WriteAllText(Path.Combine(DirectoryPath, "DUMP-COMPLETE.txt"), "FULL mounted internal MLC and online files received; all entries and inventory checksum verified.\r\n");
                Complete = true; Status = "FULL MLC received and verified. Ready for placement into a new private Cemu.";
            }
        }
    }
}
