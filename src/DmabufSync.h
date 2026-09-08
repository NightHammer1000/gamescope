#pragma once

#include <cstdint>

namespace gamescope
{
    enum class DmabufAccess : uint32_t
    {
        Read,
        Write,
    };

    // DMA_BUF_IOCTL_{IMPORT,EXPORT}_SYNC_FILE were added in Linux 5.20.
    bool DmabufSyncFileSupported();

    // The returned fd is owned by the caller. Import does not consume syncFileFd.
    int ExportDmabufSyncFile( int dmabufFd, DmabufAccess access );
    bool ImportDmabufSyncFile( int dmabufFd, DmabufAccess access, int syncFileFd );
}
