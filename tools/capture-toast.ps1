# Shows a notification-area balloon on this Windows 10 machine, which Windows
# shows as a toast, and films the corner of the screen it appears in: every
# frame goes to out/tmp/toastcap/<milliseconds>.png, to measure how Windows 10
# lays the toast out and moves it in and out.
#
#   powershell -ExecutionPolicy Bypass -File tools/capture-toast.ps1 [-Modern]
#
# -Modern shows it through ToastNotificationManager instead, for a machine set
# to show legacy balloons (EnableLegacyBalloonNotifications).
param([int]$Seconds = 9, [switch]$Modern)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System.Runtime.InteropServices;
public static class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[Dpi]::SetProcessDPIAware() | Out-Null

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "out\tmp\toastcap"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null

$screen = [System.Windows.Forms.Screen]::PrimaryScreen
$work = $screen.WorkingArea
$g = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero)
"screen $($screen.Bounds) work $work dpi $($g.DpiX)" | Out-File (Join-Path $out "info.txt")
$g.Dispose()

$w = 560; $h = 360
$x = $work.Right - $w; $y = $work.Bottom - $h

$icon = New-Object System.Windows.Forms.NotifyIcon
$icon.Icon = [System.Drawing.SystemIcons]::Information
$icon.Text = "capture"
$icon.Visible = $true

$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$frames = New-Object System.Collections.ArrayList
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$shown = $false
while ($clock.ElapsedMilliseconds -lt $Seconds * 1000) {
    if (-not $shown -and $clock.ElapsedMilliseconds -gt 300) {
        $title = "Устаткування можна безпечно витягнути"
        $text = "Тепер пристрій `"USB Mass Storage Device`" можна безпечно від'єднати від комп'ютера."
        if ($Modern) {
            [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
            $xml = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02)
            $nodes = $xml.GetElementsByTagName("text")
            $nodes.Item(0).AppendChild($xml.CreateTextNode($title)) | Out-Null
            $nodes.Item(1).AppendChild($xml.CreateTextNode($text)) | Out-Null
            $toast = [Windows.UI.Notifications.ToastNotification]::new($xml)
            $appId = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'
            [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($appId).Show($toast)
        } else {
            $icon.ShowBalloonTip(5000, $title, $text, [System.Windows.Forms.ToolTipIcon]::Info)
        }
        $shown = $true
        "shown at $($clock.ElapsedMilliseconds)" | Out-File -Append (Join-Path $out "info.txt")
    }
    $gfx.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
    $t = $clock.ElapsedMilliseconds
    $bmp.Save((Join-Path $out ("{0:D5}.png" -f $t)), [System.Drawing.Imaging.ImageFormat]::Png)
    [System.Windows.Forms.Application]::DoEvents()
}
$icon.Visible = $false
$icon.Dispose()
"frames $((Get-ChildItem $out -Filter *.png).Count) region $x,$y ${w}x$h" | Out-File -Append (Join-Path $out "info.txt")
Get-Content (Join-Path $out "info.txt")
