#include "DmabufSync.h"

#if defined(__linux__)
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace gamescope
{
#if defined(__linux__)
    static uint32_t ToFlags( DmabufAccess access )
    {
        return access == DmabufAccess::Read ? DMA_BUF_SYNC_READ : DMA_BUF_SYNC_WRITE;
    }
#endif

    bool DmabufSyncFileSupported()
    {
#if defined(__linux__)
        struct utsname uts = {};
        if ( uname( &uts ) != 0 || strcmp( uts.sysname, "Linux" ) != 0 )
            return false;

        char *minorString = nullptr;
        const long major = strtol( uts.release, &minorString, 10 );
        if ( !minorString || *minorString != '.' )
            return false;

        char *patchString = nullptr;
        const long minor = strtol( minorString + 1, &patchString, 10 );
        if ( !patchString || ( *patchString != '.' && *patchString != '\0' ) )
            return false;

        return major > 5 || ( major == 5 && minor >= 20 );
#else
        return false;
#endif
    }

    int ExportDmabufSyncFile( int dmabufFd, DmabufAccess access )
    {
#if defined(__linux__)
        dma_buf_export_sync_file data = {
            .flags = ToFlags( access ),
            .fd = -1,
        };
        if ( ioctl( dmabufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &data ) != 0 )
            return -1;

        return data.fd;
#else
        errno = ENOTSUP;
        return -1;
#endif
    }

    bool ImportDmabufSyncFile( int dmabufFd, DmabufAccess access, int syncFileFd )
    {
#if defined(__linux__)
        dma_buf_import_sync_file data = {
            .flags = ToFlags( access ),
            .fd = syncFileFd,
        };
        return ioctl( dmabufFd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data ) == 0;
#else
        errno = ENOTSUP;
        return false;
#endif
    }
}
