#include "chunkfs.h"
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <random>
#include <photon/fs/localfs.h>
#include <photon/fs/subfs.h>
#include <photon/fs/path.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/thread/thread.h>
using namespace std;
using namespace photon;
using namespace photon::fs;


namespace anystore {

inline ILogFile* new_chunk_file(IFile*);

class ChunkFS : public ILogFileSystem {
protected:
    IFileSystem* _fs;
    uint8_t _sub_dir_shift;
    char _file_path_pattern[15];
    int _sb_lock_fd;
    std::mt19937_64 fileid_gen { (uint64_t)time(0) };
    ChunkFS(IFileSystem* fs, uint8_t sub_dir_shift, int lockfd) :
             _fs(fs), _sub_dir_shift(sub_dir_shift), _sb_lock_fd(lockfd) {
        int len = gen_dir_pattern(_file_path_pattern, sizeof(_file_path_pattern), _sub_dir_shift);
        constexpr static char fn_pattern[] = "/%016llx";
        assert(len + LEN(fn_pattern) >= sizeof(_file_path_pattern));
        memcpy(_file_path_pattern + len, fn_pattern, LEN(fn_pattern));
    }
    static int gen_dir_pattern(char* pattern, size_t pattern_len, uint8_t sub_dir_shift) {
        int len = snprintf(pattern, pattern_len, "%%0%dx", (sub_dir_shift + 3) / 4);
        assert(len < pattern_len);
        return len;
    }
    ~ChunkFS() override {
        close(_sb_lock_fd);
        delete _fs;
    }

public:
    constexpr static char SB_FILE_NAME[] = ".sb.anystore.chunkfs";
    constexpr static char SB_SCHEMA[] = "{sub_dir_shift=%d}";
    constexpr static uint8_t SUB_DIR_SHIFT_MAX = 16;
    constexpr static uint8_t SUB_DIR_SHIFT_MIN = 8;
    static retval<void> format(const char* path, uint8_t sub_dir_shift = 12) {
        auto lfs = new_localfs_adaptor();
        if (!lfs)
            LOG_ERRNO_RETURN(0, errno, "failed to new_localfs_adaptor()");

        DEFER(delete lfs);
        auto dir = lfs->opendir(path);
        DEFER(delete dir);
        if (dir) {
            if (!(dir->next() && dir->next())) // no more than . and ..
                LOG_ERROR_RETVAL(EINVAL, "dir not empty: ", path);
        } else {
            int ret = mkdir_recursive(path, lfs);
            if (ret)
                LOG_ERROR_RETVAL(EINVAL, "failed to create dir recursively: ", path);
            dir = lfs->opendir(path);
            if (!dir)
                LOG_ERROR_RETVAL(EINVAL, "failed to opendir: ", path);
        }

        char buf[1024];
        int len = snprintf(buf, sizeof(buf), SB_SCHEMA, sub_dir_shift);
        if (len >= sizeof(buf))
            LOG_ERROR_RETVAL(ENOBUFS, "no enough buffer for config file: ` < `", sizeof(buf), len);
        if (auto ret = write_file(lfs, SB_FILE_NAME, buf, len); ret.failed())
            return ret;

        if (sub_dir_shift > SUB_DIR_SHIFT_MAX)
            sub_dir_shift = SUB_DIR_SHIFT_MAX;
        if (sub_dir_shift < SUB_DIR_SHIFT_MIN)
            sub_dir_shift = SUB_DIR_SHIFT_MIN;
        LOG_INFO(VALUE(sub_dir_shift));

        char pattern[16];
        gen_dir_pattern(pattern, sizeof(pattern), sub_dir_shift);
        for (size_t i = 0; i < (1UL << sub_dir_shift); ++i) {
            int ret = snprintf(buf, sizeof(buf), pattern, i);
            if (ret >= sizeof(buf)) abort();
            ret = lfs->mkdir(buf, 0777);
            if (ret < 0)
                LOG_ERRNO_RETVAL("failed to create dir");
            if (unlikely(i%128 == 0))
                write(1, ".", 1);
        }
    	puts("OK");

        const static char* dir_names[] = {"recycled", "being_incubated", "log"};
        for (auto dn: dir_names) {
            int ret = lfs->mkdir(dn, 0777);
            if (ret < 0)
                LOG_ERRNO_RETVAL("failed to create dir ", dn);
        }
        return 0;
    }
    static retval<void> write_file(IFileSystem* fs, const char* fn, const char* s) {
        return write_file(fs, fn, s, strlen(s));
    }
    static retval<void> write_file(IFileSystem* fs, const char* fn, const void* buf, size_t size) {
        int flags = O_WRONLY | O_CREAT | O_EXCL;
        auto file = fs->open(fn, flags, 0644);
        if (!file)
            LOG_ERRNO_RETVAL("failed to create file '`' for write", fn);

        DEFER(delete file);
        int ret = file->write(buf, size);
        if (ret < 0)
            LOG_ERRNO_RETVAL("failed to write to file ", fn);
        return 0;
    }
    static retval<ChunkFS*> ctor(IFileSystem* fs) {
        auto file = fs->open(SB_FILE_NAME, O_RDONLY);
        if (!file)
            LOG_ERRNO_RETVAL("failed to open config file ", SB_FILE_NAME);

        char buf[1024];
        DEFER(delete file);
        auto len = file->read(buf, sizeof(buf));
        if (len < 0)
            LOG_ERRNO_RETVAL("failed to read config file ", SB_FILE_NAME);
        if (len >= sizeof(buf))
            LOG_ERROR_RETVAL(EFBIG, "config file too large ", SB_FILE_NAME);

        buf[len] = 0;
        int sub_dir_shift = 0;
        int ret = sscanf(buf, SB_SCHEMA, &sub_dir_shift);
        if (ret != 1 || sub_dir_shift < SUB_DIR_SHIFT_MIN || sub_dir_shift > SUB_DIR_SHIFT_MAX)
            LOG_ERROR_RETVAL(EINVAL, "invalid config file ", SB_FILE_NAME);

        int fd = (int)(uint64_t) file->get_underlay_object();
        if (fd < 3)
            LOG_ERROR_RETVAL(EBADF, "invalid fd to ", SB_FILE_NAME);
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (EWOULDBLOCK == errno) {
                LOG_ERROR_RETVAL(EWOULDBLOCK, "root path busy");
            } else {
                LOG_ERRNO_RETVAL("failed to lock root path");
            }
        }
        if ((fd = dup(fd)) < 0)
            LOG_ERRNO_RETVAL("failed to dup SB fd");
        return new ChunkFS(fs, (uint8_t)sub_dir_shift, fd);
    }
    void snprintfn(char* buf, int n, uint64_t fileid) {
        auto dir = fileid & ((1 << _sub_dir_shift) - 1);
        int dirlen = snprintf(buf, n, _file_path_pattern, dir, fileid);
        assert(dirlen < n);
    }
    class FileName {
        char _fn[64];
        ChunkFS* _fs;
    public:
        FileName(ChunkFS* fs) : _fs(fs) { }
        FileName(ChunkFS* fs, uint64_t fileid) : _fs(fs) { set(fileid); }
        void set(uint64_t fileid) { _fs->snprintfn(_fn, sizeof(_fn), fileid); }
        operator const char*() const { return _fn; }
    };
    virtual retval<ILogFile*> open(uint64_t* /* IN/OUT */ fileid_ptr,
                uint64_t flags, mode_t mode = 0, Timeout timeout = {}) override {
        FileName fn(this);
        uint64_t fileid;
        if (fileid_ptr && *fileid_ptr != -1UL) {
            fn.set(*fileid_ptr);
        } else if (flags & O_CREAT) {   // create a new file with a random-genrated fileid
            flags |= O_EXCL;
            do { fn.set(fileid = fileid_gen()); }
            while (_fs->access(fn, F_OK) != 0);
        } else {
            LOG_ERROR_RETVAL(EINVAL, "fileid must be provieded");
        }
        // we use pwrite() for the underlay posix file,
        // in order to enable parallelism I/O
        flags &= ~O_APPEND;
        auto file = _fs->open(fn, flags);
        if (!file)
            LOG_ERRNO_RETVAL("failed to open file `(`) `", HEX(fileid), (const char*)fn, make_named_value("flags", HEX(flags)));
        if (fileid_ptr && *fileid_ptr == -1UL)
        *fileid_ptr = fileid;
        return new_chunk_file(file);
    }
    FileName filename(uint64_t fileid) { return {this, fileid}; }
    virtual retval<int> rename(uint64_t oldid, uint64_t newid, Timeout timeout = {}) override {
        return _fs->rename(filename(oldid), filename(newid));
    }
    virtual retval<int> unlink(uint64_t fileid, Timeout timeout = {}) override {
        return _fs->unlink(filename(fileid));
    }
    virtual retval<int> truncate(uint64_t fileid, off_t length, Timeout timeout = {}) override {
        return _fs->truncate(filename(fileid), length);
    }
    virtual retval<int> stat(uint64_t fileid, struct stat *buf, Timeout timeout = {}) override {
        return _fs->stat(filename(fileid), buf);
    }
    virtual retval<int> lstat(uint64_t fileid, struct stat *buf, Timeout timeout = {}) override {
        return _fs->lstat(filename(fileid), buf);
    }
    virtual retval<int> access(uint64_t fileid, int mode, Timeout timeout = {}) override {
        return _fs->access(filename(fileid), mode);
    }
    virtual retval<int> sync(Timeout timeout = {}) override {
        return _fs->sync();
    }
};

