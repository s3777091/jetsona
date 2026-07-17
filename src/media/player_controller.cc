#include "media/player_controller.h"

#include "esp_log.h"
#include "net/zing_music_client.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#define TAG "MusicPlayer"

namespace jetson::music {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kPreviousRestartsAfterMs = 3000;
constexpr int64_t kNearEndToleranceMs = 3000;
constexpr int64_t kMinimumHealthyPlaybackMs = 5000;
constexpr auto kWorkerPollInterval = std::chrono::milliseconds(100);
constexpr auto kTerminatePollInterval = std::chrono::milliseconds(20);
constexpr int kTerminatePollCount = 20;

int ClampVolume(int volume) {
    return std::max(0, std::min(100, volume));
}

int64_t ClampPosition(int64_t position_ms, int64_t duration_ms) {
    position_ms = std::max<int64_t>(0, position_ms);
    if (duration_ms > 0) position_ms = std::min(position_ms, duration_ms);
    return position_ms;
}

std::string PlayerExitError(int status) {
    char message[96];
    if (WIFEXITED(status)) {
        std::snprintf(message, sizeof(message),
                      "Trình phát đã thoát (mã %d)", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        std::snprintf(message, sizeof(message),
                      "Trình phát bị dừng bởi tín hiệu %d", WTERMSIG(status));
    } else {
        std::snprintf(message, sizeof(message), "Trình phát đã dừng");
    }
    return message;
}

bool ExitWasSuccessful(int status) {
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool SignalPlayer(pid_t pid, int signal_number) {
    if (pid <= 0) return false;
    // The child creates its own process group. Signalling the group also stops
    // helper processes a configured mpv wrapper may have started.
    if (::kill(-pid, signal_number) == 0) return true;
    if (errno == ESRCH) return ::kill(pid, signal_number) == 0 || errno == ESRCH;
    return ::kill(pid, signal_number) == 0;
}

bool WriteAll(int fd, const char *data, size_t size) {
    while (size > 0) {
        ssize_t written = ::send(fd, data, size, MSG_NOSIGNAL);
        if (written > 0) {
            data += written;
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool SendMpvCommand(const std::string &socket_path, const std::string &json) {
    constexpr size_t kUnixSocketPathCapacity =
        sizeof(((sockaddr_un *)nullptr)->sun_path);
    if (socket_path.empty() || socket_path.size() >= kUnixSocketPathCapacity)
        return false;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return false;
    }

    std::string line = json;
    line.push_back('\n');
    bool ok = WriteAll(fd, line.data(), line.size());
    ::close(fd);
    return ok;
}

} // namespace

struct PlayerController::Impl {
    enum class CommandType {
        Load,
        Pause,
        Resume,
        StopProcess,
        ApplyOutput,
        Shutdown,
    };

    struct Command {
        CommandType type = CommandType::StopProcess;
        uint64_t generation = 0;
        Track track;
        int64_t start_ms = 0;
    };

    Impl() {
        const char *configured = std::getenv("JETSON_MUSIC_PLAYER");
        player_binary_ = configured && configured[0] ? configured : "mpv";
        position_anchor_ = Clock::now();
        worker_ = std::thread([this]() { WorkerLoop(); });
    }

    ~Impl() {
        shutting_down_.store(true);
        generation_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            commands_.clear();
            commands_.push_back(Command{CommandType::Shutdown});
        }
        command_cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    PlayerSnapshot Snapshot() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        PlayerSnapshot copy = state_;
        copy.position_ms = DerivedPositionLocked(Clock::now());
        return copy;
    }

    void PlayQueue(std::vector<Track> queue, size_t start) {
        if (queue.empty()) {
            Stop();
            return;
        }
        if (start >= queue.size()) start = 0;

        Command command;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            command.generation = generation_.fetch_add(1) + 1;
            pause_requested_.store(false);
            state_.queue = std::move(queue);
            state_.index = start;
            state_.has_current = true;
            state_.current = state_.queue[start];
            state_.position_ms = 0;
            state_.duration_ms = state_.current.duration_ms;
            state_.status = PlaybackStatus::Resolving;
            state_.error.clear();
            position_anchor_ = Clock::now();
            RefreshCapabilitiesLocked();
            ++state_.revision;

            command.type = CommandType::Load;
            command.track = state_.current;
            command.start_ms = 0;
        }
        Enqueue(std::move(command));
    }

    void Toggle() {
        PlayerSnapshot current = Snapshot();
        if (!current.has_current) return;
        switch (current.status) {
        case PlaybackStatus::Paused:
        case PlaybackStatus::Ended:
        case PlaybackStatus::Error:
            Resume();
            break;
        case PlaybackStatus::Idle:
            break;
        default:
            Pause();
            break;
        }
    }

    void Pause() {
        bool enqueue = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!state_.has_current || state_.status == PlaybackStatus::Idle ||
                state_.status == PlaybackStatus::Ended ||
                state_.status == PlaybackStatus::Error) return;
            pause_requested_.store(true);
            CommitPositionLocked(Clock::now());
            if (state_.status != PlaybackStatus::Paused) {
                state_.status = PlaybackStatus::Paused;
                ++state_.revision;
            }
            enqueue = true;
        }
        if (enqueue) Enqueue(Command{CommandType::Pause});
    }

