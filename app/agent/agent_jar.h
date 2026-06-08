// The tab5adb-agent dex jar, embedded as a byte array (see CLAUDE.md: the agent
// is a scrcpy-style app_process server pushed to the phone and launched there).
//
// The app pushes these bytes to /data/local/tmp on the connected device and runs
// them with app_process, exactly as android-agent/run.sh / the agent_link tests
// do — but with the jar baked into the firmware instead of read from the host
// filesystem, so it works on the device with no external file.
//
// Regenerate after rebuilding the agent (nix develop -c android-agent/build.sh),
// from the app/agent directory:
//   xxd -i -n agent_jar ../../android-agent/build/tab5adb-agent.jar > agent_jar.c
// then re-apply the `const` qualifiers (see the top of agent_jar.c).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char agent_jar[];
extern const unsigned int agent_jar_len;

#ifdef __cplusplus
}
#endif
