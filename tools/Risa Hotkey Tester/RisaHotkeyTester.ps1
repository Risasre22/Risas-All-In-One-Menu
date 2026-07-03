param(
    [string]$Root = '',
    [string]$StatePath = '',
    [switch]$NoGui,
    [switch]$Report
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

[System.Windows.Forms.Application]::EnableVisualStyles()

$script:GameRoot = if ($Root) { [IO.Path]::GetFullPath($Root) } else { (Get-Location).Path }
$script:DataRoot = Join-Path $script:GameRoot 'Data'
$script:StateRoot = if ($StatePath) { [IO.Path]::GetFullPath($StatePath) } else { Join-Path $PSScriptRoot 'TesterData' }
$script:SnapshotRoot = Join-Path $script:StateRoot 'Snapshots'
$script:ExpectedCustomPath = Join-Path $script:StateRoot 'ExpectedCustom.json'
$script:LastResultsPath = Join-Path $script:StateRoot 'LastResults.txt'
$script:LastReport = ''
New-Item -ItemType Directory -Path $script:SnapshotRoot -Force | Out-Null

# When installed as its own MO2 mod, infer Mod Organizer directly from
# ...\Mod Organizer\mods\Risa's Tester. No USVFS/MO2 launch is required.
$script:ModsRoot = $null
$probe = Get-Item -LiteralPath $PSScriptRoot
while ($probe) {
    if ($probe.Name -ieq 'mods') { $script:ModsRoot = $probe.FullName; break }
    $probe = $probe.Parent
}
$script:MORoot = if ($script:ModsRoot) { Split-Path -Parent $script:ModsRoot } else { $null }
$script:SearchRoots = @()
$script:ProfileName = ''
if ($script:MORoot) {
    $overwrite = Join-Path $script:MORoot 'overwrite'
    if (Test-Path -LiteralPath $overwrite -PathType Container) { $script:SearchRoots += $overwrite }

    $enabledNames = @()
    $ini = Join-Path $script:MORoot 'ModOrganizer.ini'
    if (Test-Path -LiteralPath $ini) {
        $selected = Select-String -LiteralPath $ini -Pattern '^selected_profile=@ByteArray\((.*)\)$' | Select-Object -First 1
        if ($selected) {
            $script:ProfileName = $selected.Matches[0].Groups[1].Value
            $modList = Join-Path $script:MORoot ("profiles\$($script:ProfileName)\modlist.txt")
            if (Test-Path -LiteralPath $modList) {
                $enabledNames = @(Get-Content -LiteralPath $modList | Where-Object { $_ -like '+*' } | ForEach-Object { $_.Substring(1) })
            }
        }
    }
    # MO2 writes higher-priority enabled mods first in modlist.txt for this setup.
    foreach ($name in $enabledNames) {
        $dir = Join-Path $script:ModsRoot $name
        if (Test-Path -LiteralPath $dir -PathType Container) { $script:SearchRoots += $dir }
    }
    # If no profile could be read, fall back to all mod folders. When a profile is available,
    # intentionally exclude '-' entries so disabled mods are never inspected or modified.
    if ($enabledNames.Count -eq 0) {
        foreach ($dir in Get-ChildItem -LiteralPath $script:ModsRoot -Directory) {
            $script:SearchRoots += $dir.FullName
        }
    }
}

function Spec($mod, $setting, $type, $paths, $key, $default, $random, $section = '') {
    [pscustomobject]@{
        Id = "$mod.$setting"; Mod = $mod; Setting = $setting; Type = $type
        Paths = @($paths); Key = $key; Section = $section
        Default = $default; Random = @($random)
    }
}

# Defaults mirror Restore Supported Mod Defaults in Risa 1.5.2.
$script:Specs = @(
    (Spec 'SKSE Menu Framework' 'ToggleKey' 'ini' @('Data\SKSE\Plugins\SKSEMenuFramework.ini') 'ToggleKey' 'F1' @('F4','F8','F10')),
    (Spec 'SKSE Menu Framework' 'ToggleMode' 'ini' @('Data\SKSE\Plugins\SKSEMenuFramework.ini') 'ToggleMode' 'SinglePress' @('Hold','DoublePress')),

    (Spec 'Open Animation Replacer' 'uToggleUIKey' 'ini' @('Data\SKSE\Plugins\OpenAnimationReplacer.ini') 'uToggleUIKey' 24 @(30,32,33)),
    (Spec 'Open Animation Replacer' 'uToggleUIKeyShift' 'ini' @('Data\SKSE\Plugins\OpenAnimationReplacer.ini') 'uToggleUIKeyShift' 1 @(0)),
    (Spec 'Open Animation Replacer' 'uToggleUIKeyCtrl' 'ini' @('Data\SKSE\Plugins\OpenAnimationReplacer.ini') 'uToggleUIKeyCtrl' 0 @(1)),
    (Spec 'Open Animation Replacer' 'uToggleUIKeyAlt' 'ini' @('Data\SKSE\Plugins\OpenAnimationReplacer.ini') 'uToggleUIKeyAlt' 0 @(0,1)),

    (Spec 'dMenu' 'key_toggle_dmenu' 'ini' @('Data\SKSE\Plugins\dmenu\dmenu.ini') 'key_toggle_dmenu' 199 @(32,33,34)),
    (Spec 'dMenu' 'key_toggle_dmenu_mkb' 'ini' @('Data\SKSE\Plugins\dmenu\dmenu.ini') 'key_toggle_dmenu_mkb' 199 @(32,33,34)),
    (Spec 'dMenu' 'key_toggle_modifier_mkb' 'ini' @('Data\SKSE\Plugins\dmenu\dmenu.ini') 'key_toggle_modifier_mkb' 0 @(29,42,54)),

    (Spec 'Improved Camera' 'MenuKey' 'ini' @('Data\SKSE\Plugins\ImprovedCameraSE\ImprovedCameraSE.ini') 'MenuKey' 36 @(33,34,35)),
    (Spec 'IED' 'ToggleKeys' 'ini' @('Data\SKSE\Plugins\ImmersiveEquipmentDisplays.ini') 'ToggleKeys' 14 @(66,67,68)),
    (Spec 'IED' 'OverrideToggleKeys' 'ini' @('Data\SKSE\Plugins\ImmersiveEquipmentDisplays.ini') 'OverrideToggleKeys' 'false' @('true')),

    (Spec 'FLICK' 'iToggleFUCK_Key' 'ini' @('Data\FUCKs\FUCK\keybinds_user.ini','FUCKs\FUCK\keybinds_user.ini','Data\SKSE\Plugins\FUCKs\FUCK\keybinds_user.ini','Data\FUCKs\FUCK\keybinds.ini','FUCKs\FUCK\keybinds.ini','Data\SKSE\Plugins\FUCKs\FUCK\keybinds.ini') 'iToggleFUCK_Key' 65 @(66,67,68)),
    (Spec 'KreatE' 'GUIToggleKeys' 'ini' @('Data\KreatE\UserSettings.ini') 'GUIToggleKeys' 35 @(33,34,45)),
    (Spec 'Debug Menu' 'uOpenMenuHotkey' 'ini' @('Data\MCM\Settings\DebugMenu.ini','Data\MCM\Config\DebugMenu\settings.ini') 'uOpenMenuHotkey' 59 @(62,63,64)),

    (Spec 'Community Shaders User' 'ToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsUser.json') 'ToggleKey' 35 @(115,116,117) 'Menu'),
    (Spec 'Community Shaders User' 'CSEditorToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsUser.json') 'CSEditorToggleKey' @(16,35) @('[17,116]','[18,117]') 'Menu'),
    (Spec 'Community Shaders User' 'OverlayToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsUser.json') 'OverlayToggleKey' 121 @(115,116,117) 'Menu'),
    (Spec 'Community Shaders User' 'EffectToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsUser.json') 'EffectToggleKey' 106 @(118,119,120) 'Menu'),
    (Spec 'Community Shaders Default' 'ToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsDefault.json') 'ToggleKey' 35 @(115,116,117) 'Menu'),
    (Spec 'Community Shaders Default' 'CSEditorToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsDefault.json') 'CSEditorToggleKey' @(16,35) @('[17,116]','[18,117]') 'Menu'),
    (Spec 'Community Shaders Default' 'OverlayToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsDefault.json') 'OverlayToggleKey' 121 @(115,116,117) 'Menu'),
    (Spec 'Community Shaders Default' 'EffectToggleKey' 'jsonSection' @('Data\SKSE\Plugins\CommunityShaders\SettingsDefault.json') 'EffectToggleKey' 106 @(118,119,120) 'Menu'),

    (Spec 'CatMenu' 'toggle_key' 'jsonRoot' @('Data\SKSE\Plugins\catmenu\settings.json') 'toggle_key' 577 @(579,580,581)),
    (Spec "Dragonborn's Toolkit" 'toggleKey' 'jsonRoot' @('Data\SKSE\Plugins\SkyrimCheatMenu.json') 'toggleKey' 'F1' @('F4','F8','F10')),
    (Spec 'ReShade' 'KeyOverlay' 'ini' @('ReShade.ini','Data\..\ReShade.ini') 'KeyOverlay' '36,0,0,0' @('115,0,0,0','116,1,0,0','117,0,1,0'))
)

