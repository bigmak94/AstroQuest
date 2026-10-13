# Starts ASTRO BOT Rescue Mission in the emulator for a headset connected to this PC (Virtual
# Desktop, or anything else with an OpenXR runtime), and tells what is going on while it runs.
# Started by "Play Astro Bot VR.bat"; settings are in settings.txt next to this file, and the
# main ones can be chosen in a small window before the game starts.
param([string]$SettingsFile = "", [switch]$NoMenu)

# The 64-bit PowerShell of this PC, for a 32-bit one to hand over to; "" where this is it.
# Started from a 32-bit program (a file manager, a game launcher), "powershell" is the 32-bit
# one, and Windows shows that other system folders and another registry than the emulator
# gets, which is a 64-bit program: the Visual C++ runtime looked missing however often it was
# installed (one of its files only exists in 64 bits), and so would the OpenXR runtime.
function Get-NativePowerShell {
    if (-not [Environment]::Is64BitOperatingSystem -or [Environment]::Is64BitProcess) { return "" }
    $native = Join-Path $env:windir "Sysnative\WindowsPowerShell\v1.0\powershell.exe"
    if ([System.IO.File]::Exists($native)) { return $native }
    return ""
}
$nativePowerShell = Get-NativePowerShell
if ($nativePowerShell -ne "") {
    $again = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $MyInvocation.MyCommand.Path)
    if ($SettingsFile -ne "") { $again += @("-SettingsFile", $SettingsFile) }
    if ($NoMenu) { $again += "-NoMenu" }
    & $nativePowerShell @again
    exit $LASTEXITCODE
}

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
Set-Location $here
if ($SettingsFile -eq "") { $SettingsFile = Join-Path $here "settings.txt" }

function Say([string]$text, [string]$color = "Gray") { Write-Host $text -ForegroundColor $color }

# --- settings -------------------------------------------------------------------------------
function Read-Settings {
    $script:settings = [ordered]@{}
    $script:extraEnv = @()
    if (Test-Path $SettingsFile) {
        foreach ($line in Get-Content $SettingsFile) {
            $line = $line.Trim()
            if ($line -eq "" -or $line.StartsWith("#")) { continue }
            $at = $line.IndexOf("=")
            if ($at -lt 1) { continue }
            $key = $line.Substring(0, $at).Trim().ToLower()
            $value = $line.Substring($at + 1).Trim()
            if ($key -eq "env") { $script:extraEnv += $value } else { $script:settings[$key] = $value }
        }
    }
}
function Setting([string]$key, [string]$default = "") {
    if ($settings.Contains($key)) { return $settings[$key] }
    return $default
}
# Writes key=value into the settings file: in place of the line that sets it, or at the end.
function Save-Setting([string]$key, [string]$value) {
    $lines = @()
    if (Test-Path $SettingsFile) { $lines = @(Get-Content $SettingsFile) }
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ("^\s*" + [regex]::Escape($key) + "\s*=")) {
            $lines[$i] = "$key=$value"
            $done = $true
        }
    }
    if (-not $done) { $lines += "$key=$value" }
    Set-Content -Path $SettingsFile -Value $lines -Encoding UTF8
}
Read-Settings

# The sizes an eye can be drawn at: the console's smaller ones (for a headset with fewer pixels,
# a Rift S or a Rift), its largest (1440x1536, what a PlayStation 4 Pro draws) and larger ones
# of that shape.
$widths = @(816, 960, 1200, 1440, 1800, 2160, 2520, 2880, 3240, 3600)
$consoleHeights = @{ 816 = 870; 960 = 1080; 1200 = 1280 }
function EyeHeight([int]$width) {
    if ($consoleHeights.ContainsKey($width)) { return $consoleHeights[$width] }
    return [int]([math]::Round(1536.0 * $width / 1440 / 8) * 8)
}
$caps = @(120, 90, 80, 72, 60, 45, 40, 36, 30)
# The languages the game has, as Windows names them.
$gameLanguages = @("en-US", "en-GB", "fr-FR", "fr-CA", "es-ES", "es-419", "de-DE", "it-IT", "nl-NL",
                   "pt-PT", "pt-BR", "ru-RU", "pl-PL", "tr-TR", "sv-SE", "nb-NO", "da-DK", "fi-FI",
                   "cs-CZ", "hu-HU", "el-GR", "ro-RO", "ar-SA", "ja-JP", "ko-KR", "zh-Hant", "zh-Hans",
                   "th-TH")
# language: the language the game is played in. windows (the default): the one Windows is shown
# in; otherwise one of the above. The emulator sets its console to it, and the game takes the
# console's language as it does on a PlayStation (English where it does not have it).
function Get-GameLanguage {
    $wanted = Setting "language" "windows"
    if ($wanted -eq "" -or $wanted -eq "windows") {
        return [System.Globalization.CultureInfo]::CurrentUICulture.Name
    }
    return $wanted
}
# A language's name in that language, and in English where that differs.
function Get-LanguageName([string]$tag) {
    try {
        $culture = [System.Globalization.CultureInfo]::GetCultureInfo($tag)
        if ($culture.NativeName -eq $culture.EnglishName) { return $culture.EnglishName }
        return ($culture.NativeName + " - " + $culture.EnglishName)
    } catch { return $tag }
}

function Get-VrInstructions([string]$runtime) {
    # How to move the gamepad in the game while nothing tracks where it is.
    $placeHelp = "Hold the PS button and press the D-pad to move it (L1 nearer, R1 farther); PS + triangle switches between your place for it and the standard one."
    # For players who sit where they cannot turn round.
    $turnHelp = "To turn round without turning yourself: hold L1 (the headset's controllers: the left grip) and flick the right stick to a side."
    if ($runtime -match 'steamvr|steamxr') {
        return @(
            "Start SteamVR and check that the headset is ready (an Index: with its base stations). Virtual Desktop is not needed."
            "Set the headset to 120 Hz in SteamVR's Video settings for the game's own 60 frames a second (90 Hz gives 45)."
            "The DualSense: connect it to THIS PC by USB or Bluetooth. It keeps its motion sensors, touchpad and rumble."
            "If launching through a Steam shortcut, disable Steam Input for that shortcut so the emulator can read the DualSense."
            "SteamVR does not track bare hands: the gamepad in the game stays in front of you and turns with its own sensors."
            $placeHelp
            "Sound and microphone: the ones chosen in SteamVR's Audio settings; the game uses the microphone for blowing."
            "The headset's controllers play too, with no gamepad or whenever they were used after it (right A jump, right B punch, left X or A back, left Y or B triangle, left menu or trackpad press = OPTIONS)."
            $turnHelp
        )
    }
    if ($runtime -match 'virtualdesktop') {
        return @(
            "In the headset: connect Virtual Desktop to this PC. The game moves into the headset by itself."
            "The DualSense: connect it to THIS PC (USB cable, or Bluetooth paired with the PC). Paired with"
            "the headset, it reaches the PC through Virtual Desktop without motion sensors or touchpad."
            "Where it is in the game comes from your hands: hand tracking on in the headset, and in"
            "Virtual Desktop's settings hand tracking forwarded to the PC. Without that it stays in front of you:"
            $placeHelp
            "The Touch controllers play too, with no gamepad or whenever they were used after it (A jump, B punch, X back, Y triangle, left menu = OPTIONS)."
            $turnHelp
        )
    }
    return @(
        "Start your headset's OpenXR runtime and check that the headset is ready."
        "The DualSense: connect it to THIS PC by USB or Bluetooth, with Steam Input disabled for any Steam shortcut."
        "Without hand tracking, the gamepad in the game stays in front of you and turns with its own sensors."
        $placeHelp
        "The headset's controllers play too, with no gamepad or whenever they were used after it (right A jump, right B punch, left X or A back, left Y or B triangle)."
        $turnHelp
    )
}

