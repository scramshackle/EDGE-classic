#include "egtlib.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#endif

#ifdef _WIN32
    #include <float.h>
#endif

/* Current version of instrument/song formats */
#define EDGETRACKER_VERSION 1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define _99TO1 1/99
#define _24TO1 1/24
#define _2400TO1 1/2400
#define SEMITONE_RATIO 0.059463 * 0.01 /* 0.059463 = ratio between two semitones https://en.wikipedia.org/wiki/Twelfth_root_of_two */

#ifdef _MSC_VER
#undef min
#undef max
#endif

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define clamp(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

/* Sine wave lookup table size */

#define LUTsize 2048
#define LUTratio (LUTsize / 1024)

/* Reverb delays, in number of samples at 48000Hz. Automatically scaled for other samples rates. */

#define REVERB_DELAY_L1 1.6*4096 // 85ms
#define REVERB_DELAY_L2 1.5*2485 // 72 
#define REVERB_DELAY_R1 1.6*3801 // 79
#define REVERB_DELAY_R2 1.5*2333 // 69
#define REVERB_ALLPASS2 1.5*1170 // 5.5
#define REVERB_ALLPASS1 1.5*2508 // 7.7ms


static float wavetable[10][LUTsize];


/* Exponential tables for envelopes and volumes scales */

static float expEnv[100], expVol[100], expVolOp[100];
// waveforms
static const unsigned int lfoMasks[28] = {
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xf0000 * LUTratio,  // less reso
	0xefc00 * LUTratio,  // less reso
	0xdfc00 * LUTratio,  // less
	0xbfc00 * LUTratio,  // squarelike
	0x88000 * LUTratio, // high freq
	0x40000 * LUTratio, // square
	0x60000 * LUTratio, // pulse/near square
	0x7fc00 * LUTratio, // abs sine
	0x78000 * LUTratio, // abs sine less reso
	0x70000 * LUTratio, // less reso
	0x3fc00 * LUTratio, // saw
	0xa0000 * LUTratio, // ? 10
	0xfffc00 * LUTratio, // sine
	0x2ffc00 * LUTratio, // sine
};

static const unsigned int lfoWaveforms[28] = {
	0,
	1,
	2,
	3,
	4,
	5,
	6,
	7,
	0,  // less reso
	0,  // less reso
	0,  // less
	0,  // squarelike
	0, // high freq
	0, // square
	0, // pulse/near square
	0, // abs sine
	0, // abs sine less reso
	0, // less reso
	0, // saw
	0, // ? 10
	0, // sine
	0, // sine
};


/* Band-limited triangle, square and sawtooth generators */

float trg(float x, float theta) { return 1 - 2 * acos((1 - theta) * sin(2 * M_PI*x)) / M_PI; }

float sqr(float x, float theta) { return 2 * atan(sin(2 * M_PI* x) / theta) / M_PI; }

float swt(float x, float theta) { return (1 + trg((2 * x - 1) / 4, theta) * sqr(x / 2, theta)) / 2; }

void egt_setDefaults(egtsynth* egt)
{

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		egt->ch[ch].note = 255;
		egt->ch[ch].instrNumber = 255;
		egt->ch[ch].vol = expVol[99];
		egt->ch[ch].initial_vol = 99;
		egt->ch[ch].reverbSend = 0;
		egt->ch[ch].destPan = egt->ch[ch].pan = egt->ch[ch].initial_pan = 127;
		egt->ch[ch].noteVol = 99;
	}

	egt_setVolume(egt, 60);
	egt->initial_tempo = 120;
	egt->diviseur = 4;
	egt->initialReverbLength = egt->reverbLength = 0.875;
	egt->initialReverbRoomSize = 0.55;
	egt->looping = -1;
	egt->channelStatesDone = 0;
	egt->playbackVolume = 1;
}



static unsigned int g_seed = 0;
int fast_rand(void)
{
	g_seed = (214013 * g_seed + 2531011);
	return (g_seed >> 16) & 0x7FFF;
}

void egt_destroy(egtsynth* egt)
{
	free(egt->revBuf);
	free(egt->instrument);
	for (unsigned i = 0; i < egt->patternCount; i++)
	{
		free(egt->pattern[i]);
		free(egt->channelStates[i]);
	}
	free(egt->patternSize);
	free(egt->pattern);
	free(egt->channelStates);
	free(egt);
}

egtsynth* egt_create(int _sampleRate)
{
	egtsynth *egt = calloc(1, sizeof(egtsynth));

	if (egt)
	{
        /* Flush denormals to zero for audio performance.
           Denormal floats can cause 10-100x slowdowns in audio code. */
        #if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
            /* x86/x64: Use SSE flush-to-zero and denormals-are-zero modes */
            _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
            #ifdef _MM_DENORMALS_ZERO_ON
                _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
            #endif
            #ifdef _WIN32
                /* Round floats down (needed for FISTP fast int/float conversion) */
                _controlfp(_RC_DOWN, _MCW_RC);
            #endif
        #elif defined(__aarch64__)
            #if defined(_MSC_VER)
                unsigned __int64 fpcr = _ReadStatusReg(ARM64_FPCR);
                fpcr |= (1ULL << 24);
                _WriteStatusReg(ARM64_FPCR, fpcr);
            #else
                uint64_t fpcr;
                __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
                fpcr |= (1 << 24);
                __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
            #endif
        #endif
        /* Note: ARM32 with VFP can use fpscr, but most ARM32 targets
           are legacy and -ffast-math handles this at compile time.
           For other platforms, compile with -ffast-math if denormal
           performance is a concern. */


		/* Build waveform tables */

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[0][i] = sin(i * 2 * M_PI / LUTsize);					// 0 sine

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[1][i] = (swt((float)(i + LUTsize / 2) / LUTsize, 0.2) - 0.5)*2.5*(1 / 0.464670);	// 3 soft saw

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[2][i] = (swt((float)(i + LUTsize / 2) / LUTsize, 0.05) - 0.5) * 2 * (1 / 0.649969);	// 4 saw

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[3][i] = trg((float)i / LUTsize, 0.01)*(1 / 0.909893);			// 1 triangle

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[4][i] = sqr((float)i / LUTsize, 0.1)*0.7*(1 / 0.655584);			// 2 square

		for (unsigned i = 0; i < LUTsize / 2; i++)
			wavetable[5][i] = sin(i * 2 * M_PI / (LUTsize / 2));				// 5 double sine

		for (unsigned i = 0; i < LUTsize / 2; i++)
			wavetable[6][i] = sin(i * 2 * M_PI / LUTsize);					// 6 half period sine

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[7][i] = fast_rand() / 16383.5 - 0.5;

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[8][i] = (float)fabs(sin(i * 2 * M_PI / LUTsize));	// 8 abs-sine (OPL2 ws=2)

		for (unsigned i = 0; i < LUTsize / 4; i++) {
			float v = (float)sin(i * 4 * M_PI / LUTsize);
			wavetable[9][i]                  = v;	// 9 pulsed abs-sine (OPL2 ws=3)
			wavetable[9][i + LUTsize / 2]    = v;	// second hump at 3rd quarter
		}

		/*for (int i = 0; i < 7; i++){
			float max=0;
			for (int j = 0; j< LUTsize; j++){
			if (wavetable[i][j]>max){
			max = wavetable[i][j];

			}
			}
			printf("max %d is %f\n", i, max);
			}*/



		/* Build exponential tables for volume/envelopes */

		float ini = 0.00001;
		for (unsigned i = 1; i < 99; i++)
		{
			expVol[i] = pow(10, (log(100.0 / (i + 1)) * (-10)) / 20.0);
			expEnv[i] = ini;
			ini *= 1.1;
			expVolOp[i] = expVol[i] * (i*0.01);
		}

		expEnv[96] = 0.1;
		expEnv[97] = 0.2;
		expEnv[98] = 0.5;
		expEnv[99] = expVol[99] = expVolOp[99] = 1;

		egt_setDefaults(egt);

		if (!egt_setSampleRate(egt, _sampleRate))
		{
			free(egt);
			return 0;
		}
	}

	return egt;
}

int egt_initReverb(egtsynth *egt, float roomSize)
{
	/* Initialize reverb parameters and buffers */

	egt->reverbPhaseL = egt->reverbPhaseL2 = egt->reverbPhaseR = egt->reverbPhaseR2 = egt->allpassPhaseL = egt->allpassPhaseR = egt->allpassPhaseL2 = egt->allpassPhaseR2 = 0;

	unsigned mod1 = roomSize*REVERB_DELAY_L1 / egt->sampleRateRatio; // 85ms
	unsigned mod2 = roomSize* REVERB_DELAY_L2 / egt->sampleRateRatio; // 72 
	unsigned mod3 = roomSize*REVERB_DELAY_R1 / egt->sampleRateRatio; // 79
	unsigned mod4 = roomSize*REVERB_DELAY_R2 / egt->sampleRateRatio; // 69
	unsigned mod5 = (roomSize*REVERB_ALLPASS1) / egt->sampleRateRatio; // 5.5
	unsigned mod6 = (roomSize*REVERB_ALLPASS2) / egt->sampleRateRatio; // 7.7ms

	unsigned revBufSize = mod1 + mod2 + mod3 + mod4 + 2 * (mod5 + mod6);

	float* newR = realloc(egt->revBuf, sizeof(float)*revBufSize);

	if (!newR)
	{
		return 0;
	}

	egt->reverbRoomSize = roomSize;
	egt->revBufSize = revBufSize;
	egt->revBuf = newR;

	memset(egt->revBuf, 0, sizeof(float)*revBufSize);



	egt->reverbMod1 = mod1;
	egt->reverbMod2 = mod2;
	egt->reverbMod3 = mod3;
	egt->reverbMod4 = mod4;
	egt->allpassMod = mod5;
	egt->allpassMod2 = mod6;

	egt->revOffset2 = egt->reverbMod1 + egt->reverbMod2;
	egt->revOffset3 = egt->revOffset2 + egt->reverbMod3;
	egt->revOffset4 = egt->revOffset3 + egt->reverbMod4;
	egt->revOffset5 = egt->revOffset4 + egt->allpassMod;
	egt->revOffset6 = egt->revOffset5 + egt->allpassMod;
	egt->revOffset7 = egt->revOffset6 + egt->allpassMod2;
	return 1;
}

int egt_setSampleRate(egtsynth* egt, int sampleRate)
{

	egt->sampleRate = sampleRate;
	egt->sampleRateRatio = 48000.0 / sampleRate;
	egt->transitionSpeed = 20 * (1 / egt->sampleRateRatio);

	/* initialize the MIDI note frequencies table (converted into phase accumulator increments) */

	for (unsigned x = 0; x < 128; ++x)
		egt->noteIncr[x] = pow(2, (x - 9.0) / 12.0) / sampleRate * 32840 * 440 * LUTratio;

	/* Reset instruments so their values can be regenerated */
	for (unsigned ch = 0; ch < FM_ch; ++ch)
		egt->ch[ch].cInstr = 0;

	return egt_initReverb(egt, egt->initialReverbRoomSize);
}

/* Calculates the volume of each operator */
void egt_calcOpVol(fm_operator *o, int note, int volume)
{

	volume = clamp(volume, 0, 99);
	float noteScaling = 1 + (note - o->kbdCenterNote)*o->volScaling;
	float opVol = (expVol[volume] * o->velSensitivity + (1 - o->velSensitivity))*expVolOp[o->baseVol];
	o->vol = clamp(opVol * noteScaling, 0, 1) * 5000 * LUTratio;

}

/* Calculates the pitch of each operator */
void egt_calcPitch(egtsynth* egt, int ch, int note)
{

	note = clamp(note + egt->ch[ch].transpose + egt->transpose * ((egt->ch[ch].instr->flags & FM_INSTR_TRANSPOSABLE) >> 2), 0, 127);

	egt->ch[ch].note = note;

	float frequency = egt->noteIncr[note] + egt->noteIncr[note] * SEMITONE_RATIO* egt->ch[ch].instr->temperament[note % 12];

	for (unsigned op = 0; op < FM_op; ++op)
	{
		fm_operator* o = &egt->ch[ch].op[op];

		if (o->fixedFreq == 0)
		{
			o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1);
		}
		/* Fixed frequency */
		else
			o->incr = (o->mult * o->mult + (float)o->mult *(float)o->finetune*_24TO1) * LUTratio*egt->sampleRateRatio;

		o->incr += o->incr*egt->ch[ch].tuning;
	}
}


void egt_initChannels(egtsynth* egt)
{
	if (egt->order >= egt->patternCount || egt->row >= egt->patternSize[egt->order])
		return;

	egt->tempo = egt->channelStates[egt->order][egt->row].tempo;
	egt->globalVolume = expVol[egt->_globalVolume] * 4096 / LUTsize;
	egt->reverbLength = egt->initialReverbLength;
	if (egt->initialReverbRoomSize != egt->reverbRoomSize)
	{
		egt_initReverb(egt, egt->initialReverbRoomSize);
	}

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		egt->ch[ch].cInstr = 0;
		egt->ch[ch].pan = egt->ch[ch].destPan = egt->channelStates[egt->order][egt->row].pan[ch];
		egt->ch[ch].vol = expVol[egt->channelStates[egt->order][egt->row].vol[ch]];
		egt->ch[ch].reverbSend = expVol[egt->ch[ch].initial_reverb];
		egt->ch[ch].pitchBend = 1;
		egt->ch[ch].fadeFrom=0;
		egt->ch[ch].fadeFrom2=0;
		egt->ch[ch].currentEnvLevel=0;
	}
}




