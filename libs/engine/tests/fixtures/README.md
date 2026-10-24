# Reordered video fixture

`reordered_h264.mp4` contains four seconds of generated black video at 64x64,
30 FPS, two B-frames and closed two-second GOPs. The trim regression combines its
real encoded packets with two PCM audio tracks through the production Matroska
writer. No runtime encoder or external media tool is required by the test.

To regenerate the fixture with FFmpeg and libx264:

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i 'color=c=black:s=64x64:r=30:d=4' -c:v libx264 -preset medium -crf 28 -x264-params 'keyint=60:min-keyint=60:scenecut=0:bframes=2:open-gop=0' -an -y reordered_h264.mp4
```

# Decoder format fixtures

`timestamp_av.mkv` contains one second of 192x108 H.264 test video at 30 FPS and stereo Opus audio at 48 kHz. Timestamp tests remux its packets with a common offset and compare video selection, keyframe positions and decoded audio. Negative Opus preroll does not imply a negative container origin in every FFmpeg version.

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i 'testsrc2=size=192x108:rate=30:duration=1' -f lavfi -i 'sine=frequency=1000:sample_rate=48000:duration=1' -map 0:v -map 1:a -c:v libx264 -preset medium -g 30 -bf 2 -pix_fmt yuv420p -x264-params 'colorprim=bt709:transfer=bt709:colormatrix=bt709' -color_range tv -c:a libopus -ac 2 -b:a 96k -y timestamp_av.mkv
```

The `h264_420_8.mp4`, `h264_444_8.mp4`, `hevc_420_10.mp4` and
`hevc_444_8.mp4` fixtures contain three 32x32 red frames at 10 FPS, with
BT.709 color tags. They exercise real decoding, planar normalization and
color conversion with hardware permitted and with software decoding forced.
Passing these tests does not prove that the host used hardware decoding.
The supported 4:4:4 recording profiles are 8-bit H.264 and HEVC.

`h264_420_8_full.mp4` contains three 32x32 High-profile H.264 frames at 10 FPS,
with full-range BT.709 tags. Its neutral gray halves have luma codes 64 and 192
and chroma codes 128. These values distinguish full-range conversion from
limited-range expansion. The pinned decoder emits YUVJ420P in software.

Regenerate from this directory with an external FFmpeg that has libx264 and libx265:

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i 'color=c=red:s=32x32:r=10:d=0.3' -pix_fmt yuv420p -c:v libx264 -x264-params 'keyint=1:bframes=0' -color_primaries bt709 -color_trc bt709 -colorspace bt709 -an -y h264_420_8.mp4
ffmpeg -hide_banner -loglevel error -f lavfi -i "nullsrc=s=32x32:r=10:d=0.3,format=yuv420p,geq=lum='if(lt(X,16),64,192)':cb=128:cr=128,setparams=range=full" -pix_fmt yuv420p -c:v libx264 -profile:v high -qp 18 -x264-params 'keyint=1:bframes=0:fullrange=on:colorprim=bt709:transfer=bt709:colormatrix=bt709' -color_range pc -color_primaries bt709 -color_trc bt709 -colorspace bt709 -an -y h264_420_8_full.mp4
ffmpeg -hide_banner -loglevel error -f lavfi -i 'color=c=red:s=32x32:r=10:d=0.3' -pix_fmt yuv444p -c:v libx264 -x264-params 'keyint=1:bframes=0' -color_primaries bt709 -color_trc bt709 -colorspace bt709 -an -y h264_444_8.mp4
ffmpeg -hide_banner -loglevel error -f lavfi -i 'color=c=red:s=32x32:r=10:d=0.3' -pix_fmt yuv420p10le -c:v libx265 -x265-params 'keyint=1:bframes=0:log-level=error:pools=1:frame-threads=1' -color_primaries bt709 -color_trc bt709 -colorspace bt709 -an -y hevc_420_10.mp4
ffmpeg -hide_banner -loglevel error -f lavfi -i 'color=c=red:s=32x32:r=10:d=0.3' -pix_fmt yuv444p -c:v libx265 -x265-params 'keyint=1:bframes=0:log-level=error:pools=1:frame-threads=1' -color_primaries bt709 -color_trc bt709 -colorspace bt709 -an -y hevc_444_8.mp4
```
