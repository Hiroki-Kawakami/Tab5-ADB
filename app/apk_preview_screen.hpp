#pragma once
#include <memory>

#include "file_preview.hpp"   // app::FileRef + the preview building blocks
#include "file_transfer.hpp"  // app::TransferJob
#include "screen.hpp"

// APK preview — the ".apk" (SD card) entry in the file-preview registry.
// Shows the manifest metadata parsed locally by app::apkinfo (zip + binary
// AXML, no device needed — works with adb disconnected) and an Install action
// that runs the shared app::install_apk flow. Install is greyed out until an
// adb connection is Online.
class ApkPreviewScreen : public Screen {
public:
    explicit ApkPreviewScreen(app::FileRef ref) : ref_(std::move(ref)) {}

    void build() override;
    void onExit() override {
        if (job_) job_->abort();  // the progress modal dies with this screen
    }

private:
    app::FileRef ref_;
    std::shared_ptr<app::TransferJob> job_;

    void confirm_install();
};