function Get-DesktopView {
    if ((Setting "desktop_view" "stereo") -eq "combined") { return "combined" }
    if ((Setting "desktop_view" "stereo") -eq "spectator") { return "spectator" }
    return "stereo"
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# A message box, on top of whatever else is on the desktop (which is what the headset shows).
# Returns the name of the button chosen: OK, Yes, No, Cancel.
function Show-Box([string]$text, [string]$buttons = "OK", [string]$icon = "Information",
                  [string]$default = "Button1") {
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        return [System.Windows.Forms.MessageBox]::Show($owner, $text, "Astro Bot VR", $buttons,
                                                       $icon, $default).ToString()
    } finally { $owner.Dispose() }
}

# Shows one of the launcher's windows and waits for it. In its normal size and in front, also
# when the launcher itself was started minimized (Windows would open its first window the same).
function Show-Form($form) {
    $form.Add_Shown({ $this.WindowState = "Normal"; $this.Activate() })
    return $form.ShowDialog()
}

# --- the game ---------------------------------------------------------------------------------
# The game is looked for in the games folder next to this one, as deep as three folders down.
# An unpacked game is a folder with eboot.bin in it; a package (.pkg) is unpacked first, once.
# game= in the settings names one that is elsewhere (its eboot.bin, its folder or its package),
# and when none is found a window asks where it is.
$gamesFolder = Join-Path $root "games"
$madeFor = "CUSA12392"
# The longest path of a file inside that game: the emulator cannot open a file whose whole path
# is longer than 259 characters.
$longestInside = 126
$zeroPasscode = "0" * 32
# Where a package is unpacked before its game is moved into place: a folder in the games folder
# with a name so short that no file has a longer path there than in the game's own folder
# (games\.u\uroot\... against games\CUSA12392\...). Inside the game's folder, as before, paths
# were 24 characters longer, and PkgTool stopped partway through a game whose folder passed the
# check. Neither name is looked into for a game.
$unpackFolders = @(".u", ".unpacking")

function Gigabytes([double]$bytes) { return ("{0:N1} GB" -f ($bytes / 1073741824.0)) }

# The folders under one, itself first and the nearest first, down to so many levels. What is
# inside an unpacked game, or one being unpacked, is left out.
function Get-Folders([string]$top, [int]$depth = 3) {
    $all = New-Object System.Collections.Generic.List[string]
    if (-not [System.IO.Directory]::Exists($top)) { return $all }
    $level = @($top)
    for ($i = 0; $i -le $depth -and $level.Count -gt 0; $i++) {
        $next = @()
        foreach ($folder in $level) {
            $all.Add($folder)
            if ([System.IO.File]::Exists([System.IO.Path]::Combine($folder, "eboot.bin"))) { continue }
            try {
                foreach ($sub in [System.IO.Directory]::GetDirectories($folder)) {
                    if ($unpackFolders -notcontains [System.IO.Path]::GetFileName($sub)) { $next += $sub }
                }
            } catch {}
        }
        $level = $next
    }
    return $all
}

# What a param.sfo says, by name (TITLE_ID, APP_VER, TITLE...): the texts only.
function Read-Sfo([string]$path) {
    $values = @{}
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        if ($bytes.Length -lt 20 -or [System.BitConverter]::ToUInt32($bytes, 0) -ne 0x46535000) {
            return $values
        }
        $keys = [System.BitConverter]::ToInt32($bytes, 8)
        $data = [System.BitConverter]::ToInt32($bytes, 12)
        $count = [System.BitConverter]::ToInt32($bytes, 16)
        for ($i = 0; $i -lt $count; $i++) {
            $at = 20 + 16 * $i
            $keyAt = $keys + [System.BitConverter]::ToUInt16($bytes, $at)
            $format = [System.BitConverter]::ToUInt16($bytes, $at + 2)
            $length = [System.BitConverter]::ToInt32($bytes, $at + 4)
            $valueAt = $data + [System.BitConverter]::ToInt32($bytes, $at + 12)
            $end = [Array]::IndexOf($bytes, [byte]0, $keyAt)
            $key = [System.Text.Encoding]::ASCII.GetString($bytes, $keyAt, $end - $keyAt)
            if ($format -eq 0x0204 -and $length -gt 0) {
                $values[$key] = [System.Text.Encoding]::UTF8.GetString($bytes, $valueAt, $length - 1)
            }
        }
    } catch {}
    return $values
}
function Get-GameInfo([string]$eboot) {
    $folder = [System.IO.Path]::GetDirectoryName($eboot)
    return Read-Sfo ([System.IO.Path]::Combine($folder, "sce_sys", "param.sfo"))
}

# The unpacked game under a folder: the one this is made for, if there are several. (A folder
# named after a game with -UPDATE, -patch or -mods at the end is not a game: the emulator lays
# what is in it over the game's own files.)
function Find-Game([string]$top) {
    $first = $null
    foreach ($folder in (Get-Folders $top)) {
        if ($folder -match '-(UPDATE|patch|mods)$') { continue }
        $eboot = [System.IO.Path]::Combine($folder, "eboot.bin")
        if (-not [System.IO.File]::Exists($eboot)) { continue }
        if ((Get-GameInfo $eboot)["TITLE_ID"] -eq $madeFor) { return $eboot }
        if ($null -eq $first) { $first = $eboot }
    }
    return $first
}

# Whether a package is an update of a game (a patch), which holds the files the update changed
# and no more, rather than the game: its header says so.
function Test-UpdatePackage([string]$path) {
    try {
        $head = New-Object byte[] 128
        $stream = [System.IO.File]::OpenRead($path)
        try { $read = $stream.Read($head, 0, $head.Length) } finally { $stream.Dispose() }
        if ($read -lt $head.Length) { return $false }
        if ($head[0] -ne 0x7F -or $head[1] -ne 0x43 -or $head[2] -ne 0x4E -or $head[3] -ne 0x54) {
            return $false
        }
        # (First patch, later patch, cumulative patch: 0x00100000, 0x40000000, 0x20000000.)
        return (($head[0x78] -band 0x60) -ne 0) -or (($head[0x79] -band 0x30) -ne 0)
    } catch { return $false }
}

