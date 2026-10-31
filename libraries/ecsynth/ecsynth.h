#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum ECSynthSongFormat
    {
        kECSynthFormatAuto,
        kECSynthFormatEcs,
        kECSynthFormatMidi,
        kECSynthFormatMus,
        kECSynthFormatImf280,
        kECSynthFormatImf560,
        kECSynthFormatImf700,
        kECSynthFormatOrganya
    };

    enum ECSynthBuiltinBank
    {
        kECSynthBankDefault,
        kECSynthBankOrganya
    };

    enum ECSynthRenderType
    {
        kECSynthRender16,
        kECSynthRenderFloat
    };

    typedef struct ECSynth ECSynth;

    ECSynth *ECSynthCreate(int32_t samplerate);
    void     ECSynthDestroy(ECSynth *ec);
    int32_t  ECSynthSetSampleRate(ECSynth *ec, int32_t samplerate);
    uint32_t ECSynthGetSampleRate(const ECSynth *ec);

    int32_t ECSynthLoadBuiltinBank(ECSynth *ec, int32_t bank);
    int32_t ECSynthLoadGeneralMidiBank(ECSynth *ec, const char *filename);
    int32_t ECSynthLoadGeneralMidiBankFromMemory(ECSynth *ec, const uint8_t *data, size_t size);
    int32_t ECSynthLoadSong(ECSynth *ec, const char *filename);
    int32_t ECSynthLoadSongFromMemory(ECSynth *ec, const uint8_t *data, size_t size, int32_t format);

    void    ECSynthPlay(ECSynth *ec, uint32_t tick);
    void    ECSynthStop(ECSynth *ec);
    void    ECSynthSetLoopCount(ECSynth *ec, int32_t count);
    void    ECSynthSetPlaybackVolume(ECSynth *ec, int32_t volume);
    int32_t ECSynthIsPlaying(const ECSynth *ec);
    double  ECSynthGetTime(const ECSynth *ec);
    double  ECSynthGetSongLength(const ECSynth *ec);

    void ECSynthRender(ECSynth *ec, void *buffer, uint32_t length, uint32_t type);

