param(
  [Parameter(Mandatory = $true)]
  [string]$Executable
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

public static class OverlayInputPassthroughProbe
{
    private const uint INPUT_MOUSE = 0;
    private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    private const uint MOUSEEVENTF_LEFTUP = 0x0004;
    private const uint WM_CLOSE = 0x0010;

    [StructLayout(LayoutKind.Sequential)]
    private struct PointNative
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MouseInput
    {
        public int Dx;
        public int Dy;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public IntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)] public MouseInput Mouse;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Input
    {
        public uint Type;
        public InputUnion Data;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint count, Input[] inputs, int size);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out PointNative point);

    [DllImport("user32.dll")]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    public static int Run(string executable)
    {
        var shown = new ManualResetEventSlim(false);
        var clicked = new ManualResetEventSlim(false);
        Point clickPoint = Point.Empty;
        IntPtr targetWindow = IntPtr.Zero;
        Form targetForm = null;

        var uiThread = new Thread(() =>
        {
            targetForm = new Form
            {
                Text = "Vervormde Skerm Input Probe",
                StartPosition = FormStartPosition.Manual,
                Location = new Point(Screen.PrimaryScreen.WorkingArea.Left + 60,
                                     Screen.PrimaryScreen.WorkingArea.Top + 60),
                ClientSize = new Size(340, 210),
                ShowInTaskbar = false,
                TopMost = true
            };
            var button = new Button
            {
                Text = "Input target",
                Location = new Point(80, 70),
                Size = new Size(180, 70)
            };
            button.MouseDown += (sender, args) => clicked.Set();
            targetForm.Controls.Add(button);
            targetForm.Shown += (sender, args) =>
            {
                targetWindow = targetForm.Handle;
                clickPoint = button.PointToScreen(
                    new Point(button.ClientSize.Width / 2, button.ClientSize.Height / 2));
                shown.Set();
            };
            Application.Run(targetForm);
        });
        uiThread.IsBackground = true;
        uiThread.SetApartmentState(ApartmentState.STA);
        uiThread.Start();

        if (!shown.Wait(5000))
        {
            Console.WriteLine("input_target_ready=false");
            return 2;
        }

        PointNative originalCursor;
        GetCursorPos(out originalCursor);
        Process overlay = null;
        bool received = false;
        try
        {
            SetCursorPos(clickPoint.X, clickPoint.Y);
            overlay = Process.Start(new ProcessStartInfo
            {
                FileName = executable,
                Arguments = "--smoke-test",
                UseShellExecute = true
            });
            if (overlay == null)
            {
                Console.WriteLine("overlay_started=false");
                return 3;
            }

            IntPtr overlayWindow = IntPtr.Zero;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                overlayWindow = FindWindow("VervormdeSkermNativeOverlay", null);
                if (overlayWindow != IntPtr.Zero && IsWindowVisible(overlayWindow)) break;
                Thread.Sleep(50);
            }

            Console.WriteLine("overlay_visible=" +
                (overlayWindow != IntPtr.Zero && IsWindowVisible(overlayWindow) ? "true" : "false"));
            if (overlayWindow == IntPtr.Zero || !IsWindowVisible(overlayWindow)) return 4;

            var inputs = new[]
            {
                new Input
                {
                    Type = INPUT_MOUSE,
                    Data = new InputUnion { Mouse = new MouseInput { Flags = MOUSEEVENTF_LEFTDOWN } }
                },
                new Input
                {
                    Type = INPUT_MOUSE,
                    Data = new InputUnion { Mouse = new MouseInput { Flags = MOUSEEVENTF_LEFTUP } }
                }
            };
            uint sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(Input)));
            received = sent == inputs.Length && clicked.Wait(3000);
            Console.WriteLine("send_input_count=" + sent);
            Console.WriteLine("underlying_mouse_down=" + (received ? "true" : "false"));
        }
        finally
        {
            SetCursorPos(originalCursor.X, originalCursor.Y);
            if (overlay != null)
            {
                if (!overlay.WaitForExit(12000)) overlay.Kill();
                overlay.Dispose();
            }
            if (targetWindow != IntPtr.Zero) PostMessage(targetWindow, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
            uiThread.Join(3000);
        }

        return received ? 0 : 1;
    }
}
'@ -ReferencedAssemblies System.Windows.Forms,System.Drawing

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
exit [OverlayInputPassthroughProbe]::Run($resolvedExecutable)
