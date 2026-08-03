#include "Doom/psx_main.h"
#include "Platform_3DS.h"

#include <3ds.h>
#include <SDL.h>

#include <string>

u32 __stacksize__ = 1024 * 1024;

// How much of the process's memory libctru sets aside for the linear heap.
//
// Linear memory is only for buffers the GPU and the audio DSP read directly: the screen framebuffers, the GPU command
// buffers and the sound mixing buffers. Those come to a couple of megabytes here. Left alone, libctru reserves 32 MiB
// for them, which is half of everything an installed title gets, and the game then cannot allocate the 16 MiB zone
// heap it needs. That is why a CIA died where the same build under a homebrew launcher did not: homebrew inherits a
// 96 MiB region and could absorb the waste, a title gets 64 MiB and cannot.
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;

int main(int, char**) {
    Platform3DS::logOpen();

    std::string cuePath;

    if (!Platform3DS::prepareDisc(cuePath)) {
        Platform3DS::logf("Exiting: the game data could not be prepared");
        return 1;
    }

    const char* const argv[] = {
        "PsyDoom",
        "-nolauncher",
        "-cue",
        cuePath.c_str(),
    };

    Platform3DS::logf("Entering psx_main with cue '%s'", cuePath.c_str());
    const int exitCode = psx_main(4, argv);
    Platform3DS::logf("psx_main returned %d - exiting normally", exitCode);
    return exitCode;
}

