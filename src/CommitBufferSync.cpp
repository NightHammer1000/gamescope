#include "CommitBufferSync.h"

#include "DmabufSync.h"
#include "gpuvis_trace_utils.h"
#include "log.hpp"
#include "rc.h"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "waitable.h"
#include "wlserver.hpp"

#include "wlr_begin.hpp"
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_buffer.h>
#include "wlr_end.hpp"

#include <cerrno>
#include <linux/sync_file.h>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace gamescope
{
    static LogScope s_BufferSyncLog( "buffer-sync" );
    static std::atomic<uint64_t> s_uNextSyncId = 0;

    struct BufferSyncStats
    {
        std::atomic<uint64_t> uFailures = 0;

        ~BufferSyncStats()
        {
            s_BufferSyncLog.infof( "DMA-BUF synchronization bridge failures: %llu",
                static_cast<unsigned long long>( uFailures.load() ) );
        }
    };
    static BufferSyncStats s_BufferSyncStats;

    struct DeferredReleaseBacklog
    {
        struct PerClient
        {
            uint32_t uPending = 0;
            bool bWarned = false;
        };

        void Add( pid_t nClientPid, uint64_t ulSyncId )
        {
            if ( nClientPid <= 0 )
                return;

            std::lock_guard lock( mutex );
            PerClient &client = clients[nClientPid];
            client.uPending++;
            if ( client.uPending >= 4 && !client.bWarned )
            {
                client.bWarned = true;
                s_BufferSyncLog.warnf(
                    "Application with pid %d has %u DMA-BUF releases awaiting GPU completion (latest sync %llu).",
                    nClientPid, client.uPending, static_cast<unsigned long long>( ulSyncId ) );
            }
        }

        void Remove( pid_t nClientPid )
        {
            if ( nClientPid <= 0 )
                return;

            std::lock_guard lock( mutex );
            auto iter = clients.find( nClientPid );
            if ( iter == clients.end() )
                return;

            if ( iter->second.uPending > 0 )
                iter->second.uPending--;
            if ( iter->second.uPending < 4 )
                iter->second.bWarned = false;
            if ( iter->second.uPending == 0 )
                clients.erase( iter );
        }

        std::mutex mutex;
        std::unordered_map<pid_t, PerClient> clients;
    };
    static DeferredReleaseBacklog s_DeferredReleaseBacklog;

    class CDeferredBufferRelease final : public RcObject, public IWaitable
    {
    public:
        CDeferredBufferRelease( int syncFileFd, wlr_buffer *pBuffer,
            std::shared_ptr<CReleaseTimelinePoint> pReleasePoint, uint64_t ulSyncId, pid_t nClientPid )
            : m_nSyncFileFd{ syncFileFd }
            , m_pBuffer{ pBuffer }
            , m_pReleasePoint{ std::move( pReleasePoint ) }
            , m_ulSyncId{ ulSyncId }
            , m_nClientPid{ nClientPid }
            , m_bExplicit{ bool( m_pReleasePoint ) }
        {
        }

        ~CDeferredBufferRelease()
        {
            if ( m_nSyncFileFd >= 0 )
                close( m_nSyncFileFd );
            Release();
        }

        int GetFD() override { return m_nSyncFileFd; }
        void OnPollIn() override;
        void OnPollError() override { OnPollIn(); }
        void OnPollHangUp() override { OnPollIn(); }

        int TakeFD() { return std::exchange( m_nSyncFileFd, -1 ); }
        wlr_buffer *TakeBuffer() { return std::exchange( m_pBuffer, nullptr ); }
        std::shared_ptr<CReleaseTimelinePoint> TakeReleasePoint() { return std::move( m_pReleasePoint ); }
        void MarkRegistered()
        {
            m_bRegistered = true;
            s_DeferredReleaseBacklog.Add( m_nClientPid, m_ulSyncId );
        }

    private:
        void Release()
        {
            if ( m_bRegistered.exchange( false ) )
                s_DeferredReleaseBacklog.Remove( m_nClientPid );

            if ( !m_pBuffer )
                return;

            m_pReleasePoint = nullptr;
            wlserver_lock();
            wlr_buffer_unlock( m_pBuffer );
            wlserver_unlock();
            m_pBuffer = nullptr;
            gpuvis_trace_printf( "buffer sync %llu completed deferred %s release",
                static_cast<unsigned long long>( m_ulSyncId ), m_bExplicit ? "explicit" : "implicit" );
        }

        int m_nSyncFileFd = -1;
        wlr_buffer *m_pBuffer = nullptr;
        std::shared_ptr<CReleaseTimelinePoint> m_pReleasePoint;
        uint64_t m_ulSyncId = 0;
        pid_t m_nClientPid = -1;
        bool m_bExplicit = false;
        std::atomic<bool> m_bRegistered = false;
    };

    static CAsyncWaiter<Rc<CDeferredBufferRelease>> &GetBufferReleaseWaiter()
    {
        static CAsyncWaiter<Rc<CDeferredBufferRelease>> s_Waiter{ "buffer-release" };
        return s_Waiter;
    }

    void CDeferredBufferRelease::OnPollIn()
    {
        GetBufferReleaseWaiter().RemoveWaitable( Rc<CDeferredBufferRelease>{ this } );
        if ( m_nSyncFileFd >= 0 )
        {
            close( m_nSyncFileFd );
            m_nSyncFileFd = -1;
        }
        Release();
    }

    class CRelaySyncFileLifetime final : public RcObject, public IWaitable
    {
    public:
        explicit CRelaySyncFileLifetime( std::shared_ptr<VulkanTimelineSyncFile_t> pSyncFile )
            : m_pSyncFile{ std::move( pSyncFile ) }
            , m_nSyncFileFd{ m_pSyncFile ? m_pSyncFile->DuplicateSyncFile() : -1 }
        {
        }

        ~CRelaySyncFileLifetime()
        {
            if ( m_nSyncFileFd >= 0 )
                close( m_nSyncFileFd );
        }

        int GetFD() override { return m_nSyncFileFd; }
        void OnPollIn() override;
        void OnPollError() override { OnPollIn(); }
        void OnPollHangUp() override { OnPollIn(); }

        int TakeFD() { return std::exchange( m_nSyncFileFd, -1 ); }
        std::shared_ptr<VulkanTimelineSyncFile_t> TakeSyncFile() { return std::move( m_pSyncFile ); }

    private:
        std::shared_ptr<VulkanTimelineSyncFile_t> m_pSyncFile;
        int m_nSyncFileFd = -1;
    };

    static CAsyncWaiter<Rc<CRelaySyncFileLifetime>> &GetRelaySyncFileLifetimeWaiter()
    {
        static CAsyncWaiter<Rc<CRelaySyncFileLifetime>> s_Waiter{ "relay-sync-file" };
        return s_Waiter;
    }

    void CRelaySyncFileLifetime::OnPollIn()
    {
        GetRelaySyncFileLifetimeWaiter().RemoveWaitable( Rc<CRelaySyncFileLifetime>{ this } );
    }

    static void RetainRelaySyncFileUntilComplete( std::shared_ptr<VulkanTimelineSyncFile_t> pSyncFile )
    {
        Rc<CRelaySyncFileLifetime> lifetime = new CRelaySyncFileLifetime{ std::move( pSyncFile ) };
        if ( lifetime->GetFD() < 0 )
            return;
        if ( GetRelaySyncFileLifetimeWaiter().AddWaitable( lifetime ) )
            return;

        // Keep the pending Vulkan signal semaphore alive even if epoll could
        // not accept its exported sync_file.
        const int nSyncFileFd = lifetime->TakeFD();
        std::shared_ptr<VulkanTimelineSyncFile_t> pFallbackSyncFile = lifetime->TakeSyncFile();
        std::thread( [nSyncFileFd, pFallbackSyncFile = std::move( pFallbackSyncFile )]
        {
            pollfd pfd = { .fd = nSyncFileFd, .events = POLLIN };
            int ret;
            do
            {
                ret = poll( &pfd, 1, -1 );
            }
            while ( ret < 0 && errno == EINTR );
            close( nSyncFileFd );
            (void)pFallbackSyncFile;
        } ).detach();
    }

    class CAcquireFenceRelay final : public RcObject, public IWaitable
    {
    public:
        CAcquireFenceRelay( int nSourceSyncFile, std::shared_ptr<CTimeline> pRelayTimeline,
            std::shared_ptr<VulkanTimelineSyncFile_t> pRelaySyncFile, uint64_t ulRelayPoint,
            uint64_t ulSyncId, pid_t nClientPid )
            : m_nSourceSyncFile{ nSourceSyncFile }
            , m_pRelayTimeline{ std::move( pRelayTimeline ) }
            , m_pRelaySyncFile{ std::move( pRelaySyncFile ) }
            , m_ulRelayPoint{ ulRelayPoint }
            , m_ulSyncId{ ulSyncId }
            , m_nClientPid{ nClientPid }
        {
        }

        ~CAcquireFenceRelay()
        {
            if ( m_nSourceSyncFile >= 0 )
                close( m_nSourceSyncFile );
        }

        int GetFD() override { return m_nSourceSyncFile; }
        void OnPollIn() override;
        void OnPollError() override { Complete( true ); }
        void OnPollHangUp() override { Complete( true ); }

    private:
        void Complete( bool bPollError );

        int m_nSourceSyncFile = -1;
        std::shared_ptr<CTimeline> m_pRelayTimeline;
        std::shared_ptr<VulkanTimelineSyncFile_t> m_pRelaySyncFile;
        uint64_t m_ulRelayPoint = 0;
        uint64_t m_ulSyncId = 0;
        pid_t m_nClientPid = -1;
        std::atomic<bool> m_bComplete = false;
    };

    static CAsyncWaiter<Rc<CAcquireFenceRelay>> &GetAcquireFenceRelayWaiter()
    {
        static CAsyncWaiter<Rc<CAcquireFenceRelay>> s_Waiter{ "acquire-relay" };
        return s_Waiter;
    }

    void CAcquireFenceRelay::OnPollIn()
    {
        Complete( false );
    }

    void CAcquireFenceRelay::Complete( bool bPollError )
    {
        if ( m_bComplete.exchange( true ) )
            return;

        GetAcquireFenceRelayWaiter().RemoveWaitable( Rc<CAcquireFenceRelay>{ this } );

        int nFenceStatus = bPollError ? -EIO : 1;
        if ( m_nSourceSyncFile >= 0 )
        {
            sync_file_info info = {};
            if ( ioctl( m_nSourceSyncFile, SYNC_IOC_FILE_INFO, &info ) == 0 )
                nFenceStatus = info.status;
            close( m_nSourceSyncFile );
            m_nSourceSyncFile = -1;
        }

        if ( nFenceStatus < 0 )
        {
            s_BufferSyncLog.warnf( "DMA-BUF sync %llu for pid %d source acquire completed with error %d; unblocking local relay.",
                static_cast<unsigned long long>( m_ulSyncId ), m_nClientPid, nFenceStatus );
        }

        const bool bVulkanSignalled = m_pRelayTimeline->SignalPoint( m_ulRelayPoint );
        if ( !bVulkanSignalled )
        {
            const uint64_t count = ++s_BufferSyncStats.uFailures;
            s_BufferSyncLog.errorf( "DMA-BUF sync %llu for pid %d failed to signal its local acquire relay; failure count is %llu.",
                static_cast<unsigned long long>( m_ulSyncId ), m_nClientPid,
                static_cast<unsigned long long>( count ) );
        }
        else
        {
            gpuvis_trace_printf( "buffer sync %llu acquire relay signalled",
                static_cast<unsigned long long>( m_ulSyncId ) );
        }
    }

    static void LogModeOnce( pid_t pid, bool bExplicit, bool bInterop )
    {
        static std::mutex s_Mutex;
        static std::unordered_map<pid_t, uint8_t> s_LoggedModes;
        const uint8_t bit = bInterop ? ( bExplicit ? 1u : 2u ) : 4u;
        {
            std::lock_guard lock( s_Mutex );
            uint8_t &modes = s_LoggedModes[pid];
            if ( modes & bit )
                return;
            modes |= bit;
        }

        if ( !bInterop )
            s_BufferSyncLog.warnf( "Application with pid %d is using baseline DMA-BUF synchronization; sync_file interop is unavailable.", pid );
        else if ( bExplicit )
            s_BufferSyncLog.infof( "Application with pid %d is using relayed explicit DMA-BUF synchronization with overlapped GPU submission and completion-delayed release.", pid );
        else
            s_BufferSyncLog.infof( "Application with pid %d is using relayed implicit DMA-BUF synchronization with overlapped GPU submission and completion-delayed release.", pid );
    }

    CCommitBufferSync::CCommitBufferSync( wlr_buffer *pBuffer,
        std::shared_ptr<CAcquireTimelinePoint> pAcquirePoint,
        std::shared_ptr<CReleaseTimelinePoint> pReleasePoint,
        pid_t nClientPid )
        : m_pBuffer{ pBuffer }
        , m_ulSyncId{ ++s_uNextSyncId }
        , m_nClientPid{ nClientPid }
        , m_pAcquirePoint{ std::move( pAcquirePoint ) }
        , m_pReleasePoint{ std::move( pReleasePoint ) }
    {
        wlr_dmabuf_attributes dmabuf = {};
        if ( !wlr_buffer_get_dmabuf( pBuffer, &dmabuf ) )
            return;

        m_bDmabuf = true;
        m_bSyncFileInterop = DmabufSyncFileSupported();
        LogModeOnce( m_nClientPid, bool( m_pAcquirePoint || m_pReleasePoint ), m_bSyncFileInterop );

        m_DmabufFds.reserve( dmabuf.n_planes );
        for ( int i = 0; i < dmabuf.n_planes; i++ )
        {
            const int fd = dup( dmabuf.fd[i] );
            if ( fd < 0 )
                break;
            m_DmabufFds.push_back( fd );
        }
    }

    CCommitBufferSync::~CCommitBufferSync()
    {
        FinishRelease();

        if ( m_nAcquireSyncFile >= 0 )
            close( m_nAcquireSyncFile );

        for ( int fd : m_DmabufFds )
            close( fd );
        m_DmabufFds.clear();

    }

    void CCommitBufferSync::RecordVulkanUse( std::shared_ptr<CTimeline> pTimeline, uint64_t ulPoint )
    {
        if ( ulPoint >= m_ulLastUsePoint )
        {
            m_pLastUseTimeline = std::move( pTimeline );
            m_ulLastUsePoint = ulPoint;
            gpuvis_trace_printf( "buffer sync %llu Vulkan use point %llu",
                static_cast<unsigned long long>( m_ulSyncId ), static_cast<unsigned long long>( ulPoint ) );
        }
    }

    void CCommitBufferSync::RecordFailure( const char *pszOperation ) const
    {
        const uint64_t count = ++s_BufferSyncStats.uFailures;
        s_BufferSyncLog.errorf( "DMA-BUF sync %llu for pid %d failed during %s; fallback count is %llu.",
            static_cast<unsigned long long>( m_ulSyncId ), m_nClientPid, pszOperation,
            static_cast<unsigned long long>( count ) );
    }

    void CCommitBufferSync::FinishRelease()
    {
        if ( !m_pBuffer )
            return;

        if ( !m_pLastUseTimeline || !m_ulLastUsePoint )
        {
            gpuvis_trace_printf( "buffer sync %llu released without GPU use", static_cast<unsigned long long>( m_ulSyncId ) );
            m_pReleasePoint = nullptr;
            UnlockBuffer();
            return;
        }

        const bool bExplicit = bool( m_pReleasePoint );

        // Do not transfer a future compositor fence into an explicit producer's
        // release timeline. Together with the producer acquire imported by the
        // compositor, that creates a two-way cross-driver dependency chain.
        // Keep the release point alive and signal it from the completion worker
        // only after the compositor has actually finished reading the buffer.
        //
        // Implicit Xwayland clients need the same delayed wl_buffer.release:
        // Xwayland may otherwise signal Present idle without forwarding the
        // DMA-BUF reservation fence to an explicit Vulkan producer.
        gpuvis_trace_printf( "buffer sync %llu deferred %s release until completion",
            static_cast<unsigned long long>( m_ulSyncId ), bExplicit ? "explicit" : "implicit" );
        DeferReleaseUntilComplete( m_pLastUseTimeline->ExportSyncFile( m_ulLastUsePoint ) );
    }

    void CCommitBufferSync::UnlockBuffer()
    {
        if ( !m_pBuffer )
            return;

        wlserver_lock();
        wlr_buffer_unlock( m_pBuffer );
        wlserver_unlock();
        m_pBuffer = nullptr;
    }

    void CCommitBufferSync::DeferReleaseUntilComplete( int syncFileFd )
    {
        if ( syncFileFd < 0 && m_pLastUseTimeline )
        {
            CAcquireTimelinePoint completionPoint{ m_pLastUseTimeline, m_ulLastUsePoint };
            const std::pair<int32_t, bool> event = completionPoint.CreateEventFd();
            if ( event.second )
            {
                m_pReleasePoint = nullptr;
                UnlockBuffer();
                return;
            }
            syncFileFd = event.first;
        }

        if ( syncFileFd >= 0 )
        {
            Rc<CDeferredBufferRelease> deferred = new CDeferredBufferRelease{
                syncFileFd, std::exchange( m_pBuffer, nullptr ), std::move( m_pReleasePoint ),
                m_ulSyncId, m_nClientPid };
            deferred->MarkRegistered();
            if ( GetBufferReleaseWaiter().AddWaitable( deferred ) )
                return;

            // Fall back to the existing detached error-path waiter if epoll
            // could not accept the completion fence.
            RecordFailure( "deferred release registration" );
            syncFileFd = deferred->TakeFD();
            m_pBuffer = deferred->TakeBuffer();
            m_pReleasePoint = deferred->TakeReleasePoint();
        }

        wlr_buffer *pBuffer = std::exchange( m_pBuffer, nullptr );
        std::shared_ptr<CReleaseTimelinePoint> pReleasePoint = std::move( m_pReleasePoint );
        std::shared_ptr<CTimeline> pTimeline = m_pLastUseTimeline;
        const uint64_t ulPoint = m_ulLastUsePoint;

        std::thread( [syncFileFd, pBuffer, pReleasePoint = std::move( pReleasePoint ),
                         pTimeline = std::move( pTimeline ), ulPoint]() mutable
        {
            bool complete = false;
            if ( syncFileFd >= 0 )
            {
                pollfd pfd = { .fd = syncFileFd, .events = POLLIN };
                int ret;
                do
                {
                    ret = poll( &pfd, 1, -1 );
                }
                while ( ret < 0 && errno == EINTR );
                complete = ret > 0;
                close( syncFileFd );
            }
            if ( !complete && pTimeline )
                complete = pTimeline->WaitPoint( ulPoint );

            if ( !complete )
                return;

            pReleasePoint = nullptr;
            wlserver_lock();
            wlr_buffer_unlock( pBuffer );
            wlserver_unlock();
        } ).detach();
    }

    CCommitBufferSync::AcquireStatus CCommitBufferSync::PrepareAcquire()
    {
        if ( !UsesSyncFileInterop() || m_bAcquireFallback.load() )
            return AcquireStatus::Baseline;
        if ( m_pAcquireRelayTimeline )
            return AcquireStatus::Ready;

        if ( m_nAcquireSyncFile < 0 && m_pAcquirePoint )
        {
            if ( !m_pAcquirePoint->IsMaterialized() )
                return AcquireStatus::Pending;
            m_nAcquireSyncFile = m_pAcquirePoint->CreateSyncFile();
        }
        else if ( m_nAcquireSyncFile < 0 && !m_DmabufFds.empty() )
        {
            m_nAcquireSyncFile = ExportDmabufSyncFile( m_DmabufFds.front(), DmabufAccess::Read );
        }

        if ( m_nAcquireSyncFile >= 0 )
        {
            gpuvis_trace_printf( "buffer sync %llu acquire materialized", static_cast<unsigned long long>( m_ulSyncId ) );

            // NVIDIA's Vulkan OPAQUE_FD for a timeline can be imported by DRM
            // as a binary syncobj, so DRM timeline operations on it fail with
            // EINVAL. Keep that relay Vulkan-only and materialize a separate
            // local SYNC_FD for KMS. Other drivers retain the DRM-shared
            // timeline path, avoiding RADV's synchronous pending-fence export.
            constexpr uint32_t uNvidiaVendorId = 0x10de;
            const bool bNeedsVulkanKmsBridge = g_device.vendorID() == uNvidiaVendorId;
            std::shared_ptr<CTimeline> pRelayTimeline = bNeedsVulkanKmsBridge
                ? CTimeline::CreateVulkanOnly()
                : CTimeline::Create();
            constexpr uint64_t ulRelayPoint = 1;
            std::shared_ptr<VulkanTimelineSyncFile_t> pRelaySyncFile = pRelayTimeline && bNeedsVulkanKmsBridge
                ? g_device.CreateTimelineSyncFile( pRelayTimeline->ToVkSemaphore(), ulRelayPoint )
                : nullptr;
            if ( pRelaySyncFile )
                RetainRelaySyncFileUntilComplete( pRelaySyncFile );
            const int nRelaySource = DuplicateSourceAcquireSyncFile();
            const bool bKmsRelayReady = !bNeedsVulkanKmsBridge || bool( pRelaySyncFile );
            if ( pRelayTimeline && bKmsRelayReady && nRelaySource >= 0 )
            {
                Rc<CAcquireFenceRelay> pRelay = new CAcquireFenceRelay{
                    nRelaySource, pRelayTimeline, pRelaySyncFile, ulRelayPoint, m_ulSyncId, m_nClientPid };
                if ( GetAcquireFenceRelayWaiter().AddWaitable( pRelay ) )
                {
                    m_pAcquireRelayTimeline = std::move( pRelayTimeline );
                    m_pAcquireRelaySyncFile = std::move( pRelaySyncFile );
                    m_ulAcquireRelayPoint = ulRelayPoint;
                    gpuvis_trace_printf( "buffer sync %llu acquire relay armed",
                        static_cast<unsigned long long>( m_ulSyncId ) );
                    return AcquireStatus::Ready;
                }
            }
            else if ( nRelaySource >= 0 )
            {
                close( nRelaySource );
            }

            // A private bridge submission may already be waiting on this
            // point. It will not be exposed to consumers after setup failure,
            // so signal it to let the bridge and its semaphore retire.
            if ( pRelayTimeline && pRelaySyncFile )
                pRelayTimeline->SignalPoint( ulRelayPoint );

            RecordFailure( "acquire relay creation" );
            return AcquireStatus::Failed;
        }

        RecordFailure( "acquire export" );
        return AcquireStatus::Failed;
    }

    void CCommitBufferSync::UseAcquireFallback()
    {
        m_bAcquireFallback = true;
        m_bAcquireFallbackReady = false;
    }

    void CCommitBufferSync::MarkAcquireFallbackReady()
    {
        m_bAcquireFallbackReady = true;
        gpuvis_trace_printf( "buffer sync %llu acquire fallback complete", static_cast<unsigned long long>( m_ulSyncId ) );
    }

    bool CCommitBufferSync::BeginAcquireFallbackWait()
    {
        bool expected = false;
        if ( !m_bAcquireFallbackWaitStarted.compare_exchange_strong( expected, true ) )
            return true;

        UseAcquireFallback();
        const int syncFileFd = DuplicateSourceAcquireSyncFile();
        if ( syncFileFd < 0 )
            return false;

        std::shared_ptr<CCommitBufferSync> self = shared_from_this();
        std::thread( [self = std::move( self ), syncFileFd]
        {
            pollfd pfd = { .fd = syncFileFd, .events = POLLIN };
            int ret;
            do
            {
                ret = poll( &pfd, 1, -1 );
            }
            while ( ret < 0 && errno == EINTR );
            close( syncFileFd );
            if ( ret <= 0 )
                return;

            self->MarkAcquireFallbackReady();
            nudge_steamcompmgr();
        } ).detach();
        return true;
    }

    std::pair<int32_t, bool> CCommitBufferSync::CreateAcquireAvailabilityEvent() const
    {
        if ( !m_pAcquirePoint )
            return CAcquireTimelinePoint::k_InvalidEvent;
        return m_pAcquirePoint->CreateAvailabilityEventFd();
    }

    int CCommitBufferSync::DuplicateAcquireSyncFile() const
    {
        if ( m_pAcquireRelaySyncFile )
            return m_pAcquireRelaySyncFile->DuplicateSyncFile();
        if ( m_pAcquireRelayTimeline && m_ulAcquireRelayPoint )
            return m_pAcquireRelayTimeline->ExportSyncFile( m_ulAcquireRelayPoint );
        return DuplicateSourceAcquireSyncFile();
    }

    int CCommitBufferSync::DuplicateSourceAcquireSyncFile() const
    {
        return m_nAcquireSyncFile >= 0 ? dup( m_nAcquireSyncFile ) : -1;
    }

    std::pair<int32_t, bool> CCommitBufferSync::DuplicateBaselineWaitFd() const
    {
        if ( m_pAcquirePoint )
            return m_pAcquirePoint->CreateEventFd();
        return { !m_DmabufFds.empty() ? dup( m_DmabufFds.front() ) : -1, false };
    }
}
