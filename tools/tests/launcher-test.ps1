# Loads the functions of the PC launcher (pc-vr/launch.ps1), not its main flow, and tries the
# ones that look for the game on folder layouts made up for the purpose in build/launcher-test.
#   powershell -ExecutionPolicy Bypass -File tools/tests/launcher-test.ps1
# With the game in games/CUSA12392 its param.sfo is used for the layouts; without it, the
# checks that need one are left out.
$top = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$launcher = Join-Path $top "pc-vr\launch.ps1"
Add-Type -AssemblyName System.Windows.Forms
$ast = [System.Management.Automation.Language.Parser]::ParseFile($launcher, [ref]$null, [ref]$null)
foreach ($function in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($function.Extent.Text))
}
$unpackFolders = @(".u", ".unpacking")
$madeFor = "CUSA12392"
$realSfo = Join-Path $top "games\CUSA12392\sce_sys\param.sfo"
$haveSfo = [System.IO.File]::Exists($realSfo)
$base = Join-Path $top "build\launcher-test"
if ([System.IO.Directory]::Exists($base)) { [System.IO.Directory]::Delete($base, $true) }
$failed = 0
function Check([string]$what, $got, $want) {
    if ("$got" -eq "$want") { Write-Host "ok    $what" } else { $script:failed++; Write-Host "FAIL  $what`n      got:  $got`n      want: $want" }
}
function Make-Game([string]$folder, [bool]$withSfo) {
    [void][System.IO.Directory]::CreateDirectory($folder)
    [System.IO.File]::WriteAllText((Join-Path $folder "eboot.bin"), "x")
    if ($withSfo -and $haveSfo) {
        [void][System.IO.Directory]::CreateDirectory((Join-Path $folder "sce_sys"))
        [System.IO.File]::Copy($realSfo, (Join-Path $folder "sce_sys\param.sfo"))
    }
}
# (`$flags`: what a package's header says it is, as its four bytes at 0x78: a game's own has
# 0x0A000000 there, an update of it 0x62300000.)
function Make-Package([string]$path, [string]$contentId, [int]$size, [uint32]$flags = 0x0A000000) {
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($path))
    $bytes = New-Object byte[] $size
    $bytes[0] = 0x7F; $bytes[1] = 0x43; $bytes[2] = 0x4E; $bytes[3] = 0x54
    [System.Text.Encoding]::ASCII.GetBytes($contentId).CopyTo($bytes, 0x40)
    $bytes[0x78] = [byte](($flags -shr 24) -band 0xFF); $bytes[0x79] = [byte](($flags -shr 16) -band 0xFF)
    [System.IO.File]::WriteAllBytes($path, $bytes)
}

# a: the plain layout
$g = "$base\a\games"; Make-Game "$g\CUSA12392" $true
Check "a  games\CUSA12392\eboot.bin" (Find-Game $g) "$g\CUSA12392\eboot.bin"
if ($haveSfo) {
    $info = Get-GameInfo "$g\CUSA12392\eboot.bin"
    Check "a  param.sfo: serial" $info["TITLE_ID"] "CUSA12392"
    Check "a  param.sfo: version" $info["APP_VER"] "01.00"
    Check "a  param.sfo: name" $info["TITLE"] "ASTRO BOT Rescue Mission"
}

# b: a dump's folder inside a folder with brackets and spaces in its name
$g = "$base\b\games"; Make-Game "$g\[ABC] my dump (1)\CUSA12392-app0" $true
Check "b  two folders down, brackets in the name" (Find-Game $g) "$g\[ABC] my dump (1)\CUSA12392-app0\eboot.bin"

# c: two games, the one this is made for second
if ($haveSfo) {
    $g = "$base\c\games"; Make-Game "$g\Another" $false; Make-Game "$g\Zzz" $true
    Check "c  prefers CUSA12392 among two" (Find-Game $g) "$g\Zzz\eboot.bin"
}

# c2: the game's update in a folder of its own beside it, found first by its name
if ($haveSfo) {
    $g = "$base\c2\games"; Make-Game "$g\CUSA12392-UPDATE" $true; Make-Game "$g\Game\CUSA12392" $true
    Check "c2 an update's folder is not the game" (Find-Game $g) "$g\Game\CUSA12392\eboot.bin"
}
$g = "$base\c3\games"; Make-Game "$g\CUSA12392-UPDATE" $true
Check "c3 an update's folder alone is no game" (Find-Game $g) ""

