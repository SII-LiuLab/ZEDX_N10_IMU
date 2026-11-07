#pragma once
#include <filesystem>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
// Only opening/closing is serialized. Image acquisition runs concurrently.
// The vendor SHW driver uses a non-atomic shared stream count.
class CameraTransitionLock {
    int fd = -1;
public:
    CameraTransitionLock() { lock(); }
    void lock() {
        if(fd>=0) return;
        auto path=std::filesystem::canonical("/proc/self/exe").parent_path().parent_path()/".camera-transition.lock";
        fd=::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0660);
        if(fd<0) throw std::runtime_error("Cannot open camera transition lock");
        if(flock(fd,LOCK_EX)) { ::close(fd); fd=-1; throw std::runtime_error("Cannot lock camera transition"); }
    }
    void unlock() { if(fd>=0) { flock(fd,LOCK_UN); ::close(fd); fd=-1; } }
    ~CameraTransitionLock() { unlock(); }
    CameraTransitionLock(const CameraTransitionLock&)=delete;
    CameraTransitionLock& operator=(const CameraTransitionLock&)=delete;
};
