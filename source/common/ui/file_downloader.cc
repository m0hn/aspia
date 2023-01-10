//
// Aspia Project
// Copyright (C) 2016-2023 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "common/ui/file_downloader.h"

#include "base/environment.h"
#include "base/logging.h"
#include "base/net/curl_util.h"

namespace common {

FileDownloader::FileDownloader()
{
    LOG(LS_INFO) << "Ctor";
}

FileDownloader::~FileDownloader()
{
    LOG(LS_INFO) << "Dtor";

    delegate_ = nullptr;
    thread_.stop();
}

void FileDownloader::start(std::string_view url,
                           std::shared_ptr<base::TaskRunner> owner_task_runner,
                           Delegate* delegate)
{
    url_ = url;
    owner_task_runner_ = std::move(owner_task_runner);
    delegate_ = delegate;

    DCHECK(owner_task_runner_);
    DCHECK(delegate_);

    thread_.start(std::bind(&FileDownloader::run, this));
}

void FileDownloader::run()
{
    base::ScopedCURL curl;

    curl_easy_setopt(curl.get(), CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 15);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1);

    long verify_peer = 1;
    if (base::Environment::has("ASPIA_NO_VERIFY_TLS_PEER"))
        verify_peer = 0;

    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, verify_peer);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeDataCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, this);

    base::ScopedCURLM multi_curl;
    curl_multi_add_handle(multi_curl.get(), curl.get());

    CURLMcode error_code = CURLM_OK;
    int still_running = 1;

    do
    {
        error_code = curl_multi_perform(multi_curl.get(), &still_running);
        if (!error_code)
        {
              // Wait for activity, timeout or "nothing".
              error_code = curl_multi_poll(multi_curl.get(), nullptr, 0, 1000, nullptr);
        }

        if (error_code)
        {
            LOG(LS_WARNING) << "curl_multi_poll failed: " << error_code;
            break;
        }

        if (thread_.isStopping())
        {
            LOG(LS_INFO) << "Downloading canceled";
            break;
        }
    }
    while (still_running);

    curl_multi_remove_handle(multi_curl.get(), curl.get());

    if (!thread_.isStopping())
    {
        if (error_code != CURLM_OK)
        {
            onError(error_code);
        }
        else
        {
            LOG(LS_INFO) << "Download is finished: " << data_.size() << " bytes";
            onCompleted();
        }
    }
}

void FileDownloader::onError(int error_code)
{
    if (!owner_task_runner_->belongsToCurrentThread())
    {
        owner_task_runner_->postTask(std::bind(&FileDownloader::onError, this, error_code));
        return;
    }

    if (delegate_)
    {
        delegate_->onFileDownloaderError(error_code);
        delegate_ = nullptr;
    }
}

void FileDownloader::onCompleted()
{
    if (!owner_task_runner_->belongsToCurrentThread())
    {
        owner_task_runner_->postTask(std::bind(&FileDownloader::onCompleted, this));
        return;
    }

    if (delegate_)
    {
        delegate_->onFileDownloaderCompleted();
        delegate_ = nullptr;
    }
}

void FileDownloader::onProgress(int percentage)
{
    if (!owner_task_runner_->belongsToCurrentThread())
    {
        owner_task_runner_->postTask(std::bind(&FileDownloader::onProgress, this, percentage));
        return;
    }

    if (delegate_)
        delegate_->onFileDownloaderProgress(percentage);
}

// static
size_t FileDownloader::writeDataCallback(void* ptr, size_t size, size_t nmemb, FileDownloader* self)
{
    size_t result = 0;

    if (self)
    {
        if (self->thread_.isStopping())
        {
            LOG(LS_INFO) << "Interrupted by user";
            return 0;
        }

        result = size * nmemb;
        base::append(&self->data_, ptr, result);
    }

    return result;
}

// static
int FileDownloader::progressCallback(
    FileDownloader* self, double dltotal, double dlnow, double /* ultotal */, double /* ulnow */)
{
    if (self && !self->thread_.isStopping())
    {
        int percentage = 0;
        if (dltotal > 0)
            percentage = static_cast<int>((dlnow * 100) / dltotal);

        self->onProgress(percentage);
    }

    return 0;
}

} // namespace common