void _egt_render(egtsynth* egt, float* buffer, unsigned length)
{

	unsigned b = 0;
	while (b < length)
	{
		// player
		if (egt->playing)
		{
			/* Song frame tick */
			if (egt->frameTimer == 0)
			{

				for (unsigned ch = 0; ch < FM_ch; ++ch)
				{
					fm_cell* row = &egt->pattern[egt->order][egt->row][ch];
					egt->ch[ch].fxActive = 0;

					/* Note stop ? */
					if (row->note == 128 && row->fx != 'D')
					{
						egt_stopNote(egt, ch);
					}
					/* Note play ? */
					else if (row->note != 255)
					{
						// portamento or note delay : don't play the note now !
						if (row->fx != 'D' && (row->fx != 'G' || row->fx == 'G' && egt->ch[ch].note == 255))
						{
							egt_playNote(egt, row->instr, row->note, ch, row->vol);
							egt->ch[ch].baseArpeggioNote = row->note;
						}
					}
					// only volume change
					else if (row->vol != 255 && egt->ch[ch].instr)
					{
						egt->ch[ch].noteVol = row->vol;
						for (unsigned op = 0; op < FM_op; ++op)
						{
							egt_calcOpVol(&egt->ch[ch].op[op], egt->ch[ch].note, row->vol);
						}
					}

					/* Handle effects (after note actions) */

					egt->ch[ch].fxData = row->fxdata;
					switch (row->fx)
					{

						case 'B': // jump pattern
							egt->tempOrder = egt->ch[ch].fxData;
							break;
						case 'C': // jump row
							egt->tempRow = egt->ch[ch].fxData;
							break;
						case 'G': // portamento
							// update portamento dest frequency if note set
							if (row->note != 255)
							{
								for (unsigned op = 0; op < FM_op; ++op)
								{
									float pitchScaling = 1 + ((int)row->note - egt->ch[ch].instr->op[op].kbdCenterNote)*egt->ch[ch].instr->op[op].kbdPitchScaling*0.001;

									if (egt->ch[ch].instr->op[op].fixedFreq == 0)
									{
										egt->ch[ch].op[op].portaDestIncr = egt->noteIncr[clamp(row->note+egt->transpose,0,127)] * pitchScaling*(egt->ch[ch].op[op].mult + (double)egt->ch[ch].op[op].finetune*0.041666666667 + (double)egt->ch[ch].op[op].detune*0.00041666666667);

									}
									else // fixed frequency
										egt->ch[ch].op[op].portaDestIncr = (egt->ch[ch].op[op].mult * (egt->ch[ch].op[op].mult) + (double)egt->ch[ch].op[op].mult*(double)egt->ch[ch].op[op].finetune*0.041666666667) * LUTratio;
								}
							}
							// repeated effects
						case 'A': // arpeggio
						case 'D': // delay
						case 'E': // portamento up
						case 'F': // portamento down
						case 'W': // global volume slide
						case 'P': // panning slide

							egt->ch[ch].arpTimer = 0;
							egt->ch[ch].arpIter = 0;
							egt->ch[ch].fxActive = row->fx;
							break;
						case 'Q': // retrigger note
							if (egt->ch[ch].fxData > 0)
							{
								egt->ch[ch].arpTimer = 24 / egt->ch[ch].fxData;
								egt->ch[ch].arpIter = 0;
								egt->ch[ch].fxActive = row->fx;
							}
							break;
						case 'H': // vibrato
							egt->ch[ch].lfoEnv = 1;
							egt->ch[ch].lfoIncr = (egt->ch[ch].fxData / 16 * 128)*LUTratio;
							for (unsigned op = 0; op < FM_op; ++op)
							{
								egt->ch[ch].op[op].lfoFM = (egt->ch[ch].fxData % 16)*0.003;
							}
							break;
						case 'I': // pitch bend
							egt->ch[ch].fxActive = 'I';
							egt->ch[ch].pitchBend = 1 - (float)(128 - egt->ch[ch].fxData) * 0.00092852373168154813872606848242328;
							break;
						case 'J': // tremolo
							egt->ch[ch].lfoEnv = 1;
							egt->ch[ch].lfoIncr = (egt->ch[ch].fxData / 16 * 128)*LUTratio;
							for (unsigned op = 0; op < FM_op; ++op)
							{
								egt->ch[ch].op[op].lfoAM = (egt->ch[ch].fxData % 16)*(1.0 / 16);
							}
							break;
						case 'K':
							if (!egt->ch[ch].instr)
								break;
							/* Global instrument edit*/
							if (egt->ch[ch].instr->kfx / 32 == 0)
							{
								switch (egt->ch[ch].instr->kfx)
								{
									case 0:
										egt->ch[ch].instrVol = expVol[min(99, egt->ch[ch].fxData)];
										break;
									case 1:
										egt->ch[ch].transpose = clamp((char)egt->ch[ch].fxData, -12, 12);
										egt_calcPitch(egt, ch, egt->ch[ch].untransposedNote);
										break;
									case 2:
										egt->ch[ch].tuning = 0.0006*clamp((char)egt->ch[ch].fxData, -100, 100);
										egt_calcPitch(egt, ch, egt->ch[ch].untransposedNote);
										break;
									case 3:
										egt->ch[ch].lfoIncr = 1 + expVol[clamp(egt->ch[ch].fxData, 0, 99)] * expVol[clamp(egt->ch[ch].fxData, 0, 99)] * 5000 * egt->sampleRateRatio*LUTratio;
										break;
									case 4:
										egt->ch[ch].lfoDelayCptMax = expVol[clamp(egt->ch[ch].fxData, 0, 99)] * expVol[clamp(egt->ch[ch].fxData, 0, 99)] * 200000 * egt->sampleRateRatio;
										break;
									case 5:
										egt->ch[ch].lfoA = expEnv[clamp(egt->ch[ch].fxData, 0, 99)] * egt->sampleRateRatio;
										break;
									case 6:
										egt->ch[ch].lfoMask = lfoMasks[clamp(egt->ch[ch].fxData, 0, 19)];
										egt->ch[ch].lfoWaveform = lfoWaveforms[clamp(egt->ch[ch].fxData, 0, 19)];
										break;
									case 7:
										egt->ch[ch].lfoOffset = clamp(egt->ch[ch].fxData, 0, 31) * LUTsize / 32;
										break;

								}
							}
							/* Operator edit */
							else
							{
								fm_operator *o = &egt->ch[ch].op[egt->ch[ch].instr->kfx / 32 - 1];

								switch (egt->ch[ch].instr->kfx % 32)
								{
									case 0:
										o->baseVol = min(99, egt->ch[ch].fxData);
										egt_calcOpVol(o, egt->ch[ch].note, egt->ch[ch].noteVol);
										break;
									case 1:
										o->baseVol = o->vol * !egt->ch[ch].instr->op[egt->ch[ch].instr->kfx / 32 - 1].muted;
										egt_calcOpVol(o, egt->ch[ch].note, egt->ch[ch].noteVol);
										break;
									case 2:
										o->waveform = wavetable[clamp(egt->ch[ch].fxData, 0, 7)];
										break;
									case 3:{
											   o->mult = clamp(egt->ch[ch].fxData, 0, 40);
											   float frequency = egt->noteIncr[egt->ch[ch].note] + egt->noteIncr[egt->ch[ch].note] * SEMITONE_RATIO * egt->ch[ch].instr->temperament[egt->ch[ch].note % 12];
											   o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1) * (1 + egt->ch[ch].tuning);
											   break;
									}
									case 4:
										o->mult = clamp(egt->ch[ch].fxData, 0, 255);
										o->incr = (o->mult * o->mult + (float)o->mult *(float)o->finetune*_2400TO1) * LUTratio*egt->sampleRateRatio * (1 + egt->ch[ch].tuning);
										break;
									case 5:{
											   o->finetune = clamp(egt->ch[ch].fxData, 0, 24);
											   float frequency = egt->noteIncr[egt->ch[ch].note] + egt->noteIncr[egt->ch[ch].note] * SEMITONE_RATIO * egt->ch[ch].instr->temperament[egt->ch[ch].note % 12];
											   o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1) * (1 + egt->ch[ch].tuning);
											   break;
									}
									case 6:{
											   o->detune = clamp((char)egt->ch[ch].fxData, -100, 100);
											   float frequency = egt->noteIncr[egt->ch[ch].note] + egt->noteIncr[egt->ch[ch].note] * SEMITONE_RATIO * egt->ch[ch].instr->temperament[egt->ch[ch].note % 12];
											   o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1) * (1 + egt->ch[ch].tuning);
											   break;
									}
									case 7:
										o->delay = expEnv[egt->ch[ch].fxData] * 3000000 / egt->sampleRateRatio;
										break;
									case 8:
										o->i = expVol[egt->ch[ch].fxData];
										break;
									case 9:
										o->baseA = clamp(egt->ch[ch].fxData, 0, 99);
										break;
									case 10:
										o->h = expEnv[clamp(egt->ch[ch].fxData, 0, 80)] * 700000 / egt->sampleRateRatio;
										break;
									case 11:
										o->baseD = clamp(egt->ch[ch].fxData, 0, 99);
										break;
									case 12:
										o->s = expVol[clamp(egt->ch[ch].fxData, 0, 99)];
										break;
									case 13:{
												char value = clamp((char)egt->ch[ch].fxData, -99, 99);
												o->r = (value >= 0) ? exp(-(expEnv[value])*egt->sampleRateRatio) : 2 - exp(-(expEnv[abs(value)])*egt->sampleRateRatio);
												break;
									}
									case 14:
										o->envLoop = clamp(egt->ch[ch].fxData, 0, 1);
										break;
									case 15:
										o->lfoFM = expVol[clamp(egt->ch[ch].fxData, 0, 99)] * expVol[clamp(egt->ch[ch].fxData, 0, 99)];
										break;

									case 16:
										o->lfoAM = expVol[clamp(egt->ch[ch].fxData, 0, 99)];
										break;




								}
							}


							break;
						case 'M': // channel volume
							egt->ch[ch].vol = expVol[egt->ch[ch].fxData];
							break;
						case 'R': // reverb send
							egt->ch[ch].reverbSend = expVol[egt->ch[ch].fxData];
							break;
						case 'S': // global reverb params
							if (egt->ch[ch].fxData <= 40)
							{
								egt->reverbLength = 0.5 + egt->ch[ch].fxData*0.0125;
							}
							else
							{
								egt_initReverb(egt, clamp(egt->ch[ch].fxData - 40, 1, 40)*0.025);
							}
							break;
						case 'T': // tempo
							egt->tempo = max(1, egt->ch[ch].fxData);
							break;


						case 'X': // panning
							egt->ch[ch].destPan = egt->ch[ch].fxData;
							break;
					}

				}
			}
			egt->frameTimer += 8;
			if (egt->frameTimer >= (60.0 / egt->diviseur) * egt->sampleRate / egt->tempo)
			{

				egt->frameTimer = 0;

				if (++egt->row >= egt->patternSize[egt->order])
				{ // jump to next pattern
					egt->row = 0;
					egt->order++;
				}

				if (egt->tempOrder != -1 || egt->tempRow != -1)
				{
					egt->loopCount++;

					if (egt->tempOrder != -1)
						egt->order = min(egt->tempOrder, egt->patternCount - 1);

					if (egt->tempRow != -1)
						egt->row = min(egt->tempRow, egt->patternSize[egt->order] - 1);

					egt->tempOrder = egt->tempRow = -1;
				}

				if (egt->order >= egt->patternCount)
				{
					egt->loopCount++;
					egt->order = 0;
				}

				if (egt->looping != -1 && egt->loopCount > egt->looping)
				{
					egt->playing = 0;
				}

			}

			if (egt->frameTimerFx >= 0.005*(60.0 / egt->diviseur) * egt->sampleRate / egt->tempo)
			{

				for (unsigned ch = 0; ch < FM_ch; ++ch)
				{
					switch (egt->ch[ch].fxActive)
					{
						case 'A': // arpeggio
						{
									  egt->ch[ch].arpTimer++;
									  if (egt->ch[ch].arpTimer >= 8)
									  {
										  egt->ch[ch].arpTimer -= 8;
										  egt->ch[ch].arpIter = (egt->ch[ch].arpIter + 1) % 3;
										  egt_playNote(egt, 255, egt->ch[ch].arpIter == 0 ? egt->ch[ch].baseArpeggioNote : egt->ch[ch].arpIter == 1 ? (egt->ch[ch].baseArpeggioNote + egt->ch[ch].fxData % 16) : (egt->ch[ch].baseArpeggioNote + egt->ch[ch].fxData / 16), ch, 255);
									  }

						}
							break;
						case 'Q': // retrigger note
						{
									  egt->ch[ch].arpTimer++;
									  if (egt->ch[ch].arpTimer >= 24 / egt->ch[ch].fxData && egt->ch[ch].arpIter < egt->ch[ch].fxData)
									  {
										  egt->ch[ch].arpTimer -= 24 / egt->ch[ch].fxData;
										  egt_playNote(egt, egt->ch[ch].instrNumber, egt->ch[ch].untransposedNote, ch, 255);
										  egt->ch[ch].arpIter++;
									  }
						}
							break;
						case 'D': // delay
						{
									  int delay = egt->frameTimer / ((60.0 / egt->diviseur) * egt->sampleRate / egt->tempo / 8);
									  if (delay >= egt->ch[ch].fxData)
									  {

										  if (egt->pattern[egt->order][egt->row][ch].note < 127)
											  egt_playNote(egt, egt->pattern[egt->order][egt->row][ch].instr, egt->pattern[egt->order][egt->row][ch].note, ch, egt->pattern[egt->order][egt->row][ch].vol);
										  else if (egt->pattern[egt->order][egt->row][ch].note == 128)
											  egt_stopNote(egt, ch);

										  egt->ch[ch].fxActive = 0;
									  }
						}
							break;

						case 'E': // portamento up
							for (unsigned op = 0; op < FM_op; ++op)
							{
								egt->ch[ch].op[op].incr += egt->ch[ch].fxData*egt->ch[ch].op[op].incr*0.0001;
							}
							break;
						case 'F': // portamento down
							for (unsigned op = 0; op < FM_op; ++op)
							{
								egt->ch[ch].op[op].incr += -egt->ch[ch].fxData*egt->ch[ch].op[op].incr*0.0001;
							}
							break;
						case 'G': // portamento
							for (unsigned op = 0; op < FM_op; ++op)
							{
								egt->ch[ch].op[op].incr += (egt->ch[ch].op[op].portaDestIncr - egt->ch[ch].op[op].incr)*egt->ch[ch].fxData*0.001;
							}
							break;
						case 'I':{ // pitch bend

									 //int pos = egt->order*egt->patternSize[egt->order]+egt->row+1;

									 /*if (egt->pattern[pos / egt->patternSize[egt->order]][pos%egt->patternSize[egt->order]].fx == 'I'){
										 float nextPB = 1-(float)(64-egt->pattern[pos / egt->patternSize[egt->order]][pos%egt->patternSize[egt->order]].fxdata[ch]) / 538.1489198433845617116833784366;
										 egt->pitchBend[ch]=(egt->pitchBend[ch]*10+nextPB)/11;
										 }*/
									 break;
						}
						case 'N': // channel volume slide
							egt->ch[ch].vol = clamp(egt->ch[ch].vol + ((int)egt->ch[ch].fxData - 127)*0.0001, 0, 1);
							break;
						case 'P': // panning slide
							egt->ch[ch].pan = clamp(egt->ch[ch].pan + (127 - (int)egt->ch[ch].fxData)*-0.05, 0, 255);
							break;
						case 'W': // global volume slide
							egt->globalVolume = clamp(egt->globalVolume + ((int)egt->ch[ch].fxData - 127)*0.0001, 0, 1);
							break;

					}
				}
				egt->frameTimerFx -= 0.005*(60.0 / egt->diviseur) * egt->sampleRate / egt->tempo;
			}
			egt->frameTimerFx++;
		}


		for (unsigned ch = 0; ch < FM_ch; ++ch)
		{
			if (!egt->ch[ch].active)
				continue;


			egt->ch[ch].pan = (egt->ch[ch].pan*(egt->transitionSpeed - 1) + egt->ch[ch].destPan) / egt->transitionSpeed;
			//egt->ch[ch].vol = (egt->ch[ch].vol*(speed-1)+egt->ch[ch].destVol)/speed;


			// Update lfo
			if (egt->ch[ch].lfoDelayCpt++ >= egt->ch[ch].lfoDelayCptMax)
			{
				egt->ch[ch].lfoPhase += egt->ch[ch].lfoIncr;
				egt->ch[ch].lfoEnv += (1.f - egt->ch[ch].lfoEnv)*egt->ch[ch].lfoA;
				egt->ch[ch].lfo = wavetable[egt->ch[ch].lfoWaveform][((egt->ch[ch].lfoPhase & egt->ch[ch].lfoMask) >> 10) % LUTsize] * egt->ch[ch].lfoEnv;
			}
			int opOutUsed = 0;
			egt->ch[ch].currentEnvLevel = 0;
			for (unsigned op = 0; op < FM_op; ++op)
			{
				fm_operator* o = &egt->ch[ch].op[op];
				if (o->connectOut != &egt->noConnect)
				{
					opOutUsed += egt->ch[ch].op[o->id].state;
					egt->ch[ch].currentEnvLevel += egt->ch[ch].op[o->id].env;
				}

				/* Handle envelope */

				switch (o->state)
				{
					/* Delay */
					case 1:
						if (o->envCount++ >= o->delay)
						{

							if (egt->ch[ch].instr->op[op].pitchInitialRatio > 0)
								o->pitchMod = 1 + expVol[egt->ch[ch].instr->op[op].pitchInitialRatio] * expVol[egt->ch[ch].instr->op[op].pitchInitialRatio] * 12;
							else if (egt->ch[ch].instr->op[op].pitchInitialRatio < 0)
								o->pitchMod = 1 + (float)egt->ch[ch].instr->op[op].pitchInitialRatio*_99TO1;
							else
								o->pitchMod = 1;

							o->pitchTime = expEnv[egt->ch[ch].instr->op[op].pitchDecay] * egt->sampleRateRatio;
							o->pitchDestRatio = 1;

							if (egt->ch[ch].instr->phaseReset || o->env < 0.1)
							{
								o->phase = o->offset;
							}

							if (o->envCount >= 99999999)
							{
								o->env = o->s;
							}
							else if (egt->ch[ch].instr->envReset)
								o->env = o->i;



							o->env += (1.4f - o->env) * o->a;
							if (o->env >= 1.f)
							{
								o->env = 1.f;
								o->state = o->h > 0 ? 3 : 4;
							}
							else
								o->state = 2;
						}
						break;
						/* Attack */
					case 2:
						o->env += (1.4f - o->env) * o->a;
						if (o->env >= 1.f)
						{
							o->env = 1.f;
							o->state = o->h > 0 ? 3 : 4;
						}
						break;
						/* Hold */
					case 3:
						if (o->envCount++ >= o->h)
							o->state++;
						break;
						/* Decay - Sustain */
					case 4:
						o->env *= o->d;
						if (o->env <= o->s || o->env < 0.001f)
						{
							o->env = o->s;


							if (o->s < 0.001f)
							{
								if (o->envLoop & FM_OP_ENVLOOP_BIT)
								{
									o->envCount = 99999999;
									o->state = 1;
								}
								else
								{
									o->state = o->env = o->amp = 0;
								}
							}
							else
							{
								o->envCount = 99999999;
								if (o->envLoop & FM_OP_ENVLOOP_BIT)
								{

									o->state = 1;
								}
								else
								{
									o->state = 5;
								}
							}
						}

						break;
						/* Release */
					case 6:
						o->env *= o->r;

						if (o->r <= 1)
						{
							if (o->env < 0.001f)
								o->state = o->env = o->amp = 0;
						}
						else
						{
							if (o->env >= 1.f)
							{
								o->env = 1.f;
								o->state = 5;
							}
						}
						break;
				}

				o->pitchMod -= (o->pitchMod - o->pitchDestRatio)*o->pitchTime;
				o->ampDelta = (o->env * o->vol *(1.f - egt->ch[ch].lfo * o->lfoAM) - o->amp) / 8;
				//o->amp = o->env * o->vol *(1.f - egt->ch[ch].lfo * o->lfoAM );
				o->pitch = o->incr * o->pitchMod * egt->ch[ch].pitchBend *(1 + egt->ch[ch].lfo * o->lfoFM);

			}
			egt->ch[ch].active = opOutUsed;
		}

		/* Previous stuff didnt need to be updated for every sample, we do 8 rendering steps for 1 update step to save CPU */

		for (unsigned iter = 0; iter < 8; iter++)
		{
			float rendu = 0, renduL = 0, renduR = 0, fxL = 0, fxR = 0;

			for (unsigned ch = 0; ch < FM_ch; ++ch)
			{
				if (!egt->ch[ch].active || egt->ch[ch].muted)
					continue;

				/* FM calculations, unrolled to be sure the compiler doesn't generate a loop */

				egt->ch[ch].op[0].phase += egt->ch[ch].op[0].pitch;
				egt->ch[ch].op[0].amp += egt->ch[ch].op[0].ampDelta;
				egt->ch[ch].op[0].out = egt->ch[ch].op[0].waveform[((egt->ch[ch].op[0].phase >> 10) + (unsigned)*egt->ch[ch].op[0].connect + (unsigned)*egt->ch[ch].op[0].connect2 + (unsigned)(*egt->ch[ch].feedbackSource*egt->ch[ch].feedbackLevel)) % LUTsize] * egt->ch[ch].op[0].amp;


				egt->ch[ch].op[1].phase += egt->ch[ch].op[1].pitch;
				egt->ch[ch].op[1].amp += egt->ch[ch].op[1].ampDelta;
				egt->ch[ch].op[1].out = egt->ch[ch].op[1].waveform[((egt->ch[ch].op[1].phase >> 10) + (unsigned)*egt->ch[ch].op[1].connect + (unsigned)*egt->ch[ch].op[1].connect2) % LUTsize] * egt->ch[ch].op[1].amp;


				egt->ch[ch].op[2].phase += egt->ch[ch].op[2].pitch;
				egt->ch[ch].op[2].amp += egt->ch[ch].op[2].ampDelta;
				egt->ch[ch].op[2].out = egt->ch[ch].op[2].waveform[((egt->ch[ch].op[2].phase >> 10) + (unsigned)*egt->ch[ch].op[2].connect + (unsigned)*egt->ch[ch].op[2].connect2) % LUTsize] * egt->ch[ch].op[2].amp;


				egt->ch[ch].op[3].phase += egt->ch[ch].op[3].pitch;
				egt->ch[ch].op[3].amp += egt->ch[ch].op[3].ampDelta;
				egt->ch[ch].op[3].out = egt->ch[ch].op[3].waveform[((egt->ch[ch].op[3].phase >> 10) + (unsigned)*egt->ch[ch].op[3].connect + (unsigned)*egt->ch[ch].op[3].connect2) % LUTsize] * egt->ch[ch].op[3].amp;


				egt->ch[ch].op[4].phase += egt->ch[ch].op[4].pitch;
				egt->ch[ch].op[4].amp += egt->ch[ch].op[4].ampDelta;
				egt->ch[ch].op[4].out = egt->ch[ch].op[4].waveform[((egt->ch[ch].op[4].phase >> 10) + (unsigned)*egt->ch[ch].op[4].connect + (unsigned)*egt->ch[ch].op[4].connect2) % LUTsize] * egt->ch[ch].op[4].amp;


				egt->ch[ch].op[5].phase += egt->ch[ch].op[5].pitch;
				egt->ch[ch].op[5].amp += egt->ch[ch].op[5].ampDelta;
				egt->ch[ch].op[5].out = egt->ch[ch].op[5].waveform[((egt->ch[ch].op[5].phase >> 10) + (unsigned)*egt->ch[ch].op[5].connect + (unsigned)*egt->ch[ch].op[5].connect2) % LUTsize] * egt->ch[ch].op[5].amp;


				egt->ch[ch].mixer = *egt->ch[ch].op[0].toMix + *egt->ch[ch].op[1].toMix + *egt->ch[ch].op[2].toMix + *egt->ch[ch].op[3].toMix;

				rendu = (*egt->ch[ch].op[0].connectOut + *egt->ch[ch].op[1].connectOut + *egt->ch[ch].op[2].connectOut + *egt->ch[ch].op[3].connectOut + *egt->ch[ch].op[4].connectOut + *egt->ch[ch].op[5].connectOut)*egt->ch[ch].vol*egt->ch[ch].instrVol;

				egt->ch[ch].lastRender2 = egt->ch[ch].lastRender;
				egt->ch[ch].lastRender = rendu;

				/* Is a smooth transition needed between two notes ? */

				if (egt->ch[ch].fade > 0.00001)
				{
					rendu = rendu*(1 - egt->ch[ch].fade) + egt->ch[ch].fadeFrom*egt->ch[ch].fade;
					egt->ch[ch].fadeFrom += egt->ch[ch].delta*egt->ch[ch].fade;
					egt->ch[ch].fade *= egt->ch[ch].fadeIncr;
				}

				float trenduL = rendu*wavetable[0][LUTsize / 4 + (unsigned)egt->ch[ch].pan*LUTratio];
				float trenduR = rendu*wavetable[0][(unsigned)egt->ch[ch].pan*LUTratio];


				renduL += trenduL;
				renduR += trenduR;
				fxL += trenduL*egt->ch[ch].reverbSend;
				fxR += trenduR*egt->ch[ch].reverbSend;
			}

			/* Reverb phases */

			unsigned prevPhaseL = egt->reverbPhaseL;
			egt->reverbPhaseL = (egt->reverbPhaseL + 1) % egt->reverbMod1;
			unsigned prevPhaseL2 = egt->reverbPhaseL2;
			egt->reverbPhaseL2 = (egt->reverbPhaseL2 + 1) % egt->reverbMod2;
			unsigned prevPhaseR = egt->reverbPhaseR;
			egt->reverbPhaseR = (egt->reverbPhaseR + 1) % egt->reverbMod3;
			unsigned prevPhaseR2 = egt->reverbPhaseR2;
			egt->reverbPhaseR2 = (egt->reverbPhaseR2 + 1) % egt->reverbMod4;

			/* Two comb filters, left */

			egt->outL = ((egt->revBuf[egt->reverbPhaseL] + egt->revBuf[egt->reverbMod1 + egt->reverbPhaseL2]))*0.5;
			egt->revBuf[egt->reverbPhaseL] =  fxR + (egt->revBuf[egt->reverbPhaseL] + egt->revBuf[prevPhaseL])*0.5*egt->reverbLength;
			egt->revBuf[egt->reverbMod1 + egt->reverbPhaseL2] =fxL + (egt->revBuf[egt->reverbMod1 + egt->reverbPhaseL2] + egt->revBuf[egt->reverbMod1 + prevPhaseL2])*0.5*egt->reverbLength;

			/* Two comb filters, right */

			egt->outR = ((egt->revBuf[egt->revOffset2 + egt->reverbPhaseR] + egt->revBuf[egt->revOffset3 + egt->reverbPhaseR2]))*0.5;
			egt->revBuf[egt->revOffset2 + egt->reverbPhaseR] = fxL + (egt->revBuf[egt->revOffset2 + egt->reverbPhaseR] + egt->revBuf[egt->revOffset2 + prevPhaseR])*0.5*egt->reverbLength;
			egt->revBuf[egt->revOffset3 + egt->reverbPhaseR2] =  fxR + (egt->revBuf[egt->revOffset3 + egt->reverbPhaseR2] + egt->revBuf[egt->revOffset3 + prevPhaseR2])*0.5*egt->reverbLength;

			/* First allpass */

			float outL2 = 0.5*egt->outL + egt->revBuf[egt->revOffset4 + egt->allpassPhaseL];
			egt->revBuf[egt->revOffset4 + egt->allpassPhaseL] = egt->outL - 0.5 * outL2;
			egt->allpassPhaseL = (egt->allpassPhaseL + 1) % egt->allpassMod;

			float outR2 = 0.5*egt->outR + egt->revBuf[egt->revOffset5 + egt->allpassPhaseR];
			egt->revBuf[egt->revOffset5 + egt->allpassPhaseR] = egt->outR - 0.5 * outR2;
			egt->allpassPhaseR = (egt->allpassPhaseR + 1) % egt->allpassMod;

			/* Second allpass */

			float outL22 = 0.5*outL2 + egt->revBuf[egt->revOffset6 + egt->allpassPhaseL2];
			egt->revBuf[egt->revOffset6 + egt->allpassPhaseL2] = outL2 - 0.5 * outL22;
			egt->allpassPhaseL2 = (egt->allpassPhaseL2 + 1) % egt->allpassMod2;

			float outR22 = 0.5*outR2 + egt->revBuf[egt->revOffset7 + egt->allpassPhaseR2];
			egt->revBuf[egt->revOffset7 + egt->allpassPhaseR2] = outR2 - 0.5 * outR22;
			egt->allpassPhaseR2 = (egt->allpassPhaseR2 + 1) % egt->allpassMod2;


			/* Final mix */
			buffer[b] = (renduL + outL22) * egt->globalVolume * egt->playbackVolume;
			buffer[b + 1] =(renduR + outR22) * egt->globalVolume * egt->playbackVolume;
			b += 2;
			if (b>=length)
				return;
		}
	}
}