# The package under a folder: a game's own before any update of it, and the largest if there
# are several.
function Find-Package([string]$top) {
    $largest = $null
    $largestIsUpdate = $true
    foreach ($folder in (Get-Folders $top)) {
        try { $files = [System.IO.Directory]::GetFiles($folder, "*.pkg") } catch { continue }
        foreach ($file in $files) {
            $info = New-Object System.IO.FileInfo($file)
            $isUpdate = Test-UpdatePackage $file
            if ($null -eq $largest -or ($largestIsUpdate -and -not $isUpdate) -or
                ($largestIsUpdate -eq $isUpdate -and $info.Length -gt $largest.Length)) {
                $largest = $info
                $largestIsUpdate = $isUpdate
            }
        }
    }
    return $largest
}

# What a package calls its content ("EP9000-CUSA12392_00-..."), or nothing if the file is not
# a PlayStation 4 package.
function Read-PackageId([string]$path) {
    try {
        $head = New-Object byte[] 128
        $stream = [System.IO.File]::OpenRead($path)
        try { $read = $stream.Read($head, 0, $head.Length) } finally { $stream.Dispose() }
        if ($read -lt $head.Length) { return $null }
        if ($head[0] -ne 0x7F -or $head[1] -ne 0x43 -or $head[2] -ne 0x4E -or $head[3] -ne 0x54) {
            return $null
        }
        return [System.Text.Encoding]::ASCII.GetString($head, 0x40, 36).Trim([char]0)
    } catch { return $null }
}

# The window shown while a package is unpacked. True when the unpacking ran to its end, false
# when it was cancelled (and stopped).
function Show-Unpacking($process, [string]$drive, [double]$freeBefore) {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "Astro Bot VR"
    $form.ClientSize = New-Object System.Drawing.Size(460, 132)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.ControlBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Unpacking the game. This is done once and takes a minute or a few."
    $label.SetBounds(16, 14, 428, 22)
    $form.Controls.Add($label)
    $bar = New-Object System.Windows.Forms.ProgressBar
    $bar.Style = "Marquee"
    $bar.MarqueeAnimationSpeed = 30
    $bar.SetBounds(16, 44, 428, 18)
    $form.Controls.Add($bar)
    $state = New-Object System.Windows.Forms.Label
    $state.Name = "state"
    $state.SetBounds(16, 72, 300, 22)
    $form.Controls.Add($state)
    $cancel = New-Object System.Windows.Forms.Button
    $cancel.Name = "cancel"
    $cancel.Text = "Cancel"
    $cancel.SetBounds(356, 92, 88, 30)
    $form.Controls.Add($cancel)

    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 500
    $timer.Add_Tick({
        if ($process.HasExited) {
            $timer.Stop()
            $form.DialogResult = [System.Windows.Forms.DialogResult]::OK
            $form.Close()
            return
        }
        if ($freeBefore -ge 0) {
            try {
                $free = (New-Object System.IO.DriveInfo($drive)).AvailableFreeSpace
                $state.Text = (Gigabytes ([math]::Max(0, $freeBefore - $free))) + " unpacked"
            } catch {}
        }
    })
    $cancel.Add_Click({
        $timer.Stop()
        try { $process.Kill() } catch {}
        try { $process.WaitForExit(10000) | Out-Null } catch {}
        $form.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
        $form.Close()
    })
    $form.Add_Shown({ $timer.Start() })
    $result = Show-Form $form
    $timer.Dispose()
    $form.Dispose()
    return ($result -eq [System.Windows.Forms.DialogResult]::OK)
}