#ifdef ECSYNTH_EDITOR_API

    enum
    {
        kECSynthChannelCount  = 24,
        kECSynthOperatorCount = 6
    };

    enum ECSynthInstrumentFlags
    {
        kECSynthInstrumentLfoReset     = 1,
        kECSynthInstrumentSmooth       = 2,
        kECSynthInstrumentTransposable = 4,
        kECSynthInstrumentPizzicato    = 8,
        kECSynthInstrumentOneShot      = 16
    };

    enum
    {
        kECSynthOrganyaWaveFirst = 14,
        kECSynthOrganyaWaveCount = 100,
        kECSynthOrganyaDrumFirst = 114,
        kECSynthOrganyaDrumCount = 42,
        kECSynthWaveformCount    = 156,
        kECSynthLfoWaveformCount = 20
    };

    enum
    {
        kECSynthOperatorEnvelopeLoop = 1
    };

    enum
    {
        kECSynthReverbLengthMax     = 40,
        kECSynthReverbRoomMin       = 1,
        kECSynthReverbRoomMax       = 40,
        kECSynthReverbLengthDefault = 30,
        kECSynthReverbRoomDefault   = 22
    };

    enum
    {
        kECSynthBankInstrumentLimit     = 255,
        kECSynthGeneralMidiProgramCount = 128,
        kECSynthOplRegisterCount        = 11,
        kECSynthOplVoiceCount           = 14,
        kECSynthPreviewAll              = 0
    };

    enum ECSynthSourceFormat
    {
        kECSynthSourceNone,
        kECSynthSourceMidi,
        kECSynthSourceMus,
        kECSynthSourceImf,
        kECSynthSourceOrganya
    };

    enum ECSynthLaneKind
    {
        kECSynthLaneVolume,
        kECSynthLanePan,
        kECSynthLanePitchBend,
        kECSynthLaneController,
        kECSynthLaneChannelPressure,
        kECSynthLaneKeyPressure,
        kECSynthLaneProgram,
        kECSynthLaneInstrument,
        kECSynthLaneOplRegister,
        kECSynthLaneTempo,
        kECSynthLaneTimeSignature,
        kECSynthLaneKeySignature,
        kECSynthLaneReverbSend
    };

    enum ECSynthInstrumentSource
    {
        kECSynthInstrumentSourceNone,
        kECSynthInstrumentSourceGeneralMidi,
        kECSynthInstrumentSourceOpl,
        kECSynthInstrumentSourceOrganya
    };

    enum ECSynthEventType
    {
        kECSynthEventStart,
        kECSynthEventRelease,
        kECSynthEventPitchBend,
        kECSynthEventLevel,
        kECSynthEventVolume,
        kECSynthEventPan,
        kECSynthEventReverbSend,
        kECSynthEventVibrato
    };

    typedef struct ECSynthInstrumentOperator
    {
        uint8_t mult;
        uint8_t finetune;
        int8_t  detune;
        int8_t  vol;
        int8_t  connect_out;
        int8_t  connect;
        int8_t  connect2;

        int8_t delay, i, a, h, d, s, r;

        int8_t kbd_vol_scaling;

        int8_t  kbd_a_scaling;
        int8_t  kbd_d_scaling;
        int8_t  vel_sensitivity;
        int8_t  lfo_fm;
        int8_t  lfo_am;
        uint8_t waveform;
        int8_t  fixed_freq;

        int8_t kbd_center_note;

        int8_t pitch_initial_ratio;
        int8_t pitch_decay;
        int8_t pitch_final_ratio;
        int8_t pitch_release;

        int8_t env_flags;
        int8_t muted;

        uint8_t offset;

    } ECSynthInstrumentOperator;

    // ECI file format
    typedef struct ECSynthInstrument
    {
        char                      magic[4]; // "ECIN" identifier
        uint8_t                   reserved;
        uint8_t                   version;
        char                      name[24];
        int8_t                    to_mix[4];
        int8_t                    feedback;
        int8_t                    lfo_delay;
        int8_t                    lfo_speed;
        int8_t                    lfo_a;
        int8_t                    lfo_waveform;
        int8_t                    lfo_offset;
        int8_t                    volume;
        int8_t                    feedback_source;
        int8_t                    env_reset;
        int8_t                    tuning;
        int8_t                    transpose;
        int8_t                    phase_reset;
        int8_t                    flags;
        uint8_t                   rate_offset[2];
        ECSynthInstrumentOperator op[kECSynthOperatorCount];
    } ECSynthInstrument;

    typedef struct ECSynthVoiceParams
    {
        float fine_pitch;
        float level;
        float pitch_bend;
        float volume;
        float pan;
        float reverb_send;
        float vibrato;
    } ECSynthVoiceParams;

    typedef struct ECSynthText
    {
        char    *data;
        uint32_t length;
    } ECSynthText;

    typedef struct ECSynthNote
    {
        uint32_t tick;
        uint32_t length;
        uint8_t  key;
        int16_t  fine_pitch;
        uint16_t velocity;
        uint16_t release_velocity;
    } ECSynthNote;

    typedef struct ECSynthLanePoint
    {
        uint32_t tick;
        int32_t  value;
    } ECSynthLanePoint;

    typedef struct ECSynthLane
    {
        uint8_t           kind;
        uint8_t           number;
        ECSynthLanePoint *points;
        uint32_t          point_count;
        uint32_t          point_capacity;
    } ECSynthLane;

    typedef struct ECSynthTrackSource
    {
        uint8_t  format;
        uint16_t file_track;
        uint8_t  channel;
    } ECSynthTrackSource;

    typedef struct ECSynthTrack
    {
        ECSynthText        name;
        uint8_t            drum;
        uint16_t           instrument;
        uint8_t            level_curve;
        uint8_t            muted;
        uint8_t            solo;
        uint8_t            monophonic;
        ECSynthTrackSource source;
        ECSynthNote       *notes;
        uint32_t           note_count;
        uint32_t           note_capacity;
        ECSynthLane       *lanes;
        uint32_t           lane_count;
        uint32_t           lane_capacity;
    } ECSynthTrack;

    typedef struct ECSynthSongInstrument
    {
        ECSynthInstrument synth;
        uint8_t           source_kind;
        uint8_t           program;
        uint8_t           bank_msb;
        uint8_t           bank_lsb;
        uint8_t           drum;
        uint8_t           opl_registers[kECSynthOplRegisterCount];
        uint8_t           org_wave;
        uint16_t          org_frequency;
        uint8_t           org_pizzicato;
    } ECSynthSongInstrument;

    typedef struct ECSynthRawEvent
    {
        uint32_t tick;
        uint16_t file_track;
        uint8_t *bytes;
        uint32_t size;
    } ECSynthRawEvent;

    typedef struct ECSynthDrumKitKey
    {
        uint8_t           key;
        int16_t           cents;
        ECSynthInstrument synth;
    } ECSynthDrumKitKey;

    typedef struct ECSynthSongSource
    {
        uint8_t   format;
        uint16_t  midi_type;
        uint16_t  midi_division;
        uint16_t  midi_track_count;
        uint16_t  mus_primary_channels;
        uint16_t  mus_secondary_channels;
        uint16_t *mus_instruments;
        uint32_t  mus_instrument_count;
        uint32_t  mus_instrument_capacity;
        uint8_t   org_version;
        uint16_t  imf_rate;
        uint8_t   imf_type;
        uint8_t  *imf_trailer;
        uint32_t  imf_trailer_size;
    } ECSynthSongSource;

    typedef struct ECSynthSong
    {
        ECSynthText            name;
        ECSynthText            author;
        ECSynthText            comments;
        uint16_t               ticks_per_beat;
        uint8_t                steps_per_beat;
        uint8_t                beats_per_bar;
        uint32_t               loop_start;
        uint32_t               loop_end;
        uint8_t                volume;
        uint8_t                reverb_length;
        uint8_t                reverb_room_size;
        ECSynthSongInstrument *instruments;
        uint32_t               instrument_count;
        uint32_t               instrument_capacity;
        ECSynthTrack          *tracks;
        uint32_t               track_count;
        uint32_t               track_capacity;
        ECSynthLane           *lanes;
        uint32_t               lane_count;
        uint32_t               lane_capacity;
        ECSynthRawEvent       *raw_events;
        uint32_t               raw_event_count;
        uint32_t               raw_event_capacity;
        ECSynthSongSource      source;
        ECSynthDrumKitKey     *drum_kit;
        uint32_t               drum_kit_count;
        uint32_t               drum_kit_capacity;
    } ECSynthSong;

    typedef struct ECSynthGeneralMidi
    {
        ECSynthInstrument melodic[kECSynthGeneralMidiProgramCount];
        uint8_t           has_melodic[kECSynthGeneralMidiProgramCount];
        ECSynthInstrument percussion[kECSynthGeneralMidiProgramCount];
        uint8_t           has_percussion[kECSynthGeneralMidiProgramCount];
        int16_t           percussion_cents[kECSynthGeneralMidiProgramCount];
    } ECSynthGeneralMidi;

    typedef struct ECSynthEvent
    {
        uint64_t           sample;
        uint32_t           tick;
        uint8_t            type;
        uint8_t            voice;
        uint8_t            key;
        uint32_t           instrument;
        uint32_t           track;
        uint32_t           note;
        float              value;
        uint64_t           end_sample;
        ECSynthVoiceParams params;
    } ECSynthEvent;

    typedef struct ECSynthTempoPoint
    {
        uint32_t tick;
        double   sample;
        double   samples_per_tick;
    } ECSynthTempoPoint;

    typedef struct ECSynthCompiledSong
    {
        uint32_t           sample_rate;
        int32_t            master_volume;
        int32_t            reverb_length;
        int32_t            reverb_room_size;
        uint8_t            looping;
        uint32_t           loop_start;
        uint32_t           loop_end;
        uint32_t           end_tick;
        uint64_t           longest_note;
        ECSynthInstrument *instruments;
        uint32_t           instrument_count;
        uint32_t           instrument_capacity;
        ECSynthEvent      *events;
        uint32_t           event_count;
        uint32_t           event_capacity;
        ECSynthTempoPoint *tempo;
        uint32_t           tempo_count;
        uint32_t           tempo_capacity;
    } ECSynthCompiledSong;

    typedef struct ECSynthOplWrite
    {
        uint32_t tick;
        uint8_t  reg;
        uint8_t  value;
    } ECSynthOplWrite;

    typedef struct ECSynthOplTimeline
    {
        ECSynthOplWrite *writes;
        uint32_t         write_count;
        uint32_t         write_capacity;
        uint32_t         next;
        uint8_t          reg[256];
    } ECSynthOplTimeline;

    void     ECSynthSetSong(ECSynth *ec, const ECSynthCompiledSong *song);
    void     ECSynthStopSound(ECSynth *ec);
    int32_t  ECSynthIsSilent(const ECSynth *ec);
    uint64_t ECSynthGetPosition(const ECSynth *ec);
    uint64_t ECSynthGetPlaybackLimit(const ECSynth *ec);
    void     ECSynthPreviewStart(ECSynth *ec, uint32_t id, const ECSynthInstrument *instrument, uint32_t key,
                                 const ECSynthVoiceParams *params, int64_t preferred_track);
    void     ECSynthPreviewStop(ECSynth *ec, uint32_t id);

    void                   ECSynthSongInit(ECSynthSong *song);
    void                   ECSynthSongFree(ECSynthSong *song);
    int32_t                ECSynthTextSet(ECSynthText *text, const char *data, uint32_t length);
    ECSynthTrack          *ECSynthSongAddTrack(ECSynthSong *song);
    ECSynthSongInstrument *ECSynthSongAddInstrument(ECSynthSong *song);
    ECSynthLane           *ECSynthSongAddLane(ECSynthSong *song, uint8_t kind, uint8_t number);
    ECSynthLane           *ECSynthTrackAddLane(ECSynthTrack *track, uint8_t kind, uint8_t number);
    ECSynthLanePoint      *ECSynthLaneAddPoint(ECSynthLane *lane, uint32_t tick, int32_t value);
    ECSynthNote           *ECSynthTrackAddNote(ECSynthTrack *track);
    ECSynthRawEvent *ECSynthSongAddRawEvent(ECSynthSong *song, uint32_t tick, uint16_t file_track, const uint8_t *bytes,
                                            uint32_t size);
    ECSynthDrumKitKey *ECSynthSongAddDrumKitKey(ECSynthSong *song);
    int32_t            ECSynthSongSetMusInstruments(ECSynthSong *song, const uint16_t *instruments, uint32_t count);
    int32_t            ECSynthSongSetImfTrailer(ECSynthSong *song, const uint8_t *bytes, uint32_t size);
    void               ECSynthFree(void *pointer);

    int32_t ECSynthReadEcs(const uint8_t *data, size_t size, ECSynthSong *song);
    int32_t ECSynthWriteEcs(const ECSynthSong *song, uint8_t **data, size_t *size);
    int32_t ECSynthReadMidi(const uint8_t *data, size_t size, ECSynthSong *song);
    int32_t ECSynthReadMus(const uint8_t *data, size_t size, ECSynthSong *song);
    int32_t ECSynthReadImf(const uint8_t *data, size_t size, uint16_t rate, ECSynthSong *song);
    int32_t ECSynthReadOrganya(const uint8_t *data, size_t size, ECSynthSong *song);
    void    ECSynthFillInstrument(ECSynthSongInstrument *instrument, const ECSynthGeneralMidi *general_midi);

    ECSynthCompiledSong *ECSynthCompileSong(const ECSynthSong *song, uint32_t sample_rate,
                                            const ECSynthGeneralMidi *general_midi);
    void                 ECSynthDestroyCompiledSong(ECSynthCompiledSong *song);
    uint32_t             ECSynthSampleToTick(const ECSynthCompiledSong *song, uint64_t sample);
    double               ECSynthSampleToTickExact(const ECSynthCompiledSong *song, uint64_t sample);
    double               ECSynthSamplesPerTickAt(const ECSynthCompiledSong *song, uint64_t sample);

    int32_t  ECSynthOplTimelineBuild(ECSynthOplTimeline *timeline, const ECSynthSong *song);
    void     ECSynthOplTimelineFree(ECSynthOplTimeline *timeline);
    void     ECSynthOplTimelineAdvance(ECSynthOplTimeline *timeline, uint32_t tick);
    void     ECSynthOplTimelineVoiceRegisters(const ECSynthOplTimeline *timeline, uint8_t voice, uint8_t *registers);
    uint8_t  ECSynthOplTimelineVoiceTotalLevel(const ECSynthOplTimeline *timeline, uint8_t voice);
    uint32_t ECSynthOplTimelineVoiceFrequency(const ECSynthOplTimeline *timeline, uint8_t voice);
    double   ECSynthOplTimelineVoicePitch(const ECSynthOplTimeline *timeline, uint8_t voice);
    int32_t  ECSynthOplVoiceDrumOperator(uint8_t voice);

    void    ECSynthCreateBlankInstrument(ECSynthInstrument *instr);
    int32_t ECSynthReadInstrument(const uint8_t *data, size_t size, ECSynthInstrument *instrument);
    void    ECSynthWriteInstrument(const ECSynthInstrument *instrument, uint8_t *data);
    int32_t ECSynthReadInstrumentBank(const uint8_t *data, size_t size, ECSynthInstrument *instruments,
                                      uint32_t *count);
    int32_t ECSynthWriteInstrumentBank(const ECSynthInstrument *instruments, uint32_t count, uint8_t **data,
                                       size_t *size);
    void    ECSynthBankGeneralMidiMap(uint32_t count, int32_t *melodic, int32_t *percussion);
    void    ECSynthDefaultGeneralMidi(ECSynthGeneralMidi *general_midi);
    void    ECSynthOrganyaGeneralMidi(ECSynthGeneralMidi *general_midi);

    float ECSynthWaveformSample(int32_t waveform, float position);
    float ECSynthLfoSample(int32_t lfo_waveform, float position);

    float ECSynthLevelGain(float level);
    float ECSynthLevelFromGain(float gain);
    float ECSynthOrganyaLevel(float volume);
    float ECSynthOrganyaPan(float pan);
    float ECSynthOrganyaDrumPitch(int32_t key);
    float ECSynthOplLevel(float total_level);

    void ECSynthInstrumentFromOrganya(ECSynthInstrument *instrument, int32_t wave, int32_t frequency,
                                      int32_t pizzicato);
    void ECSynthInstrumentFromOrganyaDrum(ECSynthInstrument *instrument, int32_t drum);

    typedef struct ECSynthOrganyaProgramMatch
    {
        uint8_t wave;
        uint8_t pizzicato;
    } ECSynthOrganyaProgramMatch;

    typedef struct ECSynthOrganyaDrumMatch
    {
        uint8_t drum;
        uint8_t key;
    } ECSynthOrganyaDrumMatch;

    const ECSynthOrganyaProgramMatch *ECSynthOrganyaMatchForProgram(uint32_t program);
    const ECSynthOrganyaDrumMatch    *ECSynthOrganyaMatchForDrumKey(uint32_t key);

#endif

#ifdef __cplusplus
}
#endif