void egt_playNote(egtsynth* egt, unsigned _instrument, unsigned note, unsigned ch, unsigned volume)
{
	if (ch >= FM_ch || _instrument == 255 && !egt->ch[ch].instr || _instrument != 255 && _instrument >= egt->instrumentCount)
		return;

	/* Instrument changed, update parameters */
	if (_instrument != 255 && _instrument < egt->instrumentCount && egt->ch[ch].cInstr != &egt->instrument[_instrument])
	{

		egt->ch[ch].cInstr = &egt->instrument[_instrument];
		egt->ch[ch].instrNumber = _instrument;

		egt->ch[ch].instr = &egt->instrument[_instrument];
		egt->ch[ch].instrVol = expVol[egt->ch[ch].instr->volume];
		egt->ch[ch].lfoMask = lfoMasks[egt->ch[ch].instr->lfoWaveform];
		egt->ch[ch].lfoWaveform = lfoWaveforms[egt->ch[ch].instr->lfoWaveform];
		egt->ch[ch].feedbackLevel = expVol[egt->ch[ch].instr->feedback];
		egt->ch[ch].lfoA = expEnv[egt->ch[ch].instr->lfoA] * egt->sampleRateRatio;
		egt->ch[ch].lfoIncr = 1 + expVol[egt->ch[ch].instr->lfoSpeed] * expVol[egt->ch[ch].instr->lfoSpeed] * 5000 * egt->sampleRateRatio*LUTratio;
		egt->ch[ch].lfoDelayCptMax = expVol[egt->ch[ch].instr->lfoDelay] * expVol[egt->ch[ch].instr->lfoDelay] * 200000 * egt->sampleRateRatio;
		egt->ch[ch].lfoEnv = egt->ch[ch].lfoDelayCpt = egt->ch[ch].lfo = egt->ch[ch].lfoPhase = 0;
		egt->ch[ch].pitchBend = 1;
		egt->ch[ch].transpose = egt->ch[ch].instr->transpose;
		egt->ch[ch].tuning = 0.0006 * egt->ch[ch].instr->tuning;
		egt->ch[ch].lfoOffset = egt->ch[ch].instr->lfoOffset * LUTsize / 32;
		for (unsigned op = 0; op < FM_op; ++op)
		{
			fm_operator* o = &egt->ch[ch].op[op];
			o->env = 0;
			o->connectOut = (egt->ch[ch].instr->op[op].connectOut >= 0) ? &egt->ch[ch].op[egt->ch[ch].instr->op[op].connectOut].out : &egt->noConnect;
			o->id = egt->ch[ch].instr->op[op].connectOut;
			o->connect = (egt->ch[ch].instr->op[op].connect >= 0) ? &egt->ch[ch].op[egt->ch[ch].instr->op[op].connect].out : &egt->noConnect;
			o->connect2 = (egt->ch[ch].instr->op[op].connect2>5) ? &egt->ch[ch].mixer :
				(egt->ch[ch].instr->op[op].connect2 >= 0 ? &egt->ch[ch].op[egt->ch[ch].instr->op[op].connect2].out : &egt->noConnect);

			o->waveform = wavetable[egt->ch[ch].instr->op[op].waveform];
			o->lfoFM = expVol[egt->ch[ch].instr->op[op].lfoFM] * expVol[egt->ch[ch].instr->op[op].lfoFM];
			o->lfoAM = expVol[egt->ch[ch].instr->op[op].lfoAM];

			o->delay = expEnv[egt->ch[ch].instr->op[op].delay] * 3000000 / egt->sampleRateRatio;

			o->i = expVol[egt->ch[ch].instr->op[op].i];
			o->h = expEnv[egt->ch[ch].instr->op[op].h] * 700000 / egt->sampleRateRatio;
			o->s = expVol[egt->ch[ch].instr->op[op].s];
			o->baseR = egt->ch[ch].instr->op[op].r;
			o->r = (o->baseR >= 0) ? exp(-(expEnv[(int)o->baseR])*egt->sampleRateRatio) : 2 - exp(-(expEnv[abs(o->baseR)])*egt->sampleRateRatio);

			o->finetune = egt->ch[ch].instr->op[op].finetune;
			o->detune = egt->ch[ch].instr->op[op].detune;
			o->mult = egt->ch[ch].instr->op[op].mult;
			o->baseVol = egt->ch[ch].instr->op[op].vol * !egt->ch[ch].instr->op[op].muted;
			o->baseA = egt->ch[ch].instr->op[op].a;
			o->baseD = egt->ch[ch].instr->op[op].d;
			o->fixedFreq = egt->ch[ch].instr->op[op].fixedFreq;
			o->offset = ((unsigned int)egt->ch[ch].instr->op[op].offset)* LUTsize * 32;
			o->envLoop = egt->ch[ch].instr->op[op].envLoop;
			o->pitchFinalRatio = egt->ch[ch].instr->op[op].pitchFinalRatio;
			o->velSensitivity = (float)egt->ch[ch].instr->op[op].velSensitivity*_99TO1;
			o->volScaling = egt->ch[ch].instr->op[op].kbdVolScaling*0.001;
			o->kbdCenterNote = egt->ch[ch].instr->op[op].kbdCenterNote;
		}
		for (unsigned op = 0; op < FM_op - 2; ++op)
		{
			egt->ch[ch].op[op].toMix = (egt->ch[ch].instr->toMix[op] >= 0) ? &egt->ch[ch].op[egt->ch[ch].instr->toMix[op]].out : &egt->noConnect;
		}
		egt->ch[ch].feedbackSource = &egt->ch[ch].op[egt->ch[ch].instr->feedbackSource].out;

	}

	/* Note changed */
	if (note < 128 && egt->ch[ch].instr)
	{
		egt->ch[ch].untransposedNote = note;


		egt_calcPitch(egt, ch, note);

		if (volume < 100)
			egt->ch[ch].noteVol = volume;

		if (egt->ch[ch].instr->flags & FM_INSTR_LFORESET)
		{
			egt->ch[ch].lfoEnv = egt->ch[ch].lfoDelayCpt = egt->ch[ch].lfo = 0;
			egt->ch[ch].lfoPhase = egt->ch[ch].lfoOffset * LUTsize / 2;
		}


		/* Trigger note transition smoothing algorithm to avoid clicks/pops */
		if (egt->ch[ch].instr->flags & FM_INSTR_SMOOTH && egt->ch[ch].currentEnvLevel > 0.1 && (egt->ch[ch].instr->envReset || egt->ch[ch].instr->phaseReset))
		{

			egt->ch[ch].fade = 1;
			egt->ch[ch].fadeFrom = egt->ch[ch].lastRender;
			egt->ch[ch].delta = clamp((egt->ch[ch].lastRender - egt->ch[ch].lastRender2), -2000, 2000)*egt->sampleRateRatio;

			egt->ch[ch].fadeIncr = 0.95 - egt->ch[ch].note*0.001;
		}

		for (unsigned op = 0; op < FM_op; ++op)
		{
			fm_operator* o = &egt->ch[ch].op[op];

			egt_calcOpVol(o, egt->ch[ch].note, volume == 255 ? egt->ch[ch].noteVol : volume);
			o->amp = 0;
			o->a = expEnv[(int)max(0, min(99, (o->baseA + egt->ch[ch].instr->op[op].kbdAScaling*((int)egt->ch[ch].note - egt->ch[ch].instr->op[op].kbdCenterNote)*0.07f)))] * egt->sampleRateRatio;
			o->d = exp(-expEnv[(int)max(0, min(99, (o->baseD + egt->ch[ch].instr->op[op].kbdDScaling*((int)egt->ch[ch].note - egt->ch[ch].instr->op[op].kbdCenterNote)*0.07f)))] * egt->sampleRateRatio);
			if (egt->ch[ch].instr->op[op].envLoop & FM_OP_KSR_RELEASE_BIT)
			{
				int rIndex = (int)max(0, min(99, (abs(o->baseR) + egt->ch[ch].instr->op[op].kbdDScaling*((int)egt->ch[ch].note - egt->ch[ch].instr->op[op].kbdCenterNote)*0.07f)));
				o->r = (o->baseR >= 0) ? exp(-(expEnv[rIndex])*egt->sampleRateRatio) : 2 - exp(-(expEnv[rIndex])*egt->sampleRateRatio);
			}

			if (_instrument != 255)
			{
				if (egt->ch[ch].instr->envReset)
				{
					o->env = 0;
					o->out = 0;
				}

				egt->ch[ch].op0 = o->envCount = o->pitchTime = 0;
				o->pitchMod = o->pitchDestRatio = 1;
				o->state = 1;
			}
		}



	}



	egt->ch[ch].active = 1;
}

/* Creates a table containing all current pannings/volumes/tempo/time info for each row, for fast seeking */

void egt_buildStateTable(egtsynth* egt, unsigned orderStart, unsigned orderEnd, unsigned channelStart, unsigned channelEnd)
{

	orderStart = clamp(orderStart, 0, egt->patternCount);
	orderEnd = clamp(orderEnd, 0, egt->patternCount);
	channelStart = clamp(channelStart, 0, FM_ch);
	channelEnd = clamp(channelEnd, 0, FM_ch);


	for (int order = orderStart; order < orderEnd; order++)
	{

		if (order == 0)
		{
			for (unsigned ch = 0; ch < FM_ch; ch++)
			{
				egt->channelStates[order][0].pan[ch] = egt->ch[ch].initial_pan;
				egt->channelStates[order][0].vol[ch] = egt->ch[ch].initial_vol;
			}
			egt->channelStates[order][0].tempo = egt->initial_tempo;
			egt->channelStates[order][0].time = 0;
		}
		for (int j = 0; j < egt->patternSize[order]; j++)
		{

			/* Replicate previous row data (tempo/time) */
			if (j>0)
			{
				egt->channelStates[order][j].tempo = egt->channelStates[order][j - 1].tempo;
				egt->channelStates[order][j].time = egt->channelStates[order][j - 1].time + 60.f / (egt->channelStates[order][j].tempo*egt->diviseur);
			}
			else if (order > 0)
			{
				egt->channelStates[order][j].tempo = egt->channelStates[order - 1][egt->patternSize[order - 1] - 1].tempo;
				egt->channelStates[order][j].time = egt->channelStates[order - 1][egt->patternSize[order - 1] - 1].time + 60.f / (egt->channelStates[order][j].tempo*egt->diviseur);
			}
			for (unsigned ch = channelStart; ch< channelEnd; ch++)
			{
				/* Replicate previous row data (pan/vol for each channel) */
				if (j>0)
				{
					egt->channelStates[order][j].vol[ch] = egt->channelStates[order][j - 1].vol[ch];
					egt->channelStates[order][j].pan[ch] = egt->channelStates[order][j - 1].pan[ch];
				}
				else if (order > 0)
				{
					egt->channelStates[order][j].vol[ch] = egt->channelStates[order - 1][egt->patternSize[order - 1] - 1].vol[ch];
					egt->channelStates[order][j].pan[ch] = egt->channelStates[order - 1][egt->patternSize[order - 1] - 1].pan[ch];
				}

				switch (egt->pattern[order][j][ch].fx)
				{
					case 'T':
						egt->channelStates[order][j].tempo = egt->pattern[order][j][ch].fxdata == 0 ? 1 : egt->pattern[order][j][ch].fxdata;
						break;
					case 'X':
						egt->channelStates[order][j].pan[ch] = egt->pattern[order][j][ch].fxdata;
						break;
					case 'M':
						egt->channelStates[order][j].vol[ch] = egt->pattern[order][j][ch].fxdata;
						break;
				}
			}
		}
	}
	egt->channelStatesDone = 1;
}



void egt_render(egtsynth* egt, void* buffer, unsigned length, unsigned type)
{
	float *rendered = malloc(4*length); // float = 4bytes

	if (!rendered)
		return;

	_egt_render(egt, (float*)rendered, length);

	switch (type%64)
	{
		case EGT_RENDER_FLOAT:
		{
			float *buf_f = buffer;
			for (unsigned i = 0; i < length; i++)
			{
				buf_f[i] = clamp(rendered[i]/32768,-1.0,1.0);
			}
			break;
		}
		case EGT_RENDER_8:
		{
			if(type & EGT_RENDER_PAD32)
			{
				int *buf_32 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_32[i] = (clamp((signed char)(rendered[i]/256), -128,127));
				}
			}
			else
			{
				unsigned char *buf_8 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_8[i] = clamp(128+rendered[i]/256, 0,255);
				}
			}
			
			break;
		}
		case EGT_RENDER_16:{
			if(type & EGT_RENDER_PAD32)
			{
				int *buf_32 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_32[i] = clamp(rendered[i], -32768,32767);
				}
			}
			else
			{
				signed short *buf_16 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_16[i] = clamp(rendered[i], -32768,32767);
				}
			}
			
			break;
		}
		case EGT_RENDER_24:
		{
			unsigned char *buf_24 = buffer;

			if(type & EGT_RENDER_PAD32)
			{
				for (unsigned i = 0; i < length; i++)
				{
					int val = clamp(rendered[i]*256, -8388608,8388607);

					// negative 24bit values should stay negative 32 bit values ! (negative has the top byte to 255)
					buf_24[i*4+3] = (val  < 0) ? 255 : 0;
					
					buf_24[i*4+2] = (unsigned char)((val&0x00ff0000) >> 16);
					buf_24[i*4+1] = (unsigned char)((val&0x00ff00)>>8);
					buf_24[i*4] = (unsigned char)(val & 0xff);
					
					
				}
			}
			else
			{
				for (unsigned i = 0; i < length; i++)
				{
					int val = clamp(rendered[i]*256, -8388608,8388607);

					buf_24[i*3+2] = (unsigned char)((val&0x00ff0000) >> 16);
					buf_24[i*3+1] = (unsigned char)((val&0x00ff00)>>8);
					buf_24[i*3] = (unsigned char)(val & 0xff);

				}
			}
			break;
		}
		case EGT_RENDER_32:
		{
			int *buf_32 = buffer;
			for (unsigned i = 0; i < length; i++)
			{
				buf_32[i] = (signed int)clamp(((double)rendered[i]*256*256),-2147483648.f,2147483647.f);
			}
			break;
		}
	}

	free(rendered);
}

void egt_stopNote(egtsynth* egt, unsigned ch)
{
	if (ch >= FM_ch || !egt->ch[ch].active || egt->ch[ch].note > 127)
		return;


	for (unsigned op = 0; op < FM_op; ++op)
	{
		fm_operator *o = &egt->ch[ch].op[op];

		o->state = 6;
		o->pitchTime = expEnv[egt->ch[ch].instr->op[op].pitchRelease] * egt->sampleRateRatio;

		if (o->pitchFinalRatio>0)
			o->pitchDestRatio = 1 + expVol[o->pitchFinalRatio] * expVol[o->pitchFinalRatio] * 12;
		else if (egt->ch[ch].instr->op[op].pitchFinalRatio < 0)
			o->pitchDestRatio = 1 + (float)o->pitchFinalRatio*_99TO1;
		else
			o->pitchDestRatio = 1;
	}
	egt->ch[ch].note = 255;

}