class ChunkFile : public ILogFile {
public:
    IFile* _file;
    std::atomic<uint64_t> _off;
    ChunkFile(IFile* file) : _file(file) {
        _off = _file->lseek(0, SEEK_CUR);
    }
    ~ChunkFile() {
        delete _file;
    }
    virtual retval<ssize_t> preadv(struct iovec *iov, int iovcnt, off_t offset,
                        Timeout timeout = {}, void* ext_args = nullptr) override {
        return _file->preadv(iov, iovcnt, offset);
    }
    virtual retval<off_t> lseek(off_t offset, int whence) override {
        return _file->lseek(offset, whence);
    }
    virtual retval<ssize_t> appendv(struct iovec *iov, int iovcnt, off_t* offset = nullptr,
                        Timeout timeout = {}, void* ext_args = nullptr) override {
        iovector_view v{iov, iovcnt};
        auto sum = v.sum();
        auto off = _off.fetch_add(sum);
        // use pwritev() to realize appendv(), so as to enable parallelism
        auto ret = _file->pwritev(iov, iovcnt, off);
        if (ret >= 0 && offset) *offset = off;
        return ret;
    }
    virtual retval<int> fsync(Timeout timeout = {}) override {
        return _file->fsync();
    }
    virtual retval<int> fdatasync(Timeout timeout = {}) override {
        return _file->fdatasync();
    }
    virtual retval<int> fstat(struct stat *buf, Timeout timeout = {}) override {
        return _file->fstat(buf);
    }
    virtual retval<int> ftruncate(off_t length, Timeout timeout = {}) override {
        return _file->ftruncate(length);
    }
};

inline ILogFile* new_chunk_file(IFile* file) {
    return new ChunkFile(file);
}

retval<ILogFileSystem*> new_chunkfs(const char* path, photon::fs::IFileSystem* fs) {
    if (unlikely(!fs))
        LOG_ERROR_RETVAL(EINVAL, "a file system object must be provided");
    if (unlikely(path && path[0]))
        fs = new_subfs(fs, path, true);
    return ChunkFS::ctor(fs);
}

retval<ILogFileSystem*> new_chunkfs(const char* path, int ioengine) {
    return new_chunkfs(nullptr, new_localfs_adaptor(path, ioengine));
}

retval<void> chunkfs_format(const char* path, uint8_t sub_dir_shift) {
    return ChunkFS::format(path, sub_dir_shift);
}

} // namespace anystore
