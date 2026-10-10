# A screenshot of what the monitors show, from dwm.exe (the protocol of the
# Snipping Tool, wine/arctic_dwm.h): composited, acrylic and all, unlike
# GDI. Physical pixels.  pwsh dwmshot.ps1 -X 0 -Y 1020 -W 700 -H 60 -Out x.png
param([int]$X = 0, [int]$Y = 1020, [int]$W = 700, [int]$H = 60, [string]$Out = "$PSScriptRoot\dwm.png")
$src = @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class Dwm {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFileMappingW(IntPtr f, IntPtr a, uint p, uint hi, uint lo, string n);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr MapViewOfFile(IntPtr m, uint acc, uint hi, uint lo, UIntPtr n);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr OpenEventW(uint acc, bool inh, string n);
  [DllImport("kernel32.dll")] static extern bool SetEvent(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool ResetEvent(IntPtr h);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
  public static int[] Grab(int x, int y, int w, int h) {
    var lk = new Mutex(false, "Global\\__arctic_dwm_capture_lock"); lk.WaitOne(5000);
    try {
      long size = 20 + (long)w * h * 4;
      IntPtr req = OpenEventW(2, false, "Global\\__arctic_dwm_capture_request");
      IntPtr done = OpenEventW(0x100002, false, "Global\\__arctic_dwm_capture_done");
      IntPtr sec = CreateFileMappingW(new IntPtr(-1), IntPtr.Zero, 4, 0, (uint)size, "Global\\__arctic_dwm_capture");
      IntPtr v = MapViewOfFile(sec, 6, 0, 0, UIntPtr.Zero);
      if (req == IntPtr.Zero || done == IntPtr.Zero || v == IntPtr.Zero) throw new Exception("no dwm capture " + Marshal.GetLastWin32Error());
      Marshal.WriteInt32(v, 0, x); Marshal.WriteInt32(v, 4, y); Marshal.WriteInt32(v, 8, w); Marshal.WriteInt32(v, 12, h); Marshal.WriteInt32(v, 16, -1);
      ResetEvent(done); SetEvent(req);
      if (WaitForSingleObject(done, 5000) != 0) throw new Exception("dwm did not answer");
      int st = Marshal.ReadInt32(v, 16); if (st != 0) throw new Exception("status " + st);
      var px = new int[w * h]; Marshal.Copy(v + 20, px, 0, w * h); return px;
    } finally { lk.ReleaseMutex(); }
  }
}
'@
Add-Type -TypeDefinition $src
Add-Type -AssemblyName System.Drawing
$px = [Dwm]::Grab($X, $Y, $W, $H)
$bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
$data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $W, $H), [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
[System.Runtime.InteropServices.Marshal]::Copy($px, 0, $data.Scan0, $px.Length)
$bmp.UnlockBits($data)
$bmp.Save($Out)
"saved $Out"