void egt_stopSound(egtsynth* egt)
{
	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		egt->ch[ch].active = 0;
		egt->ch[ch].lastRender = egt->ch[ch].lastRender2 = 0;
		egt->ch[ch].note = 255;
		egt->ch[ch].cInstr = 0;
		egt->ch[ch].instrNumber = 255;
		egt->ch[ch].currentEnvLevel = 0;
		for (unsigned op = 0; op < FM_op; ++op)
		{
			egt->ch[ch].op[op].state = egt->ch[ch].op[op].env = egt->ch[ch].op[op].amp = 0;
		}
	}
	memset(egt->revBuf, 0, egt->revBufSize*sizeof(float));
}

void egt_play(egtsynth* egt)
{
	if (egt->playing)
	{
		egt_stop(egt,1);
		egt_setPosition(egt,0,0,2);
	}
	if (!egt->channelStatesDone)
		egt_buildStateTable(egt, 0, egt->patternCount, 0, FM_ch);
	egt->playing = egt->patternCount > 0;
	egt->frameTimer = egt->frameTimerFx = 0;
	egt->tempRow = egt->tempOrder = -1;
	egt->looping = -1;
	egt->loopCount = 0;
	egt_initChannels(egt);
}


void egt_stop(egtsynth* egt, int cut)
{
	if (cut)
	{
		egt_stopSound(egt);
	}
	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		egt_stopNote(egt, ch);
		egt->ch[ch].cInstr = 0;
	}
	egt->playing = 0;
}

void egt_setPosition(egtsynth* egt, int order, int row, int cutNotes)
{
	if (cutNotes == 1)
	for (unsigned ch = 0; ch < FM_ch; ++ch) egt_stopNote(egt, ch);
	else if (cutNotes == 2)
		egt_stopSound(egt);

	egt->order = clamp(order, 0, egt->patternCount - 1);
	egt->row = clamp(row, 0, (int)egt->patternSize[order] - 1);
	egt->frameTimer = egt->frameTimerFx = 0;
	if (egt->playing)
		egt_initChannels(egt);
}

#include <stdint.h>

static uint32_t adler32(const void *buf, size_t buflength)
{
	const uint8_t *buffer = (const uint8_t*)buf;

	uint32_t s1 = 1;
	uint32_t s2 = 0;

	for (size_t n = 0; n < buflength; n++)
	{
		s1 = (s1 + buffer[n]) % 65521;
		s2 = (s2 + s1) % 65521;
	}
	return (s2 << 16) | s1;
}

int egt_saveSong(egtsynth* egt, const char* filename)
{
	FILE *fp = fopen(filename, "wb+");
	if (!fp)
	{
		return 0;
	}
	fputc('E', fp);
	fputc('G', fp);
	fputc('T', fp);
	fputc('S', fp);
	fputc(0x00, fp); // unused byte
	fputc(EDGETRACKER_VERSION, fp); // version
	unsigned char temp = strlen(&egt->songName[0]);
	fwrite(&temp, sizeof(temp), 1, fp);
	fwrite(&egt->songName[0], temp, 1, fp);

	temp = strlen(&egt->author[0]);
	fwrite(&temp, sizeof(temp), 1, fp);
	fwrite(&egt->author[0], temp, 1, fp);

	temp = strlen(&egt->comments[0]);
	fwrite(&temp, sizeof(temp), 1, fp);
	fwrite(&egt->comments[0], temp, 1, fp);

	fwrite(&egt->initial_tempo, sizeof(egt->initial_tempo), 1, fp); // tempo
	fwrite(&egt->diviseur, sizeof(egt->diviseur), 1, fp); // quarter note
	fwrite(&egt->_globalVolume, sizeof(egt->_globalVolume), 1, fp);
	fwrite(&egt->transpose, sizeof(egt->transpose), 1, fp);

	temp = round(egt->initialReverbLength * 160);
	fwrite(&temp, sizeof(temp), 1, fp);

	temp = round(egt->initialReverbRoomSize * 160);
	fwrite(&temp, sizeof(temp), 1, fp);

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		fwrite(&egt->ch[ch].initial_pan, sizeof(egt->ch[ch].initial_pan), 1, fp); // ch panning
		fwrite(&egt->ch[ch].initial_vol, sizeof(egt->ch[ch].initial_vol), 1, fp); // ch volume
		fwrite(&egt->ch[ch].initial_reverb, sizeof(egt->ch[ch].initial_reverb), 1, fp); // ch volume
	}

	temp = egt->patternCount;

	fwrite(&temp, sizeof(temp), 1, fp);
	for (unsigned i = 0; i < egt->patternCount; i++)
	{
		temp = egt->patternSize[i];
		fwrite(&temp, sizeof(temp), 1, fp);
		fwrite((char*)&egt->pattern[i][0], sizeof(fm_cell)*egt->patternSize[i] * FM_ch, 1, fp);
	}
	fwrite(&egt->instrumentCount, 1, 1, fp);


	for (int slot = 0; slot < egt->instrumentCount; slot++)
	{
		egt->instrument[slot].version = EDGETRACKER_VERSION;
	}

	fwrite((char*)&egt->instrument[0], sizeof(fm_instrument)*egt->instrumentCount, 1, fp);

	int totalSize = ftell(fp);
	char *all = malloc(totalSize);
	fseek(fp, 0, SEEK_SET);
	if (fread(all, totalSize, 1, fp) != 1)
		return 0;

	unsigned checksum = adler32(all, totalSize);

	fseek(fp, 0, SEEK_END);
	fwrite((char*)&checksum, 4, 1, fp);

	fclose(fp);

	return 1;
}

void egt_patternClear(egtsynth* egt)
{
	if (egt->pattern && egt->patternCount > 0)
	{

		egt->row = egt->order = 0;

		for (unsigned i = 0; i < egt->patternCount; i++)
		{
			free(egt->pattern[i]);
			free(egt->channelStates[i]);
		}

		egt->patternCount = 0;
	}
}

void egt_instrumentRecovery(fm_instrument * i)
{
	i->magic[0] = 'F';
	i->magic[1] = 'M';
	i->magic[2] = 'C';
	i->magic[3] = 'I';

	i->lfoWaveform = clamp(i->lfoWaveform, 0, 19);
	i->volume = clamp(i->volume, 0, 99);
	i->feedbackSource = clamp(i->feedbackSource, 0, 5);
	i->transpose = clamp(i->transpose, -12, 12);
	i->tuning = clamp(i->tuning, -100, 100);

	int nbOuts = 0;

	for (int j = 0; j < 4; j++)
		i->toMix[j] = clamp(i->toMix[j], -1, 5);

	for (int op = 0; op < 6; op++)
	{
		i->op[op].vol = clamp(i->op[op].vol, 0, 99);
		i->op[op].delay = clamp(i->op[op].delay, 0, 70);
		i->op[op].a = clamp(i->op[op].a, 0, 99);
		i->op[op].h = clamp(i->op[op].h, 0, 80);
		i->op[op].d = clamp(i->op[op].d, 0, 99);
		i->op[op].s = clamp(i->op[op].s, 0, 99);
		i->op[op].r = clamp(i->op[op].r, -99, 99);
		if (i->op[op].fixedFreq)
			i->op[op].mult = clamp(i->op[op].mult, 0, 255);
		else
			i->op[op].mult = clamp(i->op[op].mult, 0, 40);
		i->op[op].finetune = clamp(i->op[op].finetune, 0, 24);
		i->op[op].detune = clamp(i->op[op].detune, -100, 100);
		i->op[op].waveform = clamp(i->op[op].waveform, 0, 9);
		i->op[op].offset = clamp(i->op[op].offset, 0, 31);
		i->op[op].pitchDecay = clamp(i->op[op].pitchDecay, 0, 99);
		i->op[op].pitchRelease = clamp(i->op[op].pitchRelease, 0, 99);
		i->op[op].pitchInitialRatio = clamp(i->op[op].pitchInitialRatio, -99, 99);
		i->op[op].pitchFinalRatio = clamp(i->op[op].pitchFinalRatio, -99, 99);


		i->op[op].connect = clamp(i->op[op].connect, -1, 5);
		i->op[op].connect2 = clamp(i->op[op].connect2, -1, 6);
		i->op[op].connectOut = clamp(i->op[op].connectOut, -1, 5);

		if (i->op[op].connectOut >= 0)
		{
			nbOuts++;
		}

		if (i->op[op].connect == op)
			i->op[op].connect = -1;

		if (i->op[op].connect2 == op)
			i->op[op].connect2 = -1;

		for (int op2 = 0; op2 < 6; op2++)
		{
			if (op != op2)
			{
				if (i->op[op].connect == op2 && i->op[op2].connect == op)
				{
					i->op[op2].connect = -1;
				}
				if (i->op[op].connect2 == op2 && i->op[op2].connect2 == op)
				{
					i->op[op2].connect2 = -1;
				}
			}
		}
	}
	if (nbOuts == 0)
	{
		i->op[0].connectOut = 0;
		i->op[1].connectOut = 1;
		i->op[2].connectOut = 2;
		i->op[3].connectOut = 3;
		i->op[4].connectOut = 4;
		i->op[5].connectOut = 5;
	}
}




static int readFromMemory(egtsynth *egt, char *dst, int len, char *from)
{
	if (egt->readSeek >= egt->totalFileSize)
		return 0;

	memcpy(dst, from + egt->readSeek, len);
	egt->readSeek += len;
	return 1;
}

static char* readFromMemoryPtr(egtsynth *egt, int len, char *from)
{
	if (egt->readSeek >= egt->totalFileSize)
		return 0;

	egt->readSeek += len;
	return &from[egt->readSeek - len];
}

typedef struct tracker_channel_properties {
	int noteOn;
	int midiChannelMappings;
	int pedalCanRelease;
	int firstNotePos;
	int vol;
	int pan;
	int isInitialVolSet;
	int isInitialPanSet;
	int lastNoteVol;
	int midiTrackMappings;
	int channelPBend;
	int oldVol;
	int oldPan;
	int age;
	int stolenUsed;
} tracker_channel_properties;

static struct tracker_channel_properties trackerCh[FM_ch];

/* MIDI channel status */
typedef struct midi_channel_properties {
	int vol;
	int pan;
	int currentInstr;
	int expression;
	int localKeyboard;
	int channelPoly;
	int drumKit;
	int pedal;
	int firstNote;
	int currentTempo;
	int legato;
	int pitchBendRange;
} midi_channel_properties;

static struct midi_channel_properties midiCh[16];
static int patternSize, currentTempo;
static int lastPos;
static double realRow;
static short midiFormat, tracks;
static int maxOrder, currentTrack, lastNotePos, loopStart;
static int order, row, tempoDivisor, totalLength;
typedef struct old_channel {
	int channel, age, priority;
} old_channel;

static struct old_channel oldestChannels[FM_ch]; // forward

int instrumentExists(unsigned char id, egtsynth *egt)
{
	if (id < egt->instrumentCount)
		return id;
	return -1;
}

static int rpnSelect1, rpnSelect2;

static int sortChannels(void const *a, void const *b)
{
	struct old_channel *pa = (struct old_channel *)a;
	struct old_channel *pb = (struct old_channel *)b;
	return (pb->priority) - (pa->priority);
}

static unsigned long readVarLen(egtsynth *egt, char *data)
{
	unsigned long value = 0;
	char c = 0;

	readFromMemory(egt, (char *)&value, 1, data);

	if (value & 0x80)
	{
		value &= 0x7F;
		do
		{
			value = (value << 7);
			readFromMemory(egt, &c, 1, data);
			value += (c & 0x7F);
		} while (c & 0x80);
	}

	return(value);
}

static int isGlobalEffect(unsigned char fx)
{
	return (fx == 'T' || fx == 'B' || fx == 'C');
}

static int midi_findoldestChannelBackward(struct egtsynth *egt)
{

	for (unsigned i = 0; i < FM_ch; i++)
	{

		int pos = order*patternSize + row;
		while (pos>0 && egt->pattern[pos / patternSize][pos % patternSize][i].note == 255)
		{
			pos--;
		}

		// if we found a note instead of a note off, this channel is still playing !
		if (egt->pattern[pos / patternSize][pos % patternSize][i].note <= 127 && trackerCh[i].midiChannelMappings != 9)
		{ // perc channel doesn't always have note off
			oldestChannels[i].priority = 0;
		}
		else
		{
			oldestChannels[i].priority = order*patternSize + row - pos;
		}

		oldestChannels[i].channel = i;

	}

	qsort(oldestChannels, FM_ch, sizeof(old_channel), sortChannels);

	return oldestChannels[0].priority > 0;
}

static int midi_findoldestChannelForward(struct egtsynth *egt)
{

	for (unsigned i = 0; i < FM_ch; i++)
	{
		if (trackerCh[i].stolenUsed)
		{
			oldestChannels[i].priority = 0;
			oldestChannels[i].channel = i;
			continue;
		}
		/* Looking forward for the next note on/off command */

		int pos = order*patternSize + row;
		if (egt->pattern[pos / patternSize][pos % patternSize][i].note == 128)
			pos++;

		while (pos < egt->patternCount * patternSize - 1 && egt->pattern[pos / patternSize][pos % patternSize][i].note == 255)
		{
			pos++;
		}


		oldestChannels[i].age = pos - (order*patternSize + row);

		/* Discard channels finishing with a note off (means that a note was playing */

		if (egt->pattern[pos / patternSize][pos % patternSize][i].note == 128)
		{
			oldestChannels[i].age = 0;
		}

		oldestChannels[i].priority = oldestChannels[i].age;

		if (oldestChannels[i].priority > 0)
		{
			/* Start walking backwards */
			pos = order*patternSize + row;
			while (pos > 0 && egt->pattern[pos / patternSize][pos % patternSize][i].note == 255)
			{
				pos--;
			}

			/* Note off found or drum : the channel is free */
			if (egt->pattern[pos / patternSize][pos % patternSize][i].note == 128
				|| egt->pattern[pos / patternSize][pos % patternSize][i].note < 128 && egt->pattern[pos / patternSize][pos % patternSize][i].instr > 127 && pos - (order*patternSize + row) < -3)
			{
				oldestChannels[i].priority += 1000;
			}
		}

		oldestChannels[i].channel = i;

	}

	qsort(oldestChannels, FM_ch, sizeof(old_channel), sortChannels);

	if (oldestChannels[0].priority > 0)
	{

		/* Store last channel vol/pan to be able to restore it afterwards */

		int pos = min(egt->patternCount * patternSize, order * patternSize + row + oldestChannels[0].age);
		int volFound = 0;
		int panFound = 0;
		while (pos > 0 && (!volFound || !panFound))
		{
			if (egt->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx == 'M')
			{
				trackerCh[oldestChannels[0].channel].oldVol = egt->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx;
				volFound = 1;
			}
			else if (egt->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx == 'X')
			{
				trackerCh[oldestChannels[0].channel].oldPan = egt->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx;
				panFound = 1;
			}
			pos--;
		}
		if (pos == 0)
		{
			if (!volFound)
			{
				trackerCh[oldestChannels[0].channel].oldVol = egt->ch[oldestChannels[0].channel].initial_vol;
			}
			if (!panFound)
			{
				trackerCh[oldestChannels[0].channel].oldPan = egt->ch[oldestChannels[0].channel].initial_pan;
			}
		}

		/* Cleanup the stolen channel */

		for (int i = order * patternSize + row; i < min(egt->patternCount * patternSize, order * patternSize + row + oldestChannels[0].age); i++)
		{

			egt->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].vol = 255;
			if (!isGlobalEffect(egt->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].fx))
			{
				egt->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].fx = 255;
				egt->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].vol = 255;
				egt->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].fxdata = 255;
			}
		}
		return 1;
	}
	return 0;
}

