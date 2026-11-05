#![no_std]

#[unsafe(no_mangle)]
pub extern "C" fn app_main() {
    app::run();
}