# d: only the leftovers of an unpacking (where it is done now, and inside the game's folder,
# where it was done before)
$g = "$base\d\games"; Make-Game "$g\CUSA12392\.unpacking\files\uroot" $true
Check "d  half-unpacked game is not taken" (Find-Game $g) ""
$g = "$base\d2\games"; Make-Game "$g\.u\uroot" $true
Check "d2 half-unpacked game beside the others is not taken" (Find-Game $g) ""

# e: packages
$g = "$base\e\games"
$package = "$g\CUSA12392\[ABC]-Some.Game-CUSA12392-EUR-(1.00+)-PS4.pkg"
Make-Package $package "EP9000-CUSA12392_00-PLATFORMERVR00EU" 4096
Make-Package "$g\update.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 1024
[System.IO.File]::WriteAllText("$g\notapackage.pkg", ("x" * 300))
Check "e  no unpacked game" (Find-Game $g) ""
Check "e  the largest package, brackets in its name" (Find-Package $g).FullName $package
Check "e  content id" (Read-PackageId $package) "EP9000-CUSA12392_00-PLATFORMERVR00EU"
Check "e  a file that is no package" (Read-PackageId "$g\notapackage.pkg") ""
Check "e  a game's package is not an update" (Test-UpdatePackage $package) $false
Check "e  a file that is no package is not an update" (Test-UpdatePackage "$g\notapackage.pkg") $false

# e2: an update next to the game's package, and larger than it: the game's is taken
$g = "$base\e2\games"
Make-Package "$g\game.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 2048
Make-Package "$g\A-Update-v1.04.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 8192 0x62300000
Make-Package "$g\first-patch.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 4096 0x00100000
Make-Package "$g\patchgo.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 16384 0x00200000
Check "e2 an update says so" (Test-UpdatePackage "$g\A-Update-v1.04.pkg") $true
Check "e2 a first patch says so" (Test-UpdatePackage "$g\first-patch.pkg") $true
Check "e2 a PATCHGO package is an update" (Test-UpdatePackage "$g\patchgo.pkg") $true
Check "e2 the game's package before a larger update" (Find-Package $g).FullName "$g\game.pkg"
# e3: nothing but updates: the largest of them, which is then turned down for what it is
$g = "$base\e3\games"
Make-Package "$g\small.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 1024 0x62300000
Make-Package "$g\large.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 4096 0x62300000
Check "e3 only updates: the largest" (Find-Package $g).FullName "$g\large.pkg"

# f: too deep, and an empty or missing games folder
$g = "$base\f\games"; Make-Game "$g\1\2\3\4" $true
Check "f  four folders down is not looked at" (Find-Game $g) ""
Check "f  missing folder" (Find-Game "$base\nowhere") ""
Check "f  missing folder, package" (Find-Package "$base\nowhere") ""

# g: what a path given by the player turns into
$g = "$base\a\games"
Check "g  a folder" (Use-Path "$base\a") "$g\CUSA12392\eboot.bin"
Check "g  an eboot.bin" (Use-Path "$g\CUSA12392\eboot.bin") "$g\CUSA12392\eboot.bin"
Check "g  nothing there" (Use-Path "$base\a\nothing.bin") ""

$steam = (Get-VrInstructions 'F:\steam\steamapps\common\SteamVR\steamxr_win64.json') -join "`n"
Check "SteamVR instructions name the Index" ($steam -match 'Index') $true
Check "SteamVR instructions require no Virtual Desktop" ($steam -match 'Virtual Desktop is not needed') $true
Check "SteamVR instructions preserve native DualSense input" ($steam -match 'disable Steam Input') $true
Check "SteamVR instructions explain the tracking limit" ($steam -match 'does not track bare hands') $true
Check "SteamVR instructions say how to move the gamepad" ($steam -match 'Hold the PS button') $true
$vd = (Get-VrInstructions 'C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json') -join "`n"
Check "VDXR instructions still forward hand tracking" ($vd -match 'hand tracking forwarded') $true
Check "VDXR instructions do not describe an Index" ($vd -match 'Index') $false
$unknown = (Get-VrInstructions '') -join "`n"
Check "Unknown runtime instructions do not assume Virtual Desktop" ($unknown -match 'Virtual Desktop') $false
Check "Unknown runtime instructions say how to move the gamepad" ($unknown -match 'Hold the PS button') $true
Check "VDXR instructions say how to move the gamepad" ($vd -match 'Hold the PS button') $true
foreach ($text in @($steam, $vd, $unknown)) {
    Check "Instructions say how to turn round" ($text -match 'hold L1') $true
    Check "Instructions say the headset's controllers play besides a gamepad" ($text -match 'whenever they were used after it') $true
}
$script:settings = [ordered]@{}
Check "Desktop defaults to stereo" (Get-DesktopView) "stereo"
$script:settings = [ordered]@{ desktop_view = "spectator" }
Check "Spectator setting selects one eye" (Get-DesktopView) "spectator"
$script:settings = [ordered]@{ desktop_view = "unknown" }
Check "Unknown desktop view falls back to stereo" (Get-DesktopView) "stereo"
$script:settings = [ordered]@{ desktop_view = "combined"; desktop_crop = "1" }
Check "Combined setting selects both eyes" (Get-DesktopView) "combined"
foreach ($assignment in $ast.EndBlock.Statements) {
    if ($assignment -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $assignment.Left.Extent.Text -in @('$env:SHADPS4_VR_DESKTOP_VIEW', '$env:SHADPS4_VR_DESKTOP_CROP')) {
        . ([scriptblock]::Create($assignment.Extent.Text))
    }
}
Check "Launcher exports combined view" $env:SHADPS4_VR_DESKTOP_VIEW "combined"
Check "Launcher exports crop enabled" $env:SHADPS4_VR_DESKTOP_CROP "1"
$script:settings = [ordered]@{}
foreach ($assignment in $ast.EndBlock.Statements) {
    if ($assignment -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $assignment.Left.Extent.Text -eq '$env:SHADPS4_VR_DESKTOP_CROP') {
        . ([scriptblock]::Create($assignment.Extent.Text))
    }
}
Check "Launcher clears inherited crop by default" $env:SHADPS4_VR_DESKTOP_CROP "0"

