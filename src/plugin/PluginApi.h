// PluginApi — the ABI a native effect add-on (.so) implements.
//
// A plugin is a Haiku shared add-on. It implements daw::IEffect (same compiler/
// C++ ABI as the host — both built with the Haiku toolchain) and exports the C
// symbols below. The host loads it with load_add_on(), reads the metadata, and
// creates instances into a track's effect chain like a built-in effect.
//
// Ownership: daw_plugin_create() returns a heap IEffect the HOST deletes (Haiku
// add-ons share libroot's allocator, so `delete` from the host is safe). The
// host keeps the add-on image loaded while any instance lives.
//
// RT contract is IEffect's: Process()/SetParam() must not allocate or block.
#pragma once

#include "../dsp/IEffect.h"

extern "C" {

// Human-readable plugin name (shown in the "Add" list; also the persisted id).
typedef const char* (*daw_plugin_name_fn)();

// Number of automatable parameters (same slot order Process/SetParam use).
typedef int (*daw_plugin_param_count_fn)();

// Fill a parameter's name + range + default. `i` in [0, param_count).
typedef void (*daw_plugin_param_info_fn)(int i, const char** name,
                                         float* mn, float* mx, float* def);

// Create a new effect instance prepared for `sampleRate`. Host owns/deletes it.
typedef daw::IEffect* (*daw_plugin_create_fn)(double sampleRate);

// The four exported symbol names the host looks up.
//   const char* daw_plugin_name();
//   int         daw_plugin_param_count();
//   void        daw_plugin_param_info(int, const char**, float*, float*, float*);
//   daw::IEffect* daw_plugin_create(double sampleRate);

}  // extern "C"