static void midi_writefx(int realChannel, int fx, int fxdata, int rowOffset, struct egtsynth *egt)
{
	int pos = order*patternSize + row + rowOffset;

	if (fx == 'M')
	{
		/* Don't write the command if the channel volume is already the same */
		if (trackerCh[realChannel].vol == fxdata)
			return;

		trackerCh[realChannel].vol = fxdata;

		/* The first channel volume command must be stored as initial_vol, not effect */

		if (pos > 0)
		{
			pos--;
			while (pos > 0 && egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 'M')
				pos--;
		}

		if (pos <= 0 && !trackerCh[realChannel].isInitialVolSet)
		{
			trackerCh[realChannel].isInitialVolSet = 1;
			egt->ch[realChannel].initial_vol = fxdata;
			return;
		}
	}
	else if (fx == 'X')
	{
		/* Don't write the command if the channel panning is already the same */
		if (trackerCh[realChannel].pan == fxdata)
			return;

		trackerCh[realChannel].pan = fxdata;

		/* The first channel panning command must be stored as initial_pan, not effect */

		if (pos > 0)
		{
			pos--;
			while (pos > 0 && egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 'X')
				pos--;
		}

		if (pos <= 0 && !trackerCh[realChannel].isInitialPanSet)
		{
			trackerCh[realChannel].isInitialPanSet = 1;
			egt->ch[realChannel].initial_pan = fxdata;
			return;
		}
	}



	// move already existing global event to another channel if needed
	if (isGlobalEffect(egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx) && egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
	{

		for (unsigned ch = 0; ch < FM_ch; ch++)
		{
			if (egt->pattern[pos / patternSize][pos%patternSize][ch].fx == 255)
			{
				egt->pattern[pos / patternSize][pos%patternSize][ch].fx = egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx;
				egt->pattern[pos / patternSize][pos%patternSize][ch].fxdata = egt->pattern[pos / patternSize][pos%patternSize][realChannel].fxdata;
				egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx = 255;
				break;
			}
			if (ch == FM_ch - 1)
			{ // no free channel found : keep the global event, don't write the new effect
				return;
			}
		}
	}

	pos = order*patternSize + row + rowOffset;


	if (egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
	{
		/* write global effects to other patterns if another effect is already there */
		if (isGlobalEffect(fx))
		{
			for (unsigned ch = 0; ch < FM_ch; ch++)
			{
				if (egt->pattern[pos / patternSize][pos%patternSize][ch].fx == 255)
				{
					realChannel = ch;
					break;
				}
			}
		}
		/* Other effects, try to put them before/after if effect already there, except Delays that are useless if on another row */
		else if (fx != 'D' && fx != 'I')
		{

			/* Try before */
			if (pos > 0 && egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 255 && egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
			{
				pos--;
				/* Try after */
				if (pos < egt->patternCount*patternSize - 2 && egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 255 && egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
				{
					pos += 2;
					/* Reset at initial position */
					if (egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 255)
					{
						pos--;
						/* Keep important channel volume 'M'/'I' effects, discard others */
						if (fx != 'M' && fx != 'I')
							return;
					}
				}
			}
		}
		else
		{
			if (egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'M' && pos > 0)
			{
				egt->pattern[(pos - 1) / patternSize][(pos - 1) % patternSize][realChannel].fx = egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx;
				egt->pattern[(pos - 1) / patternSize][(pos - 1) % patternSize][realChannel].fxdata = egt->pattern[pos / patternSize][pos%patternSize][realChannel].fxdata;
			}
		}
	}
	if (fx == 'D')
	{
		if (egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'M' || egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'X'
			|| egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'I')
		{
			return;
		}
	}
	else if (fx == 'I')
	{
		if (egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'M' || egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'X')
		{
			return;
		}
	}

	egt->pattern[pos / patternSize][pos%patternSize][realChannel].fx = fx;
	egt->pattern[pos / patternSize][pos%patternSize][realChannel].fxdata = fxdata;
}

static void midi_effect(int midiChannel, unsigned char fx, unsigned char fxdata, struct egtsynth *egt)
{

	if (fx == 'M') { midiCh[midiChannel].vol = fxdata; }

	if (fx == 'X') { midiCh[midiChannel].pan = fxdata; }

	for (unsigned i = 0; i < FM_ch; i++)
	{
		if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack)
		{
			midi_writefx(i, fx, fxdata, 0, egt);
		}
	}

}

static void updateChannelParams(int realChannel, int midiChannel, egtsynth *egt)
{

	midi_writefx(realChannel, 'X', midiCh[midiChannel].pan, 0, egt);
	midi_writefx(realChannel, 'M', midiCh[midiChannel].vol, 0, egt);

}

static int reserveChannel(int note, int midiChannel, egtsynth *egt)
{

	int channel = -1;

	for (unsigned i = 0; i < FM_ch; i++)
	{
		// same note already playing OR mono mode
		if (trackerCh[i].midiChannelMappings == midiChannel && (trackerCh[i].noteOn == note + 1 || midiCh[midiChannel].channelPoly == 0) && trackerCh[i].midiTrackMappings == currentTrack)
		{
			channel = i;
			break;
		}
	}

	if (channel == -1)
	{
		for (unsigned i = 0; i < FM_ch; i++)
		{
			// channel previously used by the same instrument
			if (trackerCh[i].midiChannelMappings == midiChannel && (trackerCh[i].noteOn == 0
				|| midiChannel == 9 && (note == 42 || note == 44 || note == 46) && (trackerCh[i].noteOn == 43 || trackerCh[i].noteOn == 45 || trackerCh[i].noteOn == 47)) // group high hat on the same channel
				&& trackerCh[i].midiTrackMappings == currentTrack)
			{
				channel = i;
				break;
			}
		}
	}

	if (channel == -1)
	{
		for (unsigned i = 0; i < FM_ch; i++)
		{
			// unused channel
			if (trackerCh[i].midiChannelMappings == -1)
			{
				channel = i;
				// copy midi channel vol/pan to the new allocated channel
				trackerCh[i].firstNotePos = order*patternSize + row;
				updateChannelParams(i, midiChannel, egt);

				break;
			}
		}
	}

	if (channel == -1)
	{

		/* MIDI format 0 */
		if (tracks == 1)
		{
			if (midi_findoldestChannelBackward(egt))
			{
				channel = oldestChannels[0].channel;
				updateChannelParams(channel, midiChannel, egt);
			}
		}
		/* MIDI format 1 is far more complicated to handle */
		else
		{

			if (midi_findoldestChannelForward(egt) /*(16*(8.0/egt->diviseur)*(currentTempo/120.0))*/)
			{ // only if long-time inactive channel

				channel = oldestChannels[0].channel;
				trackerCh[channel].stolenUsed = 1;
				trackerCh[channel].age = oldestChannels[0].age;
				updateChannelParams(channel, midiChannel, egt);
			}
		}
	}


	if (channel >= 0)
	{
		trackerCh[channel].midiChannelMappings = midiChannel;
		trackerCh[channel].midiTrackMappings = currentTrack;
		trackerCh[channel].noteOn = note + 1;
	}
	return channel;
}

static int freeChannel(int note, int midiChannel)
{

	for (unsigned i = 0; i < FM_ch; i++)
	{

		if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].noteOn - 1 == note && trackerCh[i].midiTrackMappings == currentTrack)
		{
			if (!midiCh[midiChannel].pedal)
			{
				trackerCh[i].noteOn = 0;
			}
			return i;
		}
	}
	return -1;
}

static int midi_writeDelay(int channel, egtsynth *egt)
{

	float delay = abs(realRow - (int)realRow);

	if (delay >= 0.125 - 0.5*0.125)
	{
		if ((int)round(8 * delay) == 8)
		{
			return 1;
		}
		else
		{
			midi_writefx(channel, 'D', (int)round(8 * delay), 0, egt);
		}
	}

	return 0;
}

static void midi_noteOff(int note, int midiChannel, egtsynth *egt)
{
	if (midiChannel == 9)
		return;
	int channel;
	if ((channel = freeChannel(note, midiChannel)) >= 0)
	{

		if (trackerCh[channel].stolenUsed)
			trackerCh[channel].stolenUsed = 0;

		if (midiCh[midiChannel].pedal)
		{
			trackerCh[channel].pedalCanRelease = note + 1;
			return;
		}
		trackerCh[channel].pedalCanRelease = 0;

		/* fast note on/off : tracker quantification would put them on the same row... */
		if (egt->pattern[order][row][channel].note == note)
		{
			int pos = order*patternSize + row + 1;
			if (pos / patternSize < egt->patternCount && egt->pattern[pos / patternSize][pos % patternSize][channel].note == 255)
			{
				egt->pattern[pos / patternSize][pos%patternSize][channel].note = 128;
			}
		}
		/* free slot, just write the note off */
		else if (egt->pattern[order][row][channel].note == 255)
		{
			egt->pattern[order][row][channel].note = 128;
		}
		/* note with no instr : fake pitch bend, stop */
		else if (egt->pattern[order][row][channel].instr == 255)
		{
			egt->pattern[order][row][channel].note = 128;
		}
		if (egt->pattern[order][row][channel].note == 128)
		{
			midi_writeDelay(channel, egt);
		}
	}
}

static void midi_noteOn(int note, int volume, int midiChannel, int instrument, egtsynth *egt)
{

	if (midiCh[midiChannel].localKeyboard == 0)
		return;
	int channel;
	if (volume == 0)
	{
		midi_noteOff(note, midiChannel, egt);
	}
	else
	{
		int addedPercussion = -1;
		if (midiChannel == 9)
		{
			// handle timpani from orchestra drum kit
			if (midiCh[midiChannel].drumKit == 48 && note > 40 && note < 54)
			{
				instrument = instrumentExists(47, egt);
			}
			else
			{
				addedPercussion = instrumentExists(128+(note - 24), egt);
			}
		}

		if ((channel = reserveChannel(note, midiChannel % 16, egt)) >= 0)
		{
			// same row (happen in case of very fast note < quantization)
			if (egt->pattern[order][row][channel].note < 128)
			{
				trackerCh[channel].noteOn = egt->pattern[order][row][channel].note + 1;
				if ((channel = reserveChannel(note, midiChannel % 16, egt)) < 0)
					return;
			}

			trackerCh[channel].pedalCanRelease = 0;
			int pos = order*patternSize + row + 1;
			if (pos / patternSize == egt->patternCount)
			{
				egt_insertPattern(egt, patternSize, egt->patternCount);
			}
			if (egt->pattern[pos / patternSize][pos%patternSize][channel].note == 128)
			{ // remove a note off that was added by a fast note on the same row (happen in case of very fast note < quantization)
				egt->pattern[pos / patternSize][pos%patternSize][channel].note = 255;
			}
			trackerCh[channel].lastNoteVol = volume;

			if (midi_writeDelay(channel, egt))
			{

				egt->pattern[pos / patternSize][pos%patternSize][channel].note = addedPercussion >= 0 ? 60 : note;
				egt->pattern[pos / patternSize][pos%patternSize][channel].vol = (volume / 1.282828)*midiCh[midiChannel].expression / 99.0;

				if (!midiCh[midiChannel].legato)
					egt->pattern[pos / patternSize][pos%patternSize][channel].instr = addedPercussion >= 0 ? addedPercussion : instrument;

				if (addedPercussion >= 0)
				{
					int nextPos = pos + 1;
					if (nextPos / patternSize < egt->patternCount && egt->pattern[nextPos / patternSize][nextPos % patternSize][channel].note == 255)
						egt->pattern[nextPos / patternSize][nextPos % patternSize][channel].note = 128;
				}
			}
			else
			{
				egt->pattern[order][row][channel].note = addedPercussion >= 0 ? 60 : note;
				egt->pattern[order][row][channel].vol = (volume / 1.282828)*midiCh[midiChannel].expression / 99.0;

				if (!midiCh[midiChannel].legato)
					egt->pattern[order][row][channel].instr = addedPercussion >= 0 ? addedPercussion : instrument;

				if (addedPercussion >= 0 && egt->pattern[pos / patternSize][pos % patternSize][channel].note == 255)
					egt->pattern[pos / patternSize][pos % patternSize][channel].note = 128;
			}

			trackerCh[channel].channelPBend = note;
		}
	}
}

// global effects (tempo, loops...)
static void midi_globalEffect(unsigned char fx, unsigned char fxdata, egtsynth *egt)
{

	int emptyChannel = 0;
	int pos = patternSize*order + row;

	if (fx == 'B' || fx == 'C')
		pos = max(0, pos - 1);

	while (egt->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx != 255 && emptyChannel < FM_ch - 1 && egt->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx != fx)
	{
		if (fx == egt->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx)
			return;
		emptyChannel++;
	}
	egt->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx = fx;
	egt->pattern[pos / patternSize][pos%patternSize][emptyChannel].fxdata = fxdata;
}

static void midi_expression(int midiChannel, int vol, egtsynth *egt)
{
	if (midiCh[midiChannel].expression == (int)((midiCh[midiChannel].vol*0.0101010101010101)*vol / 1.282828))
		return;

	midiCh[midiChannel].expression = (midiCh[midiChannel].vol*0.0101010101010101)*vol / 1.282828; // 0-127 to 0-1 range
	for (unsigned i = 0; i < FM_ch; i++)
	{
		if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack)
		{
			egt->pattern[order][row][i].vol = midiCh[midiChannel].expression*trackerCh[i].lastNoteVol / 127.0;
		}
	}
}

static void midi_handleEvents(int type, int midiChannel, unsigned char data, egtsynth *egt, char *egt_data)
{
	/* Check if some stolen channels have expired */
	for (unsigned i = 0; i < FM_ch; i++)
	{
		if (trackerCh[i].age > 0)
		{

			trackerCh[i].age -= order*patternSize + row - lastPos;

			if (trackerCh[i].age <= 0)
			{

				trackerCh[i].stolenUsed = 0;

				if (trackerCh[i].oldPan != trackerCh[i].pan)
					midi_writefx(i, 'X', trackerCh[i].oldPan, trackerCh[i].age, egt);
				if (trackerCh[i].oldVol != trackerCh[i].vol)
					midi_writefx(i, 'M', trackerCh[i].oldVol, trackerCh[i].age, egt);

				/* This channel is now available again, 99 (or any other fake value) to ensure those channels stays in 'stealing' mode */
				trackerCh[i].midiChannelMappings = 99;
				trackerCh[i].midiTrackMappings = 99;
				trackerCh[i].noteOn = 0;
			}
		}
	}
	lastPos = order*patternSize + row;
	unsigned char data2 = 0;
	if (type != 0xC && type != 0xD)
		readFromMemory(egt, &data2, 1, egt_data);

	switch (type)
	{
		case 8: // note off
			midi_noteOff(data, midiChannel, egt);
			break;
		case 9: // note on

			// wtf a midi without any program change ? hello Masami ?!
			if (midiCh[midiChannel].currentInstr == -1 && midiChannel != 9)
			{
				midiCh[midiChannel].currentInstr = 0;
			}
			midi_noteOn(data, data2, midiChannel, midiCh[midiChannel].currentInstr, egt);

			break;
		case 0xA: // Polyphonic Key Pressure
			break;
		case 0xB: // Controller Change

			switch (data)
			{
				case 0x01: // modulation (handled as vibrato)
					midi_effect(midiChannel, 'H', 96 + data2 / 16, egt);
					break;
				case 0x06: // RPN param (1st part)
					if (rpnSelect1 == 0 && rpnSelect2 == 0)
					{ // pitch bend sensitivity
						midiCh[midiChannel].pitchBendRange = data2;
					}
					/*if (rpnSelect1 == 0 && rpnSelect2 == 1){ // master fine tuning

					}*/
					if (rpnSelect1 == 0 && rpnSelect2 == 2)
					{ // master coarse tuning
						egt->transpose = data2 - 64;
					}
					break;
				case 0x26: // (38) RPN param (2nd part)
					if (rpnSelect1 == 0 && rpnSelect2 == 0)
					{ // pitch bend sensitivity

					}
					break;
				case 0x07: // channel volume

					midi_effect(midiChannel, 'M', data2 / 1.282828, egt);
					break;
				case 0x08: // channel balance
				case 0x0A: // (10) channel panning
					midi_effect(midiChannel, 'X', data2 * 2, egt);
					break;
				case 0x0B: // (11) expression controller -- handled as volume tracker commands

					midi_expression(midiChannel, data2, egt);

					break;
				case 0x40: // (64) sustain pedal
					midiCh[midiChannel].pedal = data2 > 63;

					/* Releasing the pedal should stop the notes playing on this channel */
					if (!midiCh[midiChannel].pedal)
					{
						for (unsigned i = 0; i < FM_ch; i++)
						{
							if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack && trackerCh[i].pedalCanRelease == trackerCh[i].noteOn)
							{

								/* Free cell */
								if (egt->pattern[order][row][i].note == 255)
								{
									egt->pattern[order][row][i].note = 128;

								}
								/* Occupied cell : write into next row */
								else
								{
									int pos = order*patternSize + row + 1;
									if (pos / patternSize < egt->patternCount && egt->pattern[pos / patternSize][pos % patternSize][i].note == 255)
									{
										egt->pattern[pos / patternSize][pos%patternSize][i].note = 128;
									}
								}
								trackerCh[i].noteOn = 0;
							}
						}
					}
					break;
				case 0x44: // (68) legato pedal
					midiCh[midiChannel].legato = data2 > 63;
					break;
				case 0x62: /* (98) NRPN select1 -- not handled atm */
					rpnSelect1 = 127;
					break;
				case 0x63: /* (99) NRPN select2 -- not handled atm */
					rpnSelect2 = 127;
					break;
				case 0x64: /* (100) RPN select1 */
					rpnSelect1 = data2;
					break;
				case 0x65: /* (101) RPN select2 */
					rpnSelect2 = data2;
					break;
				case 0x74: /* (116) loop start */
				case 0x76: /* 118 */
				case 111: /* rpg maker loop points */
					loopStart = order*patternSize + row;
					break;
				case 0x75: /* (117) loop end */
				case 0x77: /* 119 */
					midi_globalEffect('B', loopStart / patternSize, egt);
					midi_globalEffect('C', loopStart%patternSize, egt);

					loopStart = -1;
					break;
				case 0x78: // (120) all sound off */
				case 0x7B: // (123) all notes off */
					for (unsigned i = 0; i < FM_ch; i++)
					{
						if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack)
						{
							egt->pattern[order][row][i].note = 128;
							trackerCh[i].noteOn = 0;
						}
					};
					break;
				case 0x79: /* (121) controller reset */
					midiCh[midiChannel].channelPoly = 1;
					midiCh[midiChannel].pedal = 0;
					break;
				case 0x7A: /* (122) local keyboard */
					midiCh[midiChannel].localKeyboard = data2 / 64;
					break;
				case 0x7E: /* (126) mono channel */
					midiCh[midiChannel].channelPoly = 0;
					break;
				case 0x7F: /* (127) polyphonic channel */
					midiCh[midiChannel].channelPoly = 1;
					break;
			}
			break;
		case 0xC: /* Program Change */
			if (midiChannel % 16 == 9)
			{
				midiCh[midiChannel].drumKit = data;
			}
			else
			{
				midiCh[midiChannel].currentInstr = data;
			}
			break;
		case 13: /* currentChannelNumber Key Pressure */
			break;
		case 14: /* Pitch Bend */
			if (midiCh[midiChannel].pitchBendRange <= 2)
				midi_effect(midiChannel, 'I', 2 * data2 + (data > 63), egt); /* use 1 bit from lsb for more precision (0-127 to 0-255 range) */
			else
			{
				for (unsigned i = 0; i < FM_ch; i++)
				{
					if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack && trackerCh[i].noteOn)
					{
						float ratio = (float)midiCh[midiChannel].pitchBendRange / 128;
						if (egt->pattern[order][row][i].instr == 255 && egt->pattern[order][row][i].note == 255 ||
							egt->pattern[order][row][i].instr == 255 && egt->pattern[order][row][i].note <128)
						{
							int pitchBendNote = trackerCh[i].noteOn - 1 + (2 * data2 + (data>63) - 128) * ratio + 0.5;
							pitchBendNote = clamp(pitchBendNote, 0, 127);
							if (trackerCh[i].channelPBend != pitchBendNote)
							{

								egt->pattern[order][row][i].note = pitchBendNote;
								trackerCh[i].channelPBend = pitchBendNote;
								midi_writeDelay(i, egt);
							}
						}
					}
				}
			}
			break;
	}
}

