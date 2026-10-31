// The build stamp of the log header (see kBuildStamp in ctr/plugin.hpp). This file is recompiled by the Makefile after every
// other object, so the stamp is the time of the last build that changed the program, not the time plugin.cpp was last compiled.
#include "ctr/plugin.hpp"

namespace azl {
namespace ctr {

extern const char kBuildStamp[] = __DATE__ " " __TIME__;

} // namespace ctr
} // namespace azl
