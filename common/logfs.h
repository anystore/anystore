#pragma once
#include <photon/common/object.h>
#include <photon/common/retval.h>
#include <photon/common/timeout.h>
// #include <photon/fs/filesystem.h>
#include <sys/time.h> // struct timeval
#include <sys/uio.h>  // struct iovec
// #include <sys/stat.h> // uid_t, gid_t
#include <fcntl.h>

struct stat;
struct fiemap;
struct statfs;
struct statvfs;

namespace anystore {
using ::photon::Timeout;
using ::photon::retval;

#ifndef UNIMPLEMENTED
#define UNIMPLEMENTED(func)  \
    virtual func             \
    {                        \
        errno = ENOSYS;      \
        return -1;           \
    }
#endif

class ILogFile : public Object {
public:
    virtual retval<ssize_t> preadv(struct iovec *iov, int iovcnt, off_t offset,
                                    Timeout timeout = {}, void* ext_args = nullptr) = 0;
    inline  retval<ssize_t> pread(void* buf, size_t count, off_t offset,
                                    Timeout timeout = {}, void* ext_args = nullptr) {
        iovec iov{buf, count};
        return preadv(&iov, 1, offset, timeout, ext_args);
    }

    virtual retval<off_t> lseek(off_t offset, int whence) = 0;

    // Append data `iov[iovcnt]` to the end of the log file.
    // If `offset` != nullptr and `*offset != 0`, `*offset` must be the size of the file before write.
    // If `offset` != nullptr and the operation was successful, `*offset` will be stored the offset
    // where data has been written (size of the file before write)
    virtual retval<ssize_t> appendv(struct iovec *iov, int iovcnt, off_t* offset = nullptr,
                                    Timeout timeout = {}, void* ext_args = nullptr) = 0;
    inline  retval<ssize_t> append(const void *buf, size_t count, off_t* offset = nullptr,
                                    Timeout timeout = {}, void* ext_args = nullptr) {
        iovec iov{(void*)buf, count};
        return appendv(&iov, 1, offset, timeout, ext_args);
    }

    virtual retval<int> fsync(Timeout timeout = {}) = 0;
    virtual retval<int> fdatasync(Timeout timeout = {}) = 0;
    virtual retval<int> fstat(struct stat *buf, Timeout timeout = {}) = 0;
    virtual retval<int> ftruncate(off_t length, Timeout timeout = {}) = 0;

    UNIMPLEMENTED(retval<int> fchmod(mode_t mode, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> fchown(uint64_t owner, uint64_t group, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> fadvise(off_t offset, off_t len, int advice, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> sync_file_range(off_t offset, off_t nbytes, unsigned int flags, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> fiemap(struct fiemap* map, Timeout timeout = {}));   // query the extent map for
    UNIMPLEMENTED(retval<int> vioctl(int request, va_list args));
    retval<int> ioctl(int request, ...)
    {
        va_list args;
        va_start(args, request);
        auto ret = vioctl(request, args);
        va_end(args);
        return ret;
    }

    #ifndef FALLOC_FL_KEEP_SIZE
    #define FALLOC_FL_KEEP_SIZE     0x01 /* default is extend size */
    #endif
    #ifndef FALLOC_FL_PUNCH_HOLE
    #define FALLOC_FL_PUNCH_HOLE	0x02 /* de-allocates range */
    #endif
    #ifndef FALLOC_FL_ZERO_RANGE
    #define FALLOC_FL_ZERO_RANGE    0x10
    #endif
    UNIMPLEMENTED(retval<int> fallocate(int mode, off_t offset, off_t len, Timeout timeout = {}));
    retval<int> trim(off_t offset, off_t len, Timeout timeout = {}) {
        int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
        return this->fallocate(mode, offset, len, timeout);
    }
    retval<int> zero_range(off_t offset, off_t len, Timeout timeout = {}) {
        int mode = FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE;
        return this->fallocate(mode, offset, len, timeout);
    }
};

class ILogFileSystem : public Object {
public:
    // if `*fileid_ptr` == -1UL, the logfs will generate a proper id and assign to `*fileid_ptr`
    virtual retval<ILogFile*> open(uint64_t* /* IN/OUT */ fileid_ptr, uint64_t flags, mode_t mode = 0, Timeout timeout = {}) = 0;
    inline  retval<ILogFile*> open(uint64_t fileid, uint64_t flags, mode_t mode = 0, Timeout timeout = {}) {
        return open(&fileid, flags, mode, timeout);
    }
    inline  retval<ILogFile*> creat(uint64_t fileid, mode_t mode, Timeout timeout = {}) {
        return open(fileid, O_CREAT | O_RDWR | O_TRUNC, mode, timeout);
    }
    inline  retval<ILogFile*> creat(uint64_t* /* IN/OUT */ fileid_ptr, mode_t mode, Timeout timeout = {}) {
        if (fileid_ptr) *fileid_ptr = -1UL;
        return open(fileid_ptr, O_CREAT | O_RDWR | O_TRUNC, mode, timeout);
    }

    virtual retval<int> unlink(uint64_t fileid, Timeout timeout = {}) = 0;
    virtual retval<int> truncate(uint64_t fileid, off_t length, Timeout timeout = {}) = 0;
    virtual retval<int> stat(uint64_t fileid, struct stat *buf, Timeout timeout = {}) = 0;
    virtual retval<int> lstat(uint64_t fileid, struct stat *buf, Timeout timeout = {}) = 0;
    virtual retval<int> access(uint64_t fileid, int mode, Timeout timeout = {}) = 0;
    virtual retval<int> sync(Timeout timeout = {}) = 0;

    UNIMPLEMENTED(retval<int> rename(uint64_t oldid, uint64_t newid, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> chmod(uint64_t fileid, mode_t mode, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> chown(uint64_t fileid, uint64_t owner, uint64_t group, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> lchown(uint64_t fileid, uint64_t owner, uint64_t group, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> symlink(uint64_t oldid, uint64_t newid, Timeout timeout = {}));
    UNIMPLEMENTED(retval<ssize_t> readlink(uint64_t path, char *buf, size_t bufsiz, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> link(uint64_t oldid, uint64_t newid, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> statfs(struct statfs *buf, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> statvfs(struct statvfs *buf, Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> utimes(uint64_t fileid, const struct timeval times[2], Timeout timeout = {}));
    UNIMPLEMENTED(retval<int> lutimes(uint64_t fileid, const struct timeval times[2], Timeout timeout = {}));
};


} // namespace anystore