    void Resume() {
        Command command;
        bool reload = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!state_.has_current) return;
            pause_requested_.store(false);
            if (state_.status == PlaybackStatus::Ended ||
                state_.status == PlaybackStatus::Error) {
                command.type = CommandType::Load;
                command.generation = generation_.fetch_add(1) + 1;
                command.track = state_.current;
                command.start_ms = state_.status == PlaybackStatus::Ended
                                       ? 0 : state_.position_ms;
                state_.position_ms = command.start_ms;
                state_.status = PlaybackStatus::Resolving;
                state_.error.clear();
                position_anchor_ = Clock::now();
                ++state_.revision;
                reload = true;
            } else {
                position_anchor_ = Clock::now();
                if (state_.status != PlaybackStatus::Playing) {
                    state_.status = PlaybackStatus::Playing;
                    ++state_.revision;
                }
            }
        }
        Enqueue(reload ? std::move(command) : Command{CommandType::Resume});
    }

    void Previous() {
        PlayerSnapshot current = Snapshot();
        if (!current.has_current) return;
        if (current.position_ms > kPreviousRestartsAfterMs || current.index == 0) {
            SeekTo(0);
            return;
        }
        SelectIndex(current.index - 1, 0);
    }

    void Next() {
        Command command;
        bool should_load = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!state_.has_current) return;
            if (state_.index + 1 < state_.queue.size()) {
                command = SelectIndexLocked(state_.index + 1, 0);
                should_load = true;
            } else {
                generation_.fetch_add(1);
                CommitPositionLocked(Clock::now());
                if (state_.duration_ms > 0) state_.position_ms = state_.duration_ms;
                state_.status = PlaybackStatus::Ended;
                pause_requested_.store(false);
                ++state_.revision;
            }
        }
        Enqueue(should_load ? std::move(command)
                            : Command{CommandType::StopProcess});
    }

    void SeekTo(int64_t position_ms) {
        Command command;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!state_.has_current) return;
            position_ms = ClampPosition(position_ms, state_.duration_ms);
            command.type = CommandType::Load;
            command.generation = generation_.fetch_add(1) + 1;
            command.track = state_.current;
            command.start_ms = position_ms;
            state_.position_ms = position_ms;
            state_.status = pause_requested_.load()
                                ? PlaybackStatus::Paused
                                : PlaybackStatus::Resolving;
            state_.error.clear();
            position_anchor_ = Clock::now();
            ++state_.revision;
        }
        Enqueue(std::move(command));
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            generation_.fetch_add(1);
            pause_requested_.store(false);
            const int volume = state_.volume;
            const bool muted = state_.muted;
            const uint64_t revision = state_.revision + 1;
            state_ = PlayerSnapshot{};
            state_.volume = volume;
            state_.muted = muted;
            state_.revision = revision;
            position_anchor_ = Clock::now();
        }
        Enqueue(Command{CommandType::StopProcess});
    }

    void SetVolume(int volume) {
        volume = ClampVolume(volume);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (state_.volume == volume) return;
            state_.volume = volume;
            ++state_.revision;
        }
        Enqueue(Command{CommandType::ApplyOutput});
    }

    void SetMuted(bool muted) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (state_.muted == muted) return;
            state_.muted = muted;
            ++state_.revision;
        }
        Enqueue(Command{CommandType::ApplyOutput});
    }

