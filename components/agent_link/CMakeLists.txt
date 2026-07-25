# agent_link — Tab5-side link to tab5adb-agent (see inc/agent_link.hpp and
# android-agent/docs/protocol.md).
#
# App-specific protocol layer on top of the `adb` component's generic raw
# stream (adb::Client::open_stream). The wire engine stays private inside adb,
# so the dependency arrow is simply agent_link -> adb.
# Portable C++ on both targets; the headless test links these same sources.
idf_component_register(
    SRCS "src/agent_link.cpp"
    INCLUDE_DIRS "inc"
    REQUIRES adb)
