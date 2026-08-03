# Banner and icon assets

These are what the HOME Menu shows for an installed title. Each edition has its own banner and icon; the banner music
is cut from your own disc at build time and is not in this repository.

| File | Requirement | Where it comes from |
| --- | --- | --- |
| `<edition>/banner.png` | 256x128 | Here |
| `<edition>/icon.png` | 48x48 | Here |
| `<edition>/banner.wav` | 16 bit PCM, stereo, 16364 Hz, at most 3.00s (49,092 frames) | Built from your disc |

## Banner music

The build takes three seconds out of the second track of your disc - the first CD audio track - and converts it:

```
ffmpeg -f s16le -ar 44100 -ac 2 -ss 5 -i <track 2>.bin \
       -t 3 -af "volume=0.55,afade=t=in:st=0:d=0.10,afade=t=out:st=2.75:d=0.25" \
       -ar 16364 -ac 2 -c:a pcm_s16le <edition>/banner.wav
```

**16 bit stereo PCM, 16364 Hz, no longer than three seconds.** These are not preferences. Anything outside them does
not fail and does not warn: the banner simply plays ringing noise instead of music. Two things were tried before
landing here - 32728 Hz, because that is the rate the 3DS sound hardware actually runs at, and then 16364 Hz at a
track's full length - and both came out as noise. 16364 Hz at three seconds is what other 3DS homebrew with working
banner music uses, and copying that exactly is what worked.

`tools/3ds/check_banner_audio.py` checks a file against those numbers, and the build runs it on whatever it produces.

The fade is so the loop does not click, since three seconds cuts into the middle of a track.

## Replacing the artwork

Drop in your own `banner.png` and `icon.png` at the sizes above. The build never overwrites artwork that is already
there - it only fills in what is missing.
