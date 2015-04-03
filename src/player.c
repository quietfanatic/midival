#include "midieval.h"

#define SAMPLE_RATE 48000

#include <stdio.h>
#include <stdlib.h>

#include "player_tables.c"

typedef struct Voice {
    uint8_t next;
    uint8_t note;
    uint8_t velocity;
    uint8_t sample_index;
    uint8_t backwards;
    uint8_t envelope_phase;
     // 15:15 (?) fixed point
    uint32_t envelope_value;
     // I don't even know
    int32_t tremolo_sweep_position;
    int32_t tremolo_phase;
     // 32:32 fixed point
    uint64_t sample_pos;
    MDV_Patch* patch;  // Giving up and inserting this
} Voice;

typedef struct Channel {
     // TODO: a lot more controllers
    uint8_t program;
    uint8_t volume;
    uint8_t expression;
    int8_t pan;
    int16_t pitch_bend;
    uint8_t voices;
     // Usually true for drum patches
    uint8_t no_envelope;
    uint8_t no_loop;
} Channel;

struct MDV_Player {
     // Specification
    uint32_t tick_length;
    MDV_Sequence* seq;
    MDV_Bank bank;
     // State
    MDV_Timed_Event* current;
    uint32_t samples_to_tick;
    uint32_t ticks_to_event;
    int done;
    Channel channels [16];
    uint8_t inactive;  // inactive voices
    Voice voices [255];
     // Debug
    uint64_t clip_count;
};

void mdv_reset_player (MDV_Player* p) {
    for (uint32_t i = 0; i < 16; i++) {
        p->channels[i].volume = 127;
        p->channels[i].expression = 127;
        p->channels[i].pitch_bend = 0;
        p->channels[i].pan = 0;
        p->channels[i].voices = 255;
    }
    p->inactive = 0;
    for (uint32_t i = 0; i < 255; i++) {
        p->voices[i].next = i + 1;
    }
    p->clip_count = 0;
}

MDV_Player* mdv_new_player () {
    init_tables();
    MDV_Player* player = (MDV_Player*)malloc(sizeof(MDV_Player));
    mdv_reset_player(player);
    mdv_bank_init(&player->bank);
    return player;
}
void mdv_free_player (MDV_Player* player) {
    mdv_bank_free_patches(&player->bank);
    fprintf(stderr, "Clip count: %llu\n", player->clip_count);
    free(player);
}

void mdv_play_sequence (MDV_Player* player, MDV_Sequence* seq) {
     // Default tempo is 120bpm
    player->tick_length = SAMPLE_RATE / seq->tpb / 2;
    player->seq = seq;
    player->current = seq->events;
    player->samples_to_tick = player->tick_length;
    player->ticks_to_event = seq->events[0].time;
    player->done = 0;
}

int mdv_currently_playing (MDV_Player* player) {
    return player->seq && !player->done;
}

void mdv_load_config (MDV_Player* player, const char* filename) {
    mdv_bank_load_config(&player->bank, filename);
}
void mdv_load_patch (MDV_Player* player, uint8_t index, const char* filename) {
    mdv_bank_load_patch(&player->bank, index, filename);
}
void mdv_load_drum (MDV_Player* player, uint8_t index, const char* filename) {
    mdv_bank_load_drum(&player->bank, index, filename);
}

