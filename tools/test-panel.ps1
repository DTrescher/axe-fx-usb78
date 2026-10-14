[CmdletBinding()]
param([switch]$ScheduledStartup,[switch]$CrossLaunch,[switch]$DelayedTray)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root 'build\Release\axe-usb78-panel.exe'
if($DelayedTray) {
 if($ScheduledStartup -or $CrossLaunch){throw 'The delayed-tray test uses its separate executable, not the installed startup task.'}
 $exe=Join-Path $root 'build\Release\axe-usb78-panel-delayed-tray-test.exe'
}
$settings=Join-Path $env:USERPROFILE '.axe-fx-usb78\settings.ini'
$output=Join-Path $root 'artifacts'
New-Item -ItemType Directory -Force -Path $output | Out-Null
if(Get-Process -Name axe-usb78-panel,axe-usb78-panel-delayed-tray-test,axe-usb78 -ErrorAction SilentlyContinue) {throw 'Stop the panel and bridge before the hardware panel integration test.'}
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class AxePanelTest {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w,int id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr w,StringBuilder text,int count);
 [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr w,uint msg,UIntPtr wp,IntPtr lp,uint flags,uint timeout,out UIntPtr result);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w,out Rect r);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct IconId {public uint size;public IntPtr hwnd;public uint id;public Guid guid;}
 [DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref IconId id,out Rect rect);
 public static bool HasTrayIcon(IntPtr window) {
  var id=new IconId {size=(uint)Marshal.SizeOf(typeof(IconId)),hwnd=window,id=1};Rect rect;
  return Shell_NotifyIconGetRect(ref id,out rect)==0;
 }
 public static void Message(IntPtr w,uint msg,uint wp=0,int lp=0) {
  UIntPtr result;
  if(SendMessageTimeout(w,msg,new UIntPtr(wp),new IntPtr(lp),2,2000,out result)==IntPtr.Zero)
   throw new Exception("Panel message timed out: "+msg);
 }
 public static string Text(IntPtr w) {var s=new StringBuilder(4096);GetWindowText(w,s,s.Capacity);return s.ToString();}
}
'@
$hadSettings=Test-Path -LiteralPath $settings -PathType Leaf
$saved=if($hadSettings){[IO.File]::ReadAllBytes($settings)}else{$null}
$script:panel=$null
$script:launchCount=0
$log=[Collections.Generic.List[string]]::new()
function Check([bool]$value,[string]$description) {
 if(-not $value){throw "FAIL: $description"}
 $log.Add("PASS: $description"); Write-Output "PASS: $description"
}
function Wait-Condition([scriptblock]$condition,[string]$description,[int]$seconds=12) {
 $deadline=[DateTime]::UtcNow.AddSeconds($seconds)
 do {if(& $condition){return};Start-Sleep -Milliseconds 150}while([DateTime]::UtcNow -lt $deadline)
 throw "Timed out: $description"
}
function Start-Panel {
 $launchScheduled=$ScheduledStartup -or ($CrossLaunch -and ($script:launchCount % 2 -eq 1))
 $script:launchCount++
 if($launchScheduled) {
  Wait-Condition {(Get-ScheduledTask -TaskName 'Axe-Fx III USB 7-8 FAS Bridge').State -ne 'Running'} 'startup task ready'
  Start-ScheduledTask -TaskName 'Axe-Fx III USB 7-8 FAS Bridge'
  Wait-Condition {@(Get-Process -Name axe-usb78-panel -ErrorAction SilentlyContinue).Count -eq 1} 'scheduled panel launch'
  $script:panel=Get-Process -Name axe-usb78-panel
 } else {
  $script:panel=Start-Process -FilePath $exe -ArgumentList '--minimized' -PassThru
 }
 Wait-Condition {
  $w=[AxePanelTest]::FindWindow('AxeFxUsb78TrayPanelWindow','Axe-Fx III USB 7/8 Audio')
  if($w -eq [IntPtr]::Zero){return $false}
  [uint32]$owner=0;[void][AxePanelTest]::GetWindowThreadProcessId($w,[ref]$owner)
  $candidate=Get-Process -Id $owner -ErrorAction SilentlyContinue
  if(-not $candidate -or $candidate.Path -ine $exe){return $false}
  $script:panel=$candidate
  return $true
 } 'panel window'
 $script:window=[AxePanelTest]::FindWindow('AxeFxUsb78TrayPanelWindow','Axe-Fx III USB 7/8 Audio')
 Check (-not [AxePanelTest]::IsWindowVisible($script:window)) 'startup is hidden in the tray'
 if($DelayedTray) {
  Check (-not [AxePanelTest]::HasTrayIcon($script:window)) 'tray is initially unavailable in regression test'
  Wait-Condition {
   if([AxePanelTest]::IsWindowVisible($script:window)){throw 'Panel opened while waiting for Explorer tray.'}
   return [AxePanelTest]::HasTrayIcon($script:window)
  } 'tray icon appears after Explorer becomes ready'
  Check ($true) 'panel stays hidden while tray creation fails and then recovers'
 } else {
  Wait-Condition {[AxePanelTest]::HasTrayIcon($script:window)} 'tray icon exists'
 }
}
function Children { @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($script:panel.Id) AND Name='axe-usb78.exe'") }
function Stop-Panel {
 [AxePanelTest]::Message($script:window,0x111,1005)
 Wait-Condition {$script:panel.HasExited} 'graceful panel exit'
 Check (-not (Get-Process -Name axe-usb78 -ErrorAction SilentlyContinue)) 'exit leaves no routing child'
}
try {
 # Start with a deliberate disabled preference; hardware is opened only after toggling on.
 New-Item -ItemType Directory -Force -Path (Split-Path -Parent $settings) | Out-Null
 [IO.File]::WriteAllText($settings,"AxeFxUsb78PanelSettings`nversion=1`nenabled=false`n",[Text.UTF8Encoding]::new($false))
 Start-Panel
 Start-Sleep -Milliseconds 600
 Check (@(Children).Count -eq 0) 'saved disabled state starts without an ASIO child'
 [AxePanelTest]::Message([AxePanelTest]::GetDlgItem($script:window,2001),0xF5)
 Wait-Condition {@(Children).Count -eq 1} 'enable starts one bridge'
 Wait-Condition {[AxePanelTest]::Text([AxePanelTest]::GetDlgItem($script:window,2004)) -match 'Routing is active'} 'real FAS routing becomes active' 20
 Check ([IO.File]::ReadAllText($settings) -match 'enabled=true') 'enable preference is saved'
 [AxePanelTest]::Message($script:window,0x11) # canceled shutdown: query must not stop anything
 [AxePanelTest]::Message($script:window,0x16,0)
 Start-Sleep -Milliseconds 600
 Check (@(Children).Count -eq 1) 'canceled shutdown keeps routing active'
 $second=Start-Process -FilePath $exe -ArgumentList '--minimized' -PassThru
 Wait-Condition {$second.HasExited} 'second launcher exits'
 Check (-not [AxePanelTest]::IsWindowVisible($script:window)) 'duplicate minimized launch keeps existing panel hidden'
 $second=Start-Process -FilePath $exe -PassThru
 Wait-Condition {$second.HasExited} 'manual launcher exits'
 Wait-Condition {[AxePanelTest]::IsWindowVisible($script:window)} 'second launcher opens existing control panel'
 Check (@(Children).Count -eq 1) 'single instance owns exactly one bridge'
 Start-Sleep -Seconds 6 # Include live streaming statistics in the rendered panel.
 $rect=[AxePanelTest+Rect]::new()
 [void][AxePanelTest]::GetWindowRect($script:window,[ref]$rect)
 $bitmap=[Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
 $graphics=[Drawing.Graphics]::FromImage($bitmap)
 $dc=$graphics.GetHdc()
 try {Check ([AxePanelTest]::PrintWindow($script:window,$dc,0)) 'control panel renders to an image'} finally {$graphics.ReleaseHdc($dc)}
 $bitmap.Save((Join-Path $output 'control-panel.png'),[Drawing.Imaging.ImageFormat]::Png)
 $graphics.Dispose();$bitmap.Dispose()
 [AxePanelTest]::Message($script:window,0x10)
 Check (-not [AxePanelTest]::IsWindowVisible($script:window)) 'closing the panel hides it'
 Check (@(Children).Count -eq 1) 'closing the window keeps routing running'
 [AxePanelTest]::Message($script:window,[AxePanelTest]::RegisterWindowMessage('TaskbarCreated'))
 Wait-Condition {[AxePanelTest]::HasTrayIcon($script:window)} 'tray icon survives repeated TaskbarCreated notification'
 Check (-not [AxePanelTest]::IsWindowVisible($script:window)) 'tray recovery keeps the panel hidden'
 [AxePanelTest]::Message([AxePanelTest]::GetDlgItem($script:window,2001),0xF5)
 Wait-Condition {@(Children).Count -eq 0} 'disable stops the bridge'
 Check ([IO.File]::ReadAllText($settings) -match 'enabled=false') 'disable preference is saved'
 Stop-Panel
 Start-Panel
 Start-Sleep -Milliseconds 600
 Check (@(Children).Count -eq 0) 'disabled preference persists across application restart'
 # Confirmed end-session with no child must also terminate the panel.
 [AxePanelTest]::Message($script:window,0x16,1)
 Wait-Condition {$script:panel.HasExited} 'confirmed end-session exits with routing disabled'
 Start-Panel
 [AxePanelTest]::Message([AxePanelTest]::GetDlgItem($script:window,2001),0xF5)
 Wait-Condition {@(Children).Count -eq 1} 'enable before forced-exit check'
 $script:panel.Kill()
 Wait-Condition {-not (Get-Process -Name axe-usb78 -ErrorAction SilentlyContinue)} 'job kills child after panel crash'
 Check ($true) 'forced panel exit cannot orphan its bridge'
 Start-Panel
 Wait-Condition {@(Children).Count -eq 1} 'saved enabled preference starts bridge after panel restart'
 Check ($true) 'enabled preference persists across application restart'
 Stop-Panel
} catch {
 $log.Add("FAIL: $($_.Exception.Message)")
 if($script:panel -and -not $script:panel.HasExited) {
  $log.Add([AxePanelTest]::Text([AxePanelTest]::GetDlgItem($script:window,2004)))
  $log.Add([AxePanelTest]::Text([AxePanelTest]::GetDlgItem($script:window,2005)))
 }
 throw
} finally {
 if($script:panel -and -not $script:panel.HasExited){$script:panel.Kill();$script:panel.WaitForExit(5000)|Out-Null}
 if($hadSettings){[IO.File]::WriteAllBytes($settings,$saved)}elseif(Test-Path -LiteralPath $settings){Remove-Item -LiteralPath $settings}
 $log | Set-Content -LiteralPath (Join-Path $output 'panel-test.txt') -Encoding UTF8
}
