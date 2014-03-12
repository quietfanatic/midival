
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include "midi.h"
#include "player.h"

int main (int argc, char** argv) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
    }

    Player* player = new_player();

    SDL_AudioSpec spec;
    spec.freq = 48000;
    spec.format = AUDIO_S16;
    spec.channels = 1;
    spec.samples = 4096;
    spec.callback = (void(*)(void*,uint8*,int))get_audio;
    spec.userdata = player;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (dev == 0) {
        printf("SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    }
    Sequence* seq = argc == 2
        ? load_midi(argv[1])
        : load_midi("test.mid");
    play_sequence(player, seq);
    SDL_PauseAudioDevice(dev, 0);
    fgetc(stdin);
    SDL_PauseAudioDevice(dev, 1);
    free_player(player);
    free_sequence(seq);
    SDL_Quit();
    return 0;
}