# Unpacks a package into the games folder, under the game's serial, and returns the eboot.bin
# there; nothing if it was not done. PkgTool (of LibOrbisPkg) does the work. It reads packages
# that are not encrypted, which is what a dump of a game is; what the PlayStation Store hands
# out is encrypted and cannot be unpacked by anything here.
function Expand-Package($package) {
    $contentId = Read-PackageId $package.FullName
    if ($null -eq $contentId) {
        [void](Show-Box ($package.FullName + "`n`nis not a PlayStation 4 package.") "OK" "Warning")
        return $null
    }
    if (Test-UpdatePackage $package.FullName) {
        [void](Show-Box ("This package is an update of the game, not the game:`n`n" + $package.Name + "`n`nAn update holds only the files it changed. Put the package of the game itself (about 7 GB) in the games folder; the update is not needed, and is left alone when it is there as well. (A copy of the game that already has its update 1.04 in it plays too.)") "OK" "Warning")
        return $null
    }
    $tool = $null
    foreach ($candidate in @((Join-Path $here "pkgtool\PkgTool.exe"),
                             (Join-Path $root "tools\pkgtool\PkgTool.exe"))) {
        if ([System.IO.File]::Exists($candidate)) { $tool = $candidate; break }
    }
    if ($null -eq $tool) {
        [void](Show-Box ("The game is here as a package:`n" + $package.FullName + "`n`nbut PkgTool, which unpacks packages, is missing from`n" + (Join-Path $here "pkgtool") + "`n`nUnzip the whole AstroQuest package again.") "OK" "Warning")
        return $null
    }
    $serial = "game"
    if ($contentId -match "[A-Z]{4}[0-9]{5}") { $serial = $Matches[0] }
    $target = Join-Path $gamesFolder $serial
    $work = Join-Path $gamesFolder $unpackFolders[0]
    if ([math]::Max($target.Length, (Join-Path $work "uroot").Length) + 1 + $longestInside -gt 259) {
        [void](Show-Box ("The game cannot be unpacked into`n" + $target + "`n`nThat path is too long: some of the game's files would have a path of more than 259 characters, which the emulator cannot open. Move the AstroQuest folder somewhere with a shorter path, for example C:\Games\AstroQuest, and start again.") "OK" "Warning")
        return $null
    }
    # What unpacking takes, by this game's own measure (12.5 GB out of a package of 6.9).
    $needed = $package.Length * 1.85
    $drive = [System.IO.Path]::GetPathRoot($target)
    $free = -1
    try { $free = (New-Object System.IO.DriveInfo($drive)).AvailableFreeSpace } catch {}
    if ($free -ge 0 -and $free -lt $needed) {
        [void](Show-Box ("Unpacking the game takes about " + (Gigabytes $needed) + ", and drive " + $drive + " has " + (Gigabytes $free) + " free.`n`nMake room, or move the AstroQuest folder to a drive that has it.") "OK" "Warning")
        return $null
    }
    $answer = Show-Box ("The game is here as a package:`n`n" + $package.Name + "   (" + (Gigabytes $package.Length) + ")`n`nIt has to be unpacked before it can be played. That is done once, takes a minute or a few, and about " + (Gigabytes $needed) + " in`n" + $target + "`n`nUnpack it now?") "YesNo" "Question"
    if ($answer -ne "Yes") { return $null }

    # Unpacked into a folder of its own first: what is left of an unpacking that did not finish
    # is never taken for the game. (What PkgTool says goes to the temporary folder, so that
    # nothing but the game is in there.)
    try {
        if ([System.IO.Directory]::Exists($work)) { [System.IO.Directory]::Delete($work, $true) }
        [void][System.IO.Directory]::CreateDirectory($work)
    } catch {
        [void](Show-Box ("Could not write to`n" + $gamesFolder + "`n`n" + $_.Exception.Message) "OK" "Warning")
        return $null
    }
    $errors = Join-Path ([System.IO.Path]::GetTempPath()) "AstroQuest-pkgtool-errors.txt"
    $arguments = "pkg_extract --passcode " + $zeroPasscode + " `"" + $package.FullName + "`" `"" + $work + "`""
    Say ("Unpacking " + $package.FullName)
    $process = Start-Process -FilePath $tool -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path ([System.IO.Path]::GetTempPath()) "AstroQuest-pkgtool-output.txt") `
        -RedirectStandardError $errors
    # (Without this the exit code is not to be had later.)
    $null = $process.Handle
    $finished = Show-Unpacking $process $drive $free

    $unpacked = Join-Path $work "uroot"
    if (-not [System.IO.Directory]::Exists($unpacked)) { $unpacked = $work }
    $failure = $null
    if (-not $finished) {
        $failure = "cancelled"
    } elseif ($process.ExitCode -ne 0 -or -not [System.IO.File]::Exists((Join-Path $unpacked "eboot.bin"))) {
        $why = ""
        try { $why = (@(Get-Content -LiteralPath $errors -ErrorAction Stop | Where-Object { $_.Trim() -ne "" })[0]) } catch {}
        if ($why -match "PathTooLong") {
            $failure = "The package could not be unpacked into`n" + $target + "`n`nThat path is too long: some of the game's files would have a path of more than 259 characters. Move the AstroQuest folder somewhere with a shorter path, for example C:\Games\AstroQuest, and start again."
        } elseif ($why -match "IOException|UnauthorizedAccess") {
            $failure = "The package could not be unpacked: writing to`n" + $gamesFolder + "`nfailed. Is the drive full, or does a security program keep the folder from being written to?`n`n" + $package.FullName
        } else {
            $failure = "The package could not be unpacked.`n`nOnly a package made from a dump of the game can be: it is not encrypted. A package from the PlayStation Store is, and cannot be used.`n`n" + $package.FullName
        }
        if ($why) { $failure += "`n`n(PkgTool: " + $why.Trim() + ")" }
    } else {
        try {
            # Into place: over anything of the same name that an earlier attempt left there
            # (with what an unpacking of an earlier version left inside the game's folder).
            [void][System.IO.Directory]::CreateDirectory($target)
            $leftBefore = Join-Path $target $unpackFolders[1]
            if ([System.IO.Directory]::Exists($leftBefore)) { [System.IO.Directory]::Delete($leftBefore, $true) }
            foreach ($item in [System.IO.Directory]::GetFileSystemEntries($unpacked)) {
                $to = Join-Path $target ([System.IO.Path]::GetFileName($item))
                if ([System.IO.Directory]::Exists($to)) { [System.IO.Directory]::Delete($to, $true) }
                elseif ([System.IO.File]::Exists($to)) { [System.IO.File]::Delete($to) }
                [System.IO.Directory]::Move($item, $to)
            }
            # What the package keeps outside its file system: the game's own description
            # (param.sfo, which tells the emulator what game this is), pictures, trophies.
            # Named ICON0_PNG, PLAYGO_CHUNK_DAT, TROPHY__TROPHY00_TRP... there; icon0.png,
            # playgo-chunk.dat, trophy/trophy00.trp in the game's sce_sys folder.
            $leftOut = @("DIGESTS", "ENTRY_KEYS", "IMAGE_KEY", "GENERAL_DIGESTS", "METAS",
                         "ENTRY_NAMES", "LICENSE_DAT", "LICENSE_INFO")
            foreach ($line in (& $tool pkg_listentries $package.FullName 2>$null)) {
                if ($line -notmatch '^0x[0-9A-Fa-f]+\s+0x[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\d+)\s+(?:\d+\s+)?([A-Z0-9_]+)\s*$') { continue }
                $index = $Matches[1]
                $name = $Matches[2]
                if ($leftOut -contains $name -or $name.EndsWith("_DDS")) { continue }
                $file = $name.ToLower().Replace("__", "\")
                $at = $file.LastIndexOf("_")
                if ($at -gt 0) { $file = $file.Substring(0, $at) + "." + $file.Substring($at + 1) }
                if ($file.StartsWith("playgo_")) { $file = "playgo-" + $file.Substring(7) }
                $file = Join-Path (Join-Path $target "sce_sys") $file
                [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($file))
                & $tool pkg_extractentry --passcode $zeroPasscode $package.FullName $index $file 2>$null | Out-Null
            }
            if (-not [System.IO.File]::Exists((Join-Path $target "sce_sys\param.sfo"))) {
                $failure = "The package was unpacked, but its description of the game (param.sfo) could not be read from it.`n`n" + $package.FullName
            }
        } catch {
            $failure = "The package was unpacked, but the game could not be put in`n" + $target + "`n`n" + $_.Exception.Message
        }
    }
    try { [System.IO.Directory]::Delete($work, $true) } catch {}
    if ($failure) {
        if ($failure -ne "cancelled") { [void](Show-Box $failure "OK" "Warning") }
        return $null
    }
    Say ("Unpacked into " + $target)
    $answer = Show-Box ("The game is unpacked and ready.`n`nThe package is not needed any more:`n" + $package.FullName + "`n`nDelete it, to get " + (Gigabytes $package.Length) + " back? (Keep it if it is your only copy of the game.)") "YesNo" "Question" "Button2"
    if ($answer -eq "Yes") {
        try { [System.IO.File]::Delete($package.FullName) } catch {
            [void](Show-Box ("Could not delete the package:`n" + $_.Exception.Message) "OK" "Warning")
        }
    }
    return (Join-Path $target "eboot.bin")
}

# What a path named in the settings, or picked in the window, gives: the eboot.bin to run, or
# nothing.
function Use-Path([string]$path) {
    if ([System.IO.Directory]::Exists($path)) {
        $eboot = Find-Game $path
        if ($eboot) { return $eboot }
        $package = Find-Package $path
        if ($package) { return Expand-Package $package }
        return $null
    }
    if (-not [System.IO.File]::Exists($path)) { return $null }
    if ([System.IO.Path]::GetExtension($path) -ieq ".pkg") {
        return Expand-Package (New-Object System.IO.FileInfo($path))
    }
    return $path
}

