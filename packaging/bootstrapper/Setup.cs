using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Threading.Tasks;
using System.Windows.Forms;

// NAI PaintBrush 1-click installer.
//
// One window: download (or use a local MSIX next to the exe), verify SHA-256,
// provision the NAI trust chain machine-wide, install the signed MSIX and
// launch the app. The process runs elevated (see setup.manifest), so Windows
// asks for UAC exactly once.
//
// The MSIX URL and hash are injected at build time by scripts/pb-build-setup.ps1.

static class Program
{
    public const string ProductName = "NAI PaintBrush";
    public const string PackageName = "NAI.NAIPaintBrush";
    public const string MsixFileName = "NAI.PaintBrush_1.0.0.0_x64.msix";
    public const string MsixUrl = "@MSIX_URL@";
    public const string MsixSha256 = "@MSIX_SHA256@";

    [STAThread]
    static void Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm(args.Length > 0 ? args[0] : null));
    }
}

sealed class SetupForm : Form
{
    private readonly string _localMsix;
    private readonly Label _status;
    private readonly ProgressBar _progress;
    private readonly Label _hint;

    public SetupForm(string localMsix)
    {
        _localMsix = localMsix;

        Text = Program.ProductName + " setup";
        ClientSize = new Size(560, 210);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(18, 18, 22);
        ForeColor = Color.White;
        Font = new Font("Segoe UI", 10f);

        var title = new Label
        {
            Text = Program.ProductName,
            Font = new Font("Segoe UI Semibold", 18f),
            AutoSize = true,
            Location = new Point(24, 22),
            ForeColor = Color.White,
        };
        _status = new Label
        {
            Text = "Starting…",
            AutoSize = false,
            Location = new Point(26, 72),
            Size = new Size(508, 44),
            ForeColor = Color.FromArgb(200, 200, 210),
        };
        _progress = new ProgressBar
        {
            Location = new Point(26, 124),
            Size = new Size(508, 16),
            Style = ProgressBarStyle.Continuous,
            Maximum = 1000,
        };
        _hint = new Label
        {
            Text = "",
            AutoSize = false,
            Location = new Point(26, 152),
            Size = new Size(508, 40),
            ForeColor = Color.FromArgb(150, 150, 160),
            Font = new Font("Segoe UI", 8.5f),
        };

        Controls.Add(title);
        Controls.Add(_status);
        Controls.Add(_progress);
        Controls.Add(_hint);

        Shown += async (s, e) => await InstallAsync();
    }

    private void SetStatus(string text, int permille = -1)
    {
        if (InvokeRequired) { BeginInvoke(new Action(() => SetStatus(text, permille))); return; }
        _status.Text = text;
        if (permille >= 0) _progress.Value = Math.Min(1000, Math.Max(0, permille));
    }

    private async Task InstallAsync()
    {
        try
        {
            var msix = ResolveMsix();
            if (_localMsix == null && !File.Exists(msix))
                await DownloadAsync(msix);

            SetStatus("Verifying the download…", 780);
            VerifyHash(msix);

            SetStatus("Trusting the NAI signing chain…", 820);
            ImportChain();

            SetStatus("Installing…", 880);
            RemoveExisting();
            RunPowerShell("Add-AppxPackage -Path '" + Escape(msix) + "'");

            SetStatus("Launching…", 960);
            var family = RunPowerShell(
                "(Get-AppxPackage " + Program.PackageName + ").PackageFamilyName").Trim();
            if (family.Length > 0)
                Process.Start("explorer.exe", "shell:appsFolder\\" + family + "!App");

            SetStatus("Done. Welcome to " + Program.ProductName + ".", 1000);
            _hint.Text = "You can close this window — the app is opening.";
            await Task.Delay(2200);
            Close();
        }
        catch (Exception ex)
        {
            SetStatus("Installation failed.", 0);
            _hint.Text = ex.Message;
            MessageBox.Show(this, ex.Message, Program.ProductName + " setup",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private string ResolveMsix()
    {
        if (_localMsix != null)
            return Path.GetFullPath(_localMsix);

        var beside = Path.Combine(
            Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location) ?? ".",
            Program.MsixFileName);
        if (File.Exists(beside))
            return beside;

        return Path.Combine(Path.GetTempPath(), Program.MsixFileName);
    }

    private async Task DownloadAsync(string destination)
    {
        SetStatus("Downloading " + Program.ProductName + " (2.4 GB)…", 0);
        _hint.Text = "One-time download. Grab a coffee.";

        using (var client = new WebClient())
        {
            client.DownloadProgressChanged += (s, e) =>
                SetStatus(string.Format("Downloading… {0:0.0} / {1:0.0} MB",
                                        e.BytesReceived / 1048576.0,
                                        e.TotalBytesToReceive / 1048576.0),
                          e.TotalBytesToReceive > 0
                              ? (int)(e.BytesReceived * 700 / e.TotalBytesToReceive)
                              : 0);
            await client.DownloadFileTaskAsync(new Uri(Program.MsixUrl), destination);
        }
    }

    private static void VerifyHash(string path)
    {
        if (Program.MsixSha256.StartsWith("@"))
            return; // development build without an injected hash
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(path))
        {
            var actual = BitConverter.ToString(sha.ComputeHash(stream))
                                    .Replace("-", string.Empty);
            if (!string.Equals(actual, Program.MsixSha256,
                               StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(
                    "The downloaded package does not match the release hash.\n" +
                    "Expected " + Program.MsixSha256 + "\nGot      " + actual);
        }
    }

    private static void ImportChain()
    {
        ImportCertificate("PB.NAICA.Root.cer", StoreName.Root);
        ImportCertificate("PB.NAICA.Intermediate.cer", StoreName.CertificateAuthority);
        ImportCertificate("PB.NAIProduct.cer", StoreName.TrustedPeople);
    }

    private static void ImportCertificate(string resource, StoreName storeName)
    {
        using (var stream = Assembly.GetExecutingAssembly()
                                    .GetManifestResourceStream(resource))
        {
            if (stream == null)
                throw new InvalidOperationException(
                    "Installer is missing its embedded certificate: " + resource);
            using (var buffer = new MemoryStream())
            {
                stream.CopyTo(buffer);
                var cert = new X509Certificate2(buffer.ToArray());
                using (var store = new X509Store(storeName, StoreLocation.LocalMachine))
                {
                    store.Open(OpenFlags.ReadWrite);
                    store.Add(cert);
                }
            }
        }
    }

    private static void RemoveExisting()
    {
        RunPowerShell(
            "Get-AppxPackage " + Program.PackageName +
            " | Remove-AppxPackage -ErrorAction SilentlyContinue");
    }

    private static string Escape(string value)
    {
        return value.Replace("'", "''");
    }

    private static string RunPowerShell(string command)
    {
        var psi = new ProcessStartInfo
        {
            FileName = "powershell.exe",
            Arguments = "-NoProfile -ExecutionPolicy Bypass -Command \"" +
                        command.Replace("\"", "\\\"") + "\"",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        using (var process = Process.Start(psi))
        {
            var output = process.StandardOutput.ReadToEnd();
            var error = process.StandardError.ReadToEnd();
            process.WaitForExit();
            if (process.ExitCode != 0)
                throw new InvalidOperationException(
                    (error.Length > 0 ? error : output).Trim());
            return output;
        }
    }
}