foreach ($assignment in $ast.EndBlock.Statements) {
    if ($assignment -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $assignment.Left.Extent.Text -in @('$widths', '$consoleHeights', '$caps', '$gameLanguages')) {
        . ([scriptblock]::Create($assignment.Extent.Text))
    }
}

# Sizes and frame rates offered.
Check "The console's smaller sizes keep their own shape" ((@(816, 960, 1200) | ForEach-Object { EyeHeight $_ }) -join ",") "870,1080,1280"
Check "Larger sizes keep the shape of 1440x1536" ((@(1440, 2880, 3600) | ForEach-Object { EyeHeight $_ }) -join ",") "1536,3072,3840"
Check "80 frames a second is offered (a Rift S refreshes at 80 Hz)" ($caps -contains 80) $true

# The Visual C++ runtime: looked for where a 64-bit program finds it, whatever PowerShell this is.
$runtimeFolder = Join-Path $base "system"
$besideFolder = Join-Path $base "beside"
[void][System.IO.Directory]::CreateDirectory($runtimeFolder)
[void][System.IO.Directory]::CreateDirectory($besideFolder)
$runtimeFiles = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll")
Check "An empty system folder lacks all of the runtime" ((Get-MissingRuntime $runtimeFolder "") -join ",") ($runtimeFiles -join ",")
foreach ($name in $runtimeFiles) { if ($name -ne "vcruntime140_1.dll") { [System.IO.File]::WriteAllText((Join-Path $runtimeFolder $name), "x") } }
Check "The 32-bit runtime alone lacks the file that only exists in 64 bits" ((Get-MissingRuntime $runtimeFolder "") -join ",") "vcruntime140_1.dll"
[System.IO.File]::WriteAllText((Join-Path $besideFolder "vcruntime140_1.dll"), "x")
Check "A file next to the emulator counts" (Get-MissingRuntime $runtimeFolder $besideFolder).Count 0
$is64 = [Environment]::Is64BitProcess
Check "This PC's own runtime is found from this PowerShell (64-bit: $is64)" ((Get-MissingRuntime "" "") -join ",") ""
if ($is64) {
    Check "A 64-bit PowerShell hands over to no other" (Get-NativePowerShell) ""
} else {
    Check "A 32-bit PowerShell knows the 64-bit one to hand over to" ([System.IO.File]::Exists((Get-NativePowerShell))) $true
}

