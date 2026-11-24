# FM1 Sampler 0.42.3 — Song Export Status (Android preview)

- SONG screen: EXPORT SONG becomes orange and reads EXPORTING immediately after a valid request. It stays orange during DSP rendering and MediaStore publication.
- SONG EXPORTED remains orange after a successful public Downloads write. The button can then export again. Errors show EXPORT FAILED; an empty arrangement is still rejected. Duplicate taps during an active export are ignored.
- Both Light and Dark use the existing theme accent. Loading another project clears the previous result; loading is temporarily blocked during song export.
- Retains decimal tempo, live pad editing, sample library access and project compatibility. No project format or signing identity changes; Android versionCode 69.
- See [README](../README.md) and [0.42.2 notes](RELEASE_NOTES_0422.md).

