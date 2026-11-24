# Run the Paintbrush creative self-test with an opt-in real-video track.
#
# The test decodes three real PTS frames through the same edit source used by
# tracking, builds a three-keyframe ObjectTrack (synthetic foreground mask;
# real SAM3 masks are covered by _REVIEW/harness/pb_live_harness), then runs
# collectCreativePoses(), Pastel Lineup and Motion Echo with one-undo checks.
#
# Usage:
#   pwsh scripts/pb-creative-real-track.ps1 -Video "_REVIEW\harness\pb_live_harness\build\video_input.mkv"

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Video,
    [string]$LogFile = "pb_creative_real_track.log"
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Video)) {
    throw "video not found: $Video"
}
$env:PB_CREATIVE_TRACK_VIDEO = (Resolve-Path -LiteralPath $Video).Path
Write-Output "real-track video: $env:PB_CREATIVE_TRACK_VIDEO"
& (Join-Path $PSScriptRoot 'launch-pb-noskip.ps1') `
    -CreativeSelfTest -NoAutosave -LogFile $LogFile
exit $LASTEXITCODE