private:
    mutable std::mutex state_mutex_;
    PlayerSnapshot state_;
    Clock::time_point position_anchor_{};

    std::mutex command_mutex_;
    std::condition_variable command_cv_;
    std::deque<Command> commands_;
    std::thread worker_;
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> pause_requested_{false};
    std::atomic<uint64_t> generation_{0};

    jetson::ZingMusicClient zing_;
    std::string player_binary_;
    pid_t child_pid_ = -1;
    bool child_signal_stopped_ = false;
    std::string ipc_socket_path_;
    std::string active_url_;
    Track active_track_;
    uint64_t active_generation_ = 0;
    int stream_refreshes_ = 0;
    Clock::time_point child_started_at_{};

    int64_t DerivedPositionLocked(Clock::time_point now) const {
        int64_t position = state_.position_ms;
        if (state_.status == PlaybackStatus::Playing) {
            position += std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - position_anchor_).count();
        }
        return ClampPosition(position, state_.duration_ms);
    }

    void CommitPositionLocked(Clock::time_point now) {
        state_.position_ms = DerivedPositionLocked(now);
        position_anchor_ = now;
    }

    void RefreshCapabilitiesLocked() {
        state_.can_previous = state_.has_current && state_.index > 0;
        state_.can_next = state_.has_current &&
                          state_.index + 1 < state_.queue.size();
    }

    void Enqueue(Command command) {
        if (shutting_down_.load() && command.type != CommandType::Shutdown) return;
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            if (command.type == CommandType::ApplyOutput) {
                commands_.erase(std::remove_if(commands_.begin(), commands_.end(),
                                                [](const Command &pending) {
                                                    return pending.type == CommandType::ApplyOutput;
                                                }),
                                commands_.end());
            } else if (command.type == CommandType::Pause ||
                       command.type == CommandType::Resume) {
                commands_.erase(std::remove_if(commands_.begin(), commands_.end(),
                                                [](const Command &pending) {
                                                    return pending.type == CommandType::Pause ||
                                                           pending.type == CommandType::Resume;
                                                }),
                                commands_.end());
            }
            commands_.push_back(std::move(command));
        }
        command_cv_.notify_one();
    }

    Command SelectIndexLocked(size_t index, int64_t start_ms) {
        Command command;
        command.type = CommandType::Load;
        command.generation = generation_.fetch_add(1) + 1;
        state_.index = index;
        state_.current = state_.queue[index];
        state_.has_current = true;
        state_.position_ms = ClampPosition(start_ms, state_.current.duration_ms);
        state_.duration_ms = state_.current.duration_ms;
        state_.status = pause_requested_.load()
                            ? PlaybackStatus::Paused
                            : PlaybackStatus::Resolving;
        state_.error.clear();
        position_anchor_ = Clock::now();
        RefreshCapabilitiesLocked();
        ++state_.revision;
        command.track = state_.current;
        command.start_ms = state_.position_ms;
        return command;
    }

    void SelectIndex(size_t index, int64_t start_ms) {
        Command command;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (index >= state_.queue.size()) return;
            command = SelectIndexLocked(index, start_ms);
        }
        Enqueue(std::move(command));
    }

    bool GenerationIsCurrent(uint64_t generation) const {
        return !shutting_down_.load() && generation == generation_.load();
    }

    void SetWorkerStatus(uint64_t generation, PlaybackStatus status,
                         int64_t position_ms = -1) {
        if (!GenerationIsCurrent(generation)) return;
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!GenerationIsCurrent(generation) || !state_.has_current) return;
        if (position_ms >= 0)
            state_.position_ms = ClampPosition(position_ms, state_.duration_ms);
        state_.status = status;
        position_anchor_ = Clock::now();
        ++state_.revision;
    }

    void SetError(uint64_t generation, const std::string &error) {
        if (!GenerationIsCurrent(generation)) return;
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!GenerationIsCurrent(generation)) return;
        CommitPositionLocked(Clock::now());
        state_.status = PlaybackStatus::Error;
        state_.error = error.empty() ? "Không thể phát bài hát" : error;
        ++state_.revision;
    }

    void WorkerLoop() {
        for (;;) {
            Command command;
            bool have_command = false;
            {
                std::unique_lock<std::mutex> lock(command_mutex_);
                if (commands_.empty())
                    command_cv_.wait_for(lock, kWorkerPollInterval,
                                         [this]() { return !commands_.empty(); });
                if (!commands_.empty()) {
                    command = std::move(commands_.front());
                    commands_.pop_front();
                    have_command = true;
                }
            }

            if (have_command) {
                if (command.type == CommandType::Shutdown) {
                    StopChild();
                    return;
                }
                HandleCommand(command);
            }
            PollChild();
        }
    }

    void HandleCommand(const Command &command) {
        switch (command.type) {
        case CommandType::Load:
            HandleLoad(command);
            break;
        case CommandType::Pause:
            PauseChild();
            break;
        case CommandType::Resume:
            ResumeChild();
            break;
        case CommandType::StopProcess:
            StopChild();
            break;
        case CommandType::ApplyOutput:
            ApplyOutput();
            break;
        case CommandType::Shutdown:
            break;
        }
    }

    void HandleLoad(const Command &command) {
        if (!GenerationIsCurrent(command.generation)) return;
        StopChild();
        if (!GenerationIsCurrent(command.generation)) return;

        active_track_ = command.track;
        active_generation_ = command.generation;
        active_url_.clear();
        stream_refreshes_ = 0;
        ResolveAndLaunch(command.track, command.generation, command.start_ms);
    }

    void ResolveAndLaunch(const Track &track, uint64_t generation,
                          int64_t start_ms) {
        if (!GenerationIsCurrent(generation)) return;
        if (!pause_requested_.load())
            SetWorkerStatus(generation, PlaybackStatus::Resolving, start_ms);

        std::string url;
        std::string error;
        bool resolved = false;
        try {
            resolved = zing_.FetchStreamingUrl(track.id, url, error);
        } catch (const std::exception &exception) {
            error = std::string("Lỗi khi lấy luồng Zing: ") + exception.what();
        } catch (...) {
            error = "Lỗi khi lấy luồng Zing";
        }
        if (!GenerationIsCurrent(generation)) return;
        if (!resolved || url.empty()) {
            SetError(generation, error.empty() ? "Không lấy được đường dẫn phát nhạc"
                                               : error);
            return;
        }

        active_track_ = track;
        active_generation_ = generation;
        active_url_ = std::move(url);
        const bool start_paused = pause_requested_.load();
        if (!start_paused)
            SetWorkerStatus(generation, PlaybackStatus::Buffering, start_ms);

        if (!LaunchChild(active_url_, generation, start_ms, start_paused, error)) {
            SetError(generation, error);
            return;
        }
        if (!GenerationIsCurrent(generation)) {
            StopChild();
            return;
        }

        SetWorkerStatus(generation,
                        start_paused ? PlaybackStatus::Paused
                                     : PlaybackStatus::Playing,
                        start_ms);
        ESP_LOGI(TAG, "playing %s at %lld ms with pid %d",
                 track.id.c_str(), static_cast<long long>(start_ms),
                 static_cast<int>(child_pid_));
    }

    bool LaunchChild(const std::string &url, uint64_t generation,
                     int64_t start_ms, bool start_paused, std::string &error) {
        int volume = 50;
        bool muted = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            volume = state_.volume;
            muted = state_.muted;
        }

        std::string socket_path = "/tmp/jetson-fw-mpv-" +
                                  std::to_string(static_cast<long long>(::getpid())) +
                                  "-" + std::to_string(generation) + ".sock";
        constexpr size_t kUnixSocketPathCapacity =
            sizeof(((sockaddr_un *)nullptr)->sun_path);
        if (socket_path.size() >= kUnixSocketPathCapacity) {
            error = "Đường dẫn IPC của trình phát quá dài";
            return false;
        }
        ::unlink(socket_path.c_str());

        char start_option[64];
        std::snprintf(start_option, sizeof(start_option), "--start=%.3f",
                      static_cast<double>(std::max<int64_t>(0, start_ms)) / 1000.0);

        std::vector<std::string> arguments = {
            player_binary_,
            "--no-video",
            "--audio-display=no",
            "--really-quiet",
            "--no-terminal",
            "--force-window=no",
            "--keep-open=no",
            "--input-ipc-server=" + socket_path,
            "--volume=" + std::to_string(ClampVolume(volume)),
            std::string("--mute=") + (muted ? "yes" : "no"),
            start_option,
        };
        if (start_paused) arguments.emplace_back("--pause=yes");
        arguments.emplace_back("--");
        arguments.push_back(url);

        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (auto &argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);

        int exec_pipe[2];
        if (::pipe(exec_pipe) != 0) {
            error = std::string("Không tạo được pipe cho trình phát: ") +
                    std::strerror(errno);
            return false;
        }
        // Service launches normally have stdin/out/err open, but keep the
        // exec-error channel safe even when one of those descriptors was
        // closed by a supervisor before startup.
        auto move_above_stdio = [](int &fd) {
            if (fd > STDERR_FILENO) return true;
            const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (moved < 0) return false;
            ::close(fd);
            fd = moved;
            return true;
        };
        if (!move_above_stdio(exec_pipe[0]) || !move_above_stdio(exec_pipe[1])) {
            const int move_error = errno;
            ::close(exec_pipe[0]);
            ::close(exec_pipe[1]);
            error = std::string("Không chuẩn bị được pipe cho trình phát: ") +
                    std::strerror(move_error);
            return false;
        }
        ::fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);

        const pid_t parent_pid = ::getpid();
        pid_t pid = ::fork();
        if (pid < 0) {
            error = std::string("Không khởi chạy được trình phát: ") +
                    std::strerror(errno);
            ::close(exec_pipe[0]);
            ::close(exec_pipe[1]);
            return false;
        }

        if (pid == 0) {
            ::close(exec_pipe[0]);
            ::setpgid(0, 0);
            ::prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (::getppid() != parent_pid) _exit(125);

            int null_fd = ::open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDIN_FILENO);
                ::dup2(null_fd, STDOUT_FILENO);
                ::dup2(null_fd, STDERR_FILENO);
                if (null_fd > STDERR_FILENO) ::close(null_fd);
            }

            ::execvp(argv[0], argv.data());
            const int exec_error = errno;
            ssize_t ignored = ::write(exec_pipe[1], &exec_error, sizeof(exec_error));
            (void)ignored;
            _exit(127);
        }

        ::close(exec_pipe[1]);
        ::setpgid(pid, pid);
        int exec_error = 0;
        ssize_t read_size;
        do {
            read_size = ::read(exec_pipe[0], &exec_error, sizeof(exec_error));
        } while (read_size < 0 && errno == EINTR);
        ::close(exec_pipe[0]);

        if (read_size > 0) {
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            error = std::string("Không chạy được '") + player_binary_ +
                    "': " + std::strerror(exec_error);
            ::unlink(socket_path.c_str());
            return false;
        }

        child_pid_ = pid;
        child_signal_stopped_ = false;
        ipc_socket_path_ = std::move(socket_path);
        child_started_at_ = Clock::now();
        return true;
    }

    void PauseChild() {
        if (child_pid_ <= 0) return;
        if (SendMpvCommand(ipc_socket_path_,
                           "{\"command\":[\"set_property\",\"pause\",true]}")) {
            child_signal_stopped_ = false;
            return;
        }
        if (SignalPlayer(child_pid_, SIGSTOP)) child_signal_stopped_ = true;
    }

    void ResumeChild() {
        if (child_pid_ <= 0) return;
        if (child_signal_stopped_) {
            SignalPlayer(child_pid_, SIGCONT);
            child_signal_stopped_ = false;
            return;
        }
        if (!SendMpvCommand(ipc_socket_path_,
                            "{\"command\":[\"set_property\",\"pause\",false]}"))
            SignalPlayer(child_pid_, SIGCONT);
    }

    void ApplyOutput() {
        if (child_pid_ <= 0) return;
        int volume;
        bool muted;
        int64_t position;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            volume = state_.volume;
            muted = state_.muted;
            position = DerivedPositionLocked(Clock::now());
        }

        const std::string volume_command =
            "{\"command\":[\"set_property\",\"volume\"," +
            std::to_string(ClampVolume(volume)) + "]}";
        const std::string mute_command =
            std::string("{\"command\":[\"set_property\",\"mute\",") +
            (muted ? "true" : "false") + "]}";
        if (SendMpvCommand(ipc_socket_path_, volume_command) &&
            SendMpvCommand(ipc_socket_path_, mute_command)) return;

        // A custom/old mpv may not expose IPC. Relaunching at the derived
        // position applies --volume/--mute without blocking the caller.
        if (active_url_.empty() || !GenerationIsCurrent(active_generation_)) return;
        const bool paused = pause_requested_.load();
        std::string url = active_url_;
        const uint64_t generation = active_generation_;
        SetWorkerStatus(generation,
                        paused ? PlaybackStatus::Paused : PlaybackStatus::Buffering,
                        position);
        StopChild();
        std::string error;
        if (!LaunchChild(url, generation, position, paused, error)) {
            SetError(generation, error);
            return;
        }
        SetWorkerStatus(generation,
                        paused ? PlaybackStatus::Paused : PlaybackStatus::Playing,
                        position);
    }

    void StopChild() {
        const pid_t pid = child_pid_;
        if (pid <= 0) {
            CleanupChildSurface();
            return;
        }

        if (child_signal_stopped_) SignalPlayer(pid, SIGCONT);
        SignalPlayer(pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < kTerminatePollCount; ++i) {
            int status = 0;
            pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            if (waited < 0 && errno != EINTR) break;
            std::this_thread::sleep_for(kTerminatePollInterval);
        }
        if (!reaped) {
            SignalPlayer(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        child_pid_ = -1;
        child_signal_stopped_ = false;
        CleanupChildSurface();
    }

    void CleanupChildSurface() {
        if (!ipc_socket_path_.empty()) {
            ::unlink(ipc_socket_path_.c_str());
            ipc_socket_path_.clear();
        }
    }

    void PollChild() {
        if (child_pid_ <= 0) return;
        int status = 0;
        pid_t waited = ::waitpid(child_pid_, &status, WNOHANG);
        if (waited == 0 || (waited < 0 && errno == EINTR)) return;
        if (waited < 0) {
            if (errno != ECHILD) return;
            status = 1 << 8;
        }

        child_pid_ = -1;
        child_signal_stopped_ = false;
        CleanupChildSurface();
        HandleChildExit(status);
    }

    void HandleChildExit(int status) {
        const uint64_t generation = active_generation_;
        if (!GenerationIsCurrent(generation)) return;

        int64_t position = 0;
        int64_t duration = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!GenerationIsCurrent(generation)) return;
            CommitPositionLocked(Clock::now());
            position = state_.position_ms;
            duration = state_.duration_ms;
        }

        const int64_t process_lifetime_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - child_started_at_).count();
        const bool near_end = duration > 0 &&
                              position + kNearEndToleranceMs >= duration;
        const bool early = duration > 0 ? !near_end
                                        : process_lifetime_ms < kMinimumHealthyPlaybackMs;
        const bool failed = !ExitWasSuccessful(status) && !near_end;
        if ((early || failed) && stream_refreshes_ == 0) {
            ++stream_refreshes_;
            ESP_LOGW(TAG, "player ended early; refreshing stream for %s",
                     active_track_.id.c_str());
            ResolveAndLaunch(active_track_, generation, position);
            return;
        }
        if (early || failed) {
            SetError(generation, PlayerExitError(status));
            return;
        }

        AdvanceAfterNaturalEnd(generation);
    }

    void AdvanceAfterNaturalEnd(uint64_t completed_generation) {
        Command next;
        bool has_next = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!GenerationIsCurrent(completed_generation) ||
                !state_.has_current) return;
            if (state_.index + 1 < state_.queue.size()) {
                pause_requested_.store(false);
                next = SelectIndexLocked(state_.index + 1, 0);
                has_next = true;
            } else {
                if (state_.duration_ms > 0) state_.position_ms = state_.duration_ms;
                state_.status = PlaybackStatus::Ended;
                state_.error.clear();
                position_anchor_ = Clock::now();
                ++state_.revision;
            }
        }

        if (has_next) {
            active_track_ = next.track;
            active_generation_ = next.generation;
            active_url_.clear();
            stream_refreshes_ = 0;
            ResolveAndLaunch(next.track, next.generation, next.start_ms);
        }
    }
};

PlayerController &PlayerController::Instance() {
    static PlayerController instance;
    return instance;
}

PlayerController::PlayerController() : impl_(std::make_unique<Impl>()) {}
PlayerController::~PlayerController() = default;

PlayerSnapshot PlayerController::Snapshot() const { return impl_->Snapshot(); }

void PlayerController::PlayQueue(std::vector<Track> queue, size_t start) {
    impl_->PlayQueue(std::move(queue), start);
}
void PlayerController::Toggle() { impl_->Toggle(); }
void PlayerController::Pause() { impl_->Pause(); }
void PlayerController::Resume() { impl_->Resume(); }
void PlayerController::Previous() { impl_->Previous(); }
void PlayerController::Next() { impl_->Next(); }
void PlayerController::SeekTo(int64_t position_ms) { impl_->SeekTo(position_ms); }
void PlayerController::Stop() { impl_->Stop(); }
void PlayerController::SetVolume(int volume) { impl_->SetVolume(volume); }
void PlayerController::SetMuted(bool muted) { impl_->SetMuted(muted); }

} // namespace jetson::music