function Resolve-SpecPath($spec) {
    # First resolve the winning file directly from MO2's overwrite/mod folders.
    foreach ($candidate in $spec.Paths) {
        $clean = $candidate.Replace('/','\')
        $variants = [Collections.Generic.List[string]]::new()
        if ($clean.StartsWith('Data\',[StringComparison]::OrdinalIgnoreCase)) {
            $afterData = $clean.Substring(5)
            if ($afterData.StartsWith('..\')) { $afterData = $afterData.Substring(3) }
            $variants.Add($afterData)
        } else { $variants.Add($clean) }
        if ([IO.Path]::GetFileName($clean) -ieq 'ReShade.ini') { $variants.Add('Root\ReShade.ini') }

        foreach ($root in $script:SearchRoots) {
            foreach ($relative in $variants) {
                $path = Join-Path $root $relative
                if (Test-Path -LiteralPath $path -PathType Leaf) { return [IO.Path]::GetFullPath($path) }
            }
        }
    }
    # Fall back to a physical game installation for non-MO2 use.
    foreach ($candidate in $spec.Paths) {
        $path = [IO.Path]::GetFullPath((Join-Path $script:GameRoot $candidate))
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    return $null
}

function Get-SnapshotRelative($path) {
    if ($script:ModsRoot -and $path.StartsWith($script:ModsRoot,[StringComparison]::OrdinalIgnoreCase)) {
        return Join-Path 'mods' $path.Substring($script:ModsRoot.Length).TrimStart('\')
    }
    $overwrite = if ($script:MORoot) { Join-Path $script:MORoot 'overwrite' } else { '' }
    if ($overwrite -and $path.StartsWith($overwrite,[StringComparison]::OrdinalIgnoreCase)) {
        return Join-Path 'overwrite' $path.Substring($overwrite.Length).TrimStart('\')
    }
    if ($path.StartsWith($script:GameRoot,[StringComparison]::OrdinalIgnoreCase)) {
        return Join-Path 'game' $path.Substring($script:GameRoot.Length).TrimStart('\')
    }
    return Join-Path 'other' ([IO.Path]::GetFileName($path))
}

function Resolve-OriginalBackupPath {
    $backupSpec = [pscustomobject]@{ Paths = @('Data\SKSE\Plugins\RisaAllInOneMenu_OriginalHotkeys.json') }
    $existing = Resolve-SpecPath $backupSpec
    if ($existing) { return $existing }
    if ($script:MORoot) { return Join-Path $script:MORoot 'overwrite\SKSE\Plugins\RisaAllInOneMenu_OriginalHotkeys.json' }
    return Join-Path $script:GameRoot 'Data\SKSE\Plugins\RisaAllInOneMenu_OriginalHotkeys.json'
}

function Normalize-Value($value) {
    if ($null -eq $value) { return '<missing>' }
    if ($value -is [Array]) { return ($value | ConvertTo-Json -Compress) }
    $s = ([string]$value).Trim()
    if ($s -match '^0x[0-9a-f]+$') { return ([Convert]::ToInt32($s.Substring(2),16)).ToString() }
    if ($s -match '^(true|false)$') { return $s.ToLowerInvariant() }
    return $s
}

$script:DikNames = @{
    '0'='Off'; '14'='Backspace'; '24'='O'; '29'='Left Ctrl'; '30'='A'; '32'='D';
    '33'='F'; '34'='G'; '35'='H'; '36'='J'; '42'='Left Shift'; '54'='Right Shift';
    '59'='F1'; '60'='F2'; '61'='F3'; '62'='F4'; '63'='F5'; '64'='F6';
    '65'='F7'; '66'='F8'; '67'='F9'; '68'='F10'; '87'='F11'; '88'='F12';
    '199'='Home'; '207'='End'
}
$script:VkNames = @{
    '0'='Off'; '16'='Shift'; '17'='Ctrl'; '18'='Alt'; '35'='End'; '36'='Home';
    '106'='Numpad *'; '112'='F1'; '113'='F2'; '114'='F3'; '115'='F4';
    '116'='F5'; '117'='F6'; '118'='F7'; '119'='F8'; '120'='F9'; '121'='F10'
}

function Add-KeyName($raw, $table) {
    $name = $table[[string]$raw]
    if ($name) { return "$raw ($name)" }
    return [string]$raw
}

function Display-Value($spec, $normalized) {
    if ($normalized -like '<*') { return $normalized }
    if ($spec.Type -eq 'jsonSection' -and $spec.Key -eq 'CSEditorToggleKey' -and $normalized.StartsWith('[')) {
        $a = @($normalized | ConvertFrom-Json)
        return "$(Add-KeyName $a[0] $script:VkNames) + $(Add-KeyName $a[1] $script:VkNames)"
    }
    if ($spec.Mod -eq 'ReShade' -and $spec.Key -eq 'KeyOverlay') {
        $parts = $normalized.Split(',')
        if ($parts.Count -ge 4) {
            $mods = @()
            if ($parts[1] -eq '1') { $mods += 'Ctrl' }
            if ($parts[2] -eq '1') { $mods += 'Shift' }
            if ($parts[3] -eq '1') { $mods += 'Alt' }
            $mods += Add-KeyName $parts[0] $script:VkNames
            return ($mods -join ' + ')
        }
    }
    if ($spec.Type -eq 'jsonSection') { return Add-KeyName $normalized $script:VkNames }
    if ($spec.Mod -eq 'KreatE' -or $spec.Mod -eq 'Improved Camera') { return Add-KeyName $normalized $script:VkNames }
    if ($spec.Mod -eq 'CatMenu') {
        $imgui = @{ '577'='F6'; '579'='F8'; '580'='F9'; '581'='F10' }
        return Add-KeyName $normalized $imgui
    }
    if ($spec.Setting -match 'ToggleKey$' -and $normalized -match '^F\d+$') { return $normalized }
    if ($spec.Key -match 'Key|Keys|key|modifier|GUIToggle') { return Add-KeyName $normalized $script:DikNames }
    return $normalized
}

function Read-IniValue($path, $key) {
    $escaped = [regex]::Escape($key)
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        if ($line -match "^\s*$escaped\s*=\s*(.*?)\s*(?:;.*)?$") { return $Matches[1].Trim() }
    }
    return $null
}

function Write-IniValue($path, $key, $value) {
    $lines = [Collections.Generic.List[string]]::new()
    $escaped = [regex]::Escape($key)
    $found = $false
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        if ($line -match "^(\s*)$escaped(\s*=\s*)(.*?)(\s*;.*)?$") {
            $comment = if ($null -ne $Matches[4]) { $Matches[4] } else { '' }
            $lines.Add("$($Matches[1])$key$($Matches[2])$(Normalize-Value $value)$comment")
            $found = $true
        } else { $lines.Add($line) }
    }
    if (-not $found) { throw "Key '$key' was not found in $path" }
    [IO.File]::WriteAllLines($path, $lines, [Text.UTF8Encoding]::new($false))
}

function Read-SpecValue($spec) {
    $path = Resolve-SpecPath $spec
    if (-not $path) { return $null }
    if ($spec.Type -eq 'ini') { return Read-IniValue $path $spec.Key }
    $json = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ($spec.Type -eq 'jsonRoot') {
        $p = $json.PSObject.Properties[$spec.Key]
    } else {
        $section = $json.PSObject.Properties[$spec.Section].Value
        if ($null -eq $section) { return $null }
        $p = $section.PSObject.Properties[$spec.Key]
    }
    if ($null -eq $p) { return $null }
    return $p.Value
}

function Write-SpecValue($spec, $value) {
    $path = Resolve-SpecPath $spec
    if (-not $path) { return $false }
    if ($spec.Type -eq 'ini') { Write-IniValue $path $spec.Key $value; return $true }
    $json = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ($value -is [string] -and $value.Trim().StartsWith('[')) {
        $value = @($value | ConvertFrom-Json)
    }
    if ($spec.Type -eq 'jsonRoot') {
        $json.PSObject.Properties[$spec.Key].Value = $value
    } else {
        $section = $json.PSObject.Properties[$spec.Section].Value
        $section.PSObject.Properties[$spec.Key].Value = $value
    }
    $json | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $path -Encoding UTF8
    return $true
}

function New-SafetySnapshot {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $dir = Join-Path $script:SnapshotRoot $stamp
    $filesDir = Join-Path $dir 'Files'
    New-Item -ItemType Directory -Path $filesDir -Force | Out-Null
    $seen = @{}
    $manifest = @()
    foreach ($spec in $script:Specs) {
        $path = Resolve-SpecPath $spec
        if (-not $path -or $seen.ContainsKey($path)) { continue }
        $seen[$path] = $true
        $relative = Get-SnapshotRelative $path
        $dest = Join-Path $filesDir $relative
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($dest)) -Force | Out-Null
        Copy-Item -LiteralPath $path -Destination $dest -Force
        $manifest += [pscustomobject]@{ Source = $path; Relative = $relative; Existed = $true }
    }
    $backup = Resolve-OriginalBackupPath
    $backupRelative = Get-SnapshotRelative $backup
    if (Test-Path -LiteralPath $backup) {
        $dest = Join-Path $filesDir $backupRelative
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($dest)) -Force | Out-Null
        Copy-Item -LiteralPath $backup -Destination $dest -Force
        $manifest += [pscustomobject]@{ Source = $backup; Relative = $backupRelative; Existed = $true }
    } else {
        $manifest += [pscustomobject]@{ Source = $backup; Relative = $backupRelative; Existed = $false }
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dir 'SnapshotFiles.json') -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $script:StateRoot 'LatestSnapshot.txt') -Value $dir -Encoding UTF8
    return $dir
}

function Get-LatestSnapshot {
    $marker = Join-Path $script:StateRoot 'LatestSnapshot.txt'
    if (Test-Path -LiteralPath $marker) {
        $dir = (Get-Content -Raw -LiteralPath $marker).Trim()
        if (Test-Path -LiteralPath $dir) { return $dir }
    }
    return $null
}

function Restore-SafetySnapshot {
    $dir = Get-LatestSnapshot
    if (-not $dir) { throw 'No safety snapshot exists yet.' }
    $manifest = @(Get-Content -Raw -LiteralPath (Join-Path $dir 'SnapshotFiles.json') | ConvertFrom-Json)
    foreach ($entry in $manifest) {
        $saved = Join-Path (Join-Path $dir 'Files') $entry.Relative
        if ($entry.Existed) {
            Copy-Item -LiteralPath $saved -Destination $entry.Source -Force
        } elseif (Test-Path -LiteralPath $entry.Source) {
            Remove-Item -LiteralPath $entry.Source -Force
        }
    }
}

function Save-ExpectedCustom($map) {
    $map | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $script:ExpectedCustomPath -Encoding UTF8
}

function Load-ExpectedCustom {
    if (-not (Test-Path -LiteralPath $script:ExpectedCustomPath)) { return $null }
    return Get-Content -Raw -LiteralPath $script:ExpectedCustomPath | ConvertFrom-Json
}

function Randomize-ManagedSettings {
    $backup = Resolve-OriginalBackupPath
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }
    $expected = [ordered]@{}
    foreach ($spec in $script:Specs) {
        if (-not (Resolve-SpecPath $spec)) { continue }
        $choice = $spec.Random | Get-Random
        [void](Write-SpecValue $spec $choice)
        $expected[$spec.Id] = Normalize-Value (Read-SpecValue $spec)
    }
    Save-ExpectedCustom $expected
}

function Assert-SkyrimClosed {
    if (Get-Process -Name 'SkyrimSE' -ErrorAction SilentlyContinue) {
        throw 'SkyrimSE.exe is running. Close the game before changing or restoring test files.'
    }
}

if ($Report) {
    "Profile=$($script:ProfileName)"
    "ModsRoot=$($script:ModsRoot)"
    foreach ($spec in $script:Specs) {
        $resolved = Resolve-SpecPath $spec
        [pscustomobject]@{ Mod=$spec.Mod; Setting=$spec.Setting; Found=[bool]$resolved; Path=$resolved }
    }
}
if ($NoGui) { return }

$form = New-Object Windows.Forms.Form
$form.Text = 'Risa Hotkey Restore Tester'
$form.Size = New-Object Drawing.Size(1180,760)
$form.StartPosition = 'CenterScreen'
$form.MinimumSize = New-Object Drawing.Size(950,600)

$warning = New-Object Windows.Forms.Label
$warning.Text = if ($script:ModsRoot) {
    "Direct MO2 mode - profile: $($script:ProfileName). Close Skyrim before writing files."
} else {
    "Physical game-folder mode. Close Skyrim before writing files. Root: $script:GameRoot"
}
$warning.ForeColor = [Drawing.Color]::DarkRed
$warning.AutoSize = $true
$warning.Location = New-Object Drawing.Point(12,12)
$form.Controls.Add($warning)

$buttons = @()
function Add-Button($text, $x, $width, $handler) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x,40)
    $b.Size = New-Object Drawing.Size($width,34)
    $b.Add_Click($handler); $form.Controls.Add($b); $script:buttons += $b
}

