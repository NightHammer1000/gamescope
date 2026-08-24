#pragma once

#include "Timeline.h"

#include <memory>
#include <vector>

struct wlr_buffer;

namespace gamescope
{
    // Synchronization and Wayland lifetime belong to one surface commit, not
    // to the memoized Vulkan texture or backend framebuffer shared by commits.
    class CCommitBufferSync final
    {
    public:
        CCommitBufferSync( wlr_buffer *pBuffer,
            std::shared_ptr<CAcquireTimelinePoint> pAcquirePoint,
            std::shared_ptr<CReleaseTimelinePoint> pReleasePoint );
        ~CCommitBufferSync();

        CCommitBufferSync( const CCommitBufferSync & ) = delete;
        CCommitBufferSync &operator=( const CCommitBufferSync & ) = delete;

        wlr_buffer *GetBuffer() const { return m_pBuffer; }
        bool IsDmabuf() const { return m_bDmabuf; }
        const std::vector<int> &GetDmabufFds() const { return m_DmabufFds; }
        const std::shared_ptr<CAcquireTimelinePoint> &GetAcquirePoint() const { return m_pAcquirePoint; }
        const std::shared_ptr<CReleaseTimelinePoint> &GetReleasePoint() const { return m_pReleasePoint; }

    private:
        wlr_buffer *m_pBuffer = nullptr;
        bool m_bDmabuf = false;
        std::vector<int> m_DmabufFds;
        std::shared_ptr<CAcquireTimelinePoint> m_pAcquirePoint;
        std::shared_ptr<CReleaseTimelinePoint> m_pReleasePoint;
    };
}
