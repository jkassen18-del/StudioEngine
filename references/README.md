# Reference acapellas

Put the mastered acapellas you want Studio Engine to match here, one folder per
sound/artist, e.g.

```
references/
  my_sound/
    song1_acapella.wav
    song1_instrumental.wav   (optional: lets the matcher learn vocal-vs-beat level)
    song2_acapella.wav
```

* WAV only (16/24/32-bit PCM or 32-bit float). Convert MP3s to WAV first.
* Use the acapella exactly as it appears in the finished master: don't
  normalise or re-export it with processing.
* Two to five references of the same "sound" give a more reliable target than one.

Then build the target (see the main README):

```
python tools/reference_match/studio_match.py analyze references/my_sound/*_acapella.wav -o profiles/targets/my_sound.json
```
