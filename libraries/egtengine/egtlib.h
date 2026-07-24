#ifdef __cplusplus
extern "C"{
#endif

#ifndef EGTLIB_H
#define EGTLIB_H

	/* Number of channels (polyphony) */
#define FM_ch 24
	/* Number of operators */
#define FM_op 6


	enum{ FM_NOTE, FM_INSTR, FM_VOL, FM_FXTYPE, FM_FXVALUE };
	enum { EGT_ERR_FILEIO = -1, EGT_ERR_FILECORRUPTED = -2, EGT_ERR_FILEVERSION = -3 };
	enum fmInstrumentFlags{FM_INSTR_LFORESET=1, FM_INSTR_SMOOTH=2, FM_INSTR_TRANSPOSABLE=4};
	enum { FM_OP_ENVLOOP_BIT = 1, FM_OP_KSR_RELEASE_BIT = 2 };
	enum egtRenderTypes
	{
		EGT_RENDER_8, 
		EGT_RENDER_16, 
		EGT_RENDER_24, 
		EGT_RENDER_32, 
		EGT_RENDER_FLOAT,
		EGT_RENDER_PAD32=64
	};
	typedef struct fm_instrument_operator
	{
		unsigned char mult;
		unsigned char finetune;
		char detune;
		char vol;
		char connectOut;
		char connect;
		char connect2;

		char delay, i, a, h, d, s, r;

		char kbdVolScaling;

		char kbdAScaling;
		char kbdDScaling;
		char velSensitivity;
		char lfoFM;
		char lfoAM;
		char waveform;
		char fixedFreq;

		char kbdCenterNote;
		char kbdPitchScaling;

		char pitchInitialRatio;
		char pitchDecay;
		char pitchFinalRatio;
		char pitchRelease;

		char envLoop;
		char muted;

		unsigned char offset;

	}fm_instrument_operator;


	/* EGTI file format */
	typedef struct fm_instrument{
		char magic[4]; /* "EGTI" identifier */
		unsigned char dummy;
		unsigned char version;
		char name[24];
		char toMix[4];
		char feedback;
		char lfoDelay;
		char lfoSpeed;
		char lfoA;
		char lfoWaveform;
		char lfoOffset;
		char volume;
		char feedbackSource;
		char envReset;
		char tuning;
		char transpose;
		char phaseReset;
		char flags;
		char temperament[12];
		unsigned char kfx;
		fm_instrument_operator op[FM_op];
	}fm_instrument;



	typedef struct fm_cell{
		unsigned char note;
		unsigned char instr;
		unsigned char vol;
		unsigned char fx;
		unsigned char fxdata;
	}fm_cell;


	typedef struct fm_channel_state{
		float time;
		unsigned char tempo;
		unsigned char vol[FM_ch];
		unsigned char pan[FM_ch];
	}fm_channel_state;

	typedef struct fm_operator{
		// dynamic operator data
		float *connect, *connect2, *connectOut, *toMix;
		float out;
		float *waveform;
		unsigned int phase; // 10.10 bit phase accumulator (10 MSB used for sine lookup table)
		unsigned int pitch;
		float amp, ampDelta;



		float			env;
		unsigned		state;
		float				incr;
		float				pitchMod, pitchTime, pitchDestRatio;
		// static operator data
		float		kbdVolScaling;
		unsigned		envCount;
		float	vol;

		unsigned	delay, offset;
		float	i, a, h, d, s, r;
		float prevAmp, realAmp, ampTarget;
		float	lfoFM, lfoAM;
		float portaDestIncr;
		unsigned char id, mult, baseVol, kbdCenterNote;
		char baseA, baseD, baseR;
		char finetune, detune, fixedFreq, envLoop, pitchFinalRatio;
		float velSensitivity, volScaling;
	}fm_operator;


	typedef struct fm_channel{
		int newNote;

		// real time panning/volume
		float pan, vol;

		// used for smoothing panning/volume changes (avoid clicks)
		float destPan, destVol;

		// initial song channel pannings/volumes/reverb amounts
		unsigned char initial_pan, initial_vol, initial_reverb;


		float instrVol;
		float reverbSend;
		int muted;

		float *feedbackSource;
		float mixer;


		// DAHDSR envelope
		float currentEnvLevel;
		float	feedbackLevel;
		int op0;
		int realChannelNumber;
		float lfo;
		// channels
		unsigned int	lfoPhase, lfoMask, lfoIncr, lfoWaveform;
		unsigned algo;
		unsigned char note, baseArpeggioNote;
		float arpTimer;
		int arpIter;
		fm_instrument* instr;
		unsigned char instrNumber;
		float ramping;
		float rampingPicture;
		float lfoEnv, lfoA, lfoFMCurrentValue, lfoAMCurrentValue;
		unsigned lfoDelayCpt, lfoDelayCptMax, lfoOffset;
		unsigned char fxActive, fxData, noteVol, untransposedNote;
		char transpose;
		unsigned active;


		float fadeFrom, fadeFrom2, fadeIncr, fade, tuning;
		float lastRender, lastRender2, delta;
		float pitchBend;
		fm_instrument* cInstr;
		fm_operator op[FM_op];

	}fm_channel;


	typedef struct egtsynth{
		char songName[64], author[64], comments[256];
		float globalVolume;
		float playbackVolume;
		unsigned char _globalVolume;
		fm_instrument *instrument; // here are stored your instruments
		unsigned char instrumentCount;
		char transpose, looping;
		unsigned char loopCount;
		int channelStatesDone;

		// reverb
		unsigned reverbPhaseL, reverbPhaseL2, reverbPhaseR, reverbPhaseR2;
		float* revBuf;
		unsigned revBufSize;

		unsigned allpassPhaseL, allpassPhaseR, allpassPhaseL2, allpassPhaseR2;

		unsigned allpassMod, allpassMod2, reverbMod1, reverbMod2, reverbMod3, reverbMod4;
		unsigned revOffset2, revOffset3, revOffset4, revOffset5, revOffset6, revOffset7;

		float reverbRoomSize, initialReverbRoomSize;
		float reverbLength, initialReverbLength;

		unsigned char tempo, initial_tempo;
		unsigned row, order, playing, saturated;

		float noConnect;
		fm_channel_state **channelStates;
		fm_cell(**pattern)[FM_ch];
		unsigned patternCount;
		unsigned *patternSize;
		unsigned frameTimer;
		float frameTimerFx;

		float outL, outR;
		unsigned char diviseur;
		float sampleRateRatio;


		unsigned sampleRate;
		float noteIncr[128];


		fm_channel ch[FM_ch];

		float transitionSpeed;
		int tempRow, tempOrder;
		unsigned readSeek, totalFileSize;
	}egtsynth;


	/** Creates the synth
		@param samplerate : sample rate in Hz
		@return pointer to the allocated fm synth
		*/
	egtsynth* egt_create(int samplerate);

	void egt_destroy(egtsynth* egt);

	/** Load a song
		@param filename
		@param rate : non-zero to import filename as an id Software IMF file,
			using rate as the timer tick rate in Hz (250, 560, or 700).
			0 autodetects any other supported song format.
		@return 1 if ok, 0 if failed
		*/
	int egt_loadSong(egtsynth* egt, const char* filename, int rate);



	/** Load a song
		@param data
		@param len
		@param rate : non-zero to import data as an id Software IMF file,
			using rate as the timer tick rate in Hz (250, 560, or 700).
			0 autodetects any other supported song format.
		@return 1 if ok, 0 if failed
		*/
	int egt_loadSongFromMemory(egtsynth* egt, char* data, unsigned len, int rate);

	/** Get total song length
		@return length in seconds
		*/
	float egt_getSongLength(egtsynth* egt);

	/** Play the song */
	void egt_play(egtsynth* egt);

	/** Stop the song
		@param mode : 0 = force note off, 1 = hard cut
		*/
	void egt_stop(egtsynth* egt, int mode);


	void egt_setPlaybackVolume(egtsynth *egt, int volume);

	/** Set the global volume
		@param volume : volume, 0-99
		*/
	void egt_setVolume(egtsynth *egt, int volume);

	/** Render the sound
		@param buffer : audio buffer, left and right channels are interleaved
		@param length : number of samples to render
		@param channels : number of channels to output (1 and 2 are the only valid values)
		*/
	void egt_render(egtsynth* egt, void* buffer, unsigned length, unsigned type);

	/** Play a note
		@param instrument : instrument number, 0-255
		@param note : midi note number, 0-127 (C0 - G10)
		@param channel : channel number, 0-23
		@param volume : volume, 0-99
		*/
	void egt_playNote(egtsynth* egt, unsigned instrument, unsigned note, unsigned channel, unsigned volume);

	/** Stops a note on a channel
		@param channel : channel number, 0-23
		*/
	void egt_stopNote(egtsynth* egt, unsigned channel);





	/** Set the playing position
		@param pattern : pattern number
		@param row : row number
		@param mode : 0 = keep playing notes, 1 = force note off, 2 = hard cut
		*/
	void egt_setPosition(egtsynth* egt, int pattern, int row, int mode);

	/** Set the playing position in seconds
		@param time : position in seconds
		@param mode : 0 = keep playing notes, 1 = force note off, 2 = hard cut
		*/
	void egt_setTime(egtsynth* egt, int time, int mode);

	/** Set the tempo
		@param tempo : tempo in BPM, 0-255
		*/
	void egt_setTempo(egtsynth* egt, int tempo);


	/** Get the current playing position
		@param *order : current pattern number
		@param *row : current row number
		*/
	void egt_getPosition(egtsynth* egt, int *pattern, int *row);

	/** Get the playing time in seconds
		@return current playing time in seconds
		*/
	float egt_getTime(egtsynth* egt);

	/** Set the sample rate
		@param samplerate : sample rate in Hz
		@return 1 if ok, 0 if failed (keeps the previous sample rate)
		*/
	int egt_setSampleRate(egtsynth* egt, int samplerate);

	/** Set channel volume
		@param channel : channel number, 0-23
		@param volume : volume, 0-99
		*/
	void egt_setChannelVolume(egtsynth *egt, int channel, int volume);

	/** Set channel panning
		@param channel : channel number, 0-23
		@param panning : panning, 0-255
		*/
	void egt_setChannelPanning(egtsynth *egt, int channel, int panning);

	/** Set channel reverb
		@param channel : channel number, 0-23
		@param panning : reverb amount, 0-99
		*/
	void egt_setChannelReverb(egtsynth *egt, int channel, int reverb);

	/** Write data to a pattern
		@param pattern : pattern number
		@param row : row number
		@param channel : channel number
		@param type : the column to write into (FM_NOTE, FM_INSTR, FM_VOL, FM_FXTYPE, FM_FXVALUE)
		@param value : the value to write. 255 is considered empty
		@return 1 if success, 0 if failed (pattern/row/channel/type out of bounds)
		*/
	int egt_write(egtsynth *egt, unsigned pattern, unsigned row, unsigned channel, fm_cell data);

	/** Create a new pattern at the desired position
		@param rows : number of rows
		@param position : the position where to insert the pattern
		@return 1 if success, 0 if failed
		*/
	int egt_insertPattern(egtsynth* egt, unsigned rows, unsigned position);

	/** Remove a pattern
		@param pattern : the pattern number
		@return 1 if success, 0 if failed
		*/
	int egt_removePattern(egtsynth* egt, unsigned pattern);

	/** Resize a pattern. Contents are not stretched/scaled.
		@param pattern : the pattern number
		@param size : the new size
		@return 1 if success, 0 if failed
		*/
	int egt_resizePattern(egtsynth* egt, unsigned pattern, unsigned size, unsigned scaleContent);







	/* ########################################################### */

	/* Stops a note by its number */
	void egt_stopNoteID(egtsynth* egt, unsigned note);



	void egt_clearSong(egtsynth* egt);

	int egt_getPatternSize(egtsynth *egt, int pattern);

	int egt_resizeInstrumentList(egtsynth* egt, unsigned size);
	void egt_portamento(egtsynth* egt, unsigned channel, float value);


	void egt_patternClear(egtsynth* egt);
	int egt_resizePatterns(egtsynth* egt, unsigned count);
	/** Load an instrument
		@param filename : path to an .egti file
		@return 0 if ok, EGT_ERR_* if failed
		*/
	int egt_loadInstrument(egtsynth* egt, const char *filename, unsigned slot);
	int egt_loadInstrumentFromMemory(egtsynth* egt, char *data, unsigned len, unsigned slot);

	/** Load an instrument bank
		@param filename : path to an .egtb/.mdtb file
		@return 0 if ok, EGT_ERR_* if failed
		*/
	int egt_loadInstrumentBank(egtsynth* egt, const char *filename);
	int egt_loadInstrumentBankFromMemory(egtsynth* egt, char *data, unsigned len);
	int egt_saveInstrument(egtsynth* egt, const char* filename, unsigned slot);
	int egt_saveInstrumentBank(egtsynth* egt, const char* filename);
	void egt_removeInstrument(egtsynth* egt, unsigned slot, int removeOccurences);
	void egt_movePattern(egtsynth* egt, int from, int to);
	/* Saves the song to file */
	int egt_saveSong(egtsynth* egt, const char* filename);


	void egt_buildStateTable(egtsynth* egt, unsigned orderStart, unsigned orderEnd, unsigned channelStart, unsigned channelEnd);
	int egt_initReverb(egtsynth *egt, float roomSize);

	/* Forces all sound to stop. Cut notes and reverb. */
	void egt_stopSound(egtsynth* egt);
	void egt_moveChannels(egtsynth* egt, int from, int to);
	int egt_clearPattern(egtsynth* egt, unsigned pattern, unsigned rowStart, unsigned count);
	int egt_insertRows(egtsynth *egt, unsigned pattern, unsigned row, unsigned count);
	int egt_removeRows(egtsynth *egt, unsigned pattern, unsigned row, unsigned count);


	float egt_volumeToExp(int volume);

	int egt_isInstrumentUsed(egtsynth *egt, unsigned id);
	void egt_createDefaultInstrument(egtsynth* egt, unsigned slot);

#endif

#ifdef __cplusplus
}
#endif