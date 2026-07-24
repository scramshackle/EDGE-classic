# Adding EDGETracker song playback to your software

- Copy the files from this folder to your project's folder
- Include "egtlib.h" to your C/C++ code
- Create the EGT engine with the desired playback frequency
```
egtsynth* mt = egt_create(44100);
```

- Load some song 
```
// from a file
egt_loadSong(mt,"mysong.egts");

// from memory
egt_loadSongFromMemory(mt, char* data, unsigned length)
```
- Play !
```
egt_play(mt);
```

To get the sound output, you need to request it using the egt_render function. Usually, the framework/sound library you use will give you some callback function for that.

```
void myAudioCallback(short *out, int nbFrames){

  // nbFrames*2 because the output is stereo
  // EGT_RENDER_16 will output 16 bit signed short
  // There are also EGT_RENDER_24, EGT_RENDER_FLOAT and other other formats available
  
  egt_render(mt,out,nbFrames*2,EGT_RENDER_16);

}
```

- Change the output volume
```
egt_setPlaybackVolume(mt,int volume); // 0 to 99
```

- Get the song length in seconds
```
float songLength = egt_getSongLength(mt);
```

- Set the playback position
```
// pattern is the index of the pattern to seek at
// row is the row number
// cutMode tells how the playing notes are affected : 0 = keep playing notes, 1 = force note off, 2 = hard cut
egt_setPosition(mt, int pattern, int row, int cutMode);
```

- Get the playback position
```
int currentPattern, currentRow;
egt_getPosition(mt, &currentPattern, &currentRow);
```

- Change the song tempo
```
// tempo is in BPM, from 1 to 255. Setting may be overrided by the song if some pattern use Tempo commands
egt_setTempo(mt, int tempo);
```

- Once you are tired of this
```
egt_destroy(mt); // free resources allocated with egt_create

```

There are a lot more functions in egtlib.h, take a look at it for more informations