# The window for when the game is nowhere to be found. Returns pick (show where it is), again
# (look again) or quit.
function Show-NotFound {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "Astro Bot VR"
    $form.ClientSize = New-Object System.Drawing.Size(600, 250)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $title = New-Object System.Windows.Forms.Label
    $title.Text = "Where is the game?"
    $title.Font = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $title.SetBounds(16, 14, 568, 26)
    $form.Controls.Add($title)
    $text = New-Object System.Windows.Forms.Label
    $text.Text = "ASTRO BOT Rescue Mission was not found. AstroQuest does not contain the game: it plays your own copy of it.`n`nPut that copy in the games folder - either the game's folder (the one with eboot.bin in it) or its .pkg file - and choose Look again. Or leave it where it is and show where that is."
    $text.SetBounds(16, 50, 568, 96)
    $form.Controls.Add($text)
    $where = New-Object System.Windows.Forms.Label
    $where.Text = "The games folder: " + $gamesFolder
    $where.ForeColor = [System.Drawing.SystemColors]::GrayText
    $where.SetBounds(16, 150, 568, 40)
    $form.Controls.Add($where)

    $pick = New-Object System.Windows.Forms.Button
    $pick.Name = "pick"
    $pick.Text = "Show where it is..."
    $pick.SetBounds(16, 204, 150, 30)
    $pick.DialogResult = [System.Windows.Forms.DialogResult]::Yes
    $form.Controls.Add($pick)
    $open = New-Object System.Windows.Forms.Button
    $open.Name = "open"
    $open.Text = "Open the games folder"
    $open.SetBounds(174, 204, 170, 30)
    $open.Add_Click({
        try {
            [void][System.IO.Directory]::CreateDirectory($gamesFolder)
            Start-Process explorer.exe -ArgumentList ("`"" + $gamesFolder + "`"")
        } catch {}
    })
    $form.Controls.Add($open)
    $again = New-Object System.Windows.Forms.Button
    $again.Name = "again"
    $again.Text = "Look again"
    $again.SetBounds(392, 204, 96, 30)
    $again.DialogResult = [System.Windows.Forms.DialogResult]::Retry
    $form.Controls.Add($again)
    $quit = New-Object System.Windows.Forms.Button
    $quit.Name = "quit"
    $quit.Text = "Quit"
    $quit.SetBounds(496, 204, 88, 30)
    $quit.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($quit)
    $form.AcceptButton = $again
    $form.CancelButton = $quit

    $result = Show-Form $form
    $form.Dispose()
    if ($result -eq [System.Windows.Forms.DialogResult]::Yes) { return "pick" }
    if ($result -eq [System.Windows.Forms.DialogResult]::Retry) { return "again" }
    return "quit"
}

# The file picker: the game's eboot.bin or its package, wherever they are.
function Select-GameFile {
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Where is ASTRO BOT Rescue Mission? Choose its eboot.bin, or its .pkg file"
    $dialog.Filter = "The game (eboot.bin, *.pkg)|eboot.bin;*.pkg|All files (*.*)|*.*"
    $dialog.CheckFileExists = $true
    if ([System.IO.Directory]::Exists($gamesFolder)) { $dialog.InitialDirectory = $gamesFolder }
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dialog.FileName
        }
        return $null
    } finally {
        $owner.Dispose()
        $dialog.Dispose()
    }
}

# The eboot.bin to run, or nothing when the player gives up.
function Resolve-Game {
    $named = Setting "game"
    if ($named -ne "") {
        $eboot = Use-Path $named
        if ($eboot) { return $eboot }
        Say ("The settings name the game at " + $named + ", where it is not: looking in " + $gamesFolder) "Yellow"
    }
    while ($true) {
        $eboot = Find-Game $gamesFolder
        if ($eboot) { return $eboot }
        $package = Find-Package $gamesFolder
        if ($package) {
            $eboot = Expand-Package $package
            if ($eboot) { return $eboot }
        }
        $choice = Show-NotFound
        if ($choice -eq "pick") {
            $file = Select-GameFile
            if ($file) {
                $eboot = Use-Path $file
                if ($eboot) {
                    # In the games folder it is found again by itself; elsewhere it is noted.
                    $inGames = $eboot.StartsWith($gamesFolder + "\", [System.StringComparison]::OrdinalIgnoreCase)
                    if (-not $inGames) { Save-Setting "game" $eboot }
                    return $eboot
                }
            }
        } elseif ($choice -ne "again") {
            return $null
        }
    }
}

# The Microsoft Visual C++ runtime, which the emulator is built against: the files of it that
# are neither in Windows' (64-bit) system folder nor next to the emulator.
function Get-MissingRuntime([string]$system = "", [string]$beside = "") {
    if ($system -eq "") {
        $system = [System.Environment]::SystemDirectory
        # (To a 32-bit PowerShell that name shows the 32-bit files.)
        if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
            $system = Join-Path $env:windir "Sysnative"
        }
    }
    $missing = @()
    foreach ($name in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                        "msvcp140_2.dll", "msvcp140_atomic_wait.dll")) {
        if ([System.IO.File]::Exists((Join-Path $system $name))) { continue }
        if ($beside -ne "" -and [System.IO.File]::Exists((Join-Path $beside $name))) { continue }
        $missing += $name
    }
    return ,$missing
}

# --- the window -------------------------------------------------------------------------------
function Show-Menu {

    $form = New-Object System.Windows.Forms.Form
    $form.Text = "Astro Bot VR"
    $form.ClientSize = New-Object System.Drawing.Size(560, 514)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $y = 14
    $title = New-Object System.Windows.Forms.Label
    $title.Text = "ASTRO BOT Rescue Mission - PC VR"
    $title.Font = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $title.SetBounds(16, $y, 520, 26)
    $form.Controls.Add($title)
    $y += 40

    # Resolution.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Resolution of each eye"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 22
    $resolution = New-Object System.Windows.Forms.TrackBar
    $resolution.Name = "resolution"
    $resolution.Minimum = 0
    $resolution.Maximum = $widths.Count - 1
    $resolution.TickFrequency = 1
    $resolution.LargeChange = 1
    $resolution.SetBounds(12, $y, 530, 40)
    $current = 0
    [void][int]::TryParse((Setting "resolution" "2880"), [ref]$current)
    $index = [array]::IndexOf($widths, $current)
    if ($index -lt 0) { $index = [array]::IndexOf($widths, 2880) }
    $resolution.Value = $index
    $form.Controls.Add($resolution)
    $y += 42
    $resolutionText = New-Object System.Windows.Forms.Label
    $resolutionText.SetBounds(16, $y, 530, 38)
    $form.Controls.Add($resolutionText)
    $update = {
        $w = $widths[$resolution.Value]
        $h = EyeHeight $w
        $times = ($w * $h) / (1440.0 * 1536.0)
        if ($w -lt 1440) {
            $resolutionText.Text = "$w x $h pixels an eye: one of the console's smaller sizes, for a headset with fewer pixels (a Rift S has 1280 x 1440 an eye). The game keeps to it."
        } else {
            $what = if ($w -eq 1440) { "the console's own, as a PlayStation 4 Pro draws it" } else { "{0:N2} times the pixels of the console" -f $times }
            $resolutionText.Text = "$w x $h pixels an eye: $what. The game draws smaller by itself when the graphics card cannot keep up."
        }
    }
    $resolution.Add_ValueChanged($update)
    & $update
    $y += 46

    # Frame rate.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Frames a second, at most"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 250, 20)
    $form.Controls.Add($label)
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Language of the game"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(284, $y, 260, 20)
    $form.Controls.Add($label)
    $y += 24
    $language = New-Object System.Windows.Forms.ComboBox
    $language.Name = "language"
    $language.DropDownStyle = "DropDownList"
    [void]$language.Items.Add("As Windows: " + (Get-LanguageName ([System.Globalization.CultureInfo]::CurrentUICulture.Name)))
    foreach ($tag in $gameLanguages) { [void]$language.Items.Add((Get-LanguageName $tag)) }
    # (A language written into the settings that is not in the list stays what it is unless
    # another is chosen here.)
    $languageBefore = [array]::IndexOf($gameLanguages, (Setting "language" "windows")) + 1
    $language.SelectedIndex = $languageBefore
    $language.SetBounds(284, $y, 260, 26)
    $form.Controls.Add($language)
    $fps = New-Object System.Windows.Forms.ComboBox
    $fps.Name = "fps"
    $fps.DropDownStyle = "DropDownList"
    foreach ($cap in $caps) {
        $text = "$cap"
        if ($cap -eq 60) { $text = "60 (the console's own)" }
        [void]$fps.Items.Add($text)
    }
    $fps.SetBounds(16, $y, 200, 26)
    $current = 0
    [void][int]::TryParse((Setting "fps" "60"), [ref]$current)
    $index = [array]::IndexOf($caps, $current)
    if ($index -lt 0) { $index = [array]::IndexOf($caps, 60) }
    $fps.SelectedIndex = $index
    $form.Controls.Add($fps)
    $y += 32
    $fpsText = New-Object System.Windows.Forms.Label
    $fpsText.Text = "A frame lasts a whole number of the headset's refreshes: at 120 Hz, 120, 60, 40 or 30 frames a second; at 90 Hz, 90, 45 or 30; at 80 Hz, 80 or 40. Choose 120 Hz for 60 frames a second. Set it in SteamVR Video settings for an Index, or Virtual Desktop Streaming settings for a Quest."
    $fpsText.SetBounds(16, $y, 530, 84)
    $form.Controls.Add($fpsText)
    $y += 88

    # Field of view.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Field of view"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 22
    $fov = New-Object System.Windows.Forms.TrackBar
    $fov.Minimum = 14
    $fov.Maximum = 20
    $fov.TickFrequency = 1
    $fov.LargeChange = 1
    $fov.SetBounds(12, $y, 300, 40)
    $fov.Value = [math]::Max(14, [math]::Min(20, [int]([int](Setting "fov" "100") / 5)))
    $form.Controls.Add($fov)
    $fovText = New-Object System.Windows.Forms.Label
    $fovText.SetBounds(316, $y + 4, 230, 40)
    $form.Controls.Add($fovText)
    $ofPsvr = (Setting "fov_of" "headset") -eq "psvr"
    $updateFov = {
        $percent = $fov.Value * 5
        if ($percent -eq 100) {
            $fovText.Text = $(if ($ofPsvr) { "100%: PlayStation VR's own" } else { "100%: all that the headset shows" })
        } else {
            $fovText.Text = "$percent% of it: sharper, with a dark border"
        }
    }
    $fov.Add_ValueChanged($updateFov)
    & $updateFov
    $y += 46

    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Desktop view"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 22
    $desktopView = New-Object System.Windows.Forms.ComboBox
    $desktopView.Name = "desktopView"
    $desktopView.DropDownStyle = "DropDownList"
    $desktopModes = @("stereo", "spectator", "combined")
    $desktopView.Items.AddRange(@("Stereo (both eyes)", "Single eye (spectator)", "Combined eyes (spectator)"))
    $desktopView.SelectedIndex = [array]::IndexOf($desktopModes, (Get-DesktopView))
    $desktopView.SetBounds(16, $y, 248, 26)
    $form.Controls.Add($desktopView)
    $desktopCrop = New-Object System.Windows.Forms.CheckBox
    $desktopCrop.Name = "desktopCrop"
    $desktopCrop.Text = "Crop top/bottom to fill"
    $desktopCrop.Checked = (Setting "desktop_crop" "0") -eq "1"
    $desktopCrop.SetBounds(284, $y, 250, 26)
    $desktopCrop.Enabled = $desktopView.SelectedIndex -ne 0
    $desktopView.Add_SelectedIndexChanged({ $desktopCrop.Enabled = $desktopView.SelectedIndex -ne 0 })
    $form.Controls.Add($desktopCrop)
    $y += 40

    $again = New-Object System.Windows.Forms.CheckBox
    $again.Text = "Show this window at every start"
    $again.Checked = (Setting "menu" "1") -ne "0"
    $again.SetBounds(16, $y, 300, 24)
    $form.Controls.Add($again)

    $play = New-Object System.Windows.Forms.Button
    $play.Text = "Play"
    $play.SetBounds(360, $y - 2, 88, 30)
    $play.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $form.Controls.Add($play)
    $form.AcceptButton = $play
    $quit = New-Object System.Windows.Forms.Button
    $quit.Text = "Quit"
    $quit.SetBounds(456, $y - 2, 88, 30)
    $quit.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($quit)
    $form.CancelButton = $quit

    $result = Show-Form $form
    if ($result -ne [System.Windows.Forms.DialogResult]::OK) { return $false }
    Save-Setting "resolution" ($widths[$resolution.Value])
    Save-Setting "fps" ($caps[$fps.SelectedIndex])
    if ($language.SelectedIndex -ne $languageBefore) {
        Save-Setting "language" ($(if ($language.SelectedIndex -le 0) { "windows" } else { $gameLanguages[$language.SelectedIndex - 1] }))
    }
    Save-Setting "fov" ($fov.Value * 5)
    Save-Setting "menu" ($(if ($again.Checked) { "1" } else { "0" }))
    Save-Setting "desktop_view" ($desktopModes[$desktopView.SelectedIndex])
    Save-Setting "desktop_crop" ($(if ($desktopCrop.Checked) { "1" } else { "0" }))
    Read-Settings
    return $true
}

$emulator = Join-Path $here "shadps4.exe"
if (-not [System.IO.File]::Exists($emulator)) {
    [void](Show-Box ("The emulator, shadps4.exe, is missing from`n" + $here + "`n`nUnzip the whole AstroQuest package again. (Built from the source: run tools/make-pc-vr.sh.)") "OK" "Error")
    exit 1
}
# (Never a dead end: whoever has installed it and is still told it is missing can go on, and
# Windows itself says so if a file really is not there.)
$missingRuntime = Get-MissingRuntime "" $here
if ($missingRuntime.Count -gt 0) {
    Say ("Of the Microsoft Visual C++ runtime, not found on this PC: " + ($missingRuntime -join ", ")) "Yellow"
    $answer = Show-Box ("The emulator needs the Microsoft Visual C++ runtime (64-bit), and this PC seems to lack it: " + ($missingRuntime -join ", ") + " not found.`n`nYes: download its installer from Microsoft. Run it, then start Play Astro Bot VR again.`nNo: it is installed, start the game all the same.`nCancel: quit.") "YesNoCancel" "Warning"
    if ($answer -eq "Yes") { Start-Process "https://aka.ms/vs/17/release/vc_redist.x64.exe" }
    if ($answer -ne "No") { exit 1 }
}

$game = Resolve-Game
if (-not $game) { exit 0 }
Say ("The game: " + $game)
$info = Get-GameInfo $game
if ($info.Count -eq 0) {
    Say "sce_sys\param.sfo is missing next to it: this is not a complete copy of the game, and the emulator may not know it." "Yellow"
} elseif ($info["TITLE_ID"] -ne $madeFor) {
    Say ("This is " + $info["TITLE"] + ", " + $info["TITLE_ID"] + ". AstroQuest is made for ASTRO BOT Rescue Mission in its European release, " + $madeFor + ": its fixes for the game's speed and picture do not apply to another, and it may not run.") "Yellow"
} elseif (@("01.00", "01.04") -notcontains $info["APP_VER"]) {
    # (What decides is the executable itself, which the emulator looks at as it loads it and
    # names below; what the game says its version is can be wrong.)
    Say ("This copy of the game says it is version " + $info["APP_VER"] + ". AstroQuest knows versions 01.00 and 01.04 from inside: another plays in slow motion where frames take long, and at the console's resolution.") "Yellow"
}
$gameFolder = [System.IO.Path]::GetDirectoryName($game)
if ($info["TITLE_ID"] -eq $madeFor -and -not [System.IO.File]::Exists([System.IO.Path]::Combine($gameFolder, "sce_module", "libc.prx"))) {
    [void](Show-Box ("This is not a complete copy of the game:`n" + $gameFolder + "`n`nParts that every copy has are missing (sce_module\libc.prx for one). A folder with only an update of the game in it looks like this: an update holds the files it changed and no more. Put the update's files over a copy of the whole game, or use the game without its update.") "OK" "Warning")
    exit 1
}
if ($gameFolder.Length + 1 + $longestInside -gt 259) {
    [void](Show-Box ("The game is in`n" + $gameFolder + "`n`nThat path is too long: some of the game's files have a path of more than 259 characters there, which the emulator cannot open, and the game would stop when it needs them. Move the folder somewhere with a shorter path, for example C:\Games\AstroQuest, and start again.") "OK" "Warning")
    exit 1
}

if (-not $NoMenu -and (Setting "menu" "1") -ne "0") {
    if (-not (Show-Menu)) { exit 0 }
}

# What the settings mean to the emulator.
# resolution: the width of an eye (1440 the console's; larger ones are the game's sizes grown,
# with the memory that takes). game: the console's sizes, chosen by the game itself.
$resolution = Setting "resolution" "2880"
$dynamic = (Setting "dynamic" "1") -ne "0"
if ($resolution -eq "game") {
    $env:SHADPS4_TITLE_RESOLUTION = "title"
} else {
    $width = 0
    if (-not [int]::TryParse($resolution, [ref]$width)) { $width = 2880 }
    # (The console's other sizes, 816 to 1200, as they were offered before.)
    $smaller = @{ 816 = "3"; 960 = "4"; 1200 = "5" }
    if ($smaller.ContainsKey($width)) {
        $env:SHADPS4_TITLE_RESOLUTION = $smaller[$width]
    } else {
        $width = [math]::Max(1440, [math]::Min(4320, [int]([math]::Round($width / 8) * 8)))
        if ($width -gt 1440) { $env:SHADPS4_TITLE_EYE_WIDTH = "$width" }
        # Left to choose, the emulator draws smaller where the graphics card falls behind.
        if (-not $dynamic) { $env:SHADPS4_TITLE_RESOLUTION = "6" }
    }
}
$env:SHADPS4_VR_SHARPEN = Setting "sharpen" "0.3"
$env:SHADPS4_VR_DESKTOP_VIEW = Get-DesktopView
$env:SHADPS4_VR_DESKTOP_CROP = $(if ((Setting "desktop_crop" "0") -eq "1") { "1" } else { "0" })
if ((Setting "msaa") -ne "") { $env:SHADPS4_MAX_MSAA = Setting "msaa" }
if ((Setting "antialias" "1") -eq "0") { $env:SHADPS4_RESOLVE_AA = "0" }
if ((Setting "hands" "1") -eq "0") { $env:SHADPS4_XR_HANDS = "0" }
if ((Setting "predict_ms") -ne "") { $env:SHADPS4_XR_PREDICT_MS = Setting "predict_ms" }
if ((Setting "stick_touchpad" "1") -eq "0") { $env:SHADPS4_STICK_TOUCHPAD = "0" }
if ((Setting "mic_gain") -ne "") { $env:SHADPS4_MIC_GAIN = Setting "mic_gain" }
if ((Setting "surround" "1") -eq "0") { $env:SHADPS4_VIRTUAL_SURROUND = "0" }
if ((Setting "real_time" "1") -eq "0") { $env:SHADPS4_TITLE_TIMESTEP = "0" }
$fovSetting = Setting "fov" "100"
if ($fovSetting -ne "100") { $env:SHADPS4_VR_FOV = $fovSetting }
# fov_of: what fov is a percent of. headset: what the headset being worn shows, all of it at 100
# (the emulator asks the headset as it starts). psvr: a PlayStation VR's, as the game was made.
if ((Setting "fov_of" "headset") -ne "psvr") { $env:SHADPS4_VR_FOV_OF = "headset" }
# fps: the most frames a second. (pace, the older way to say it: refreshes of the headset a
# frame is given, 1 or more.)
$env:SHADPS4_VR_FPS_CAP = Setting "fps" "60"
$pace = Setting "pace" ""
if ($pace -eq "1") { $env:SHADPS4_VR_FASTEST_PACE = "1"; $env:SHADPS4_VR_FPS_CAP = "" } elseif ($pace -ne "" -and $pace -ne "2") { $env:SHADPS4_VR_PACE = $pace }
if ((Setting "headset" "1") -eq "0") { $env:SHADPS4_OPENXR = "0" }
if ((Setting "pause" "1") -eq "0") { $env:SHADPS4_XR_PAUSE = "0" }
if ((Setting "controllers" "1") -eq "0") { $env:SHADPS4_XR_CONTROLLERS = "0" }
if ((Setting "controller_hand" "right") -eq "left") { $env:SHADPS4_XR_PAD_HAND = "left" }
$env:SHADPS4_XR_WAIT = Setting "wait" "60"
$env:SHADPS4_CONSOLE_LANGUAGE = Get-GameLanguage
if ((Setting "turn") -ne "") { $env:SHADPS4_VR_TURN = Setting "turn" }
foreach ($pair in $extraEnv) {
    $at = $pair.IndexOf("=")
    if ($at -ge 1) { Set-Item -Path ("Env:" + $pair.Substring(0, $at)) -Value $pair.Substring($at + 1) }
}

# --- what is there ----------------------------------------------------------------------------
Say "ASTRO BOT Rescue Mission - PC VR" "Cyan"
if ($env:SHADPS4_TITLE_EYE_WIDTH) {
    Say ("Each eye up to " + $env:SHADPS4_TITLE_EYE_WIDTH + " x " + (EyeHeight ([int]$env:SHADPS4_TITLE_EYE_WIDTH)) + ", at most " + $env:SHADPS4_VR_FPS_CAP + " frames a second.")
}
$runtime = ""
if ($env:XR_RUNTIME_JSON) {
    $runtime = $env:XR_RUNTIME_JSON
} else {
    try { $runtime = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction Stop).ActiveRuntime } catch {}
}
if ($runtime -eq "") {
    Say "No OpenXR runtime is set up on this PC: the game will only show on the monitor." "Yellow"
    Say "For an Index, use SteamVR Settings > OpenXR > Set SteamVR as OpenXR Runtime."
    Say "For Virtual Desktop, use Streamer Options > OpenXR Runtime: VDXR."
} else {
    Say "OpenXR runtime: $runtime"
    if ($runtime -match "virtualdesktop") {
        $streamer = Get-Process "VirtualDesktop.Streamer" -ErrorAction SilentlyContinue
        if (-not $streamer) {
            $exe = Join-Path (Split-Path -Parent (Split-Path -Parent $runtime)) "VirtualDesktop.Streamer.exe"
            if (Test-Path $exe) {
                Say "Starting Virtual Desktop Streamer..."
                Start-Process $exe
            } else {
                Say "Virtual Desktop Streamer is not running: start it, then connect from the headset." "Yellow"
            }
        }
    }
}
Say ""
Get-VrInstructions $runtime | ForEach-Object { Say $_ }
if ($env:SHADPS4_OPENXR -ne "0" -and [int]$env:SHADPS4_XR_WAIT -gt 0) {
    Say ("The game waits up to " + $env:SHADPS4_XR_WAIT + " seconds for the headset before it starts on the monitor.")
}
Say "Hold OPTIONS for a second (or press the PS button) to reset the view."
Say "Where the game wants you to blow: into the headset's microphone, or hold the PS button and square"
Say "(X and Y together on VR controllers)."
Say "With VR controllers, or a gamepad that has no touchpad, the touchpad is on buttons:"
Say "  right trigger (R2): press it and hold (water, guns)"
Say "  right grip (R1): swipe forward (hook, throwing stars, chests)"
Say "  left trigger (L2): pull back, let go to shoot (the catapult at the end of a level)"
Say "  right stick: a finger on it, for anything else"
Say "Both sticks of VR controllers pressed in reset the view."
Say "Close the game's window to quit."
Say ""
# The emulator asks Windows for about 14 GB at once (the console's memory, and what the larger
# pictures take). Where Windows cannot promise that much, the emulator stops as it starts.
$memoryShort = $false
try {
    $spare = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).FreeVirtualMemory * 1024.0
    if ($spare -lt 16GB) {
        $memoryShort = $true
        Say ("Windows has " + (Gigabytes $spare) + " of memory left to hand out, and the emulator asks for about 14 GB: if the game does not start, close other programs and start again.") "Yellow"
        Say ""
    }
} catch {}

# --- run --------------------------------------------------------------------------------------
$logDir = Join-Path $here "user\log"
$log = Join-Path $logDir "shad_log.txt"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
if (Test-Path $log) { Copy-Item $log (Join-Path $logDir "shad_log.prev.txt") -Force }

# (In this console, with what it prints kept out of the way: a window style given here would
# also be the game window's.)
$startedAt = Get-Date
$process = Start-Process -FilePath $emulator -ArgumentList @("-g", "`"$game`"") -WorkingDirectory $here `
    -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $logDir "console.txt") `
    -RedirectStandardError (Join-Path $logDir "console-errors.txt")
# (Without this the exit code is not to be had later.)
$null = $process.Handle
$position = 0
$shown = @{}
function Show-Log {
    if (-not (Test-Path $log)) { return }
    try {
        $stream = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    } catch { return }
    try {
        if ($stream.Length -lt $script:position) { $script:position = 0 }
        [void]$stream.Seek($script:position, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { break }
            if ($line -match '^\[Core\.Vr\] <(Info|Warning)> \([^)]*\) \S+ (?:\w+: )?(.*)$') {
                $warning = $Matches[1] -eq "Warning"
                $text = $Matches[2]
                # The lines that repeat every few seconds only once in a while.
                if ($text -match '^(The title (has|now takes) the player|Hands:|Virtual headset connected)') { continue }
                if ($text -match '^Headset: the title delivered') {
                    $script:reports++
                    if (($script:reports % 6) -ne 1) { continue }
                }
                if ($text -match '^Controllers: standing in') {
                    if (($script:reports % 6) -ne 1) { continue }
                }
                if ($warning) { Say ("  " + $text) "Yellow" } else { Say ("  " + $text) }
            } elseif ($line -match '^\[Input\] <Info> \([^)]*\) \S+ (?:\w+: )?(Controller .*|The controller.s touchpad .*)$') {
                Say ("  " + $Matches[1])
            } elseif ($line -match '^\[Input\] <Warning> \([^)]*\) \S+ (?:\w+: )?(Controller .*)$') {
                Say ("  " + $Matches[1]) "Yellow"
            } elseif ($line -match '^\[Core\] <Info> \([^)]*\) \S+ (?:\w+: )?(CUSA12392 in a build .*|The title draws at up to .*|The scene is drawn at .*|Frames are given .*|The console.s language: .*)$') {
                Say ("  " + $Matches[1])
            } elseif ($line -match '^\[Core\] <Warning> \([^)]*\) \S+ (?:\w+: )?(This build of CUSA12392 .*)$') {
                Say ("  " + $Matches[1]) "Yellow"
            } elseif ($line -match '^\[Lib\.AudioIn\] <(Info|Warning)> \([^)]*\) \S+ (?:\w+: )?(Microphone: .*)$') {
                if ($Matches[1] -eq "Warning") { Say ("  " + $Matches[2]) "Yellow" } else { Say ("  " + $Matches[2]) }
            } elseif ($line -match '<Critical>.*?: (.*)$') {
                $text = $Matches[1]
                if (-not $shown.ContainsKey($text)) { $shown[$text] = 1; Say ("  ! " + $text) "Red" }
            }
        }
        $script:position = $stream.Position
    } finally { $stream.Dispose() }
}
$reports = 0
while (-not $process.HasExited) {
    Start-Sleep -Milliseconds 700
    Show-Log
}
Show-Log
Say ""
if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
    Say ("The emulator ended with code " + $process.ExitCode + ". Its log is $log") "Yellow"
    if ($process.ExitCode -eq -1073741515) {
        Say "Windows could not find a file the emulator needs: most likely the Microsoft Visual C++ runtime (64-bit) is not installed. Its installer: https://aka.ms/vs/17/release/vc_redist.x64.exe"
    }
    if ($memoryShort -and ((Get-Date) - $startedAt).TotalSeconds -lt 30) {
        Say "It stopped as it started, and Windows was short of memory then (see above): close other programs and start again."
    }
    Read-Host "Press Enter to close"
} else {
    Say "The game was closed."
    Start-Sleep -Seconds 2
}
