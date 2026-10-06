# Reordered video fixture

`reordered_h264.mp4` contains four seconds of generated black video at 64x64,
30 FPS, two B-frames and closed two-second GOPs. The trim regression combines its
real encoded packets with two PCM audio tracks through the production Matroska
writer. No runtime encoder or external media tool is required by the test.

To regenerate the fixture with FFmpeg and libx264:

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i 'color=c=black:s=64x64:r=30:d=4' -c:v libx264 -preset medium -crf 28 -x264-params 'keyint=60:min-keyint=60:scenecut=0:bframes=2:open-gop=0' -an -y reordered_h264.mp4
```
