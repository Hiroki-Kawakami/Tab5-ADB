# pf_port — the platform abstraction interface shared by device and simulator.
# Header-only: each platform links its own implementation (device: esp32p4/main,
# simulator: simulator/platform/platform_port_sim.cpp).
idf_component_register(INCLUDE_DIRS ".")
