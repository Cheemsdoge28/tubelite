#include "image_manager.hpp"
#include "thumbnail_atlas.hpp"
#include "stb_image.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <cstdio>

ImageManager::ImageManager(SDL_Renderer* renderer) : renderer_(renderer) {
    // A few workers so a grid's worth of thumbnails fetch concurrently instead
    // of trickling in one at a time (each fetch is network-bound). curl is
    // lightweight, so this doesn't compete meaningfully with decode/UI.
    const int kWorkers = 3;
    for (int i = 0; i < kWorkers; ++i) {
        workers_.emplace_back(&ImageManager::workerThread, this);
    }
}

ImageManager::~ImageManager() {
    running_ = false;
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    clearCache();
}

bool ImageManager::renderThumbnail(SDL_Renderer* renderer, const std::string& videoId, const SDL_Rect& dst) {
    if (atlas_) {
        if (atlas_->render(renderer, videoId, dst)) {
            return true;
        }
    } else {
        SDL_Texture* tex = getThumbnail(videoId);
        if (tex) {
            SDL_RenderCopy(renderer, tex, nullptr, &dst);
            return true;
        }
    }

    if (loading_.find(videoId) == loading_.end()) {
        loading_[videoId] = true;
        std::lock_guard<std::mutex> lock(mutex_);
        downloadQueue_.push(videoId);
        cv_.notify_one();
    }
    return false;
}

bool ImageManager::isLoaded(const std::string& videoId) {
    if (atlas_) {
        return atlas_->isLoaded(videoId);
    }
    return cache_.find(videoId) != cache_.end() && cache_[videoId] != nullptr;
}

SDL_Texture* ImageManager::getThumbnail(const std::string& videoId) {
    if (cache_.find(videoId) != cache_.end()) {
        return cache_[videoId];
    }
    
    if (loading_.find(videoId) == loading_.end()) {
        loading_[videoId] = true;
        std::lock_guard<std::mutex> lock(mutex_);
        downloadQueue_.push(videoId);
        cv_.notify_one();
    }
    
    return nullptr;
}

bool ImageManager::update() {
    std::queue<PendingImage> pendingTextures;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Refresh the snapshot counters the debug overlay reads.
        tele_.queue_depth.store((int)downloadQueue_.size(), std::memory_order_relaxed);
        tele_.texture_queue_depth.store((int)textureQueue_.size(), std::memory_order_relaxed);
        tele_.cache_size.store((int)cache_.size(), std::memory_order_relaxed);
        if (textureQueue_.empty()) return false;
        std::swap(pendingTextures, textureQueue_);
    }

    bool updated = false;
    while (!pendingTextures.empty()) {
        auto pending = pendingTextures.front();
        pendingTextures.pop();
        
        if (pending.data) {
            if (atlas_) {
                atlas_->upload(pending.videoId, pending.data, pending.width, pending.height);
                loading_[pending.videoId] = true;
                updated = true;
            } else {
                SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, pending.width, pending.height);
                if (tex) {
                    SDL_UpdateTexture(tex, nullptr, pending.data, pending.width * 4);
                    cache_[pending.videoId] = tex;
                    cacheOrder_.push_back(pending.videoId);
                    
                    if (cacheOrder_.size() > 20) {
                        std::string oldest = cacheOrder_.front();
                        cacheOrder_.pop_front();
                        if (cache_.find(oldest) != cache_.end()) {
                            if (cache_[oldest]) SDL_DestroyTexture(cache_[oldest]);
                            cache_.erase(oldest);
                            loading_.erase(oldest);
                        }
                    }
                } else {
                    cache_[pending.videoId] = nullptr;
                }
                updated = true;
            }
            stbi_image_free(pending.data);
        } else {
            if (atlas_) {
                loading_[pending.videoId] = true;
            } else {
                cache_[pending.videoId] = nullptr;
            }
        }
    }
    return updated;
}

void ImageManager::clearCache() {
    if (atlas_) {
        atlas_->clear();
    }
    for (auto& pair : cache_) {
        if (pair.second) SDL_DestroyTexture(pair.second);
    }
    cache_.clear();
    loading_.clear();
    cacheOrder_.clear();
    
    std::lock_guard<std::mutex> lock(mutex_);
    while (!downloadQueue_.empty()) downloadQueue_.pop();
    while (!textureQueue_.empty()) {
        if (textureQueue_.front().data) stbi_image_free(textureQueue_.front().data);
        textureQueue_.pop();
    }
}

void ImageManager::workerThread() {
    while (running_) {
        std::string videoId;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !downloadQueue_.empty() || !running_; });
            if (!running_) break;

            videoId = downloadQueue_.front();
            downloadQueue_.pop();
        }
        tele_.downloads_inflight.fetch_add(1, std::memory_order_relaxed);
        
        // mqdefault is 320x180 (true 16:9) — matches the card thumb aspect and
        // is ~1/3 the pixels of the 4:3 hqdefault, so it downloads, decodes and
        // stores much cheaper while still being sharp at card size.
        std::string url = "https://i.ytimg.com/vi/" + videoId + "/mqdefault.jpg";
        std::string dl = "curl -k -s -L \"" + url + "\"";
        
#ifdef _WIN32
        FILE* pipe = _popen(dl.c_str(), "rb");
#else
        FILE* pipe = popen(dl.c_str(), "r");
#endif

        std::vector<unsigned char> buffer;
        buffer.reserve(128 * 1024); // hqdefault.jpg is typically 20-60 KB
        if (pipe) {
            unsigned char temp[8192];
            while (true) {
                size_t n = fread(temp, 1, sizeof(temp), pipe);
                if (n == 0) break;
                buffer.insert(buffer.end(), temp, temp + n);
            }
#ifdef _WIN32
            _pclose(pipe);
#else
            pclose(pipe);
#endif
        }
        
        unsigned char* data = nullptr;
        int w = 0, h = 0, channels = 0;
        if (!buffer.empty()) {
            data = stbi_load_from_memory(buffer.data(), buffer.size(), &w, &h, &channels, 4);
            if (data) {
                // Swap Red and Blue channels (RGBA -> BGRA) to match SDL_PIXELFORMAT_ARGB8888 byte order
                for (int i = 0; i < w * h; ++i) {
                    unsigned char r = data[i * 4];
                    data[i * 4] = data[i * 4 + 2];
                    data[i * 4 + 2] = r;
                }
            }
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        if (data) {
            textureQueue_.push({videoId, w, h, data});
            tele_.thumbnails_loaded_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            textureQueue_.push({videoId, 0, 0, nullptr});
            tele_.thumbnails_failed_total.fetch_add(1, std::memory_order_relaxed);
        }
        tele_.downloads_inflight.fetch_sub(1, std::memory_order_relaxed);
    }
}