static int parseMidiRows(unsigned short delta_time_ticks, egtsynth *egt, char *data)
{
	unsigned char eventType, eventData;
	realRow = 0;
	row = 0, order = -1;
	int lastStatus = 0, temp = 0;
	long long deltaAcc = 0;

	rpnSelect1 = rpnSelect2 = 127; /* rpn default is null */
	patternSize = 128;

	for (unsigned i = 0; i < 16; i++)
	{
		midiCh[i].currentInstr = -1;
		midiCh[i].vol = 99;
		midiCh[i].drumKit = 1;
		midiCh[i].pan = 127;
		midiCh[i].channelPoly = 1;
		midiCh[i].expression = 99;
		midiCh[i].pitchBendRange = 2; /* default is 2 semitones */
		midiCh[i].localKeyboard = 1;
		midiCh[i].pedal = midiCh[i].firstNote = midiCh[i].legato = 0;
	}

	double roundRow = 0.5;

	while (order >= -1 && egt->readSeek < egt->totalFileSize)
	{ /* order set to -1 when end of track is found */

		deltaAcc += readVarLen(egt, data) / tempoDivisor;
		realRow = deltaAcc / (delta_time_ticks / (double)egt->diviseur) + roundRow;

		while (realRow >= patternSize*(order + 1))
		{
			order++;
			if (order > maxOrder)
			{
				if (order < 255)
				{
					egt_insertPattern(egt, patternSize, egt->patternCount);
					maxOrder = order;
				}
				else
				{
					return 0;
				}
			}
		}
		row = (int)(realRow) % patternSize;
		readFromMemory(egt, &eventType, 1, data);

		// Running status !
		if (eventType / 16 < 8)
		{

			midi_handleEvents(lastStatus / 16, lastStatus % 16, eventType, egt, data);
		}
		else
		{ // New status
			if (eventType / 16 < 15)
			{
				readFromMemory(egt, &eventData, 1, data);
				midi_handleEvents(eventType / 16, eventType % 16, eventData, egt, data);
			}
			else
			{ // Meta event
				if (eventType % 16 < 8)
				{ //sysex
					temp = readVarLen(egt, data);
					char *sysex = (char*)malloc(temp);
					readFromMemory(egt, sysex, temp, data);
					// discard sysex message and advance
				}
				else
				{// meta 
					readFromMemory(egt, &eventType, 1, data);
					switch (eventType)
					{
						case 0x00: // seq number
							egt->readSeek += 3;
							break;
						case 0x01: // text
						case 0x02: // copyright
						case 0x03: // seq name
						case 0x04: // instr name
						case 0x05: // lyrics
						case 0x06:// marker
						case 0x07: // cue point
						case 0x7F:{ // sequencer specific data
									  temp = readVarLen(egt, data);
									  char* d = (char*)malloc(temp);
									  readFromMemory(egt, d, temp, data);

									  if (eventType == 0x03) // sequence name
										  strncpy(egt->songName, d, min(63, temp));
									  else if (eventType == 0x02) // copyright
										  strncpy(egt->author, d, min(63, temp));
									  else if (eventType == 0x01)
									  { // text
										  strncpy(egt->comments, d, min(255, temp));
									  }
									  else if (eventType == 0x06)
									  {
										  if (strncmp(d, "loopStart", temp) == 0)
										  { // FF7's loop points
											  loopStart = order*patternSize + row;
										  }
										  else if (strncmp(d, "loopEnd", temp) == 0)
										  {
											  midi_globalEffect('B', loopStart / patternSize, egt);
											  midi_globalEffect('C', loopStart%patternSize, egt);
										  }
									  }
									  free(d);
						}break;

						case 0x20: // MIDI currentChannelNumber Prefix
						case 0x21: // prefix port
							egt->readSeek += 2;
							break;
						case 0x2F: // end of track
							if (order*patternSize + row > totalLength)
							{
								totalLength = order*patternSize + row;
							}

							egt->readSeek += 1;
							order = -2;
							break;
						case 0x51:{ // (81) tempo
									  egt->readSeek += 1;
									  unsigned char tempoRaw[3];
									  readFromMemory(egt, &tempoRaw[0], 3, data);

									  int tempo = 60000000 / ((tempoRaw[0] << 16) | (tempoRaw[1] << 8) | tempoRaw[2]);

									  // tempo > 255 : scale import speed to handle it
									  tempoDivisor = 1;
									  while (tempo > 255)
									  {
										  tempoDivisor *= 2;
										  tempo *= 0.5;
									  }

									  if (order == 0 && row == 0)
									  {
										  egt->initial_tempo = tempo;
									  }
									  else if (tempo != currentTempo)
									  {
										  midi_globalEffect('T', tempo, egt);
									  }
									  currentTempo = tempo;
						}break;
						case 0x54: // smpte offset (dunno whats this shit)
							egt->readSeek += 6;
							break;
						case 0x58: // time
							egt->readSeek += 5;
							break;

						case 0x59: // key
							egt->readSeek += 3;
							break;

					}
				}
			}
			lastStatus = eventType;
		}
	}

	return 1;
}

static int egt_loadMIDIFromMemory(egtsynth* egt, char* data)
{
	egt_clearSong(egt);
	egt_setVolume(egt, 60);
	egt->diviseur = 16;
	egt->initial_tempo = 120;
	loopStart = -1;
	totalLength = 0;
	tempoDivisor = 1;
	for (unsigned i = 0; i < FM_ch; i++)
	{
		egt->ch[i].initial_reverb = 20;
		trackerCh[i].firstNotePos = -1;
		trackerCh[i].midiChannelMappings = -1;
		trackerCh[i].midiTrackMappings = -1;
		trackerCh[i].noteOn = 0;
		trackerCh[i].age = -1;
		trackerCh[i].pan = -1;
		trackerCh[i].vol = -1;
		trackerCh[i].channelPBend = -1;
		trackerCh[i].isInitialPanSet = 0;
		trackerCh[i].isInitialVolSet = 0;
		trackerCh[i].stolenUsed = 0;
	}

	unsigned short delta_time_ticks;

	egt->readSeek += 4; // ignore header chunk
	readFromMemory(egt, (char *)&midiFormat, 2, data);
	midiFormat = (midiFormat >> 8) | (midiFormat << 8);
	readFromMemory(egt, (char *)&tracks, 2, data);
	tracks = (tracks >> 8) | (tracks << 8);
	readFromMemory(egt, (char *)&delta_time_ticks, 2, data);
	delta_time_ticks = (delta_time_ticks >> 8) | (delta_time_ticks << 8);

	maxOrder = -1;

	for (currentTrack = 0; currentTrack < tracks; currentTrack++)
	{
		egt->readSeek += 8; // expecting MTrk + chunk size, we dont need them

		if (!parseMidiRows(delta_time_ticks, egt, data))
			break;
	}
	/* rpg maker loop point , */
	if (loopStart >= 0)
	{
		row = totalLength%patternSize;
		order = totalLength / patternSize;
		midi_globalEffect('B', loopStart / patternSize, egt);
		midi_globalEffect('C', loopStart%patternSize, egt);
		loopStart = -1;
	}

	egt_buildStateTable(egt, 0, egt->patternCount, 0, FM_ch);

	return 0;
}

#pragma pack(push, 1)
typedef struct
{
    char        id[4];
    uint16_t        scoreLen;
    uint16_t        scoreStart;
} HDR_MUS;

typedef struct
{
    char        id[4];
    int         length;
    uint16_t        type;
    uint16_t        ntracks;
    uint16_t        ticks;
} HDR_MID;
#pragma pack(pop)

static char        magicMus[4] = {'M', 'U', 'S', 0x1a};
static char        magicMid[4] = {'M', 'T', 'h', 'd'};
static char        magicTrk[4] = {'M', 'T', 'r', 'k'};

static int         controllerMap[16] = {-1, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121, -1};

static uint8_t        *midData;
static int         midSize;

static uint8_t        *musPos;
static int         musEOT;

static uint8_t        deltaBytes[4];
static int         deltaCount;

// maintain a list of channel volume
static uint8_t        musChannel[16];

static void mus_event_convert()
{
    uint8_t        data, last, channel;
    uint8_t        event[3];
    int         count;

    data = *musPos++;
    last = data & 0x80;
    channel = data & 0xf;

    switch (data & 0x70)
    {
      case 0x00:
        event[0] = 0x80;
        event[1] = *musPos++ & 0x7f;
        event[2] = musChannel[channel];
        count = 3;
        break;

      case 0x10:
        event[0] = 0x90;
        data = *musPos++;
        event[1] = data & 0x7f;
        event[2] = data & 0x80 ? *musPos++ : musChannel[channel];
        musChannel[channel] = event[2];
        count = 3;
        break;

      case 0x20:
        event[0] = 0xe0;
        event[1] = (*musPos & 0x01) << 6;
        event[2] = *musPos++ >> 1;
        count = 3;
        break;

      case 0x30:
        event[0] = 0xb0;
        event[1] = controllerMap[*musPos++ & 0xf];
        event[2] = 0x7f;
        count = 3;
        break;

      case 0x40:
        data = *musPos++;
        if (data == 0)
        {
            event[0] = 0xc0;
            event[1] = *musPos++;
            count = 2;
            break;
        }
        event[0] = 0xb0;
        event[1] = controllerMap[data & 0xf];
        event[2] = *musPos++;
        count = 3;
        break;

      case 0x50:
        return;

      case 0x60:
        event[0] = 0xff;
        event[1] = 0x2f;
        event[2] = 0x00;
        count = 3;

        // this prevents deltaBytes being read past the end of the MUS data
        last = 0;

        musEOT = 1;
        break;

      case 0x70:
        musPos++;
        return;
    }

    if (channel == 9)
        channel = 15;
    else if (channel == 15)
        channel = 9;

    event[0] |= channel;

    midData = realloc(midData, midSize + deltaCount + count);

    memcpy(midData + midSize, &deltaBytes, deltaCount);
    midSize += deltaCount;
    memcpy(midData + midSize, &event, count);
    midSize += count;

    if (last)
    {
        deltaCount = 0;
        do
        {
            data = *musPos++;
            deltaBytes[deltaCount] = data;
            deltaCount++;
        } while (data & 128);
    }
    else
    {
        deltaBytes[0] = 0;
        deltaCount = 1;
    }
}

static uint8_t *mus2midi(uint8_t *data, int *length)
{
    HDR_MUS     *hdrMus = (HDR_MUS *)data;
    HDR_MID     hdrMid;
    int         midTrkLenOffset;
    int         trackLen;
    int         i;

    if (strncmp(hdrMus->id, magicMus, 4) != 0)
        return NULL;

    if (*length != hdrMus->scoreStart + hdrMus->scoreLen)
        return NULL;

    midSize = sizeof(HDR_MID);
    memcpy(hdrMid.id, magicMid, 4);
    hdrMid.length = 6;
	hdrMid.length = ((hdrMid.length << 24) | ((hdrMid.length << 8) & 0x00FF0000) | ((hdrMid.length >> 8) & 0x0000FF00) | (hdrMid.length >> 24));
    hdrMid.type = 0;
    hdrMid.ntracks = 1;
	hdrMid.ntracks = ((hdrMid.ntracks << 8) | (hdrMid.ntracks >> 8));
    // maybe, set 140ppqn and set tempo to 1000000µs
    hdrMid.ticks = 70; // 70 ppqn = 140 per second @ tempo = 500000µs (default)
    hdrMid.ticks = ((hdrMid.ticks << 8) | (hdrMid.ticks >> 8));
	midData = malloc(midSize);
    memcpy(midData, &hdrMid, midSize);

    midData = realloc(midData, midSize + 8);
    memcpy(midData + midSize, magicTrk, 4);
    midSize += 4;
    midTrkLenOffset = midSize;
    midSize += 4;

    trackLen = 0;

    musPos = data + hdrMus->scoreStart;
    musEOT = 0;
    deltaBytes[0] = 0;
    deltaCount = 1;

    for (i = 0; i < 16; i++)
        musChannel[i] = 0;

    while (!musEOT)
        mus_event_convert();

    trackLen = (midSize - sizeof(HDR_MID) - 8);
	trackLen = ((trackLen << 24) | ((trackLen << 8) & 0x00FF0000) | ((trackLen >> 8) & 0x0000FF00) | (trackLen >> 24));
    memcpy(midData + midTrkLenOffset, &trackLen, 4);

    *length = midSize;

    return midData;
}

static int egt_loadEGTSFromMemory(egtsynth* egt, char* data)
{
	unsigned char nbOrd, nbRow, temp;
	unsigned int error = 0;

	if (egt->totalFileSize < 3 * FM_ch + 6)
	{
		return EGT_ERR_FILECORRUPTED;
	}

	egt->readSeek = 5;
	readFromMemory(egt, (char *)&temp, 1, data);

	if (temp != EDGETRACKER_VERSION)
	{
		return EGT_ERR_FILEVERSION;
	}

	egt_clearSong(egt);

	egt->order = egt->row = 0;
	egt_patternClear(egt);

	readFromMemory(egt, (char *)&temp, 1, data);

	readFromMemory(egt, &egt->songName[0], temp, data);
	egt->songName[temp] = 0;

	readFromMemory(egt, (char *)&temp, 1, data);
	readFromMemory(egt, &egt->author[0], temp, data);
	egt->author[temp] = 0;

	readFromMemory(egt, (char *)&temp, 1, data);
	readFromMemory(egt, &egt->comments[0], temp, data);
	egt->comments[temp] = 0;

	readFromMemory(egt, (char *)&egt->initial_tempo, sizeof(egt->initial_tempo), data);
	egt->initial_tempo = max(1, egt->initial_tempo);

	readFromMemory(egt, (char *)&egt->diviseur, sizeof(egt->diviseur), data);
	egt->diviseur = clamp(egt->diviseur, 1, 32);

	readFromMemory(egt, (char *)&egt->_globalVolume, sizeof(egt->_globalVolume), data);

	egt_setVolume(egt, egt->_globalVolume);

	readFromMemory(egt, &egt->transpose, sizeof(egt->transpose), data);

	readFromMemory(egt, (char *)&temp, sizeof(temp), data);
	egt->initialReverbLength = (float)temp / 160;

	readFromMemory(egt, (char *)&temp, sizeof(temp), data);
	egt->initialReverbRoomSize = (float)temp / 160;

	egt_initReverb(egt, egt->initialReverbRoomSize);

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		egt->ch[ch].cInstr = 0;
		readFromMemory(egt, (char *)&egt->ch[ch].initial_pan, sizeof(egt->ch[ch].initial_pan), data); // ch panning

		readFromMemory(egt, (char *)&egt->ch[ch].initial_vol, sizeof(egt->ch[ch].initial_vol), data); // ch volume
		egt->ch[ch].initial_vol = min(egt->ch[ch].initial_vol, 99);

		readFromMemory(egt, (char *)&egt->ch[ch].initial_reverb, sizeof(egt->ch[ch].initial_reverb), data); // ch volume
		egt->ch[ch].initial_reverb = min(egt->ch[ch].initial_reverb, 99);
	}

	readFromMemory(egt, (char *)&nbOrd, sizeof(nbOrd), data);
	egt_resizePatterns(egt, nbOrd);

	for (unsigned i = 0; i < nbOrd; i++)
	{
		readFromMemory(egt, (char *)&nbRow, sizeof(nbRow), data);

		egt_resizePattern(egt, i, max(1, nbRow), 0);

		readFromMemory(egt, (char*)&egt->pattern[i][0], sizeof(fm_cell) * egt->patternSize[i] * FM_ch, data);
	}

	readFromMemory(egt, (char *)&egt->instrumentCount, 1, data);
	egt_resizeInstrumentList(egt, egt->instrumentCount);

	if (egt->instrumentCount <= 0 || egt->instrumentCount > 255)
	{
		egt_resizeInstrumentList(egt, 1);
	}
	if (egt->patternCount == 0)
	{
		egt_resizePatterns(egt, 1);
	}

	readFromMemory(egt, (char*)&egt->instrument[0], sizeof(fm_instrument) * egt->instrumentCount, data);

	unsigned checksum;

	if (!readFromMemory(egt, (char*)&checksum, 4, data))
	{
		error++;
	}

	if (checksum != adler32(data, max(0, (int)egt->totalFileSize - 4)))
	{
		error++;
	}


	if (error)
	{
		for (int i = 0; i < egt->instrumentCount; i++)
		{
			egt_instrumentRecovery(&egt->instrument[i]);
		}
		return EGT_ERR_FILECORRUPTED;
	}

	egt_buildStateTable(egt, 0, egt->patternCount, 0, FM_ch);

	return 0;
}

static int egt_loadIMFFromMemory(egtsynth *egt, char *data, unsigned len, int rate);

int egt_loadSongFromMemory(egtsynth* egt, char* data, unsigned len, int rate)
{
	egt->readSeek = 0;
	egt->totalFileSize = len;

	if (rate != 0)
		return egt_loadIMFFromMemory(egt, data, len, rate);

	char magic_check[4] = {0, 0, 0, 0};

	readFromMemory(egt, magic_check, 4, data);

	if (!memcmp(magic_check, "MThd", 4))
	{
		return egt_loadMIDIFromMemory(egt, data);
	}
	else if (!memcmp(magic_check, "RIFF", 4))
	{
		egt->readSeek += 20;
		readFromMemory(egt, magic_check, 4, data);
		if (!memcmp(magic_check, "MThd", 4))
			return egt_loadMIDIFromMemory(egt, data);
		else
			return EGT_ERR_FILECORRUPTED;
	}
	else if (!memcmp(magic_check, "MUS\x1a", 4))
	{
		int new_length = len;
		char *new_data = mus2midi(data, &new_length);
		egt->totalFileSize = new_length;
		int result = egt_loadMIDIFromMemory(egt, new_data);
		free(new_data);
		return result;
	}
	else if (!memcmp(magic_check, "EGTS", 4) || !memcmp(magic_check, "MDTS", 4))
	{
		return egt_loadEGTSFromMemory(egt, data);
	}
	else
	{
		return EGT_ERR_FILECORRUPTED;
	}
}

char* egt_fileToMemory(egtsynth *egt, const char* filename)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp)
	{
		return 0;
	}

	fseek(fp, 0, SEEK_END);
	egt->totalFileSize = ftell(fp);
	char *all = malloc(egt->totalFileSize);
	if (!all)
	{
		return 0;
	}

	fseek(fp, 0, SEEK_SET);
	if (fread(all, egt->totalFileSize, 1, fp) != 1)
	{
		return 0;
	}
	fclose(fp);
	egt->readSeek = 0;
	return all;
}

int egt_loadSong(egtsynth* egt, const char* filename, int rate)
{
	char *data = egt_fileToMemory(egt, filename);

	if (!data)
		return EGT_ERR_FILEIO;

	int result = egt_loadSongFromMemory(egt, data, egt->totalFileSize, rate);
	free(data);


	return result;
}

void egt_clearSong(egtsynth* egt)
{
	egt_resizePatterns(egt, 0);
	egt->order = egt->row = egt->transpose = 0;
	egt_setDefaults(egt);
	memset(egt->songName, 0, 64);
	memset(egt->author, 0, 64);
	memset(egt->comments, 0, 256);
}

void egt_createDefaultInstrument(egtsynth* egt, unsigned slot)
{
	strncpy((char*)&egt->instrument[slot].name[0], "Default", 7);
	strncpy((char*)&egt->instrument[slot].magic[0], "EGTI", 4);
	egt->instrument[slot].dummy = 0;
	egt->instrument[slot].version = EDGETRACKER_VERSION;
	for (unsigned op = 0; op < FM_op; ++op)
	{
		egt->instrument[slot].op[op].connectOut = op;
		egt->instrument[slot].op[op].connect = -1;
		egt->instrument[slot].op[op].connect2 = -1;
	}
	egt->instrument[slot].volume = 99;
	egt->instrument[slot].op[0].a = 99;
	egt->instrument[slot].op[0].mult = 1;
	egt->instrument[slot].op[0].vol = 99;
	egt->instrument[slot].op[0].r = 99;
}

int egt_resizeInstrumentList(egtsynth* egt, unsigned size)
{
	if (size > 255)
	{
		return 0;
	}
	if (egt->instrumentCount > 0 && size == 0)
	{
		egt->instrumentCount = 0;
		return 1;
	}

	fm_instrument* newI = realloc(egt->instrument, sizeof(fm_instrument)*size);

	if (!newI)
	{
		return 0;
	}

	egt->instrument = newI;

	if (size > egt->instrumentCount)
	{
		memset((char*)&egt->instrument[egt->instrumentCount], 0, (size - egt->instrumentCount)*sizeof(fm_instrument));
		for (unsigned i = egt->instrumentCount; i < size; i++)
		{
			egt_createDefaultInstrument(egt,i);
		}
	}
	egt->instrumentCount = size;
	return 1;
}

int egt_resizePatterns(egtsynth* egt, unsigned count)
{
	if (count > 256)
		return 0;

	if (count < egt->patternCount && egt->pattern)
	{
		for (unsigned i = count; i < egt->patternCount; i++)
		{
			free(egt->pattern[i]);
			free(egt->channelStates[i]);
		}
		if (egt->order >= count)
			egt->order = max(0, count - 1);
	}

	unsigned oldPatternCount = egt->patternCount;

	if (count > 0)
	{
		unsigned int* newPs = realloc(egt->patternSize, sizeof(unsigned) * count);
		fm_cell(**newPa)[FM_ch] = realloc(egt->pattern, sizeof(fm_cell*) * count);
		fm_channel_state** newC = realloc(egt->channelStates, sizeof(fm_channel_state*) * count);

		if (!newPs || !newPa || !newC)
		{
			return 0;
		}
		egt->patternSize = newPs;
		egt->pattern = newPa;
		egt->channelStates = newC;
	} else {
		free(egt->patternSize);
		free(egt->pattern);
		free(egt->channelStates);
		egt->patternSize = NULL;
		egt->pattern = NULL;
		egt->channelStates = NULL;
	}
	egt->patternCount = count;

	if (count > oldPatternCount)
	{
		for (unsigned i = oldPatternCount; i < count; i++)
		{
			egt->pattern[i] = 0;
			egt->channelStates[i] = 0;
			egt->patternSize[i] = 0;
			if (!egt_resizePattern(egt, i, 1, 0))
			{
				return 0;
			}
		}
	}


	egt->channelStatesDone = 0;
	return 1;
}


