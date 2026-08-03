#pragma once

#include <string>

namespace Platform3DS {

bool prepareDisc(std::string& cuePath) noexcept;

// Is this a New 3DS/New 2DS (804 MHz ARM11 with L2 cache) rather than an Old 3DS/2DS (268 MHz)?
// The answer is cached after the first query.
bool isNew3DS() noexcept;

//--------------------------------------------------------------------------------------------------------------------------------------
// Startup log.
//
// A failure during startup used to drop the player straight back to the HOME Menu with nothing to go on: a fatal error
// ends in 'abort', and an aborting process leaves no crash dump because as far as the system is concerned it asked to
// exit. That is especially unhelpful for an installed CIA, which has no console to print to and no way to run under a
// debugger. So every step of startup writes a line to a file on the SD card instead.
//
// Each line is opened, written, flushed and closed on its own, so the log survives an abort, a hang or a hard crash
// immediately afterwards. That is far too slow for anything per frame, and is only used while starting up.
//--------------------------------------------------------------------------------------------------------------------------------------
void logOpen() noexcept;

// Arranges for settings to be written out when the player presses HOME, which is how the game is usually left.
// Call once at startup; safe to call before anything the callback touches has been used.
void installHomeButtonSaveHook() noexcept;
void logf(const char* const format, ...) noexcept;

// Where 'logf' writes, for telling the player where to look
const char* logPath() noexcept;

// A one line summary of how much memory is left, for spotting an allocation failure in the log.
// Returns a pointer to a static buffer, so the result is only good until the next call.
const char* memoryStatusString() noexcept;

// Shows a fatal error on the top screen and waits, rather than vanishing back to the HOME Menu.
// The message is written to the startup log first, in case the display cannot be brought up either.
void reportFatalError(const char* const msg) noexcept;

}

//--------------------------------------------------------------------------------------------------------------------------------------
// Convenience macro for startup logging from code shared with the desktop builds, where it compiles to nothing
//--------------------------------------------------------------------------------------------------------------------------------------
#if PSYDOOM_3DS
    #define PSYDOOM_3DS_LOG(...) Platform3DS::logf(__VA_ARGS__)
#else
    #define PSYDOOM_3DS_LOG(...) ((void) 0)
#endif
