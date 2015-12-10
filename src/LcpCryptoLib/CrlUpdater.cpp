//
//  Created by Artem Brazhnikov on 11/15.
//  Copyright © 2015 Mantano. All rights reserved.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
//  Licensed under Gnu Affero General Public License Version 3 (provided, notwithstanding this notice,
//  Readium Foundation reserves the right to license this material under a different separate license,
//  and if you have done so, the terms of that separate license control and the following references
//  to GPL do not apply).
//
//  This program is free software: you can redistribute it and/or modify it under the terms of the GNU
//  Affero General Public License as published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version. You should have received a copy of the GNU
//  Affero General Public License along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <algorithm>
#include <iterator>
#include <thread>
#include "CrlUpdater.h"
#include "SimpleMemoryWritableStream.h"
#include "DownloadInMemoryRequest.h"
#include "DateTime.h"
#include "ThreadTimer.h"

namespace lcp
{
    const int CrlUpdater::TenMinutesPeriod = 1000 * 60 * 10;
    
    CrlUpdater::CrlUpdater(
        INetProvider * netProvider,
        ICertificateRevocationList * revocationList,
        ThreadTimer * threadTimer,
        const std::string & defaultCrlUrl
        )
        : m_requestRunning(false)
        , m_netProvider(netProvider)
        , m_revocationList(revocationList)
        , m_threadTimer(threadTimer)
        , m_currentRequestStatus(Status(StatusCode::ErrorCommonSuccess))
    {
        if (!defaultCrlUrl.empty())
        {
            m_crlUrls.push_back(defaultCrlUrl);
        }
    }

    void CrlUpdater::UpdateCrlUrls(ICrlDistributionPoints * distributionPoints)
    {
        std::unique_lock<std::mutex> locker(m_downloadSync);

        if (distributionPoints != nullptr && distributionPoints->HasCrlDistributionPoints())
        {
            const StringsList & newUrls = distributionPoints->CrlDistributionPointUrls();
            std::copy_if(
                newUrls.begin(), newUrls.end(),
                std::back_inserter(m_crlUrls),
                [&](const std::string & val) { return (std::find(m_crlUrls.begin(), m_crlUrls.end(), val) == m_crlUrls.end()); }
            );
        }
    }

    bool CrlUpdater::ContainsUrl(const std::string & url)
    {
        std::unique_lock<std::mutex> locker(m_downloadSync);
        return (std::find(m_crlUrls.begin(), m_crlUrls.end(), url) != m_crlUrls.end());
    }

    bool CrlUpdater::ContainsAnyUrl() const
    {
        std::unique_lock<std::mutex> locker(m_downloadSync);
        return !m_crlUrls.empty();
    }

    void CrlUpdater::Update()
    {
        std::unique_lock<std::mutex> locker(m_downloadSync);

        m_currentRequestStatus = Status(StatusCode::ErrorCommonFail);
        for (auto const & url : m_crlUrls)
        {
            this->Download(url);
            m_conditionDownload.wait(locker, [&]() { return !m_requestRunning; });

            if (Status::IsSuccess(m_currentRequestStatus) || m_downloadRequest->Canceled())
            {
                break;
            }
        }
    }

    void CrlUpdater::Cancel()
    {
        std::unique_lock<std::mutex> locker(m_downloadSync);
        if (m_downloadRequest.get() != nullptr)
        {
            m_downloadRequest->SetCanceled(true);
        }
    }

    void CrlUpdater::Download(const std::string & url)
    {
        m_crlStream.reset(new SimpleMemoryWritableStream());
        m_downloadRequest.reset(new DownloadInMemoryRequest(url, m_crlStream.get()));
        m_netProvider->StartDownloadRequest(m_downloadRequest.get(), this);
        m_requestRunning = true;
    }

    void CrlUpdater::OnRequestStarted(INetRequest * request)
    {
    }

    void CrlUpdater::OnRequestProgressed(INetRequest * request, float progress)
    {
    }

    void CrlUpdater::OnRequestCanceled(INetRequest * request)
    {
        std::unique_lock<std::mutex> locker(m_downloadSync);
        m_requestRunning = false;
        m_conditionDownload.notify_one();
    }

    void CrlUpdater::OnRequestEnded(INetRequest * request, Status result)
    {
        std::unique_lock<std::mutex> locker(m_downloadSync);
        if (Status::IsSuccess(result))
        {
            m_revocationList->UpdateRevocationList(m_crlStream->Buffer());
            this->ResetNextUpdate();
        }
        m_currentRequestStatus = result;
        m_requestRunning = false;
        m_conditionDownload.notify_one();
    }

    void CrlUpdater::ResetNextUpdate()
    {
        if (m_revocationList->HasNextUpdateDate())
        {
            DateTime nextUpdate(m_revocationList->NextUpdateDate());
            if (nextUpdate > DateTime::Now())
            {
                m_threadTimer->SetUsage(ThreadTimer::TimePointUsage);
                m_threadTimer->SetTimePoint(ThreadTimer::ClockType::from_time_t(nextUpdate.ToTime()));
            }
            else
            {
                m_threadTimer->SetUsage(ThreadTimer::DurationUsage);
                m_threadTimer->SetDuration(ThreadTimer::DurationType(TenMinutesPeriod));
            }
        }
        else
        {
            m_threadTimer->SetUsage(ThreadTimer::DurationUsage);
            m_threadTimer->SetDuration(ThreadTimer::DurationType(TenMinutesPeriod));
        }
    }
}