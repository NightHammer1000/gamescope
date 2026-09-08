#pragma once

#include "Timeline.h"

#include <atomic>
#include <memory>
#include <sys/types.h>
#include <vector>

struct wlr_buffer;
struct VulkanTimelineSyncFile_t;

namespace gamescope
{
    // Synchronization and Wayland lifetime belong to one surface commit, not
    // to the memoized Vulkan texture or backend framebuffer shared by commits.
    class CCommitBufferSync final : public std::enable_shared_from_this<CCommitBufferSync>
    {
    public:
        enum class AcquireStatus
        {
            Ready,
            Pending,
            Failed,
            Baseline,
        };

        CCommitBufferSync( wlr_buffer *pBuffer,
            std::shared_ptr<CAcquireTimelinePoint> pAcquirePoint,
            std::shared_ptr<CReleaseTimelinePoint> pReleasePoint,
            pid_t nClientPid );
        ~CCommitBufferSync();

        CCommitBufferSync( const CCommitBufferSync & ) = delete;
        CCommitBufferSync &operator=( const CCommitBufferSync & ) = delete;

        wlr_buffer *GetBuffer() const { return m_pBuffer; }
        bool IsDmabuf() const { return m_bDmabuf; }
        const std::vector<int> &GetDmabufFds() const { return m_DmabufFds; }
        const std::shared_ptr<CAcquireTimelinePoint> &GetAcquirePoint() const { return m_pAcquirePoint; }
        const std::shared_ptr<CReleaseTimelinePoint> &GetReleasePoint() const { return m_pReleasePoint; }
        const std::shared_ptr<CTimeline> &GetAcquireRelayTimeline() const { return m_pAcquireRelayTimeline; }
        uint64_t GetAcquireRelayPoint() const { return m_ulAcquireRelayPoint; }

        AcquireStatus PrepareAcquire();
        std::pair<int32_t, bool> CreateAcquireAvailabilityEvent() const;
        int DuplicateAcquireSyncFile() const;
        std::pair<int32_t, bool> DuplicateBaselineWaitFd() const;
        void UseAcquireFallback();
        void MarkAcquireFallbackReady();
        bool BeginAcquireFallbackWait();
        bool IsAcquireFallback() const { return m_bAcquireFallback.load(); }
        bool IsAcquireFallbackReady() const { return m_bAcquireFallbackReady.load(); }
        bool UsesSyncFileInterop() const { return m_bDmabuf && m_bSyncFileInterop; }
        void RecordVulkanUse( std::shared_ptr<CTimeline> pTimeline, uint64_t ulPoint );
        void RecordFailure( const char *pszOperation ) const;

    private:
        wlr_buffer *m_pBuffer = nullptr;
        bool m_bDmabuf = false;
        bool m_bSyncFileInterop = false;
        std::atomic<bool> m_bAcquireFallback = false;
        std::atomic<bool> m_bAcquireFallbackReady = false;
        std::atomic<bool> m_bAcquireFallbackWaitStarted = false;
        int m_nAcquireSyncFile = -1;
        std::shared_ptr<CTimeline> m_pAcquireRelayTimeline;
        std::shared_ptr<VulkanTimelineSyncFile_t> m_pAcquireRelaySyncFile;
        uint64_t m_ulAcquireRelayPoint = 0;
        uint64_t m_ulSyncId = 0;
        pid_t m_nClientPid = -1;
        std::vector<int> m_DmabufFds;
        std::shared_ptr<CAcquireTimelinePoint> m_pAcquirePoint;
        std::shared_ptr<CReleaseTimelinePoint> m_pReleasePoint;
        std::shared_ptr<CTimeline> m_pLastUseTimeline;
        uint64_t m_ulLastUsePoint = 0;

        void FinishRelease();
        void UnlockBuffer();
        void DeferReleaseUntilComplete( int syncFileFd );
        int DuplicateSourceAcquireSyncFile() const;
    };
}