$list = New-Object Windows.Forms.ListView
$list.Location = New-Object Drawing.Point(12,86)
$list.Size = New-Object Drawing.Size(1140,610)
$list.Anchor = 'Top,Bottom,Left,Right'
$list.View = 'Details'; $list.FullRowSelect = $true; $list.GridLines = $true
[void]$list.Columns.Add('Mod',190); [void]$list.Columns.Add('Setting',205)
[void]$list.Columns.Add('Current',125); [void]$list.Columns.Add('Expected',125)
[void]$list.Columns.Add('Result',100); [void]$list.Columns.Add('Active file',380)
$form.Controls.Add($list)

function Show-Results($mode) {
    $list.Items.Clear()
    $custom = if ($mode -eq 'custom') { Load-ExpectedCustom } else { $null }
    if ($mode -eq 'custom' -and $null -eq $custom) { throw 'No randomized expected-values file exists. Run Snapshot + Randomize first.' }
    foreach ($spec in $script:Specs) {
        $path = Resolve-SpecPath $spec
        $current = if ($path) { Normalize-Value (Read-SpecValue $spec) } else { '<mod not installed>' }
        if ($mode -eq 'defaults') { $expected = Normalize-Value $spec.Default }
        elseif ($mode -eq 'custom') {
            $p = $custom.PSObject.Properties[$spec.Id]
            $expected = if ($null -ne $p) { Normalize-Value $p.Value } else { '<not randomized>' }
        } else { $expected = '' }
        $result = if (-not $path) { 'N/A' } elseif ($mode -eq 'current') { '' } elseif ($current -eq $expected) { 'PASS' } else { 'FAIL' }
        $item = New-Object Windows.Forms.ListViewItem($spec.Mod)
        [void]$item.SubItems.Add($spec.Setting); [void]$item.SubItems.Add((Display-Value $spec $current))
        [void]$item.SubItems.Add((Display-Value $spec $expected)); [void]$item.SubItems.Add($result)
        [void]$item.SubItems.Add($(if($path){$path}else{''}))
        if ($result -eq 'PASS') { $item.BackColor = [Drawing.Color]::PaleGreen }
        elseif ($result -eq 'FAIL') { $item.BackColor = [Drawing.Color]::MistyRose }
        elseif ($result -eq 'N/A') { $item.ForeColor = [Drawing.Color]::Gray }
        [void]$list.Items.Add($item)
    }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("Risa Hotkey Restore Tester")
    $lines.Add("Profile: $($script:ProfileName)")
    $lines.Add("Mode: $mode")
    $lines.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
    $lines.Add('')
    $lines.Add("Mod`tSetting`tCurrent`tExpected`tResult`tActive file")
    foreach ($row in $list.Items) {
        $values = @($row.SubItems | ForEach-Object { $_.Text -replace "`t", ' ' })
        $lines.Add(($values -join "`t"))
    }
    $script:LastReport = $lines -join [Environment]::NewLine
    [IO.File]::WriteAllText($script:LastResultsPath, $script:LastReport, [Text.UTF8Encoding]::new($false))
}