static void do_event (MDV_Player* player, MDV_Event* event) {
    switch (event->type) {
        case MDV_NOTE_OFF: {
            do_note_off:
            if (event->channel != 9) {
                for (uint8_t i = player->channels[event->channel].voices; i != 255; i = player->voices[i].next) {
                    Voice* v = &player->voices[i];
                    if (v->note == event->param1) {
                        if (v->envelope_phase < 3) {
                            v->envelope_phase = 3;
                            break;
                        }
                    }
                }
            }
            break;
        }
        case MDV_NOTE_ON: {
            if (event->param2 == 0)
                goto do_note_off;
            if (player->inactive != 255) {
                Voice* v = &player->voices[player->inactive];
                Channel* ch = &player->channels[event->channel];
                player->inactive = v->next;
                v->next = ch->voices;
                ch->voices = v - player->voices;
                v->note = event->param1;
                v->velocity = event->param2;
                v->backwards = 0;
                v->sample_pos = 0;
                v->sample_index = 0;
                v->envelope_phase = 0;
                v->envelope_value = 0;
                v->tremolo_sweep_position = 0;
                v->tremolo_phase = 0;
                 // Decide which patch sample we're using
                v->patch = event->channel == 9
                    ? player->bank.drums[v->note]
                    : player->bank.patches[ch->program];
                if (v->patch) {
                    if (v->patch->note >= 0)
                        v->note = v->patch->note;
                    uint32_t freq = get_freq(v->note << 8);
                    for (uint8_t i = 0; i < v->patch->n_samples; i++) {
                        if (v->patch->samples[i].high_freq > freq) {
                            v->sample_index = i;
                            break;
                        }
                    }
                }
            }
            break;
        }
        case MDV_CONTROLLER: {
            switch (event->param1) {
                case MDV_VOLUME:
                    player->channels[event->channel].volume = event->param2;
                    break;
                case MDV_EXPRESSION:
                    player->channels[event->channel].expression = event->param2;
                    break;
                case MDV_PAN:
                    player->channels[event->channel].pan = event->param2 - 64;
                    break;
                default:
                    break;
            }
            break;
        }
        case MDV_PROGRAM_CHANGE: {
             // Silence all voices in this channel.
            Channel* ch = &player->channels[event->channel];
            uint8_t* np = &ch->voices;
            while (*np != 255) {
                Voice* v = &player->voices[*np];
                *np = v->next;
                v->next = player->inactive;
                player->inactive = v - player->voices;
            }
            ch->program = event->param1;
            break;
        }
        case MDV_PITCH_BEND: {
            player->channels[event->channel].pitch_bend =
                (event->param2 << 7 | event->param1) - 8192;
            break;
        }
        case MDV_SET_TEMPO: {
            uint32_t ms_per_beat = event->channel << 16 | event->param1 << 8 | event->param2;
            player->tick_length = (uint64_t)SAMPLE_RATE * ms_per_beat / 1000000 / player->seq->tpb;
            break;
        }
        default:
            break;
    }
}

typedef struct Samp {
    int16_t l;
    int16_t r;
} Samp;

void mdv_fast_forward_to_note (MDV_Player* player) {
    if (!player->seq) return;
    player->samples_to_tick = 1;
    player->ticks_to_event = 0;
    while (!player->done && player->current->event.type != MDV_NOTE_ON) {
        do_event(player, &player->current->event);
        player->current += 1;
        if (player->current >= player->seq->events + player->seq->n_events) {
            player->done = 1;
        }
    }
}