# The game's language.
$script:settings = [ordered]@{}
Check "The language is Windows' own unless one is set" (Get-GameLanguage) ([System.Globalization.CultureInfo]::CurrentUICulture.Name)
$script:settings = [ordered]@{ language = "windows" }
Check "language=windows is Windows' own" (Get-GameLanguage) ([System.Globalization.CultureInfo]::CurrentUICulture.Name)
$script:settings = [ordered]@{ language = "ja-JP" }
Check "A language that is set is the one asked for" (Get-GameLanguage) "ja-JP"
foreach ($assignment in $ast.EndBlock.Statements) {
    if ($assignment -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $assignment.Left.Extent.Text -eq '$env:SHADPS4_CONSOLE_LANGUAGE') {
        . ([scriptblock]::Create($assignment.Extent.Text))
    }
}
Check "Launcher tells the emulator the language" $env:SHADPS4_CONSOLE_LANGUAGE "ja-JP"
Check "The game's 28 languages are offered" $gameLanguages.Count 28
Check "Each has a name" (@($gameLanguages | Where-Object { (Get-LanguageName $_) -eq "" -or (Get-LanguageName $_) -eq $_ }).Count) 0
$script:settings = [ordered]@{}
$SettingsFile = Join-Path $base 'menu-settings.txt'
$script:menuAction = "choose"
function Show-Form($form) {
    $script:menuForm = $form
    $view = $form.Controls["desktopView"]
    $crop = $form.Controls["desktopCrop"]
    $language = $form.Controls["language"]
    foreach ($control in $form.Controls) {
        Check "Menu bounds contain $($control.Text) $($control.Name)" $form.ClientRectangle.Contains($control.Bounds) $true
    }
    Check "Menu offers three desktop modes" $view.Items.Count 3
    Check "Menu offers Windows' language and the game's 28" $language.Items.Count 29
    foreach ($control in $form.Controls) {
        if ($control -ne $language -and $control.Bounds.IntersectsWith($language.Bounds)) {
            Check "Language list overlaps $($control.Text) $($control.Name)" $true $false
        }
    }
    Check "Desktop view and crop controls do not overlap" $view.Bounds.IntersectsWith($crop.Bounds) $false
    $resolution = $form.Controls["resolution"]
    $fps = $form.Controls["fps"]
    if ($script:menuAction -eq "choose") {
        Check "Menu defaults to 2880 an eye" $widths[$resolution.Value] 2880
        Check "Menu defaults to 60 frames a second" $caps[$fps.SelectedIndex] 60
        $resolution.Value = [array]::IndexOf($widths, 960)
        $fps.SelectedIndex = [array]::IndexOf($caps, 80)
        Check "Menu defaults to Windows' language" $language.SelectedIndex 0
        $language.SelectedIndex = 1 + [array]::IndexOf($gameLanguages, "fr-FR")
        Check "Menu defaults to stereo" $view.SelectedIndex 0
        Check "Menu defaults to uncropped image" $crop.Checked $false
        Check "Stereo disables crop control" $crop.Enabled $false
        $view.SelectedIndex = 1
        Check "Single eye enables crop control" $crop.Enabled $true
        $view.SelectedIndex = 2
        Check "Combined eyes enable crop control" $crop.Enabled $true
        $crop.Checked = $true
        return [System.Windows.Forms.DialogResult]::OK
    }
    Check "Menu restores French" $language.SelectedIndex (1 + [array]::IndexOf($gameLanguages, "fr-FR"))
    Check "Menu restores 960 an eye" $widths[$resolution.Value] 960
    Check "Menu restores 80 frames a second" $caps[$fps.SelectedIndex] 80
    if ($script:menuAction -eq "restore") { $language.SelectedIndex = 0 }
    Check "Menu restores combined eyes" $view.SelectedIndex 2
    Check "Menu restores crop" $crop.Checked $true
    $view.SelectedIndex = 0
    Check "Switching to stereo disables crop" $crop.Enabled $false
    $crop.Checked = $false
    if ($script:menuAction -eq "cancel") { return [System.Windows.Forms.DialogResult]::Cancel }
    return [System.Windows.Forms.DialogResult]::OK
}
Read-Settings
Check "Menu accepts combined eyes" (Show-Menu) $true
$script:menuForm.Dispose()
Check "Menu saves combined eyes" (Setting "desktop_view") "combined"
Check "Menu saves French" (Setting "language") "fr-FR"
Check "Menu saves crop" (Setting "desktop_crop") "1"
Check "Menu saves 960 an eye" (Setting "resolution") "960"
Check "Menu saves 80 frames a second" (Setting "fps") "80"
$script:menuAction = "cancel"
Check "Menu cancellation does not start game" (Show-Menu) $false
$script:menuForm.Dispose()
Read-Settings
Check "Cancel preserves combined eyes" (Get-DesktopView) "combined"
Check "Cancel preserves crop" (Setting "desktop_crop") "1"
$script:menuAction = "restore"
Check "Menu accepts stereo again" (Show-Menu) $true
$script:menuForm.Dispose()
Check "Menu saves stereo again" (Get-DesktopView) "stereo"
Check "Menu saves Windows' language again" (Setting "language") "windows"
Check "Menu saves uncropped image again" (Setting "desktop_crop") "0"

[System.IO.Directory]::Delete($base, $true)
"failed: $failed"
if ($failed -gt 0) { exit 1 }
