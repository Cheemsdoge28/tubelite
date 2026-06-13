#include "image_manager.hpp"
#include "thumbnail_atlas.hpp"
#include "stb_image.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <cstdio>

ImageManager::ImageManager(SDL_Renderer* renderer) : renderer_(renderer) {
    worker_ = std::thread(&ImageManager::workerThread, this);
}

ImageManager::~ImageManager() {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
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
        
        std::string url = "https://i.ytimg.com/vi/" + videoId + "/hqdefault.jpg";
        std::string dl = "curl -s -L \"" + url + "\"";
        
#ifdef _WIN32
        FILE* pipe = _popen(dl.c_str(), "rb");
#else
        FILE* pipe = popen(dl.c_str(), "r");
#endif

        std::vector<unsigned char> buffer;
        if (pipe) {
            unsigned char temp[4096];
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
        } else {
            textureQueue_.push({videoId, 0, 0, nullptr});
        }
    }
}
