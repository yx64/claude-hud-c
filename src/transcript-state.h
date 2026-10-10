// The parser state kept between refreshes, so that a refresh parses only what was appended to the
// transcript since the one before; how and when a state is trusted is at the top of
// transcript-state.c. Defining
// HUD_C_NO_TRANSCRIPT_STATE builds without it: every refresh then parses the whole transcript.
#pragma once
#include "transcript-parser.h"

// The open transcript a saved state is checked against.
typedef struct {
    us path;  // resolved
    void *h;  // open for reading
    uint64_t volume, file, size;
} TranscriptFile;

// Where one transcript's state is kept, and what loading it already found out.
typedef struct {
    us path;
    uint64_t head;  // the hash of the transcript's first window, when known
    bool head_known;
} StateSlot;

#if defined(HUD_C_NO_TRANSCRIPT_STATE)
static inline bool state_load(const TranscriptFile *f, StateSlot *slot, Parser *s, uint64_t *off) { return false; }
static inline bool state_save(const TranscriptFile *f, const StateSlot *slot, uint64_t off, const char *buf, uint64_t from, size_t n,
                              Parser *s) { return false; }
#else
// The state saved for f, into s and s->r, and the offset parsing resumes at. False when there is
// none that may be used; s is then to be discarded.
bool state_load(const TranscriptFile *f, StateSlot *slot, Parser *s, uint64_t *off);
// Saves s as the state at off. buf holds n bytes of the transcript, from offset `from` on.
bool state_save(const TranscriptFile *f, const StateSlot *slot, uint64_t off, const char *buf, uint64_t from, size_t n, Parser *s);
#endif
