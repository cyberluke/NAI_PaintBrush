# Run the Paintbrush creative self-test with an opt-in LIVE TrackingService run.
#
# The test starts a real SAM3 track through TrackingService::startTrack() from
# the seed PTS of a real video (64320 ms for the NewJeans clip), waits for the
# asynchronous completion, verifies the persisted ObjectTrack (complete, >= 2
# keyframes, 8-bit masks at the seed frame size, monotonic PTS starting at the
# seed PTS, at least one propagated mask differing from the seed), then runs
# collectCreativePoses(6), Pastel Lineup and Motion Echo with one-undo checks.
#
# Pass -SeedMask with a real SAM3 mask for the seed frame (the live harness
# artifact live_mask_B.png is one for 64320 ms of the same clip) so keyframe 0
# is real too. Without it keyframe 0 is a clearly labelled synthetic
# Grayscale8 ellipse; the propagated keyframes remain real SAM3 output in both
# cases and the self-test report states which seed was used.
#
# Usage:
#   pwsh scripts/pb-creative-live-track.ps1 `
#     -Video "_REVIEW\harness\pb_live_harness\build\video_input.mkv" `
#     -SeedMask "_REVIEW\harness\pb_live_harness\build\live-tracked\live_mask_B.png" `
#     -SamModel "D:\_SATIN_AI\models\sam3-q4_0.ggml" -SamDevice cpu `
#     -LogFile "pb_creative_live_track.log"

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Video,
    [string]$SeedMask = "",
    [string]$SamModel = "",
    [ValidateSet("", "auto", "cpu", "gpu")]
    [string]$SamDevice = "",
    [string]$LogFile = "pb_creative_live_track.log"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Video)) {
    throw "video not found: $Video"
}
$resolvedVideo = (Resolve-Path -LiteralPath $Video).Path
$env:PB_CREATIVE_LIVE_TRACK_VIDEO = $resolvedVideo

# The synthetic-frame controller test is a different opt-in; keep this run
# scoped to the live TrackingService path.
Remove-Item Env:PB_CREATIVE_TRACK_VIDEO -ErrorAction SilentlyContinue

if ($SeedMask -ne "") {
    if (-not (Test-Path -LiteralPath $SeedMask)) {
        throw "seed mask not found: $SeedMask"
    }
    $env:PB_CREATIVE_LIVE_TRACK_SEED_MASK = (Resolve-Path -LiteralPath $SeedMask).Path
    Write-Output "live-track seed mask: $env:PB_CREATIVE_LIVE_TRACK_SEED_MASK"
} else {
    Remove-Item Env:PB_CREATIVE_LIVE_TRACK_SEED_MASK -ErrorAction SilentlyContinue
    Write-Output 'live-track seed mask: none (synthetic Grayscale8 ellipse; keyframe 0 is labelled seeded-synthetic)'
}

Write-Output "live-track video: $resolvedVideo"

$launchArgs = @{
    CreativeSelfTest = $true
    NoAutosave = $true
    LogFile = $LogFile
}
if ($SamModel -ne "") {
    if (-not (Test-Path -LiteralPath $SamModel)) {
        throw "SAM model not found: $SamModel"
    }
    $launchArgs.SamModel = (Resolve-Path -LiteralPath $SamModel).Path
}
if ($SamDevice -ne "") {
    $launchArgs.SamDevice = $SamDevice
}

& (Join-Path $PSScriptRoot 'launch-pb-noskip.ps1') @launchArgs
exit $LASTEXITCODE
