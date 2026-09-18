# Structural guard for the diagnostic baseline. This is NOT an in-game regression test.
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$baseline = '62cb33722793166f3454a5ba08e03c67279f4ef8'
$paths = @(
    'src/xrGame/ai/monsters/monster_corpse_memory.cpp'
    'src/xrGame/ai/monsters/dog/dog_state_manager.cpp'
    'src/xrGame/ai/monsters/basemonster/base_monster.cpp'
    'src/xrGame/ai/monsters/group_states/group_state_eat_eat_inline.h'
    'src/xrGame/ai/monsters/group_states/group_state_eat_drag_inline.h'
    'src/xrGame/entity_alive.cpp'
)

$changed = @(& git -C $root diff --name-only $baseline -- src)
if ($LASTEXITCODE -ne 0) {
    throw 'Cannot compare with the 2026.8.17 baseline. Fetch the upstream tags/history first.'
}
foreach ($path in $changed) {
    if ($path -notin $paths) {
        throw "Unexpected engine change outside diagnostic files: $path"
    }
}

# The closing brace must have the same indentation as the diagnostic guard.
# This also handles the nested, read-only state-transition condition.
$pattern = '(?m)^(?<indent>[ \t]*)if \(strstr\(Core\.Params, "-corpse_debug"\)\)\n\k<indent>\{\n(?s:.*?)^\k<indent>\}\n'
$blockCount = 0
foreach ($path in $paths) {
    $originalLines = @(& git -C $root show "${baseline}:$path")
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot read the baseline version of $path"
    }
    $original = ($originalLines -join "`n").TrimEnd("`n")
    $current = [IO.File]::ReadAllText((Join-Path $root $path)).Replace("`r`n", "`n")
    $matches = [regex]::Matches($current, $pattern)
    if ($matches.Count -eq 0) {
        throw "No diagnostic block found in $path"
    }
    $blockCount += $matches.Count
    $stripped = [regex]::Replace($current, $pattern, '').TrimEnd("`n")
    if ($stripped -cne $original) {
        throw "Non-diagnostic changes found in $path. The baseline must retain the original behavior."
    }
    Write-Host "PASS: $path ($($matches.Count) logging blocks; remaining source matches 2026.8.17)"
}

if ($blockCount -ne 10) {
    throw "Expected 10 diagnostic blocks, found $blockCount. Review changes before updating this assertion."
}
Write-Host 'PASS: structural baseline verification. Compilation and in-game reproduction are separate checks.'