int egt_clearPattern(egtsynth* egt, unsigned pattern, unsigned rowStart, unsigned count)
{
	if (pattern >= egt->patternCount || rowStart > 255 || count > 256)
		return 0;
	memset(&egt->pattern[pattern][rowStart], 255, count*sizeof(fm_cell)*FM_ch);
	memset(&egt->channelStates[pattern][rowStart], 255, count*sizeof(fm_channel_state));
	egt->channelStatesDone = 0;
	return 1;
}


int egt_insertPattern(egtsynth* egt, unsigned rows, unsigned pos)
{
	{
		if (pos > egt->patternCount || !egt_resizePatterns(egt, egt->patternCount + 1))
		return 0;
	}
	free(egt->pattern[pos]);
	free(egt->channelStates[pos]);

	for (unsigned i = egt->patternCount - 1; i > pos; i--)
	{
		egt->pattern[i] = egt->pattern[i - 1];
		egt->channelStates[i] = egt->channelStates[i - 1];
		egt->patternSize[i] = egt->patternSize[i - 1];
	}
	egt->pattern[pos] = NULL;
	egt->channelStates[pos] = NULL;

	if (!egt_resizePattern(egt, pos, rows, 0))
		return 0;
	egt_clearPattern(egt, pos, 0, rows);
	egt->channelStatesDone = 0;
	return 1;
}

int egt_removePattern(egtsynth* egt, unsigned order)
{
	if (order >= egt->patternCount)
		return 0;

	if (egt->patternCount == 1)
	{
		egt_clearPattern(egt, egt->patternCount - 1, 0, egt->patternSize[0]);
	}
	else
	{
		free(egt->pattern[order]);
		free(egt->channelStates[order]);
		for (unsigned i = order; i < egt->patternCount - 1; i++)
		{
			egt->pattern[i] = egt->pattern[i + 1];
			egt->channelStates[i] = egt->channelStates[i + 1];
			egt->patternSize[i] = egt->patternSize[i + 1];
		}
		egt->patternCount--;

	}
	egt->order = min(egt->order, egt->patternCount - 1);
	egt->row = min(egt->row, egt->patternSize[egt->order] - 1);
	egt->channelStatesDone = 0;
	return 1;
}

int egt_resizePattern(egtsynth* egt, unsigned order, unsigned size, unsigned scaleContent)
{
	if (order >= egt->patternCount || size == 0)
	{
		return 0;
	}

	int oldPatternSize = egt->patternSize[order];

	size = clamp(size, 1, 256);

	float scaleRatio = 0;
	if (scaleContent)
	{
		scaleRatio = (float)size / oldPatternSize;
	}

	/* Shrink content */
	if (scaleContent && scaleRatio < 1)
	{
		for (int i = 0; i < egt->patternSize[order]; i++)
		{
			for (int ch = 0; ch < FM_ch; ch++)
				egt->pattern[order][(unsigned)round(i*0.5)][ch] = egt->pattern[order][i][ch];
		}
	}

	fm_cell(*newP)[FM_ch] = realloc(egt->pattern[order], sizeof(fm_cell) * size * FM_ch);
	if (!newP)
	{
		return 0;
	}

	fm_channel_state* newC = realloc(egt->channelStates[order], sizeof(fm_channel_state) * size);
	if (!newC)
	{
		free(newP);
		return 0;
	}

	egt->pattern[order] = newP;
	egt->channelStates[order] = newC;

	if (size > egt->patternSize[order])
		egt_clearPattern(egt, order, egt->patternSize[order], size - egt->patternSize[order]);


	egt->patternSize[order] = size;
	egt->row = min(egt->row, egt->patternSize[egt->order] - 1);


	/* Expand content */
	if (scaleContent && scaleRatio > 1)
	{
		for (int i = oldPatternSize - 1; i >= 0; i--)
		{
			for (int ch = 0; ch < FM_ch; ch++)
				egt->pattern[order][(unsigned)(i*scaleRatio)][ch] = egt->pattern[order][i][ch];

			memset(&egt->pattern[order][(unsigned)round(i*scaleRatio) + 1], 255, sizeof(fm_cell)*FM_ch);
		}
	}
	egt->channelStatesDone = 0;
	return 1;
}

float egt_getTime(egtsynth* egt)
{
	if (egt->order >= egt->patternCount || egt->row > egt->patternSize[egt->order])
		return 0;

	return egt->channelStates[egt->order][egt->row].time;
}

int egt_saveInstrument(egtsynth* egt, const char* filename, unsigned slot)
{
	if (slot < egt->instrumentCount)
	{
		FILE *fp = fopen(filename, "wb");
		if (!fp)
			return 0;
		fputc('E', fp);
		fputc('G', fp);
		fputc('T', fp);
		fputc('I', fp);
		fputc(0x00, fp); // unused byte
		fputc(EDGETRACKER_VERSION, fp); // version
		fwrite((char*)&egt->instrument[slot].name[0], sizeof(fm_instrument)-6, 1, fp);
		fclose(fp);
		return 1;
	}
	return 0;
}

int egt_saveInstrumentBank(egtsynth* egt, const char *filename)
{
	FILE *fp = fopen(filename, "wb");

	if (!fp)
		return 0;

	fputc('E', fp);
	fputc('G', fp);
	fputc('T', fp);
	fputc('B', fp);
	fputc(0x00, fp); 				//unused byte
	fputc(EDGETRACKER_VERSION, fp);
	fputc(0x00, fp); 				//unused byte
	fputc(egt->instrumentCount, fp);
	for (int i = 0; i < egt->instrumentCount; ++i)
	{
		fputc('S', fp);
		fputc('L', fp);
		fputc('O', fp);
		fputc('T', fp);
		fputc(i, fp);
		fputc('E', fp);
		fputc('G', fp);
		fputc('T', fp);
		fputc('I', fp);
		fputc(0x00, fp); // unused byte
		fputc(EDGETRACKER_VERSION, fp);
		fwrite((char*)&egt->instrument[i].name[0], sizeof(fm_instrument)-6, 1, fp);
	}
	fclose(fp);

	return 1;
}

static int egt_loadInstrumentFromMemory_impl(egtsynth* egt, char *data, unsigned slot)
{
	if (slot >= egt->instrumentCount)
	{
		egt_resizeInstrumentList(egt, slot + 1);
	}

	readFromMemory(egt, (char*)&egt->instrument[slot].magic, 4, data);
	readFromMemory(egt, (char*)&egt->instrument[slot].dummy, 1, data);
	readFromMemory(egt, (char*)&egt->instrument[slot].version, 1, data);

	if (egt->instrument[slot].version != EDGETRACKER_VERSION)
	{
		return EGT_ERR_FILEVERSION;
	}

	readFromMemory(egt, (char*)&egt->instrument[slot].name[0], sizeof(fm_instrument)-6, data);

	return 0;
}

int egt_loadInstrumentFromMemory(egtsynth* egt, char *data, unsigned len, unsigned slot)
{
	egt->readSeek = 0;
	egt->totalFileSize = len;

	return egt_loadInstrumentFromMemory_impl(egt, data, slot);
}


int egt_loadInstrument(egtsynth* egt, const char* filename, unsigned slot)
{

	char *data = egt_fileToMemory(egt, filename);

	if (!data)
		return EGT_ERR_FILEIO;

	int result = egt_loadInstrumentFromMemory(egt, data, egt->totalFileSize, slot);
	free(data);

	return result;
}

int egt_loadInstrumentBankFromMemory(egtsynth* egt, char *data, unsigned len)
{
	egt->totalFileSize = len;

	egt->readSeek = 0;
	char magic_check[4] = {0, 0, 0, 0};
	uint8_t version = 0;
	uint8_t instruments = 0;
	uint8_t slot = 0;

	// check bank magic
	readFromMemory(egt, magic_check, 4, data);
	if (memcmp(magic_check, "EGTB", 4) != 0 && memcmp(magic_check, "MDTB", 4) != 0)
	{
		return EGT_ERR_FILECORRUPTED;
	}
	readFromMemory(egt, (char *)&version, 1, data); //padding
	readFromMemory(egt, (char *)&version, 1, data); //version
	if (version != EDGETRACKER_VERSION)
	{
		return EGT_ERR_FILEVERSION;
	}
	readFromMemory(egt, (char *)&instruments, 1, data); //padding
	readFromMemory(egt, (char *)&instruments, 1, data); //instrument count
	if (8 /* header */ + (instruments * (5 /* 'SLOT' + # */ + sizeof(fm_instrument))) > egt->totalFileSize)
	{
		return EGT_ERR_FILECORRUPTED;
	}

	egt_resizeInstrumentList(egt, 0);

	for (int i = 0; i < instruments; ++i)
	{
		readFromMemory(egt, magic_check, 4, data);
		if (memcmp(magic_check, "SLOT", 4) != 0)
		{
			return EGT_ERR_FILECORRUPTED;
		}
		readFromMemory(egt, (char *)&slot, 1, data);
		if (slot >= instruments)
		{
			return EGT_ERR_FILECORRUPTED;
		}
		if (egt_loadInstrumentFromMemory_impl(egt, data, slot) != 0)
		{
			return EGT_ERR_FILECORRUPTED;
		}
	}

	return 0;

}

int egt_loadInstrumentBank(egtsynth* egt, const char* filename)
{
	char *data = egt_fileToMemory(egt, filename);

	if (!data)
		return EGT_ERR_FILEIO;

	int result = egt_loadInstrumentBankFromMemory(egt, data, egt->totalFileSize);
	free(data);

	return result;
}

void egt_removeInstrument(egtsynth* egt, unsigned slot, int removeOccurences)
{
	if (slot >= egt->instrumentCount)
		return;

	if (removeOccurences)
	{
		for (unsigned i = 0; i < egt->patternCount; i++)
		{
			for (unsigned j = 0; j < egt->patternSize[i]; j++)
			{
				for (unsigned ch = 0; ch < FM_ch; ch++)
				{
					if (egt->pattern[i][j][ch].instr == slot)
					{
						egt->pattern[i][j][ch].instr = egt->pattern[i][j][ch].vol = egt->pattern[i][j][ch].note = egt->pattern[i][j][ch].fx = egt->pattern[i][j][ch].fxdata = 255;
					}
					else if (egt->pattern[i][j][ch].instr < 255 && egt->pattern[i][j][ch].instr > slot)
					{
						egt->pattern[i][j][ch].instr--;
					}
				}
			}
		}
	}

	if (egt->instrumentCount == 1)
		return;

	for (unsigned i = slot; i < egt->instrumentCount - 1; i++)
	{
		egt->instrument[i] = egt->instrument[i + 1];
	}
	egt_resizeInstrumentList(egt, egt->instrumentCount - 1);
}

void egt_setVolume(egtsynth *egt, int volume)
{
	volume = clamp(volume, 0, 99);
	egt->_globalVolume = volume;
	egt->globalVolume = expVol[volume] * 4096 / LUTsize;
}


void egt_setPlaybackVolume(egtsynth *egt, int volume)
{
	volume = clamp(volume, 0, 99);
	egt->playbackVolume = expVol[volume] * 4096 / LUTsize;
}

void egt_getPosition(egtsynth* egt, int *order, int *row)
{
	*order = egt->order;
	*row = egt->row;
}

void egt_setTime(egtsynth* egt, int time, int cutNotes)
{
	for (int i = 0; i < egt->patternCount; i++)
	{
		for (int j = 0; j < egt->patternSize[i]; j++)
		{
			if (egt->channelStates[i][j].time >= time)
			{
				egt_setPosition(egt, i, j, cutNotes);
				return;
			}
		}
	}
	egt_setPosition(egt, egt->patternCount - 1, egt->patternSize[egt->patternCount - 1] - 1, cutNotes);
}

void egt_movePattern(egtsynth* egt, int from, int to)
{
	if (from < 0 || from >= egt->patternCount || to<0 || to >= egt->patternCount)
		return;

	/* Move patterns by pairs until the elements are in the right position */
	for (int j = from; j >to; j--)
	{

		fm_cell(*ptr)[FM_ch] = egt->pattern[j];
		egt->pattern[j] = egt->pattern[j - 1];
		egt->pattern[j - 1] = ptr;

		fm_channel_state* ptr2 = egt->channelStates[j];
		egt->channelStates[j] = egt->channelStates[j - 1];
		egt->channelStates[j - 1] = ptr2;

		unsigned int size = egt->patternSize[j];
		egt->patternSize[j] = egt->patternSize[j - 1];
		egt->patternSize[j - 1] = size;
	}

	for (int j = from; j < to; j++)
	{

		fm_cell(*ptr)[FM_ch] = egt->pattern[j];
		egt->pattern[j] = egt->pattern[j + 1];
		egt->pattern[j + 1] = ptr;

		fm_channel_state* ptr2 = egt->channelStates[j];
		egt->channelStates[j] = egt->channelStates[j + 1];
		egt->channelStates[j + 1] = ptr2;

		unsigned int size = egt->patternSize[j];
		egt->patternSize[j] = egt->patternSize[j + 1];
		egt->patternSize[j + 1] = size;
	}
	egt->channelStatesDone = 0;
}

void egt_moveChannels(egtsynth* egt, int from, int to)
{
	if (from < 0 || from >= FM_ch || to < 0 || to >= FM_ch)
		return;

	/* Move pattern contents */
	for (int i = 0; i < egt->patternCount; i++)
	{
		for (int j = 0; j < egt->patternSize[i]; j++)
		{

			for (int ch = from; ch >to; ch--)
			{

				fm_cell ptr = egt->pattern[i][j][ch];
				egt->pattern[i][j][ch] = egt->pattern[i][j][ch - 1];
				egt->pattern[i][j][ch - 1] = ptr;

				unsigned char ptr2 = egt->channelStates[i][j].pan[ch];
				egt->channelStates[i][j].pan[ch] = egt->channelStates[i][j].pan[ch - 1];
				egt->channelStates[i][j].pan[ch - 1] = ptr2;

				unsigned char ptr3 = egt->channelStates[i][j].vol[ch];
				egt->channelStates[i][j].vol[ch] = egt->channelStates[i][j].vol[ch - 1];
				egt->channelStates[i][j].vol[ch - 1] = ptr3;
			}

			for (int ch = from; ch < to; ch++)
			{

				fm_cell ptr = egt->pattern[i][j][ch];
				egt->pattern[i][j][ch] = egt->pattern[i][j][ch + 1];
				egt->pattern[i][j][ch + 1] = ptr;

				unsigned char ptr2 = egt->channelStates[i][j].pan[ch];
				egt->channelStates[i][j].pan[ch] = egt->channelStates[i][j].pan[ch + 1];
				egt->channelStates[i][j].pan[ch + 1] = ptr2;

				unsigned char ptr3 = egt->channelStates[i][j].vol[ch];
				egt->channelStates[i][j].vol[ch] = egt->channelStates[i][j].vol[ch + 1];
				egt->channelStates[i][j].vol[ch + 1] = ptr3;

			}

		}
	}

	egt_stopSound(egt);

	/* Move channels */

	for (int ch = from; ch > to; ch--)
	{

		fm_channel channel = egt->ch[ch];
		egt->ch[ch] = egt->ch[ch - 1];
		egt->ch[ch - 1] = channel;
	}

	for (int ch = from; ch < to; ch++)
	{

		fm_channel channel = egt->ch[ch];
		egt->ch[ch] = egt->ch[ch + 1];
		egt->ch[ch + 1] = channel;
	}


	egt->channelStatesDone = 0;
}

void egt_setChannelVolume(egtsynth *egt, int channel, int volume)
{
	volume = clamp(volume, 0, 99);
	egt->ch[channel].initial_vol = volume;
	egt->ch[channel].vol = expVol[volume];
	egt->channelStatesDone = 0;
}
void egt_setChannelPanning(egtsynth *egt, int channel, int panning)
{
	panning = clamp(panning, 0, 255);
	egt->ch[channel].initial_pan = panning;
	egt->ch[channel].destPan = panning;
	egt->channelStatesDone = 0;
}

void egt_setChannelReverb(egtsynth *egt, int channel, int reverb)
{
	reverb = clamp(reverb, 0, 99);
	egt->ch[channel].initial_reverb = reverb;
	egt->ch[channel].reverbSend = expVol[reverb];
}

void egt_setTempo(egtsynth* egt, int tempo)
{
	tempo = clamp(tempo, 1, 255);
	egt->tempo = egt->initial_tempo = tempo;
	egt->channelStatesDone = 0;
}

float egt_getSongLength(egtsynth* egt)
{
	if (egt->patternCount == 0)
		return 0;

	if (!egt->channelStatesDone)
		egt_buildStateTable(egt, 0, egt->patternCount, 0, FM_ch);

	return egt->channelStates[egt->patternCount - 1][egt->patternSize[egt->patternCount - 1] - 1].time + 1.0 / egt->channelStates[egt->patternCount - 1][egt->patternSize[egt->patternCount - 1] - 1].tempo*(60.0 / egt->diviseur);
}

float egt_volumeToExp(int volume)
{
	return expVol[volume];
}

int egt_write(egtsynth *egt, unsigned pattern, unsigned row, unsigned channel, fm_cell data)
{
	if (pattern >= egt->patternCount || row >= egt->patternSize[pattern] || channel >= FM_ch)
		return 0;

	struct fm_cell *current = &egt->pattern[pattern][row][channel];

	if (data.note != 255)
		current->note = data.note;

	if (data.instr != 255)
		current->instr = data.instr;

	if (data.vol != 255)
		current->vol = data.vol;

	if (data.fx != 255)
	{
		current->fx = data.fx;
		egt->channelStatesDone = 0;
	}

	if (data.fxdata != 255)
	{
		current->fxdata = data.fxdata;
		egt->channelStatesDone = 0;
	}

	return 1;
}

int egt_getPatternSize(egtsynth *egt, int pattern)
{
	if (pattern >= egt->patternCount)
		return 0;
	return egt->patternSize[pattern];
}

int egt_insertRows(egtsynth *egt, unsigned pattern, unsigned row, unsigned count)
{
	if (pattern >= egt->patternCount || row >= egt->patternSize[pattern] || egt->patternSize[pattern] + count > 256)
		return 0;

	if (!egt_resizePattern(egt, pattern, egt->patternSize[pattern] + count, 0))
		return 0;

	egt->channelStatesDone = 0;

	for (int i = egt->patternSize[pattern] - 1; i >= row; i--)
	{
		for (unsigned ch = 0; ch < FM_ch; ch++)
		{
			egt->pattern[egt->order][i][ch] = egt->pattern[egt->order][i - count][ch];
		}
	}

	if (!egt_clearPattern(egt, pattern, row, count))
		return 0;
	return 1;
}

