#include "image_manager.hpp"
#include "stb_image.h"
#include <iostream>
#include <cstdlib>

ImageManager::ImageManager(SDL_Renderer* renderer) : renderer_(renderer) {
    std::string mkdirCmd = "mkdir -p /tmp/tubelite_thumbs";
    system(mkdirCmd.c_str());
    worker_ = std::thread(&ImageManager::workerThread, this);
}

ImageManager::~ImageManager() {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    clearCache();
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

void ImageManager::update() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!textureQueue_.empty()) {
        auto pending = textureQueue_.front();
        textureQueue_.pop();
        
        if (pending.data) {
            SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, pending.width, pending.height);
            if (tex) {
                SDL_UpdateTexture(tex, nullptr, pending.data, pending.width * 4);
                cache_[pending.videoId] = tex;
            } else {
                cache_[pending.videoId] = nullptr;
            }
            stbi_image_free(pending.data);
        } else {
            cache_[pending.videoId] = nullptr;
        }
    }
}

void ImageManager::clearCache() {
    for (auto& pair : cache_) {
        if (pair.second) SDL_DestroyTexture(pair.second);
    }
    cache_.clear();
    loading_.clear();
    
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
        
        std::string path = "/tmp/tubelite_thumbs/" + videoId + ".jpg";
        
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            std::string url = "https://i.ytimg.com/vi/" + videoId + "/hqdefault.jpg";
            std::string dl = "curl -s -o " + path + " \"" + url + "\"";
            system(dl.c_str());
            f = fopen(path.c_str(), "rb");
        }
        
        if (f) {
            fclose(f);
            int w, h, channels;
            unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
            
            std::lock_guard<std::mutex> lock(mutex_);
            textureQueue_.push({videoId, w, h, data});
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            textureQueue_.push({videoId, 0, 0, nullptr});
        }
    }
}