void mdv_get_audio (MDV_Player* player, uint8_t* buf_, int len) {
    MDV_Sequence* seq = player->seq;
    Samp* buf = (Samp*)buf_;
    len /= 4;  // Assuming always a whole number of samples
    if (!seq || player->done) {
        for (int i = 0; i < len; i++) {
            buf[i].l = 0;
            buf[i].r = 0;
        }
        return;
    }
    for (int i = 0; i < len; i++) {
         // Advance event timeline.
        if (!player->done && !player->samples_to_tick) {
            while (!player->done && !player->ticks_to_event) {
                do_event(player, &player->current->event);
                uint32_t old_time = player->current->time;
                player->current += 1;
                if (player->current >= seq->events + seq->n_events) {
                    player->done = 1;
                }
                else {
                    player->ticks_to_event = player->current->time - old_time;
                }
            }
            --player->ticks_to_event;
            player->samples_to_tick = player->tick_length;
        }
        --player->samples_to_tick;
         // Now mix voices
        int32_t left = 0;
        int32_t right = 0;
        for (Channel* ch = player->channels+0; ch < player->channels+16; ch++) {
            uint8_t* next_ip;
            for (uint8_t* ip = &ch->voices; *ip != 255; ip = next_ip) {
                Voice* v = &player->voices[*ip];
                next_ip = &v->next;
                goto skip_delete_voice;
                delete_voice: {
                    next_ip = ip;
                    *ip = v->next;
                    v->next = player->inactive;
                    player->inactive = v - player->voices;
                    continue;
                }
                skip_delete_voice: { }
                uint8_t no_envelope = 0;
                uint8_t no_loop = 0;
                if (ch == player->channels+9) {
                    no_envelope = !v->patch->keep_envelope;
                    no_loop = !v->patch->keep_loop;
                }
                if (v->patch) {
                    MDV_Sample* sample = &v->patch->samples[v->sample_index];
                     // Account for pitch bend
                    uint32_t freq = get_freq(v->note * 256 + ch->pitch_bend / 16);
                     // Do envelopes  TODO: fade to 0 at end
                    if (no_envelope) {
                        v->envelope_value = 1023 << 20;
                    }
                    else {
                        uint32_t rate = sample->envelope_rates[v->envelope_phase];
                        uint32_t target = sample->envelope_offsets[v->envelope_phase];
                        if (target > v->envelope_value) {
                            if (v->envelope_value + rate < target) {
                                v->envelope_value += rate;
                            }
                            else if (v->envelope_phase == 5) {
                                goto delete_voice;
                            }
                            else {
                                v->envelope_value = target;
                                if (v->envelope_phase != 2) {
                                    v->envelope_phase += 1;
                                }
                            }
                        }
                        else {
                            if (target + rate < v->envelope_value) {
                                v->envelope_value -= rate;
                            }
                            else if (v->envelope_phase == 5 || target == 0) {
                                goto delete_voice;
                            }
                            else {
                                v->envelope_value = target;
                                if (v->envelope_phase != 2) {
                                    v->envelope_phase += 1;
                                }
                            }
                        }
                    }
                     // Tremolo
                    v->tremolo_sweep_position += sample->tremolo_sweep_increment;
                    if (v->tremolo_sweep_position > 1 << 16)
                        v->tremolo_sweep_position = 1 << 16;
                    int32_t tremolo_depth = (sample->tremolo_depth << 7) * v->tremolo_sweep_position;
                    v->tremolo_phase += sample->tremolo_phase_increment;
                    double tremolo_volume = 1.0 + sines[(v->tremolo_phase >> 5) % 1024]
                         * tremolo_depth * 38 * (1.0 / (double)(1<<17));

                     // Calculate new position
                    uint64_t next_pos;
                    if (v->backwards) {
                        next_pos = v->sample_pos - 0x100000000LL * sample->sample_rate / SAMPLE_RATE * freq / sample->root_freq;
                    }
                    else {
                        next_pos = v->sample_pos + 0x100000000LL * sample->sample_rate / SAMPLE_RATE * freq / sample->root_freq;
                    }
                     // Loop
                    if (sample->loop && !no_loop) {
                        if (v->backwards) {
                            if (next_pos <= sample->loop_start * 0x100000000LL) {
                                v->backwards = 0;
                                next_pos = 2 * sample->loop_start * 0x100000000LL - next_pos;
                            }
                        }
                        else {
                            if (v->sample_pos >= sample->loop_end * 0x100000000LL) {
                                if (sample->pingpong) {
                                    v->backwards = 1;
                                    next_pos = 2 * sample->loop_end * 0x100000000LL - next_pos;
                                }
                                else {
                                    next_pos -= (sample->loop_end - sample->loop_start) * 0x100000000LL;
                                }
                            }
                        }
                    }
                     // With interpolation, the length of a sample is one minus the number of points
                    else if (v->sample_pos >= sample->data_size * 0x100000000LL - 1) {
                        goto delete_voice;
                    }
                     // Linear interpolation.
                    int64_t samp = sample->data[v->sample_pos / 0x100000000LL] * (0x100000000LL - (v->sample_pos & 0xffffffffLL));
                    samp += sample->data[v->sample_pos / 0x100000000LL + 1] * (v->sample_pos & 0xffffffffLL);
                     // Volume calculation.  Is there a better way to do this?
                    if (v->envelope_value > 1023 << 20) {
                        printf("Warning: envelope_value too high!\n");
                        v->envelope_value = 1023 << 20;
                    }
                    double envelope_volume = pows[v->envelope_value >> 20];
                    uint32_t volume = (uint32_t)v->patch->volume * 128
                                    * vols[ch->volume] / 65535
                                    * vols[ch->expression] / 65535
                                    * vols[v->velocity] / 65535
                                    * envelope_volume
                                    * (1.0 + (tremolo_volume / 2000000.0));
                    uint64_t val = samp / 0x100000000LL * volume / 65535;
                    left += val * (64 + ch->pan) / 64;
                    right += val * (64 - ch->pan) / 64;
                     // Move position
                    v->sample_pos = next_pos;
                }
                else {  // No patch, do a square wave!
                     // Loop
                    v->sample_pos %= 0x100000000LL;
                     // Add value
                    int32_t sign = v->sample_pos < 0x80000000LL ? -1 : 1;
                    uint32_t val = sign * v->velocity * ch->volume * ch->expression / (32*127);
                    left += val;
                    right += val;
                     // Move position
                    uint32_t freq = get_freq(v->note << 8);
                    v->sample_pos += 0x100000000LL * freq / 1000 / SAMPLE_RATE;
                }
            }
        }
        buf[i].l = left > 32767 ? 32767 : left < -32768 ? -32768 : left;
        buf[i].r = right > 32767 ? 32767 : right < -32768 ? -32768 : right;
        if (buf[i].l == 32767 || buf[i].l == -32768)
            player->clip_count += 1;
        if (buf[i].r == 32767 || buf[i].r == -32768)
            player->clip_count += 1;
    }
}