Add-Button 'Read Current Values' 12 165 {
    try { Show-Results 'current' } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Tester error') }
}
Add-Button 'Snapshot + Randomize' 185 175 {
    try {
        Assert-SkyrimClosed
        $answer = [Windows.Forms.MessageBox]::Show('Create a complete safety snapshot and randomize every installed managed setting?','Confirm test setup','YesNo','Warning')
        if ($answer -ne 'Yes') { return }
        $dir = New-SafetySnapshot
        Randomize-ManagedSettings
        Show-Results 'custom'
        [Windows.Forms.MessageBox]::Show("Randomized successfully.`nSafety snapshot: $dir",'Ready for Skyrim')
    } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Randomize failed') }
}
Add-Button 'Verify Custom Restore' 368 175 {
    try { Show-Results 'custom' } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Verification error') }
}
Add-Button 'Verify Supported Defaults' 551 195 {
    try { Show-Results 'defaults' } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Verification error') }
}
Add-Button 'Restore Safety Snapshot' 754 185 {
    try {
        Assert-SkyrimClosed
        $answer = [Windows.Forms.MessageBox]::Show('Restore every complete config file from the latest safety snapshot?','Confirm safety restore','YesNo','Warning')
        if ($answer -ne 'Yes') { return }
        Restore-SafetySnapshot; Show-Results 'current'; [Windows.Forms.MessageBox]::Show('Safety snapshot restored.','Complete')
    }
    catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Snapshot restore failed') }
}
Add-Button 'Copy Results' 947 140 {
    try {
        if ([string]::IsNullOrWhiteSpace($script:LastReport)) { Show-Results 'current' }
        [Windows.Forms.Clipboard]::SetText($script:LastReport)
        [Windows.Forms.MessageBox]::Show("Results copied to the clipboard and saved to:`n$($script:LastResultsPath)",'Results copied')
    } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Copy failed') }
}

if (-not $script:ModsRoot -and -not (Test-Path -LiteralPath (Join-Path $script:GameRoot 'SkyrimSE.exe'))) {
    [Windows.Forms.MessageBox]::Show("SkyrimSE.exe was not found in the Start In folder.`nSet this executable's Start In path to the Skyrim game folder in MO2.",'Incorrect working folder','OK','Warning') | Out-Null
}

Show-Results 'current'
[void]$form.ShowDialog()
