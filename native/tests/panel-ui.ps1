param(
  [Parameter(Mandatory = $true)]
  [string]$Executable
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class PanelUiProbe
{
    private const int GWL_STYLE = -16;
    private const int GWL_EXSTYLE = -20;
    private const long WS_VSCROLL = 0x00200000L;
    private const long WS_EX_TOOLWINDOW = 0x00000080L;
    private const long WS_EX_APPWINDOW = 0x00040000L;
    private const int SB_VERT = 1;
    private const int SB_BOTTOM = 7;
    private const uint SIF_ALL = 0x17;
    private const uint WM_COMMAND = 0x0111;
    private const uint WM_VSCROLL = 0x0115;
    private const uint WM_MOUSEWHEEL = 0x020A;
    private const uint WM_CLOSE = 0x0010;
    private const int IDC_TOGGLE = 102;
    private const int IDC_MINIMIZE = 123;
    private const int IDC_EXIT = 124;

    [StructLayout(LayoutKind.Sequential)]
    private struct ScrollInfo
    {
        public uint Size;
        public uint Mask;
        public int Min;
        public int Max;
        public uint Page;
        public int Position;
        public int TrackPosition;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtr(IntPtr window, int index);

    [DllImport("user32.dll")]
    private static extern bool GetScrollInfo(IntPtr window, int bar, ref ScrollInfo info);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDlgItem(IntPtr window, int id);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr window);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    private static IntPtr WaitForWindow(string className, int timeoutMs)
    {
        var deadline = Environment.TickCount + timeoutMs;
        IntPtr window;
        do
        {
            window = FindWindow(className, null);
            if (window != IntPtr.Zero) return window;
            Thread.Sleep(50);
        } while (Environment.TickCount < deadline);
        return IntPtr.Zero;
    }

    private static bool WaitFor(Func<bool> condition, int timeoutMs)
    {
        var deadline = Environment.TickCount + timeoutMs;
        do
        {
            if (condition()) return true;
            Thread.Sleep(50);
        } while (Environment.TickCount < deadline);
        return condition();
    }

    private static ScrollInfo ReadScrollInfo(IntPtr panel)
    {
        var info = new ScrollInfo
        {
            Size = (uint)Marshal.SizeOf(typeof(ScrollInfo)),
            Mask = SIF_ALL
        };
        if (!GetScrollInfo(panel, SB_VERT, ref info))
            throw new InvalidOperationException("GetScrollInfo failed.");
        return info;
    }

    public static int Run(string executable)
    {
        Process app = null;
        IntPtr panel = IntPtr.Zero;
        try
        {
            app = Process.Start(new ProcessStartInfo
            {
                FileName = executable,
                UseShellExecute = true
            });
            if (app == null) return 2;

            panel = WaitForWindow("VervormdeSkermNativePanel", 5000);
            if (panel == IntPtr.Zero) return 3;

            long style = GetWindowLongPtr(panel, GWL_STYLE).ToInt64();
            long exStyle = GetWindowLongPtr(panel, GWL_EXSTYLE).ToInt64();
            bool taskbarStyle = (exStyle & WS_EX_APPWINDOW) != 0 &&
                                (exStyle & WS_EX_TOOLWINDOW) == 0;
            bool verticalScrollbar = (style & WS_VSCROLL) != 0;

            IntPtr minimizeButton = GetDlgItem(panel, IDC_MINIMIZE);
            var buttonText = new StringBuilder(128);
            if (minimizeButton != IntPtr.Zero)
                GetWindowText(minimizeButton, buttonText, buttonText.Capacity);
            bool minimizeButtonReady = minimizeButton != IntPtr.Zero &&
                                       buttonText.ToString().Contains("\u53CD\u7578\u53D8\u7EE7\u7EED");

            IntPtr exitButton = GetDlgItem(panel, IDC_EXIT);
            Rect beforeScroll;
            Rect afterScroll;
            bool beforeRectReady = GetWindowRect(exitButton, out beforeScroll);
            PostMessage(panel, WM_VSCROLL, new IntPtr(SB_BOTTOM), IntPtr.Zero);
            Thread.Sleep(250);
            ScrollInfo bottom = ReadScrollInfo(panel);
            bool afterRectReady = GetWindowRect(exitButton, out afterScroll);
            bool scrollbarMoved = bottom.Position > 0 && beforeRectReady && afterRectReady &&
                                  afterScroll.Top < beforeScroll.Top;

            long wheelUp = ((long)(ushort)120) << 16;
            PostMessage(panel, WM_MOUSEWHEEL, new IntPtr(wheelUp), IntPtr.Zero);
            Thread.Sleep(250);
            ScrollInfo afterWheel = ReadScrollInfo(panel);
            bool mouseWheelMoved = afterWheel.Position < bottom.Position;

            PostMessage(panel, WM_COMMAND, new IntPtr(IDC_TOGGLE), IntPtr.Zero);
            IntPtr overlay = WaitForWindow("VervormdeSkermNativeOverlay", 3000);
            bool overlayEnabled = overlay != IntPtr.Zero &&
                                  WaitFor(() => IsWindowVisible(overlay), 3000);

            PostMessage(panel, WM_COMMAND, new IntPtr(IDC_MINIMIZE), IntPtr.Zero);
            bool minimized = WaitFor(() => IsIconic(panel), 3000);
            bool overlayContinues = overlay != IntPtr.Zero && IsWindowVisible(overlay);

            Process activation = Process.Start(new ProcessStartInfo
            {
                FileName = executable,
                UseShellExecute = true
            });
            bool activationExited = activation != null && activation.WaitForExit(5000);
            if (activation != null) activation.Dispose();
            bool existingPanelRestored = activationExited &&
                                         WaitFor(() => !IsIconic(panel), 3000);

            Console.WriteLine("taskbar_style=" + (taskbarStyle ? "true" : "false"));
            Console.WriteLine("vertical_scrollbar=" + (verticalScrollbar ? "true" : "false"));
            Console.WriteLine("minimize_button_ready=" + (minimizeButtonReady ? "true" : "false"));
            Console.WriteLine("scrollbar_moved=" + (scrollbarMoved ? "true" : "false"));
            Console.WriteLine("mouse_wheel_moved=" + (mouseWheelMoved ? "true" : "false"));
            Console.WriteLine("overlay_enabled=" + (overlayEnabled ? "true" : "false"));
            Console.WriteLine("panel_minimized=" + (minimized ? "true" : "false"));
            Console.WriteLine("overlay_continues=" + (overlayContinues ? "true" : "false"));
            Console.WriteLine("existing_panel_restored=" + (existingPanelRestored ? "true" : "false"));

            bool success = taskbarStyle && verticalScrollbar && minimizeButtonReady &&
                           scrollbarMoved && mouseWheelMoved && overlayEnabled && minimized &&
                           overlayContinues && existingPanelRestored;
            return success ? 0 : 1;
        }
        finally
        {
            if (panel != IntPtr.Zero)
                PostMessage(panel, WM_COMMAND, new IntPtr(IDC_EXIT), IntPtr.Zero);
            if (app != null)
            {
                if (!app.WaitForExit(12000))
                {
                    if (panel != IntPtr.Zero) PostMessage(panel, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
                    if (!app.WaitForExit(3000)) app.Kill();
                }
                app.Dispose();
            }
        }
    }
}
'@

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
exit [PanelUiProbe]::Run($resolvedExecutable)
