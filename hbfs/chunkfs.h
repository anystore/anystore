#pragma once
#include "../common/logfs.h"

namespace photon {
namespace fs {
class IFileSystem;
}
}

namespace anystore {


retval<void> chunkfs_format(const char* path, uint8_t sub_dir_shift /* 8~16 */ = 12);

retval<ILogFileSystem*> new_chunkfs(const char* path, int ioengine = 0);

retval<ILogFileSystem*> new_chunkfs(const char* path, photon::fs::IFileSystem* localfs);

} // namespace anystore
