using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

internal static class Launcher {
    [STAThread]
    private static int Main(string[] args) {
        bool checkOnly = args.Length == 1 && args[0] == "--check";
        try {
            string root = AppDomain.CurrentDomain.BaseDirectory;
            string pointer = Path.Combine(root, "tool-root.txt");
            if (File.Exists(pointer)) root = Path.GetFullPath(Path.Combine(root, File.ReadAllText(pointer).Trim()));
            string script = Path.Combine(root, "pc", "Start-Cafe-au-Lait.ps1");
            if (!File.Exists(script)) throw new FileNotFoundException("Cafe au Lait's PC component is missing. Keep the package together.");
            if (checkOnly) return 0;
            var start = new ProcessStartInfo {
                FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "WindowsPowerShell", "v1.0", "powershell.exe"),
                Arguments = "-NoProfile -ExecutionPolicy Bypass -STA -File \"" + script + "\"",
                WorkingDirectory = root,
                UseShellExecute = false,
                CreateNoWindow = true
            };
            string key;
            using (var hash = SHA256.Create()) key = BitConverter.ToString(hash.ComputeHash(Encoding.UTF8.GetBytes(root.ToLowerInvariant()))).Replace("-", "");
            bool first;
            using (var mutex = new Mutex(true, "Local\\CafeAuLait-" + key, out first)) {
                if (!first) { MessageBox.Show("Cafe au Lait is already open for this setup package.", "Cafe au Lait"); return 0; }
                try { using (var process = Process.Start(start)) { process.WaitForExit(); return process.ExitCode; } }
                finally { mutex.ReleaseMutex(); }
            }
        } catch (Exception error) {
            if (!checkOnly) MessageBox.Show(error.Message, "Cafe au Lait", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
