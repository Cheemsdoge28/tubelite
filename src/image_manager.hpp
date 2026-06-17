#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <list>
#include <vector>

class ThumbnailAtlas;

class ImageManager {
public:
    ImageManager(SDL_Renderer* renderer);
    ~ImageManager();

    void setAtlas(ThumbnailAtlas* atlas) { atlas_ = atlas; }
    bool renderThumbnail(SDL_Renderer* renderer, const std::string& videoId, const SDL_Rect& dst);
    bool isLoaded(const std::string& videoId);

    // Returns texture if loaded. If not loaded, returns nullptr and queues download/load.
    SDL_Texture* getThumbnail(const std::string& videoId);
    
    // Call on main thread to convert loaded image data to SDL_Texture
    bool update(); 
    
    // Free all textures
    void clearCache();

private:
    void workerThread();

    SDL_Renderer* renderer_;
    std::unordered_map<std::string, SDL_Texture*> cache_;
    std::unordered_map<std::string, bool> loading_;
    std::list<std::string> cacheOrder_;
    
    struct PendingImage {
        std::string videoId;
        int width;
        int height;
        unsigned char* data;
    };
    
    std::queue<std::string> downloadQueue_;
    std::queue<PendingImage> textureQueue_;
    
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
    std::vector<std::thread> workers_;   // small pool; downloads are I/O-bound
    ThumbnailAtlas* atlas_{nullptr};
};
