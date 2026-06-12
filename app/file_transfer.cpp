#include "file_transfer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include "esp_heap_caps.h"

#include "adb.hpp"
#include "adb_app.hpp"
#include "file_preview.hpp"  // format_size
#include "lvgl.hpp"          // lv_async_call / lv_obj_add_event_fn
#include "modal.hpp"

namespace app {

namespace {

constexpr size_t kChunk = 16 * 1024;  // SD fast-path chunk (cache-aligned buffer)
constexpr const char *kRemoteApk = "/data/local/tmp/tab5adb_install.apk";

std::string leaf(const std::string &p) {
    size_t pos = p.rfind('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string join(const std::string &dir, const std::string &name) {
    return dir == "/" ? dir + name : dir + "/" + name;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) ++i;
    return s.substr(i);
}

// One transfer. The Sync worker thread only touches the atomics, fd/buf and
// the byte counter; every UI member is LVGL-thread-only and nulled by the
// LV_EVENT_DELETE watches when the initiating screen dies mid-transfer.
struct Job : public TransferJob,
             public adb::SyncListener,
             public std::enable_shared_from_this<Job> {
    // shared with the Sync worker
    std::shared_ptr<adb::Sync> sync;
    std::atomic<bool> abort_flag{false};
    std::atomic<bool> io_error{false};
    std::atomic<size_t> bytes{0};
    size_t total = 0;

    // local file (worker-thread use during the transfer)
    int fd = -1;
    uint8_t *buf = nullptr;  // push read chunk
    size_t buf_len = 0, buf_off = 0;
    uint32_t mtime = 0;
    std::string tmp_path, final_path;  // pull: .part -> rename target

    // LVGL thread only
    lv_obj_t *parent = nullptr;  // initiating screen's root; null once it died
    lv_obj_t *card = nullptr;    // progress modal; null when closed/died
    lv_obj_t *bar = nullptr;
    lv_obj_t *progress_label = nullptr;
    lv_timer_t *timer = nullptr;
    std::function<void(bool)> on_done;
    bool done_fired = false;

    ~Job() override {
        if (sync) sync->close();
        if (fd >= 0) ::close(fd);
        if (buf) heap_caps_free(buf);
    }

    void abort() override { abort_flag = true; }
    void on_sync_close(adb::Sync *, adb::Error) override {}  // op completions carry the result

    // The screen owning `parent` may pop mid-transfer: watch the deletions so
    // no later UI call touches a freed widget. Weak capture — the watch must
    // not keep the job alive past its last completion.
    void watch(lv_obj_t *obj) {
        lv_obj_add_event_fn(obj, LV_EVENT_DELETE,
                            [w = std::weak_ptr<Job>(shared_from_this()), obj](lv_event_t *) {
            if (auto self = w.lock()) self->ui_died(obj);
        });
    }
    void ui_died(lv_obj_t *obj) {
        if (obj != parent && obj != card) return;
        if (obj == parent) parent = nullptr;
        // The card dies with the parent either way; clear regardless of which
        // DELETE arrives first.
        card = bar = progress_label = nullptr;
        if (timer) {
            lv_timer_delete(timer);
            timer = nullptr;
        }
    }

    void open_progress(const char *title_text) {
        if (!parent) return;
        card = modal_open(parent);
        watch(card);
        auto title = lv_label_create(card);
        lv_label_set_text(title, title_text);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        progress_label = lv_label_create(card);
        lv_label_set_text(progress_label, "");
        lv_obj_set_style_text_font(progress_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(progress_label, lv_color_hex(0x444444), 0);
        bar = lv_bar_create(card);
        lv_obj_set_size(bar, LV_PCT(100), 16);
        lv_bar_set_range(bar, 0, 100);
        auto cancel = lv_button_create(card);
        lv_obj_set_height(cancel, 72);
        lv_obj_set_width(cancel, LV_PCT(100));
        lv_obj_set_style_radius(cancel, 12, 0);
        lv_obj_set_style_bg_color(cancel, lv_color_hex(0xe0e0e0), 0);
        lv_obj_set_style_text_color(cancel, lv_color_black(), 0);
        auto cancel_label = lv_label_create(cancel);
        lv_label_set_text(cancel_label, "Cancel");
        lv_obj_center(cancel_label);
        lv_obj_add_event_fn(cancel, LV_EVENT_CLICKED, [this](lv_event_t *) {
            abort_flag = true;  // the source/sink turns this into a transfer abort
        });
        timer = lv_timer_create([](lv_timer_t *t) {
            static_cast<Job *>(lv_timer_get_user_data(t))->update_progress();
        }, 200, this);
        update_progress();
    }

    void update_progress() {
        if (!bar) return;
        size_t sent = bytes;
        if (total) lv_bar_set_value(bar, (int32_t)(sent * 100 / total), LV_ANIM_OFF);
        lv_label_set_text_fmt(progress_label, "%s / %s", format_size(sent).c_str(),
                              format_size(total).c_str());
    }

    void close_progress() {
        if (card) modal_close(card);  // its DELETE watch clears card/bar/timer
    }

    void fire_done(bool ok) {
        if (done_fired) return;
        done_fired = true;
        if (on_done) on_done(ok);
    }

    // Standard transfer end (LVGL thread): drop the progress dialog and show
    // the result — silently after an abort or once the screen died.
    void finish(bool ok, const char *title, const std::string &fail_msg) {
        bool aborted = abort_flag;
        close_progress();
        if (parent) {
            if (ok) {
                modal_message(parent, title, (std::string(title) + " complete.").c_str());
            } else if (!aborted) {
                modal_message(parent, (std::string(title) + " failed").c_str(),
                              fail_msg.c_str());
            }
        }
        fire_done(ok);
    }

    // ---- the three flows ----
    void start_pull(const std::string &remote);
    void start_push(const std::string &remote_target, std::function<void()> on_pushed,
                    std::function<void(adb::Error)> on_failed);
    void run_pm_install();
};

void Job::start_pull(const std::string &remote) {
    fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (parent) modal_message(parent, "Copy failed", "Cannot create the file.");
        fire_done(false);
        return;
    }
    open_progress("Copying to SD Card");
    auto self = shared_from_this();
    sync->pull(
        remote,
        [self](const uint8_t *d, size_t n) -> bool {  // Sync worker thread
            if (self->abort_flag) return false;
            const uint8_t *p = d;
            for (size_t left = n; left > 0;) {
                ssize_t w = ::write(self->fd, p, left);
                if (w <= 0) {
                    self->io_error = true;
                    return false;
                }
                p += w;
                left -= (size_t)w;
            }
            self->bytes += n;
            return true;
        },
        [self](adb::Error err) {  // Sync worker thread
            lv_async_call([self, err]() {
                bool ok = err == adb::Error::Ok && !self->io_error && !self->abort_flag;
                ::close(self->fd);
                self->fd = -1;
                if (ok && ::rename(self->tmp_path.c_str(), self->final_path.c_str()) != 0) ok = false;
                if (!ok) ::unlink(self->tmp_path.c_str());
                self->sync->close();
                self->finish(ok, "Copy",
                             self->io_error ? std::string("Write failed (SD card full?).")
                                            : std::string("pull: ") + adb::to_string(err));
            });
        });
}

// Pushes fd's content to `remote_target`; on success calls on_pushed (LVGL
// thread), on failure on_failed (LVGL thread, abort included as Cancelled).
void Job::start_push(const std::string &remote_target, std::function<void()> on_pushed,
                     std::function<void(adb::Error)> on_failed) {
    auto self = shared_from_this();
    adb::SyncSource source = [self](uint8_t *dst, size_t cap) -> int {  // worker
        if (self->abort_flag) return -1;
        if (self->buf_off >= self->buf_len) {
            ssize_t n = ::read(self->fd, self->buf, kChunk);
            if (n < 0) return -1;
            if (n == 0) return 0;
            self->buf_len = (size_t)n;
            self->buf_off = 0;
        }
        size_t n = std::min(cap, self->buf_len - self->buf_off);
        std::memcpy(dst, self->buf + self->buf_off, n);
        self->buf_off += n;
        self->bytes += n;
        return (int)n;
    };
    sync->push(remote_target, 0644, mtime, std::move(source),
               [self, on_pushed = std::move(on_pushed),
                on_failed = std::move(on_failed)](adb::Error err) {  // worker
        lv_async_call([self, err, on_pushed, on_failed]() {
            if (err == adb::Error::Ok && !self->abort_flag) on_pushed();
            else on_failed(self->abort_flag ? adb::Error::Cancelled : err);
        });
    });
}

void Job::run_pm_install() {
    auto self = shared_from_this();
    app::adb_client()->exec(std::string("pm install -r ") + kRemoteApk,
                            [self](adb::Error err, const std::string &out) {  // reader thread
        auto box = std::make_shared<std::string>(out);
        lv_async_call([self, err, box]() {
            self->close_progress();
            app::adb_client()->exec(std::string("rm -f ") + kRemoteApk,
                                    [](adb::Error, const std::string &) {});
            bool ok = err == adb::Error::Ok && box->find("Success") != std::string::npos;
            if (self->parent) {
                if (ok) modal_message(self->parent, "Install", "Install complete.");
                else modal_message(self->parent, "Install failed",
                                   trimmed(err == adb::Error::Ok ? *box
                                                                 : adb::to_string(err)).c_str());
            }
            self->fire_done(ok);
        });
    });
}

// Shared setup: connection gate, local file open (push side), sync session.
// Returns false after reporting the failure.
bool open_local_for_push(const std::shared_ptr<Job> &job, const std::string &local_path,
                         const char *fail_title) {
    adb::Client *client = app::adb_client();
    if (!client || client->state() != adb::ConnectionState::Online) {
        if (job->parent) modal_message(job->parent, fail_title, "Not connected.");
        job->fire_done(false);
        return false;
    }
    job->fd = ::open(local_path.c_str(), O_RDONLY);
    if (job->fd < 0) {
        if (job->parent) modal_message(job->parent, fail_title, "Cannot open the file.");
        job->fire_done(false);
        return false;
    }
    struct stat st = {};
    fstat(job->fd, &st);
    job->total = (size_t)st.st_size;
    job->mtime = (uint32_t)st.st_mtime;
    job->buf = (uint8_t *)heap_caps_malloc(kChunk, MALLOC_CAP_CACHE_ALIGNED);
    if (!job->buf) {
        if (job->parent) modal_message(job->parent, fail_title, "Out of memory.");
        job->fire_done(false);
        return false;
    }
    job->sync = client->open_sync(
        std::shared_ptr<adb::SyncListener>(job, static_cast<adb::SyncListener *>(job.get())));
    if (!job->sync) {
        if (job->parent) modal_message(job->parent, fail_title, "Not connected.");
        job->fire_done(false);
        return false;
    }
    return true;
}

}  // namespace

std::shared_ptr<TransferJob> pull_to_sd(lv_obj_t *parent, std::string remote_path,
                                        uint32_t size, std::string local_dir,
                                        std::function<void(bool)> on_done) {
    auto job = std::make_shared<Job>();
    job->parent = parent;
    job->on_done = std::move(on_done);
    job->total = size;
    job->watch(parent);

    adb::Client *client = app::adb_client();
    if (!client || client->state() != adb::ConnectionState::Online) {
        modal_message(parent, "Copy failed", "Not connected.");
        job->fire_done(false);
        return job;
    }
    job->sync = client->open_sync(
        std::shared_ptr<adb::SyncListener>(job, static_cast<adb::SyncListener *>(job.get())));
    if (!job->sync) {
        modal_message(parent, "Copy failed", "Not connected.");
        job->fire_done(false);
        return job;
    }

    job->final_path = join(local_dir, leaf(remote_path));
    job->tmp_path = job->final_path + ".part";

    struct stat st = {};
    if (::stat(job->final_path.c_str(), &st) == 0) {
        // A declined overwrite never starts the transfer (no on_done).
        modal_confirm(parent, "Overwrite?",
                      (leaf(remote_path) + " already exists in the destination.").c_str(),
                      "Overwrite", true,
                      [job, remote_path]() { job->start_pull(remote_path); });
    } else {
        job->start_pull(remote_path);
    }
    return job;
}

std::shared_ptr<TransferJob> push_to_android(lv_obj_t *parent, std::string local_path,
                                             std::string remote_dir,
                                             std::function<void(bool)> on_done) {
    auto job = std::make_shared<Job>();
    job->parent = parent;
    job->on_done = std::move(on_done);
    job->watch(parent);
    if (!open_local_for_push(job, local_path, "Copy failed")) return job;

    std::string name = leaf(local_path);
    std::string remote_target = join(remote_dir, name);

    auto start = [job, remote_target]() {
        job->open_progress("Copying to Android");
        job->start_push(remote_target,
                        [job]() {
                            job->sync->close();
                            job->finish(true, "Copy", "");
                        },
                        [job](adb::Error err) {
                            job->sync->close();
                            job->finish(false, "Copy", std::string("push: ") + adb::to_string(err));
                        });
    };

    // Overwrite check first — stat completes on the Sync worker thread.
    job->sync->stat(remote_target, [job, name, start](adb::Error err, adb::FileStat st) {
        lv_async_call([job, name, start, err, st]() {
            if (job->abort_flag) {  // the screen left while the stat was in flight
                job->fire_done(false);
                return;
            }
            if (err != adb::Error::Ok) {
                if (job->parent) modal_message(job->parent, "Copy failed", adb::to_string(err));
                job->fire_done(false);
                return;
            }
            if (st.exists() && !st.is_reg()) {
                if (job->parent) modal_message(job->parent, "Copy failed",
                                               "A folder with that name exists there.");
                job->fire_done(false);
                return;
            }
            if (st.exists()) {
                if (!job->parent) {  // nowhere to ask — don't overwrite silently
                    job->fire_done(false);
                    return;
                }
                // A declined overwrite never starts the transfer (no on_done).
                modal_confirm(job->parent, "Overwrite?",
                              (name + " already exists in the destination.").c_str(),
                              "Overwrite", true, start);
            } else {
                start();
            }
        });
    });
    return job;
}

std::shared_ptr<TransferJob> install_apk(lv_obj_t *parent, std::string local_path,
                                         std::function<void(bool)> on_done) {
    auto job = std::make_shared<Job>();
    job->parent = parent;
    job->on_done = std::move(on_done);
    job->watch(parent);
    if (!open_local_for_push(job, local_path, "Install failed")) return job;

    job->open_progress("Installing APK");
    job->start_push(kRemoteApk,
                    [job]() {
                        // The APK landed in /data/local/tmp; hand off to pm install.
                        // Stop the byte counter so the label keeps the text.
                        if (job->timer) {
                            lv_timer_delete(job->timer);
                            job->timer = nullptr;
                        }
                        if (job->bar) lv_bar_set_value(job->bar, 100, LV_ANIM_OFF);
                        if (job->progress_label) lv_label_set_text(job->progress_label, "Installing...");
                        job->run_pm_install();
                    },
                    [job](adb::Error err) {
                        bool aborted = job->abort_flag;
                        job->close_progress();
                        app::adb_client()->exec(std::string("rm -f ") + kRemoteApk,
                                                [](adb::Error, const std::string &) {});
                        if (!aborted && job->parent) {
                            modal_message(job->parent, "Install failed",
                                          (std::string("push: ") + adb::to_string(err)).c_str());
                        }
                        job->fire_done(false);
                    });
    return job;
}

}  // namespace app