int egt_removeRows(egtsynth *egt, unsigned pattern, unsigned row, unsigned count)
{
	if (pattern >= egt->patternCount || row + count > egt->patternSize[pattern])
		return 0;

	for (int i = row; i < egt->patternSize[pattern] - count; i++)
	{
		for (unsigned ch = 0; ch < FM_ch; ch++)
		{
			egt->pattern[pattern][i][ch] = egt->pattern[pattern][i + count][ch];
		}
	}

	egt->channelStatesDone = 0;

	if (!egt_resizePattern(egt, pattern, egt->patternSize[pattern] - count, 0))
		return 0;

	return 1;
}

int egt_isInstrumentUsed(egtsynth *egt, unsigned id)
{
	unsigned instrCount=0;

	for (int j = 0; j < egt->patternCount; j++)
	{
		for (int k = 0; k < egt->patternSize[j]; k++)
		{
			for (int l = 0; l < FM_ch; l++)
			{
				if (egt->pattern[j][k][l].instr == id)
					instrCount++;
			}
		}
	}
	return instrCount >0;
}

/* =====================================================================
   IMF import
   ===================================================================== */

/* OPL2 operator slot offset (0-21) → channel (0-8) and role (0=mod, 1=car).
   Offsets 6, 7, 14, 15 are unused in OPL2 hardware. */
static const signed char imf_slot_ch[22] = {
	0, 1, 2, 0, 1, 2,-1,-1,
	3, 4, 5, 3, 4, 5,-1,-1,
	6, 7, 8, 6, 7, 8
};
static const signed char imf_slot_type[22] = { /* 0=modulator, 1=carrier */
	0, 0, 0, 1, 1, 1,-1,-1,
	0, 0, 0, 1, 1, 1,-1,-1,
	0, 0, 0, 1, 1, 1
};

/* OPL2 frequency multiplier register value → egtengine mult.
   OPL2 value 0 means ×0.5; represented as mult=0 + finetune=12 (see imf_build_instr). */
static const unsigned char imf_mult_map[16] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15
};

/* OPL2 TL (0-63, 0.75 dB/step) → egtengine vol (0-99).
   Derived by matching expVolOp[vol] to 10^(-tl*0.75/20). */
static const unsigned char imf_tl_map[64] = {
	 99,  96,  92,  88,  85,  81,  78,  75,
	 72,  69,  66,  64,  61,  59,  56,  54,
	 52,  50,  48,  46,  44,  43,  41,  39,
	 38,  36,  35,  33,  32,  31,  29,  28,
	 27,  26,  25,  24,  23,  22,  21,  20,
	 20,  19,  18,  17,  17,  16,  15,  15,
	 14,  13,  13,  12,  12,  11,  11,  10,
	 10,  10,   9,   9,   8,   8,   8,   7
};

/* OPL2 waveform index → egtengine waveform index.
   0=sine→mt0, 1=half-sine→mt6, 2=abs-sine→mt8, 3=quarter-sine→mt9 */
static const unsigned char imf_wf_map[4] = { 0, 6, 8, 9 };

/* OPL2 SL (0-15, 3 dB/step) → egtengine s (0-99).
   Derived by matching expVol[s] to 10^(-sl*3/20). */
static const unsigned char imf_sl_map[16] = {
	99, 73, 54, 40, 29, 21, 16, 11, 8, 6, 4, 3, 2, 1, 1, 0
};

static int opl_rate_map(int rate)
{
	int index = 10 + rate * 15 / 2;
	return clamp(index, 0, 99);
}

typedef struct {
	unsigned char reg20[2]; /* [0]=mod [1]=car: AM/VIB/EG/KSR/MULT */
	unsigned char reg40[2]; /* KSL/TL */
	unsigned char reg60[2]; /* AR/DR */
	unsigned char reg80[2]; /* SL/RR */
	unsigned char regE0[2]; /* WS */
	unsigned char regA0;    /* frequency number low byte */
	unsigned char regB0;    /* key-on / block / freq-number high bits */
	unsigned char regC0;    /* feedback / connection algorithm */
	int instrIdx;           /* assigned instrument slot, -1 = unassigned */
	int dirty;              /* patch registers changed since last key-on */
	int keyOn;              /* 1 = note currently sounding */
} imf_ch_state;


static void imf_apply_slot(imf_ch_state *cs, int base, unsigned char reg, unsigned char val)
{
	int off = (int)(unsigned char)reg - base;
	if (off < 0 || off > 21) return;
	int ch  = imf_slot_ch[off];
	int opi = imf_slot_type[off];
	if (ch < 0 || opi < 0) return;
	switch (base) {
		case 0x20: cs[ch].reg20[opi] = val; break;
		case 0x40: cs[ch].reg40[opi] = val; break;
		case 0x60: cs[ch].reg60[opi] = val; break;
		case 0x80: cs[ch].reg80[opi] = val; break;
		case 0xE0: cs[ch].regE0[opi] = val; break;
	}
	cs[ch].dirty = 1;
}

static void imf_fill_op(const imf_ch_state *cs, int opi, fm_instrument_operator *op)
{
	static const char ksl_vol[4]  = { 0, -7, -6, -8 };
	static const char ksl_note[4] = { 0,  0,  0,  0 };
	static const char ksr_scale[2] = { 1, 3 };
	int mult = cs->reg20[opi] & 0xF;
	int egt  = (cs->reg20[opi] >> 5) & 1;
	int ksl  = (cs->reg40[opi] >> 6) & 0x3;
	int tl   = cs->reg40[opi] & 0x3F;
	int ar   = (cs->reg60[opi] >> 4) & 0xF;
	int dr   =  cs->reg60[opi]       & 0xF;
	int sl   = (cs->reg80[opi] >> 4) & 0xF;
	int rr   =  cs->reg80[opi]       & 0xF;
	int ws   =  cs->regE0[opi]       & 0x3;
	int kr   = (cs->reg20[opi] >> 4) & 1;
	op->mult          = imf_mult_map[mult];
	if (mult == 0) op->finetune = 12;
	op->vol           = (char)imf_tl_map[tl];
	op->kbdVolScaling = ksl_vol[ksl];
	op->kbdCenterNote = ksl_note[ksl];
	op->kbdAScaling   = ksr_scale[kr];
	op->kbdDScaling   = ksr_scale[kr];
	if (kr) op->envLoop |= FM_OP_KSR_RELEASE_BIT;
	op->a             = (char)opl_rate_map(ar);
	op->r             = (char)opl_rate_map(rr);
	if (egt) {
		op->d = (char)opl_rate_map(dr);
		op->s = (char)imf_sl_map[sl];
	} else {
		float sl_frac = imf_sl_map[sl] / 99.0f;
		float d_units = (float)opl_rate_map(dr);
		op->d = (char)(sl_frac * op->r + (1.0f - sl_frac) * d_units);
		op->s = 0;
	}
	op->waveform      = (char)imf_wf_map[ws];
}

static void imf_build_drum_instr(const imf_ch_state *cs, int opi, fm_instrument *instr)
{
	int op;
	memset(instr, 0, sizeof(fm_instrument));
	strncpy(instr->magic, "EGTI", 4);
	instr->version = EDGETRACKER_VERSION;
	instr->volume  = 99;
	instr->toMix[0] = instr->toMix[1] = instr->toMix[2] = instr->toMix[3] = -1;
	instr->op[0].connectOut = 1;
	instr->op[0].connect    = -1;
	instr->op[0].connect2   = -1;
	for (op = 1; op < FM_op; op++) {
		instr->op[op].connectOut = (char)op;
		instr->op[op].connect    = -1;
		instr->op[op].connect2   = -1;
		instr->op[op].muted      = 1;
	}
	imf_fill_op(cs, opi, &instr->op[0]);
}

static void imf_build_instr(imf_ch_state *cs, fm_instrument *instr)
{
	int alg, opi;
	memset(instr, 0, sizeof(fm_instrument));
	strncpy(instr->magic, "EGTI", 4);
	instr->version = EDGETRACKER_VERSION;
	instr->volume  = 99;
	instr->toMix[0] = instr->toMix[1] = instr->toMix[2] = instr->toMix[3] = -1;

	alg = cs->regC0 & 1;
	{
		static const char imf_fb_map[8] = {0, 0, 1, 2, 3, 4, 6, 9};
		int fb = (cs->regC0 >> 1) & 7;
		instr->feedback = imf_fb_map[fb];
	}
	instr->feedbackSource = 0;

	if (alg == 0) {
		instr->op[0].connectOut = -1;
		instr->op[0].connect    = -1;
		instr->op[0].connect2   = -1;
		instr->op[1].connectOut =  1;
		instr->op[1].connect    =  0;
		instr->op[1].connect2   = -1;
	} else {
		instr->op[0].connectOut =  0;
		instr->op[0].connect    = -1;
		instr->op[0].connect2   = -1;
		instr->op[1].connectOut =  1;
		instr->op[1].connect    = -1;
		instr->op[1].connect2   = -1;
	}

	for (int op = 2; op < FM_op; op++) {
		instr->op[op].connectOut = (char)op;
		instr->op[op].connect    = -1;
		instr->op[op].connect2   = -1;
		instr->op[op].muted      =  1;
	}

	for (opi = 0; opi < 2; opi++)
		imf_fill_op(cs, opi, &instr->op[opi]);
}

static int egt_loadIMFFromMemory(egtsynth *egt, char *data, unsigned len, int rate)
{
	unsigned filesize    = len;
	unsigned data_offset = 0;
	unsigned data_len    = filesize;

	/* Detect type 1 (2-byte length prefix) vs type 0 (raw stream).
	   Type 1: first two bytes are a little-endian data length that must be
	   a non-zero multiple of 4 and fit within the file. */
	{
		unsigned short claimed = (unsigned char)data[0]
		                       | ((unsigned char)data[1] << 8);
		if (claimed > 0 && claimed % 4 == 0
		    && (unsigned)(claimed + 2) <= filesize)
		{
			data_offset = 2;
			data_len    = claimed;
		}
	}

	if (data_len == 0) {
		return EGT_ERR_FILECORRUPTED;
	}
	if (data_len % 4 == 2) data_len += 2;
	if (data_len % 4 != 0) {
		return EGT_ERR_FILECORRUPTED;
	}

	egt_clearSong(egt);
	egt_resizeInstrumentList(egt, 0);

	{
		int psize = 64;
		egt_insertPattern(egt, psize, 0);
		egt->diviseur      = 16;
		egt->initial_tempo = 120;

		double tpr  = (double)rate * 60.0 / (120.0 * 16.0); /* ticks per row */
		double tacc = 0.0;
		int cur_order = 0;
		int cur_row   = 0;

		imf_ch_state cs[9];
		memset(cs, 0, sizeof(cs));
		for (int i = 0; i < 9; i++) {
			cs[i].instrIdx = -1;
			cs[i].dirty    = 1;
		}

		/* Rhythm mode state.
		   drum_ch: OPL2 channel (6-8) and operator index (0=mod,1=car) for each voice.
		   BD uses both ops of ch6 and is handled via imf_build_instr.
		   Tracker channels 9-13 are used for the 5 drum voices. */
		int rhythmMode = 0;
		unsigned char prevBD = 0;
		int drumInstrIdx[5];
		int drumDirty[5];
		int drumKeyOn[5];
		static const int drum_tch[5]  = { 9, 10, 11, 12, 13 };
		static const int drum_opl_ch[5] = { 6,  7,  7,  8,  8 };
		static const int drum_opi[5]    = { 0,  1,  0,  1,  0 };
		for (int d = 0; d < 5; d++) {
			drumInstrIdx[d] = -1;
			drumDirty[d]    =  1;
			drumKeyOn[d]    =  0;
		}

		unsigned end = data_offset + data_len;
		unsigned pos = data_offset;

		while (pos + 3 < end) {
			unsigned char  reg = (unsigned char)data[pos];
			unsigned char  val = (unsigned char)data[pos + 1];
			unsigned short dly = (unsigned char)data[pos + 2]
			                   | ((unsigned char)data[pos + 3] << 8);
			pos += 4;

			/* Dispatch register write to OPL2 state */
			if      (reg >= 0x20 && reg <= 0x35) imf_apply_slot(cs, 0x20, reg, val);
			else if (reg >= 0x40 && reg <= 0x55) imf_apply_slot(cs, 0x40, reg, val);
			else if (reg >= 0x60 && reg <= 0x75) imf_apply_slot(cs, 0x60, reg, val);
			else if (reg >= 0x80 && reg <= 0x95) imf_apply_slot(cs, 0x80, reg, val);
			else if (reg >= 0xA0 && reg <= 0xA8) cs[reg - 0xA0].regA0 = val;
			else if (reg >= 0xC0 && reg <= 0xC8) { cs[reg - 0xC0].regC0 = val; cs[reg - 0xC0].dirty = 1; }
			else if (reg >= 0xE0 && reg <= 0xF5) imf_apply_slot(cs, 0xE0, reg, val);
			else if (reg == 0xBD) {
				/* Rhythm mode enable/disable and drum key-on bits */
				int newRhythm = (val >> 5) & 1;
				if (newRhythm && !rhythmMode)
					for (int d = 0; d < 5; d++) drumDirty[d] = 1;
				rhythmMode = newRhythm;

				/* BD=bit4  SD=bit3  TT=bit2  CY=bit1  HH=bit0 */
				static const int drum_bits[5] = { 4, 3, 2, 1, 0 };
				for (int d = 0; d < 5; d++) {
					int new_on = (val >> drum_bits[d]) & 1;
					int old_on = drumKeyOn[d];

					if (new_on && !old_on) {
						if (cur_order >= (int)egt->patternCount)
							egt_insertPattern(egt, psize, cur_order);

						int slot = -1;
						if (drumDirty[d] || drumInstrIdx[d] < 0) {
							fm_instrument cand;
							if (d == 0)
								imf_build_instr(&cs[drum_opl_ch[d]], &cand);
							else
								imf_build_drum_instr(&cs[drum_opl_ch[d]], drum_opi[d], &cand);

							for (unsigned i = 0; i < egt->instrumentCount; i++) {
								fm_instrument tmp = egt->instrument[i];
								memset(tmp.name, 0, sizeof(tmp.name));
								if (memcmp(&tmp, &cand, sizeof(fm_instrument)) == 0) {
									slot = (int)i;
									break;
								}
							}
							if (slot < 0 && egt->instrumentCount < 255) {
								egt_resizeInstrumentList(egt, egt->instrumentCount + 1);
								slot = egt->instrumentCount - 1;
								egt->instrument[slot] = cand;
								static const char *drum_names[5] = {
									"BD", "SD", "TT", "CY", "HH"
								};
								snprintf(egt->instrument[slot].name,
								         sizeof(egt->instrument[slot].name),
								         "OPL2 %s", drum_names[d]);
							}
							drumInstrIdx[d] = slot;
							drumDirty[d]    = 0;
						} else {
							slot = drumInstrIdx[d];
						}

						if (slot >= 0) {
							/* BD uses frequency from B6/A6; others use a fixed note */
							int note = 60;
							if (d == 0) {
								int block   = (cs[6].regB0 >> 2) & 7;
								int fnumber = ((cs[6].regB0 & 3) << 8) | cs[6].regA0;
								if (fnumber > 0) {
									double freq = fnumber * 49716.0
									            / (double)(1u << (20 - block));
									note = (int)(69.0 + 12.0 * log2(freq / 440.0) + 0.5);
									if (note < 0)   note = 0;
									if (note > 127) note = 127;
								}
							}
							fm_cell cell;
							memset(&cell, 0xFF, sizeof(fm_cell));
							cell.note  = (unsigned char)note;
							cell.instr = (unsigned char)slot;
							egt_write(egt, cur_order, cur_row, drum_tch[d], cell);
						}
					} else if (!new_on && old_on) {
						if (cur_order < (int)egt->patternCount) {
							fm_cell cell;
							memset(&cell, 0xFF, sizeof(fm_cell));
							cell.note = 128;
							egt_write(egt, cur_order, cur_row, drum_tch[d], cell);
						}
					}
					drumKeyOn[d] = new_on;
				}
				prevBD = val;
			}
			else if (reg >= 0xB0 && reg <= 0xB8) {
				int ch      = reg - 0xB0;
				int new_on  = (val >> 5) & 1;
				int old_on  = cs[ch].keyOn;
				cs[ch].regB0 = val;
				/* In rhythm mode channels 6-8 are drum voices; suppress melodic key-on */
				if (rhythmMode && ch >= 6) {
					cs[ch].keyOn = new_on;
					goto imf_advance_time;
				}

				if (new_on && !old_on) {
					/* Key-on: ensure current pattern exists */
					if (cur_order >= (int)egt->patternCount)
						egt_insertPattern(egt, psize, cur_order);

					/* Convert OPL2 block/fnumber to MIDI note */
					int block   = (val >> 2) & 7;
					int fnumber = ((val & 3) << 8) | cs[ch].regA0;
					int note    = 0;
					if (fnumber > 0) {
						double freq = fnumber * 49716.0
						            / (double)(1u << (20 - block));
						note = (int)(69.0 + 12.0 * log2(freq / 440.0) + 0.5);
						if (note < 0)   note = 0;
						if (note > 127) note = 127;
					}

					/* Find or create matching instrument */
					int slot = -1;
					if (cs[ch].dirty || cs[ch].instrIdx < 0) {
						fm_instrument cand;
						imf_build_instr(&cs[ch], &cand);

						for (unsigned i = 0; i < egt->instrumentCount; i++) {
							fm_instrument tmp = egt->instrument[i];
							memset(tmp.name, 0, sizeof(tmp.name));
							if (memcmp(&tmp, &cand, sizeof(fm_instrument)) == 0) {
								slot = (int)i;
								break;
							}
						}
						if (slot < 0 && egt->instrumentCount < 255) {
							egt_resizeInstrumentList(egt, egt->instrumentCount + 1);
							slot = egt->instrumentCount - 1;
							egt->instrument[slot] = cand;
							snprintf(egt->instrument[slot].name,
							         sizeof(egt->instrument[slot].name),
							         "OPL2 ch%d", ch + 1);
						}
						cs[ch].instrIdx = slot;
						cs[ch].dirty    = 0;
					} else {
						slot = cs[ch].instrIdx;
					}

					if (slot >= 0) {
						fm_cell cell;
						memset(&cell, 0xFF, sizeof(fm_cell));
						cell.note  = (unsigned char)note;
						cell.instr = (unsigned char)slot;
						egt_write(egt, cur_order, cur_row, ch, cell);
					}
					cs[ch].keyOn = 1;

				} else if (!new_on && old_on) {
					/* Key-off */
					if (cur_order < (int)egt->patternCount) {
						fm_cell cell;
						memset(&cell, 0xFF, sizeof(fm_cell));
						cell.note = 128;
						egt_write(egt, cur_order, cur_row, ch, cell);
					}
					cs[ch].keyOn = 0;
				}
			}

			imf_advance_time:
			/* Advance time by delay ticks */
			if (dly > 0) {
				tacc += dly;
				while (tacc >= tpr) {
					tacc -= tpr;
					cur_row++;
					if (cur_row >= psize) {
						cur_row = 0;
						cur_order++;
						if (cur_order >= (int)egt->patternCount)
							egt_insertPattern(egt, psize, cur_order);
					}
				}
			}
		}
	}

	egt_buildStateTable(egt, 0, egt->patternCount, 0, FM_ch);
	return 0;
}
